#include <U8x8lib.h>
#include "soft_ads1115.h"

const uint8_t PIN_A = 2, PIN_B = 3, PIN_SW = 4, PIN_LED = 6, PIN_BUZ = 9;
const uint8_t CH[2] = {1, 0};
const char* const NM[2] = {"SAMPLE", "REF"};
U8X8_SH1106_128X64_NONAME_SW_I2C oled(A4, A5, U8X8_PIN_NONE);
SoftADS1115 ads;
const uint16_t GAINS[5] = {GAIN_ONE, GAIN_TWO, GAIN_FOUR, GAIN_EIGHT, GAIN_SIXTEEN};
const float FS[5] = {4.096, 2.048, 1.024, 0.512, 0.256};
const uint8_t GX[5] = {1, 2, 4, 8, 16};
const uint16_t SETTLE_MS = 50;
const uint8_t NAVG = 4;

uint8_t g[2] = {2, 2};
float sig[2] = {0, 0}, pk[2] = {0, 0}, roomV[2] = {0, 0};
bool fresh[2] = {true, true}, clip[2] = {false, false};
uint8_t mode = 0;
bool spot = false;

volatile int8_t q = 0; volatile uint8_t lastAB = 0; volatile bool swHit = false; volatile unsigned long hitAt = 0;
ISR(PCINT2_vect) {
  static const int8_t T[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  uint8_t ab = (digitalRead(PIN_A) << 1) | digitalRead(PIN_B);
  int8_t v = q + T[(lastAB << 2) | ab]; if (v > -120 && v < 120) q = v; lastAB = ab;
  if (!swHit && digitalRead(PIN_SW) == LOW) { swHit = true; hitAt = millis(); }
}
int8_t turns() { noInterrupts(); int8_t c = q / 4; q -= c * 4; interrupts(); return c; }
bool down = false, held = false; unsigned long at = 0, upAt = 0;
uint8_t button() {
  bool d = digitalRead(PIN_SW) == LOW; uint8_t e = 0;
  noInterrupts(); bool h = swHit; unsigned long ht = hitAt; swHit = false; interrupts();
  if (h && !down && !d && (long)(ht - upAt) > 120) { down = true; at = ht; held = false; }
  if (d && !down) { at = millis(); held = false; }
  if (d && !held && millis() - at > 700) { held = true; e = 2; }
  if (!d && down) { if (!held && millis() - at > 25) e = 1; upAt = millis(); }
  down = d; return e;
}

char* f2s(float v, uint8_t p) {
  static char s[14]; if (isnan(v)) { strcpy(s, "--"); return s; }
  long m = 1; for (uint8_t i = 0; i < p; i++) m *= 10;
  long n = fabs(v) * m + 0.5; char* o = s; if (v < 0 && n) *o++ = '-';
  ltoa(n / m, o, 10);
  if (p) { o += strlen(o); *o++ = '.'; long f = n % m; for (long dd = m / 10; dd > 0; dd /= 10) *o++ = '0' + (f / dd) % 10; *o = 0; }
  return s;
}
void line(uint8_t r, const char* t, bool inv = false) {
  char l[17]; uint8_t i = 0; while (i < 16 && t[i]) { l[i] = t[i]; i++; } while (i < 16) l[i++] = ' '; l[16] = 0;
  oled.setInverseFont(inv); oled.drawString(0, r, l); oled.setInverseFont(0);
}
char* mv(float v) { return f2s(v * 1000, v < 0.1 ? 2 : 1); }
void barPk(uint8_t r, float cur, float best) {
  float scale = max(max(cur, best), 1e-6f) * 1.08;
  uint8_t fill = constrain(cur / scale * 126, 0, 126), mk = constrain(best / scale * 126, 0, 126), t[8];
  for (uint8_t c = 0; c < 16; c++) {
    for (uint8_t k = 0; k < 8; k++) { uint8_t x = c * 8 + k;
      t[k] = (x == 0 || x == 127) ? 0x7E : (x == mk && best > 0) ? 0xFF : (x <= fill ? 0x7E : 0x42); }
    oled.drawTile(c, r, 1, t);
  }
}
uint8_t pct(uint8_t i) { return pk[i] > 0 ? constrain(sig[i] / pk[i] * 100 + 0.5, 0, 100) : 0; }

float rd(uint8_t i) {
  ads.setGain(GAINS[g[i]]); ads.readADC_SingleEnded(CH[i]);
  long a = 0; for (uint8_t n = 0; n < NAVG; n++) a += ads.readADC_SingleEnded(CH[i]);
  return a / (float)NAVG * FS[g[i]] / 32768.0;
}
void cycle() {
  digitalWrite(PIN_LED, LOW);  delay(SETTLE_MS); float of0 = rd(0), of1 = rd(1);
  digitalWrite(PIN_LED, HIGH); delay(SETTLE_MS); float on0 = rd(0), on1 = rd(1);
  digitalWrite(PIN_LED, LOW);
  float on[2] = {on0, on1}, of[2] = {of0, of1};
  for (uint8_t i = 0; i < 2; i++) {
    uint8_t g0 = g[i];
    clip[i] = on[i] > 0.97 * FS[g0];
    if (on[i] > 0.85 * FS[g0] && g0 > 0) g[i]--; else if (g0 < 4 && on[i] < 0.35 * FS[g0 + 1]) g[i]++;
    float s = on[i] - of[i];
    sig[i] = fresh[i] ? s : sig[i] + 0.5 * (s - sig[i]);
    fresh[i] = false; roomV[i] = of[i];
    if (!clip[i] && g[i] == g0 && sig[i] > pk[i]) pk[i] = sig[i];
  }
  Serial.print(F("S_mV:")); Serial.print(sig[0] * 1000, 2); Serial.print(F(" R_mV:")); Serial.println(sig[1] * 1000, 2);
}

char b[24];
void hint() {
  if (clip[0] || clip[1]) line(7, "OVER RANGE");
  else if (mode == 0 && pct(0) >= 97 && pct(1) >= 97) line(7, "BOTH AT PEAK", true);
  else if (mode && pct(mode - 1) >= 97) line(7, "AT PEAK - hold it", true);
  else if (roomV[0] > sig[0] + 0.002 || roomV[1] > sig[1] + 0.002) line(7, "room light: cover");
  else line(7, "press=reset best");
}
void chanRows(uint8_t i, uint8_t r) {
  strcpy(b, i ? "R " : "S "); strcat(b, mv(sig[i])); strcat(b, "mV x"); itoa(GX[g[i]], b + strlen(b), 10); line(r, b);
  barPk(r + 1, sig[i], pk[i]);
  strcpy(b, "best "); strcat(b, mv(pk[i])); strcat(b, "  "); itoa(pct(i), b + strlen(b), 10); strcat(b, "%"); line(r + 2, b);
}
void drawBoth() { line(0, "ALIGN  both  >", true); chanRows(0, 1); chanRows(1, 4); hint(); }
void drawOne(uint8_t i) {
  strcpy(b, "ALIGN  "); strcat(b, NM[i]); strcat(b, "  >"); line(0, b, true);
  char p[6]; itoa(pct(i), p, 10); strcat(p, "% "); oled.draw2x2String(0, 2, "      "); oled.draw2x2String(0, 2, p);
  barPk(4, sig[i], pk[i]);
  strcpy(b, mv(sig[i])); strcat(b, " / "); strcat(b, mv(pk[i])); strcat(b, "mV"); line(5, b);
  strcpy(b, "gain x"); itoa(GX[g[i]], b + strlen(b), 10); line(6, b);
  hint();
  uint8_t pc = pct(i);
  if (pc >= 97) { tone(PIN_BUZ, 2600, 25); } else if (pc > 0) tone(PIN_BUZ, 300 + pc * 18, 25);
}
void spotMode() {
  oled.clearDisplay(); line(0, "SPOT: LED ON", true); line(2, "look where the"); line(3, "beam lands"); line(7, "hold = back");
  digitalWrite(PIN_LED, HIGH);
  while (true) {
    strcpy(b, "S raw "); strcat(b, mv(rd(0))); strcat(b, "mV"); line(5, b);
    strcpy(b, "R raw "); strcat(b, mv(rd(1))); strcat(b, "mV"); line(6, b);
    uint8_t e = button(); if (e == 2) break;
  }
  digitalWrite(PIN_LED, LOW); oled.clearDisplay();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT); pinMode(PIN_BUZ, OUTPUT);
  pinMode(PIN_A, INPUT_PULLUP); pinMode(PIN_B, INPUT_PULLUP); pinMode(PIN_SW, INPUT_PULLUP);
  lastAB = (digitalRead(PIN_A) << 1) | digitalRead(PIN_B);
  PCMSK2 = _BV(PIN_A) | _BV(PIN_B) | _BV(PIN_SW); PCIFR |= _BV(PCIF2); PCICR |= _BV(PCIE2);
  oled.begin(); oled.setFont(u8x8_font_chroma48medium8_r);
  bool ok = ads.begin(0x48); ads.setDataRate(RATE_ADS1115_475SPS);
  oled.clearDisplay(); line(0, "PD ALIGN TOOL", true);
  line(2, "turn = screen"); line(3, "press= reset best"); line(4, "hold = LED spot");
  line(6, ok ? "sensor OK" : "SENSOR MISSING!"); line(7, "press to start");
  if (!ok) while (true) {}
  while (button() != 1) {}
  oled.clearDisplay();
}

void loop() {
  int8_t c = turns(); if (c) { mode = (mode + 3 + (c > 0 ? 1 : 2)) % 3; oled.clearDisplay(); }
  uint8_t e = button();
  if (e == 1) { pk[0] = pk[1] = 0; tone(PIN_BUZ, 1500, 60); }
  if (e == 2) { spotMode(); held = true; }
  cycle();
  if (mode == 0) drawBoth(); else drawOne(mode - 1);
}
