#include <U8x8lib.h>
#include <EEPROM.h>
#include "soft_ads1115.h"

const uint8_t PIN_A = 2, PIN_B = 3, PIN_SW = 4, PIN_LED = 6;
const uint8_t CH[2] = {1, 0};
U8X8_SH1106_128X64_NONAME_SW_I2C oled(A4, A5, U8X8_PIN_NONE);
SoftADS1115 ads;
const uint16_t GAINS[5] = {GAIN_ONE, GAIN_TWO, GAIN_FOUR, GAIN_EIGHT, GAIN_SIXTEEN};
const float FS[5] = {4.096, 2.048, 1.024, 0.512, 0.256};
const uint8_t GX[5] = {1, 2, 4, 8, 16};
const uint16_t R128 = 0x0080, R250 = 0x00A0, R860 = 0x00E0;
const uint8_t DUTY[8] = {255, 191, 128, 96, 64, 32, 16, 8};
const uint8_t NAV[4] = {1, 4, 16, 64};
const int EE_R = 400; const uint16_t RMAGIC = 0xCA1B;
const uint8_t NPH = 8; const uint8_t NMIN = 12;

struct Res {
  uint16_t magic; uint8_t ver, done;
  uint8_t g[2]; uint16_t settle;
  float led[2], ledOnV[2];
  float dMean[2][5], dSD[2][5];
  float t90[2], t99[2], t999[2], tOff[2];
  float gr[2][5];
  float lin[2][8];
  float nA[2][4]; float msCyc[4];
  float dS[2 * NMIN], dR[2 * NMIN];
} R;

volatile int8_t q = 0; volatile uint8_t lastAB = 0;
ISR(PCINT2_vect) {
  static const int8_t T[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  uint8_t ab = (digitalRead(PIN_A) << 1) | digitalRead(PIN_B);
  int8_t v = q + T[(lastAB << 2) | ab]; if (v > -120 && v < 120) q = v; lastAB = ab;
}
int8_t turns() { noInterrupts(); int8_t c = q / 4; q -= c * 4; interrupts(); return c; }
bool pressing = false; unsigned long pAt = 0; bool abortReq = false;
void pollAbort() {
  bool d = digitalRead(PIN_SW) == LOW;
  if (d && !pressing) pAt = millis();
  if (d && millis() - pAt > 3000) abortReq = true;
  pressing = d;
}
uint8_t button() {
  static bool down = false, held = false; static unsigned long at = 0;
  bool d = digitalRead(PIN_SW) == LOW; uint8_t e = 0;
  if (d && !down) { at = millis(); held = false; }
  if (d && !held && millis() - at > 3000) { held = true; e = 2; }
  if (!d && down && !held && millis() - at > 30) e = 1;
  down = d; return e;
}

char b[24];
char* f2s(float v, uint8_t p) {
  static char s[14]; if (isnan(v) || isinf(v)) { strcpy(s, "--"); return s; }
  long m = 1; for (uint8_t i = 0; i < p; i++) m *= 10;
  float x = fabs(v) * m + 0.5; if (x > 2e9) { strcpy(s, "ovr"); return s; }
  long n = x; char* o = s; if (v < 0 && n) *o++ = '-';
  ltoa(n / m, o, 10);
  if (p) { o += strlen(o); *o++ = '.'; long f = n % m; for (long dd = m / 10; dd > 0; dd /= 10) *o++ = '0' + (f / dd) % 10; *o = 0; }
  return s;
}
void line(uint8_t r, const char* t, bool inv = false) {
  char l[17]; uint8_t i = 0; while (i < 16 && t[i]) { l[i] = t[i]; i++; } while (i < 16) l[i++] = ' '; l[16] = 0;
  oled.setInverseFont(inv); oled.drawString(0, r, l); oled.setInverseFont(0);
}
void lineF(uint8_t r, const __FlashStringHelper* t, bool inv = false) { strcpy_P(b, (const char*)t); line(r, b, inv); }
void bar(uint8_t r, float f) {
  f = constrain(f, 0, 1); uint8_t fill = f * 126, t[8];
  for (uint8_t c = 0; c < 16; c++) {
    for (uint8_t k = 0; k < 8; k++) { uint8_t x = c * 8 + k; t[k] = (x == 0 || x == 127) ? 0x7E : (x <= fill ? 0x7E : 0x42); }
    oled.drawTile(c, r, 1, t);
  }
}
const char p0[] PROGMEM = "Dark+noise"; const char p1[] PROGMEM = "Settling"; const char p2[] PROGMEM = "Gain check"; const char p3[] PROGMEM = "Linearity";
const char p4[] PROGMEM = "Noise/avg"; const char p5[] PROGMEM = "Cool-down"; const char p6[] PROGMEM = "Drift pulse"; const char p7[] PROGMEM = "Drift LED on";
const char* const PNAME[NPH] PROGMEM = {p0, p1, p2, p3, p4, p5, p6, p7};
uint8_t curPh = 0; unsigned long runStart = 0, lastShow = 0;
float liveS = NAN, liveR = NAN;
void show(float frac, bool force = false) {
  if (!force && millis() - lastShow < 400) return; lastShow = millis();
  strcpy_P(b, PSTR("CAL ")); itoa(curPh + 1, b + 4, 10); strcat_P(b, PSTR("/8 ")); strcat_P(b, (const char*)pgm_read_word(&PNAME[curPh])); line(0, b, true);
  bar(2, frac);
  strcpy_P(b, PSTR("S ")); strcat(b, f2s(liveS, 2)); strcat_P(b, PSTR("mV")); line(4, b);
  strcpy_P(b, PSTR("R ")); strcat(b, f2s(liveR, 2)); strcat_P(b, PSTR("mV")); line(5, b);
  strcpy_P(b, PSTR("time ")); itoa((millis() - runStart) / 60000, b + 5, 10); strcat_P(b, PSTR(" / ~34 min")); line(6, b);
  lineF(7, F("hold 3s = stop"));
}

void ledOff() { digitalWrite(PIN_LED, LOW); }
void ledOn(uint8_t d = 255) { if (d == 255) digitalWrite(PIN_LED, HIGH); else analogWrite(PIN_LED, d); }
void wait(unsigned long ms, float frac = -1) { unsigned long t = millis(); while (millis() - t < ms) { pollAbort(); if (frac >= 0) show(frac); } }
float readV(uint8_t i, uint8_t g, uint8_t n) {
  ads.setGain(GAINS[g]); ads.readADC_SingleEnded(CH[i]);
  long a = 0; for (uint8_t k = 0; k < n; k++) a += ads.readADC_SingleEnded(CH[i]);
  pollAbort(); return a / (float)n * FS[g] / 32768.0;
}
void sigBoth(uint8_t n, float* out, uint8_t duty = 255) {
  ledOff(); wait(R.settle); float o0 = readV(0, R.g[0], n), o1 = readV(1, R.g[1], n);
  ledOn(duty); wait(R.settle); float a0 = readV(0, R.g[0], n), a1 = readV(1, R.g[1], n);
  ledOff(); out[0] = a0 - o0; out[1] = a1 - o1; liveS = out[0] * 1000; liveR = out[1] * 1000;
}
void save(uint8_t ph) { R.done |= 1 << ph; EEPROM.put(EE_R, R); }

void chooseGains() {
  ledOn(); wait(300);
  for (uint8_t i = 0; i < 2; i++) {
    float v = readV(i, 0, 8); uint8_t g = 0;
    while (g < 4 && v < 0.70 * FS[g + 1]) g++;
    R.g[i] = g; R.ledOnV[i] = v;
  }
  ledOff();
}
void phDark() {
  ledOff(); wait(2000);
  for (uint8_t i = 0; i < 2; i++) for (uint8_t g = 0; g < 5; g++) {
    float s = 0, ss = 0; const uint8_t N = 60;
    ads.setGain(GAINS[g]); ads.readADC_SingleEnded(CH[i]);
    for (uint8_t k = 0; k < N; k++) { float v = ads.readADC_SingleEnded(CH[i]) * FS[g] / 32.768; s += v; ss += v * v; pollAbort(); }
    R.dMean[i][g] = s / N; R.dSD[i][g] = sqrt(max(0.0f, (ss - s * s / N) / (N - 1)));
    show((i * 5 + g + 1) / 10.0); if (abortReq) return;
  }
  save(0);
}
const uint8_t NS = 70, NF = 15;
int16_t sv[NS]; uint16_t st[NS];
float lastOut(float ref, float tol, uint8_t from) {
  int8_t last = -1; for (uint8_t n = from; n < NS; n++) if (fabs(sv[n] - ref) > tol) last = n;
  return last < 0 ? 0 : (last < NS - 1 ? st[last + 1] : st[NS - 1]) / 10.0;
}
void phSettle() {
  ads.setDataRate(R860); const uint8_t REPS = 8;
  for (uint8_t i = 0; i < 2; i++) {
    float a90 = 0, a99 = 0, a999 = 0, aOff = 0;
    ads.setGain(GAINS[R.g[i]]);
    for (uint8_t r = 0; r < REPS; r++) {
      ledOff(); wait(400);
      long bs = 0; for (uint8_t k = 0; k < 16; k++) bs += ads.readADC_SingleEnded(CH[i]); float base = bs / 16.0;
      ledOn(); unsigned long t0 = micros();
      for (uint8_t n = 0; n < NS; n++) { sv[n] = ads.readADC_SingleEnded(CH[i]); st[n] = (micros() - t0) / 100; }
      float fin = 0, ss = 0; for (uint8_t n = NS - NF; n < NS; n++) fin += sv[n]; fin /= NF;
      for (uint8_t n = NS - NF; n < NS; n++) ss += (sv[n] - fin) * (sv[n] - fin); float sd = sqrt(ss / (NF - 1)), step = fin - base;
      float t9 = st[NS - 1] / 10.0; for (uint8_t n = 0; n < NS; n++) if (sv[n] - base >= 0.9 * step) { t9 = st[n] / 10.0; break; }
      a90 += t9; a99 += lastOut(fin, max(0.01f * fabs(step), 3 * sd), 0); a999 += lastOut(fin, max(0.001f * fabs(step), 3 * sd), 0);
      wait(100); ledOff(); t0 = micros();
      for (uint8_t n = 0; n < NS; n++) { sv[n] = ads.readADC_SingleEnded(CH[i]); st[n] = (micros() - t0) / 100; }
      aOff += lastOut(base, max(0.01f * fabs(step), 3 * sd), 0);
      show((i * REPS + r + 1) / (2.0 * REPS)); if (abortReq) { ads.setDataRate(R250); return; }
    }
    R.t90[i] = a90 / REPS; R.t99[i] = a99 / REPS; R.t999[i] = a999 / REPS; R.tOff[i] = aOff / REPS;
  }
  ads.setDataRate(R250);
  float worst = max(max(R.t999[0], R.t999[1]), max(R.tOff[0], R.tOff[1]));
  R.settle = constrain((uint16_t)(worst * 1.3 + 10), 20, 800);
  save(1);
}
void phGain() {
  float acc[2][5] = {{0}}; uint8_t cnt[2][5] = {{0}}; const uint8_t ROUNDS = 6;
  for (uint8_t r = 0; r < ROUNDS; r++) for (uint8_t k = 0; k < 5; k++) {
    uint8_t g = (r & 1) ? 4 - k : k;
    for (uint8_t i = 0; i < 2; i++) {
      if (g > R.g[i]) continue;
      ledOff(); wait(R.settle); float o = readV(i, g, 16);
      ledOn(); wait(R.settle); float a = readV(i, g, 16); ledOff();
      acc[i][g] += a - o; cnt[i][g]++; if (i == 0) liveS = (a - o) * 1000; else liveR = (a - o) * 1000;
    }
    show((r * 5 + k + 1) / (5.0 * ROUNDS)); if (abortReq) return;
  }
  for (uint8_t i = 0; i < 2; i++) for (uint8_t g = 0; g < 5; g++)
    R.gr[i][g] = cnt[i][g] ? (acc[i][g] / cnt[i][g]) / (acc[i][R.g[i]] / cnt[i][R.g[i]]) : NAN;
  save(2);
}
void phLin() {
  ads.setDataRate(R128);
  float acc[2][8] = {{0}}; const uint8_t ROUNDS = 4; float ref[2], x[2];
  for (uint8_t r = 0; r < ROUNDS; r++) for (uint8_t d = 1; d < 8; d++) {
    sigBoth(8, ref, 255); sigBoth(8, x, DUTY[d]);
    for (uint8_t i = 0; i < 2; i++) acc[i][d] += x[i] / (ref[i] * DUTY[d] / 255.0);
    show((r * 7 + d) / (7.0 * ROUNDS)); if (abortReq) { ads.setDataRate(R250); return; }
  }
  for (uint8_t i = 0; i < 2; i++) { R.lin[i][0] = 1; for (uint8_t d = 1; d < 8; d++) R.lin[i][d] = acc[i][d] / ROUNDS; }
  ads.setDataRate(R250); save(3);
}
void phNoise() {
  const uint8_t REP = 25; float s[2];
  for (uint8_t k = 0; k < 4; k++) {
    float m[2] = {0, 0}, q2[2] = {0, 0}; unsigned long t0 = millis();
    for (uint8_t r = 0; r < REP; r++) {
      sigBoth(NAV[k], s); for (uint8_t i = 0; i < 2; i++) { m[i] += s[i]; q2[i] += s[i] * s[i]; }
      show((k * REP + r + 1) / (4.0 * REP)); if (abortReq) return;
    }
    R.msCyc[k] = (millis() - t0) / (float)REP;
    for (uint8_t i = 0; i < 2; i++) { float mu = m[i] / REP, sd = sqrt(max(0.0f, (q2[i] - m[i] * m[i] / REP) / (REP - 1))); R.nA[i][k] = mu > 0 ? 0.4343 * sd / mu : NAN; }
  }
  save(4);
}
void phCool() {
  ledOff(); unsigned long t0 = millis(), D = 300000UL;
  while (millis() - t0 < D) { pollAbort(); show((millis() - t0) / (float)D); if (abortReq) return; }
  save(5);
}
void phDrift(bool steady, uint8_t off) {
  float s[2]; unsigned long t0 = millis();
  for (uint8_t m = 0; m < NMIN; m++) {
    float aS = 0, aR = 0;
    for (uint8_t k = 0; k < 6; k++) {
      unsigned long tNext = t0 + (unsigned long)(m * 6 + k) * 10000UL;
      while ((long)(millis() - tNext) < 0) { pollAbort(); show((m * 6 + k) / (6.0 * NMIN)); if (abortReq) { ledOff(); return; } }
      sigBoth(16, s); aS += s[0]; aR += s[1];
      if (steady) ledOn();
    }
    R.dS[off + m] = aS / 6 * 1000; R.dR[off + m] = aR / 6 * 1000;
    Serial.print(steady ? F("DRIFT_ON,") : F("DRIFT_PULSED,")); Serial.print(m + 1); Serial.print(','); Serial.print(R.dS[off + m], 3); Serial.print(','); Serial.println(R.dR[off + m], 3);
  }
  ledOff(); save(steady ? 7 : 6);
}

void pr(const __FlashStringHelper* k, uint8_t i, float v, uint8_t p = 4) { Serial.print(k); Serial.print(i ? F("_R,") : F("_S,")); Serial.println(v, p); }
void report() {
  Serial.println(F("\n=== PD CHARACTERISATION REPORT v1 (copy everything to Claude) ==="));
  Serial.print(F("phases_done_mask,")); Serial.println(R.done, BIN);
  Serial.print(F("settle_recommended_ms,")); Serial.println(R.settle);
  for (uint8_t i = 0; i < 2; i++) {
    Serial.print(i ? F("--- REFERENCE (ADS A0) ---\n") : F("--- SAMPLE (ADS A1) ---\n"));
    pr(F("gain_chosen_x"), i, GX[R.g[i]], 0); pr(F("led_on_raw_V"), i, R.ledOnV[i]);
    for (uint8_t g = 0; g < 5; g++) { Serial.print(F("dark_x")); Serial.print(GX[g]); pr(F("_mean_sd_mV"), i, R.dMean[i][g], 4); Serial.print(F("   sd ")); Serial.println(R.dSD[i][g], 4); }
    pr(F("t90_ms"), i, R.t90[i], 1); pr(F("t99_ms"), i, R.t99[i], 1); pr(F("t99.9_ms"), i, R.t999[i], 1); pr(F("toff99_ms"), i, R.tOff[i], 1);
    for (uint8_t g = 0; g < 5; g++) { Serial.print(F("gain_ratio_x")); Serial.print(GX[g]); pr(F(""), i, R.gr[i][g], 5); }
    for (uint8_t d = 0; d < 8; d++) { Serial.print(F("lin_duty")); Serial.print(DUTY[d]); Serial.print(F("_A")); Serial.print(-log10(DUTY[d] / 255.0), 2); pr(F(""), i, R.lin[i][d], 5); }
    for (uint8_t k = 0; k < 4; k++) { Serial.print(F("noiseAbs_avg")); Serial.print(NAV[k]); pr(F(""), i, R.nA[i][k], 5); }
    Serial.print(F("drift_pulsed_mV")); Serial.print(i ? F("_R") : F("_S"));
    for (uint8_t m = 0; m < 2 * NMIN; m++) { if (m == NMIN) { Serial.print(F("\ndrift_ledon_mV")); Serial.print(i ? F("_R") : F("_S")); } Serial.print(','); Serial.print(i ? R.dR[m] : R.dS[m], 3); }
    Serial.println();
  }
  for (uint8_t k = 0; k < 4; k++) { Serial.print(F("ms_per_cycle_avg")); Serial.print(NAV[k]); Serial.print(','); Serial.println(R.msCyc[k], 0); }
  Serial.println(F("=== END ==="));
}
float pct(float a, float b2) { return (b2 - a) / a * 100; }
void page(uint8_t p) {
  oled.clearDisplay(); strcpy_P(b, PSTR("RESULT ")); itoa(p + 1, b + 7, 10); strcat_P(b, PSTR("/6 turn")); line(0, b, true);
  for (uint8_t i = 0; i < 2; i++) {
    uint8_t r = 1 + i * 3; if (i) lineF(r, F("REFERENCE")); else lineF(r, F("SAMPLE"));
    if (p == 0) { strcpy_P(b, PSTR("LED ")); strcat(b, f2s(R.led[i], 1)); strcat_P(b, PSTR("mV x")); itoa(GX[R.g[i]], b + strlen(b), 10); line(r + 1, b);
                  strcpy_P(b, PSTR("dark sd ")); strcat(b, f2s(R.dSD[i][R.g[i]], 3)); line(r + 2, b); }
    if (p == 1) { strcpy_P(b, PSTR("t99 ")); strcat(b, f2s(R.t99[i], 0)); strcat_P(b, PSTR(" .9 ")); strcat(b, f2s(R.t999[i], 0)); line(r + 1, b);
                  strcpy_P(b, PSTR("off ")); strcat(b, f2s(R.tOff[i], 0)); strcat_P(b, PSTR("ms set ")); itoa(R.settle, b + strlen(b), 10); line(r + 2, b); }
    if (p == 2) { strcpy_P(b, PSTR("g ")); for (uint8_t g = 0; g < 3; g++) { strcat(b, f2s(R.gr[i][g], 3)); strcat_P(b, PSTR(" ")); } line(r + 1, b);
                  strcpy_P(b, PSTR("  ")); for (uint8_t g = 3; g < 5; g++) { strcat(b, f2s(R.gr[i][g], 3)); strcat_P(b, PSTR(" ")); } line(r + 2, b); }
    if (p == 3) { strcpy_P(b, PSTR("50% ")); strcat(b, f2s(R.lin[i][2], 3)); strcat_P(b, PSTR(" 13% ")); strcat(b, f2s(R.lin[i][5], 3)); line(r + 1, b);
                  strcpy_P(b, PSTR("6% ")); strcat(b, f2s(R.lin[i][6], 3)); strcat_P(b, PSTR(" 3% ")); strcat(b, f2s(R.lin[i][7], 3)); line(r + 2, b); }
    if (p == 4) { strcpy_P(b, PSTR("1:")); strcat(b, f2s(R.nA[i][0], 4)); strcat_P(b, PSTR(" 4:")); strcat(b, f2s(R.nA[i][1], 4)); line(r + 1, b);
                  strcpy_P(b, PSTR("16:")); strcat(b, f2s(R.nA[i][2], 4)); strcat_P(b, PSTR(" 64:")); strcat(b, f2s(R.nA[i][3], 4)); line(r + 2, b); }
    if (p == 5) { float* d = i ? R.dR : R.dS;
                  strcpy_P(b, PSTR("pulse ")); strcat(b, f2s(pct(d[0], d[NMIN - 1]), 2)); strcat_P(b, PSTR("%")); line(r + 1, b);
                  strcpy_P(b, PSTR("LEDon ")); strcat(b, f2s(pct(d[NMIN], d[2 * NMIN - 1]), 2)); strcat_P(b, PSTR("%")); line(r + 2, b); }
  }
  lineF(7, F("press=USB report"));
}
void resultsLoop() {
  uint8_t p = 0; page(p);
  while (true) {
    int8_t c = turns(); if (c) { p = (p + 6 + (c > 0 ? 1 : 5)) % 6; page(p); }
    uint8_t e = button(); if (e == 1) { report(); lineF(7, F("report sent"), true); delay(700); lineF(7, F("press=USB report")); }
    if (e == 2) return;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT); ledOff();
  pinMode(PIN_A, INPUT_PULLUP); pinMode(PIN_B, INPUT_PULLUP); pinMode(PIN_SW, INPUT_PULLUP);
  lastAB = (digitalRead(PIN_A) << 1) | digitalRead(PIN_B);
  PCMSK2 = _BV(PIN_A) | _BV(PIN_B); PCIFR |= _BV(PCIF2); PCICR |= _BV(PCIE2);
  oled.begin(); oled.setFont(u8x8_font_chroma48medium8_r);
  bool ok = ads.begin(0x48); ads.setDataRate(R250);
  oled.clearDisplay(); if (!ok) { lineF(3, F("SENSOR MISSING!")); while (true) {} }
  EEPROM.get(EE_R, R);
  if (R.magic == RMAGIC && R.done) {
    lineF(0, F("RESULTS SAVED"), true); lineF(2, F("press = view")); lineF(3, F("hold 3s = re-run")); report();
    while (true) { uint8_t e = button(); if (e == 1) { resultsLoop(); break; } if (e == 2) break; }
    oled.clearDisplay();
  }
}

void loop() {
  lineF(0, F("PD CALIBRATION"), true);
  lineF(1, F("slot EMPTY,gel IN")); lineF(2, F("lid ON, no touch")); lineF(3, F("LED cool first"));
  lineF(4, F("takes ~34 min")); lineF(5, F("USB log optional")); lineF(7, F("press = START"));
  while (button() != 1) {}
  oled.clearDisplay();
  memset(&R, 0, sizeof(R)); R.magic = RMAGIC; R.ver = 1; R.settle = 200;
  for (uint8_t i = 0; i < 2 * NMIN; i++) R.dS[i] = R.dR[i] = NAN;
  abortReq = false; runStart = millis();
  Serial.println(F("PD characterisation start"));
  curPh = 0; show(0, true); phDark();
  chooseGains(); { float s[2]; sigBoth(16, s); R.led[0] = s[0] * 1000; R.led[1] = s[1] * 1000; }
  if (!abortReq) { curPh = 1; show(0, true); phSettle(); }
  if (!abortReq) { curPh = 2; show(0, true); phGain(); }
  if (!abortReq) { curPh = 3; show(0, true); phLin(); }
  if (!abortReq) { curPh = 4; show(0, true); phNoise(); }
  if (!abortReq) { curPh = 5; show(0, true); phCool(); }
  if (!abortReq) { curPh = 6; show(0, true); phDrift(false, 0); }
  if (!abortReq) { curPh = 7; show(0, true); phDrift(true, NMIN); }
  ledOff(); EEPROM.put(EE_R, R);
  oled.clearDisplay(); if (abortReq) lineF(0, F("STOPPED - saved"), true); else lineF(0, F("DONE - saved"), true); lineF(2, F("release knob")); delay(800);
  while (digitalRead(PIN_SW) == LOW) {}
  report(); resultsLoop(); oled.clearDisplay();
}
