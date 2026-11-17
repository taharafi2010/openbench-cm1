#include <U8x8lib.h>
#include <EEPROM.h>
#include <avr/sleep.h>
#include <setjmp.h>
#include "soft_ads1115.h"
#include "ui_types.h"

const uint8_t PIN_A = 2, PIN_B = 3, PIN_SW = 4, PIN_LED = 6, PIN_BUZ = 9;
const uint8_t SAMPLE_CH = 1, REF_CH = 0;
U8X8_SH1106_128X64_NONAME_SW_I2C oled(A4, A5, U8X8_PIN_NONE);
SoftADS1115 ads;
const uint16_t GAINS[5] = {GAIN_ONE, GAIN_TWO, GAIN_FOUR, GAIN_EIGHT, GAIN_SIXTEEN};
const float FS[5] = {4.096, 2.048, 1.024, 0.512, 0.256};
const uint8_t GAINX[5] = {1, 2, 4, 8, 16};

struct Settings {
  uint16_t magic; uint8_t ver;
  uint8_t gainMode, avgIdx, useRef, refSmooth, autoCap, stabIdx, warmup, kinInt, kinDur,
          buzzer, contrast, reverse, curve, settleIdx, autoOff;
  uint8_t crc;
} S;
struct Curve { uint8_t used, name, unit, n; float m, b, r2; };
struct Blank { uint8_t valid; float s0, r0; };
Curve C[3]; Blank BL;
const uint16_t MAGIC = 0xC01C;
const int EE_S = 0, EE_C = 40, EE_B = 120;
const uint8_t AVG[5] = {4, 8, 16, 32, 64};
const uint8_t RSM[4] = {1, 4, 8, 16};
const float STAB[4] = {0.001, 0.002, 0.005, 0.010};
const uint16_t SETTLE[4] = {100, 150, 300, 600};
const uint8_t AUTOOFF[5] = {0, 5, 10, 30, 60};
const char n0[] PROGMEM = "Glucose"; const char n1[] PROGMEM = "Dye"; const char n2[] PROGMEM = "Protein"; const char n3[] PROGMEM = "Custom";
const char* const NAMES[] PROGMEM = {n0, n1, n2, n3};
const char u0[] PROGMEM = "mmol/L"; const char u1[] PROGMEM = "mg/mL"; const char u2[] PROGMEM = "ug/mL"; const char u3[] PROGMEM = "%"; const char u4[] PROGMEM = "units";
const char* const UNITS[] PROGMEM = {u0, u1, u2, u3, u4};
char tbuf[12];
const char* pstr(const char* const* t, uint8_t i) { strcpy_P(tbuf, (const char*)pgm_read_word(&t[i])); return tbuf; }

uint8_t crc8(const uint8_t* p, uint8_t n) { uint8_t c = 0; while (n--) c = (c << 1 | c >> 7) ^ *p++; return c; }
void defaults() { S = {MAGIC, 4, 0, 2, 1, 2, 1, 1, 3, 5, 10, 1, 8, 0, 0, 1, 2, 0}; }
void saveSettings() { S.crc = crc8((uint8_t*)&S, sizeof(S) - 1); EEPROM.put(EE_S, S); }
void saveCurves() { EEPROM.put(EE_C, C); }
void saveBlank() { EEPROM.put(EE_B, BL); }
void loadAll() {
  EEPROM.get(EE_S, S);
  if (S.magic != MAGIC || S.crc != crc8((uint8_t*)&S, sizeof(S) - 1)) {
    bool upgrade = S.magic == 0xC01B;
    defaults(); saveSettings(); BL.valid = 0; saveBlank();
    if (upgrade) EEPROM.get(EE_C, C); else { memset(C, 0, sizeof(C)); saveCurves(); }
  } else { EEPROM.get(EE_C, C); EEPROM.get(EE_B, BL); }
}

volatile int8_t qsteps = 0; volatile uint8_t lastAB = 0;
volatile bool swHit = false; volatile unsigned long swHitAt = 0;
void encOn() { PCMSK2 = _BV(PIN_A) | _BV(PIN_B) | _BV(PIN_SW); PCIFR |= _BV(PCIF2); PCICR |= _BV(PCIE2); }
void isrEnc() {
  static const int8_t T[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  uint8_t ab = (digitalRead(PIN_A) << 1) | digitalRead(PIN_B);
  int8_t v = qsteps + T[(lastAB << 2) | ab]; if (v > -120 && v < 120) qsteps = v; lastAB = ab;
}
unsigned long lastActivity = 0, lastTurnMs = 0;
int8_t turns() {
  noInterrupts(); int8_t q = qsteps, c = q / 4; qsteps -= c * 4; interrupts();
  if (c) lastActivity = millis();
  return S.reverse ? -c : c;
}
int turnsAccel() { int8_t c = turns(); if (!c) return 0; unsigned long t = millis(); int m = (t - lastTurnMs < 60) ? 10 : 1; lastTurnMs = t; return c * m; }
bool swDown = false, holdSent = false; unsigned long swAt = 0, swUpAt = 0; Ev pending = EV_NONE;
const unsigned long HOLD_OFF_MS = 10000;
bool inShutdown = false;
void holdShutdown();
void pollButton() {
  bool d = digitalRead(PIN_SW) == LOW;
  noInterrupts(); bool h = swHit; unsigned long ht = swHitAt; swHit = false; interrupts();

  if (h && !swDown && !d && (long)(ht - swUpAt) > 120) { swDown = true; swAt = ht; holdSent = false; }
  if (d && !swDown) { swAt = (h && (long)(ht - swUpAt) > 120) ? ht : millis(); holdSent = false; }
  if (d && !holdSent && millis() - swAt > 700) { holdSent = true; pending = EV_HOLD; }
  if (!d && swDown) { if (!holdSent && millis() - swAt > 25) pending = EV_PRESS; swUpAt = millis(); }
  swDown = d;
  if (d && !inShutdown && millis() - swAt > HOLD_OFF_MS) holdShutdown();
}
Ev button() { pollButton(); Ev e = pending; pending = EV_NONE; if (e) lastActivity = millis(); return e; }
void idle(unsigned long ms) { unsigned long t = millis(); while (millis() - t < ms) pollButton(); }
void waitPress() { while (button() == EV_NONE) {} }
void beep(uint16_t f, uint16_t ms) {
  if (!S.buzzer) return;
  uint16_t h = 500000UL / f; uint32_t n = (uint32_t)ms * 1000UL / (2 * h);
  while (n--) { digitalWrite(PIN_BUZ, HIGH); delayMicroseconds(h); digitalWrite(PIN_BUZ, LOW); delayMicroseconds(h); }
}

char buf[40];
char* f2s(float v, uint8_t p) {
  static char s[14]; if (isnan(v) || isinf(v)) { strcpy(s, "--"); return s; }
  long m = 1; for (uint8_t i = 0; i < p; i++) m *= 10;
  float x = fabs(v) * m + 0.5; if (x > 2e9) { strcpy(s, "ovr"); return s; }
  long n = x; char* o = s; if (v < 0 && n) *o++ = '-';
  ltoa(n / m, o, 10);
  if (p) { o += strlen(o); *o++ = '.'; long f = n % m; for (long d = m / 10; d > 0; d /= 10) *o++ = '0' + (f / d) % 10; *o = 0; }
  return s;
}
char* ap(char* o, const char* t) { return strcat(o, t); }
char* apP(char* o, const __FlashStringHelper* t) { return strcat_P(o, (const char*)t); }
char* apI(char* o, long v) { char n[12]; ltoa(v, n, 10); return strcat(o, n); }
char* lab(char* o, const __FlashStringHelper* t) { return strcpy_P(o, (const char*)t); }
void line(uint8_t r, const char* t, bool inv = false) {
  char l[17]; uint8_t i = 0; while (i < 16 && t[i]) { l[i] = t[i]; i++; } while (i < 16) l[i++] = ' '; l[16] = 0;
  oled.setInverseFont(inv); oled.drawString(0, r, l); oled.setInverseFont(0);
}
void lineP(uint8_t r, const __FlashStringHelper* t, bool inv = false) { lab(buf, t); line(r, buf, inv); }
void bigLine(uint8_t r, const char* t, const __FlashStringHelper* unit) {
  const char* dot = strchr(t, '.'); if ((dot ? (uint8_t)(dot - t) : strlen(t)) > 6) t = "HIGH";
  char l[9]; uint8_t i = 0; while (i < 6 && t[i]) { l[i] = t[i]; i++; } if (i && l[i - 1] == '.') i--;
  while (i < 6) l[i++] = ' '; l[6] = 0;
  oled.draw2x2String(0, r, l); lab(buf, unit); buf[4] = 0; char u[5]; strcpy(u, buf);
  while (strlen(u) < 4) strcat(u, " "); oled.drawString(12, r + 1, u); oled.drawString(12, r, "    ");
}
void clearAll() { oled.clearDisplay(); }
void msg(const __FlashStringHelper* a, const __FlashStringHelper* b) { clearAll(); lineP(3, a); lineP(5, b); }
void bar(uint8_t r, float f) {
  f = constrain(f, 0, 1); uint8_t fill = f * 126, t[8];
  for (uint8_t c = 0; c < 16; c++) {
    for (uint8_t k = 0; k < 8; k++) { uint8_t x = c * 8 + k;
      t[k] = (x == 0 || x == 127) ? 0x7E : (x <= fill ? 0x7E : 0x42); }
    oled.drawTile(c, r, 1, t);
  }
}
void graph(uint8_t r0, uint8_t rows, BandFn fn) {
  uint8_t t[8];
  for (uint8_t r = 0; r < rows; r++) for (uint8_t c = 0; c < 16; c++) {
    for (uint8_t k = 0; k < 8; k++) t[k] = fn(c * 8 + k, r);
    oled.drawTile(c, r0 + r, 1, t);
  }
}
uint8_t spanBits(int16_t a, int16_t b, uint8_t band) {
  if (a > b) { int16_t x = a; a = b; b = x; }
  int16_t lo = max(a, (int16_t)(band * 8)), hi = min(b, (int16_t)(band * 8 + 7)); uint8_t m = 0;
  for (int16_t p = lo; p <= hi; p++) m |= 1 << (p - band * 8);
  return m;
}

uint8_t gS = 4, gR = 4;
float sOn, sOff, rOn, rOff, sig, ref, A = NAN, T = NAN, conc = NAN, darkBase = NAN;
float aHist[5]; uint8_t aN = 0, aI = 0; float rHist[16]; uint8_t rN = 0, rI = 0;
bool stable = false, overRange = false, leak = false, forceRef = false, measDone = false;

float gc[2][5] = {{0.99955, 1.0, 1.0, 1.0, 1.0},
                  {1.01454, 1.00905, 1.01520, 0.99943, 1.0}};
uint16_t calSettle = 0; bool calOwn = false;
struct CalHead {
  uint16_t magic; uint8_t ver, done; uint8_t g[2]; uint16_t settle;
  float led[2], ledOnV[2], dMean[2][5], dSD[2][5], t90[2], t99[2], t999[2], tOff[2];
  float gr[2][5];
};
void loadCal() {
  CalHead h; EEPROM.get(400, h);
  if (h.magic != 0xCA1B) return;
  if ((h.done & 2) && h.settle < 800) calSettle = h.settle;
  if (h.done & 4) {
    for (uint8_t c = 0; c < 2; c++) for (uint8_t g = 0; g < 5; g++) {
      float r = h.gr[c][g];
      gc[c][g] = (r > 0.8 && r < 1.25) ? 1.0 / r : 1.0;
    }
    calOwn = true;
  }
}
float readCh(uint8_t ch, uint8_t g) {
  ads.setGain(GAINS[g]); long a = 0; uint8_t n = AVG[S.avgIdx];
  ads.readADC_SingleEnded(ch);
  for (uint8_t i = 0; i < n; i++) a += ads.readADC_SingleEnded(ch);
  return (a / (float)n) * FS[g] / 32768.0 * gc[ch][g];
}
void autoGain(uint8_t& g, float raw) {
  if (S.gainMode) { g = S.gainMode - 1; return; }
  if (raw > 0.80 * FS[g] && g > 0) g--;
  else if (g < 4 && raw < 0.35 * FS[g + 1]) g++;
}
void resetStability() { aN = aI = rN = rI = 0; }
float spread() { float lo = 99, hi = -99; for (uint8_t i = 0; i < aN; i++) { lo = min(lo, aHist[i]); hi = max(hi, aHist[i]); } return aN < 2 ? 1 : hi - lo; }
void measure() {
  measDone = false; bool ur = S.useRef || forceRef;
  digitalWrite(PIN_LED, LOW);  idle(max(SETTLE[S.settleIdx], calSettle));
  if (pending) return;
  sOff = readCh(SAMPLE_CH, gS); rOff = ur ? readCh(REF_CH, gR) : 0;
  digitalWrite(PIN_LED, HIGH); idle(max(SETTLE[S.settleIdx], calSettle));
  if (pending) { digitalWrite(PIN_LED, LOW); return; }
  sOn = readCh(SAMPLE_CH, gS); rOn = ur ? readCh(REF_CH, gR) : 0;
  digitalWrite(PIN_LED, LOW);
  overRange = sOn > 0.97 * FS[gS] || (ur && rOn > 0.97 * FS[gR]);
  uint8_t og = gS; autoGain(gS, sOn); if (ur) autoGain(gR, rOn);
  sig = sOn - sOff;
  uint8_t rs = RSM[S.refSmooth]; rHist[rI] = rOn - rOff; rI = (rI + 1) % rs; if (rN < rs) rN++;
  ref = 0; for (uint8_t i = 0; i < rN; i++) ref += rHist[i]; ref /= rN;
  if (isnan(darkBase)) darkBase = sOff;
  leak = sOff - darkBase > 0.003 + 0.02 * fabs(darkBase);
  A = T = conc = NAN;
  if (BL.valid && sig > 0) {
    bool refOk = S.useRef && ref > 0.0005 && BL.r0 > 0.0005;
    T = refOk ? (sig / ref) / (BL.s0 / BL.r0) : sig / BL.s0;
    A = -log10(T);
    Curve& k = C[S.curve ? S.curve - 1 : 0];
    if (S.curve && k.used && k.m != 0) conc = (A - k.b) / k.m;
  }
  aHist[aI] = A; aI = (aI + 1) % 5; if (aN < 5) aN++;
  stable = aN == 5 && og == gS && !isnan(A) && spread() <= STAB[S.stabIdx];
  measDone = true;
}

int8_t listMenu(const __FlashStringHelper* t, uint8_t n, ItemFn item, uint8_t sel = 0) {
  bool redraw = true; clearAll(); lineP(0, t, true);
  while (true) {
    int8_t c = turns(); if (c) { sel = (sel + n * 4 + c) % n; redraw = true; }
    Ev e = button(); if (e == EV_PRESS) return sel; if (e == EV_HOLD) return -1;
    if (redraw) {
      uint8_t first = sel > 6 ? sel - 6 : 0;
      for (uint8_t r = 0; r < 7; r++) { uint8_t i = first + r;
        if (i < n) { buf[0] = 0; item(i, buf); line(r + 1, buf, i == sel); } else line(r + 1, ""); }
      redraw = false;
    }
  }
}
bool editFloat(const __FlashStringHelper* t, float& v, float step, uint8_t prec) {
  float x = v; bool redraw = true; clearAll(); lineP(0, t, true); lineP(6, F("turn fast=x10")); lineP(7, F("press=OK hold=x"));
  while (true) {
    int c = turnsAccel(); if (c) { x = constrain(x + c * step, 0, 999); redraw = true; }
    Ev e = button(); if (e == EV_PRESS) { v = x; return true; } if (e == EV_HOLD) return false;
    if (redraw) { bigLine(2, f2s(x, prec), F("")); redraw = false; }
  }
}
bool captureStable(const __FlashStringHelper* t, bool isBlank) {
  resetStability(); unsigned long t0 = millis(); bool now = false; clearAll(); lineP(0, t, true); lineP(7, F("press=now hold=x"));
  while (true) {
    measure();
    if (isBlank) { lab(buf, F("S ")); ap(buf, f2s(sig * 1000, 2)); apP(buf, F(" mV")); line(2, buf); }
    else bigLine(2, f2s(A, 3), F("Abs"));
    lineP(5, stable ? F("STABLE") : F("settling..."));
    bar(6, isBlank ? (millis() - t0) / 4000.0 : 1 - spread() / (5 * STAB[S.stabIdx]));
    Ev e = button(); if (e == EV_HOLD) return false; if (e == EV_PRESS) now = true;
    if (now && aN) return true;
    if (isBlank ? (millis() - t0 > 4000 && aN >= 3) : (stable && S.autoCap)) return true;
    if (millis() - t0 > 60000) return true;
  }
}

Rec hist[10]; uint8_t hN = 0; uint16_t nextId = 1;
float pc[8], pa[8]; uint8_t pn = 0, calSlot = 0;
int8_t calActive = -1;
void csv(const char* t) { Serial.print(t); Serial.print(','); }
void logReading() {
  Rec r = {nextId++, A, conc, T * 100}; memmove(&hist[1], &hist[0], sizeof(Rec) * 9); hist[0] = r; if (hN < 10) hN++;
  Serial.print(F("DATA,")); Serial.print(r.id); Serial.print(','); csv(f2s(millis() / 1000.0, 1)); csv(f2s(A, 4));
  csv(f2s(T * 100, 2)); csv(f2s(conc, 4)); csv(f2s(sig * 1000, 3)); Serial.println(f2s(ref * 1000, 3));
  beep(2400, 60);
}

uint8_t homeView = 0, qa = 0;
const uint8_t NQA = 8;
void qaLabel(uint8_t i, char* o) {
  switch (i) {
    case 0: lab(o, F("READ")); break;      case 1: lab(o, F("BLANK")); break;
    case 2: apP(apI(lab(o, F("+STD (")), pn), F(")")); break;
    case 3: lab(o, F("FIT CURVE")); break; case 4: lab(o, F("KINETICS")); break;
    case 5: lab(o, F("LIVE")); break; case 6: lab(o, F("VIEW A/%T/c")); break; default: lab(o, F("MENU"));
  }
}
void doBlank() {
  msg(F("Insert BLANK"), F("press hold=skip"));
  while (true) { Ev e = button(); if (e == EV_PRESS) break; if (e == EV_HOLD) return; }
  if (!captureStable(F("BLANKING"), true)) return;
  if (sig <= 0.0005) { msg(F("Too little light"), F("LED/cuvette?")); beep(300, 400); idle(2000); return; }
  BL.s0 = sig; BL.r0 = S.useRef ? ref : 0; BL.valid = 1; saveBlank(); darkBase = sOff;
  Serial.print(F("BLANK,")); csv(f2s(sig * 1000, 3)); Serial.println(f2s(ref * 1000, 3));
  beep(2000, 80); beep(2600, 80); resetStability();
}
void drawBar() {
  char q[24]; q[0] = 0; qaLabel(qa, q); q[12] = 0;
  char c[24]; lab(c, F("< ")); ap(c, q); ap(c, " >");
  char c16[24]; uint8_t L = strlen(c), pad = L < 16 ? (16 - L) / 2 : 0;
  memset(c16, ' ', pad); strcpy(c16 + pad, c); line(0, c16, true);
}
void drawHome() {
  drawBar();
  float a = hN ? hist[0].a : NAN, cc = hN ? hist[0].c : NAN;
  if (!BL.valid) lab(buf, F("NO BLANK"));
  else if (hN && overRange) lab(buf, F("LAST: OVER RANGE"));
  else if (hN && leak) lab(buf, F("LAST: LIGHT LEAK"));
  else if (!isnan(a) && a > 2.0) lab(buf, F("DILUTE SAMPLE"));
  else if (!isnan(a) && a < -0.05) lab(buf, F("RE-BLANK?"));
  else if (hN) apP(apI(lab(buf, F("#")), hist[0].id), F(" saved"));
  else lab(buf, F("READY"));
  line(1, buf);
  Curve& k = C[S.curve ? S.curve - 1 : 0]; bool hasC = S.curve && k.used;
  if (homeView == 0) bigLine(3, f2s(a, 3), F("Abs"));
  else if (homeView == 1) bigLine(3, f2s(hN ? hist[0].t : NAN, 1), F("%T"));
  else bigLine(3, f2s(cc, 2), F("conc"));
  buf[0] = 0; if (hasC && homeView == 2) ap(buf, pstr(UNITS, k.unit));
  else if (calActive >= 0 && pn) { apP(apI(apP(apI(lab(buf, F("slot ")), calActive + 1), F(": ")), pn), F(" std")); }
  line(5, buf);
  buf[0] = 0;
  if (homeView != 0) { apP(buf, F("A ")); ap(buf, f2s(a, 3)); }
  else if (hasC) { ap(buf, pstr(NAMES, k.name)); ap(buf, " "); ap(buf, f2s(cc, 2)); }
  line(6, buf);
  apI(lab(buf, F("next #")), nextId); if (BL.valid) apP(buf, F("  BL ok")); line(7, buf);
}
void doRead() {
  if (!BL.valid) { doBlank(); return; }
  if (captureStable(F("MEASURING"), false)) logReading();
}
void liveView() {
  clearAll(); lineP(0, F("LIVE press=stop"), true); resetStability();
  while (true) {
    measure();
    if (BL.valid) bigLine(2, f2s(A, 3), F("Abs")); else lineP(2, F("no blank: raw"));
    lineP(5, stable ? F("STABLE") : overRange ? F("OVER RANGE") : leak ? F("LIGHT LEAK?") : F("changing"));
    lab(buf, F("S")); ap(buf, f2s(sig * 1000, 2)); apP(buf, F("mV x")); apI(buf, GAINX[gS]); line(6, buf);
    buf[0] = 0; if (S.useRef) { apP(buf, F("R")); ap(buf, f2s(ref * 1000, 2)); apP(buf, F("mV")); } line(7, buf);
    if (button() != EV_NONE) return;
  }
}

void settingItem(uint8_t i, char* o) {
  switch (i) {
    case 0: lab(o, F("Gain      ")); if (S.gainMode) { ap(o, "x"); apI(o, GAINX[S.gainMode - 1]); } else apP(o, F("Auto")); break;
    case 1: apI(lab(o, F("Average   ")), AVG[S.avgIdx]); break;
    case 2: apP(lab(o, F("Reference ")), S.useRef ? F("On") : F("Off")); break;
    case 3: apI(lab(o, F("Ref smooth ")), RSM[S.refSmooth]); break;
    case 4: apP(lab(o, F("Auto take ")), S.autoCap ? F("On") : F("Off")); break;
    case 5: ap(lab(o, F("Stable +-")), f2s(STAB[S.stabIdx], 3)); break;
    case 6: apP(apI(lab(o, F("Settle   ")), SETTLE[S.settleIdx]), F("ms")); break;
    case 7: apP(apI(lab(o, F("Warm-up  ")), S.warmup * 10), F("s")); break;
    case 8: apP(apI(lab(o, F("Kin every ")), S.kinInt), F("s")); break;
    case 9: apP(apI(lab(o, F("Kin len  ")), S.kinDur), F("min")); break;
    case 10: apP(lab(o, F("Buzzer    ")), S.buzzer ? F("On") : F("Off")); break;
    case 11: apI(lab(o, F("Contrast  ")), S.contrast); break;
    case 12: apP(lab(o, F("Knob dir  ")), S.reverse ? F("Rev") : F("Normal")); break;
    case 13: lab(o, F("Curve ")); if (S.curve) { apI(o, S.curve); ap(o, " "); ap(o, pstr(NAMES, C[S.curve - 1].name)); } else apP(o, F("    None")); break;
    case 14: lab(o, F("Auto off  ")); if (S.autoOff) apP(apI(o, AUTOOFF[S.autoOff]), F("min")); else apP(o, F("Never")); break;
    default: lab(o, F("Reset defaults"));
  }
}
void cyc(uint8_t& v, int8_t d, uint8_t lo, uint8_t hi) { int x = v + d; while (x < lo) x += hi - lo + 1; while (x > hi) x -= hi - lo + 1; v = x; }
void clp(uint8_t& v, int d, uint8_t lo, uint8_t hi) { v = constrain((int)v + d, lo, hi); }
void stepSetting(uint8_t i, int8_t d) {
  switch (i) {
    case 0: cyc(S.gainMode, d, 0, 5); if (S.gainMode) gS = gR = S.gainMode - 1; break;
    case 1: cyc(S.avgIdx, d, 0, 4); break;      case 2: cyc(S.useRef, d, 0, 1); break;
    case 3: cyc(S.refSmooth, d, 0, 3); break;   case 4: cyc(S.autoCap, d, 0, 1); break;
    case 5: cyc(S.stabIdx, d, 0, 3); break;     case 6: cyc(S.settleIdx, d, 0, 3); break;
    case 7: clp(S.warmup, d, 0, 30); break;     case 8: clp(S.kinInt, d, 1, 60); break;
    case 9: clp(S.kinDur, d, 1, 60); break;     case 10: cyc(S.buzzer, d, 0, 1); break;
    case 11: clp(S.contrast, d, 0, 10); oled.setContrast(S.contrast * 25); break;
    case 12: cyc(S.reverse, d, 0, 1); break;    case 13: cyc(S.curve, d, 0, 3); break;
    case 14: cyc(S.autoOff, d, 0, 4); break;
  }
}
void settingsMenu() {
  static uint8_t sel = 0;
  while (true) {
    int8_t i = listMenu(F("SETTINGS (saved)"), 16, settingItem, sel); if (i < 0) return; sel = i;
    if (i == 15) { defaults(); saveSettings(); oled.setContrast(S.contrast * 25); msg(F("Defaults"), F("restored")); idle(900); continue; }
    clearAll(); lineP(0, F("EDIT: turn"), true); lineP(6, F("press/hold=save"));
    bool redraw = true;
    while (true) {
      int8_t c = turns(); if (c) { stepSetting(i, c); redraw = true; }
      if (button() != EV_NONE) break;
      if (redraw) { buf[0] = 0; settingItem(i, buf); line(3, buf, true); redraw = false; }
    }
    saveSettings(); resetStability(); beep(1800, 40);
  }
}

float gMaxC, gMaxA; Curve* gK;
bool fitCurve(Curve& k) {
  float sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
  for (uint8_t i = 0; i < pn; i++) { sx += pc[i]; sy += pa[i]; sxx += pc[i] * pc[i]; sxy += pc[i] * pa[i]; syy += pa[i] * pa[i]; }
  float d = pn * sxx - sx * sx; if (fabs(d) < 1e-9) return false;
  k.m = (pn * sxy - sx * sy) / d; k.b = (sy - k.m * sx) / pn;
  float num = pn * sxy - sx * sy, den = sqrt(d * (pn * syy - sy * sy));
  k.r2 = den > 0 ? (num / den) * (num / den) : 0; k.n = pn; k.used = 1;
  return true;
}
int16_t aToY(float a) { return 47 - (int16_t)constrain(a / gMaxA * 46, -1, 48); }
uint8_t curveBand(uint8_t x, uint8_t band) {
  float c0 = (x ? x - 1 : 0) * gMaxC / 127, c = x * gMaxC / 127;
  uint8_t m = spanBits(aToY(gK->m * c0 + gK->b), aToY(gK->m * c + gK->b), band);
  for (uint8_t i = 0; i < pn; i++) { int16_t px = pc[i] / gMaxC * 127; if (abs(px - x) <= 1) { int16_t py = aToY(pa[i]); m |= spanBits(py - 1, py + 1, band); } }
  return m;
}
void calItem(uint8_t i, char* o) {
  Curve& k = C[calSlot];
  switch (i) {
    case 0: ap(lab(o, F("Name  ")), pstr(NAMES, k.name)); break;
    case 1: ap(lab(o, F("Unit  ")), pstr(UNITS, k.unit)); break;
    case 2: apP(apI(lab(o, F("Add standard(")), pn), F(")")); break;
    case 3: lab(o, F("Undo last std")); break;
    case 4: lab(o, F("Fit+plot+save")); break;
    case 5: lab(o, F("Use this curve")); break;
    default: lab(o, F("Delete curve"));
  }
}
void slotItem(uint8_t i, char* o) {
  apI(o, i + 1); ap(o, " ");
  if (C[i].used) { ap(o, pstr(NAMES, C[i].name)); ap(o, " "); ap(o, f2s(C[i].r2, 3)); } else apP(o, F("(empty)"));
}
bool needBlank();
float predictNext() {
  if (pn >= 2) return max(0.0f, 2 * pc[pn - 1] - pc[pn - 2]);
  return pn ? (pc[0] > 0 ? pc[0] * 2 : 1) : 0;
}
void addStandard() {
  if (needBlank()) return;
  if (calActive < 0) { calActive = S.curve ? S.curve - 1 : 0; calSlot = calActive; pn = 0; }
  if (pn >= 8) { msg(F("Max 8 standards"), F("FIT or undo")); idle(900); return; }
  float c = predictNext();
  if (!editFloat(F("STD conc, insert"), c, 0.1, 2)) return;
  if (!captureStable(F("READING STD"), false)) return;
  pc[pn] = c; pa[pn] = A; pn++;
  Serial.print(F("STD,")); csv(f2s(c, 4)); Serial.println(f2s(A, 4)); beep(2200, 60);
}
void fitAndShow() {
  if (calActive < 0) calActive = calSlot;
  Curve& k = C[calSlot];
  if (pn < 2) { msg(F("Need 2+ stds"), F("use +STD")); idle(1000); return; }
  if (!fitCurve(k)) { msg(F("Need 2+ diff"), F("concentrations")); idle(1200); return; }
  saveCurves(); S.curve = calSlot + 1; saveSettings();
  Serial.print(F("FIT,m=")); csv(f2s(k.m, 5)); Serial.print(F("b=")); csv(f2s(k.b, 5)); Serial.print(F("R2=")); Serial.println(f2s(k.r2, 5));
  gMaxC = 0.001; gMaxA = 0.01; for (uint8_t j = 0; j < pn; j++) { gMaxC = max(gMaxC, pc[j]); gMaxA = max(gMaxA, pa[j]); }
  gMaxA *= 1.1; gK = &k; clearAll();
  lab(buf, F("R2 ")); ap(buf, f2s(k.r2, 4)); apP(buf, F(" n=")); apI(buf, pn); line(0, buf, true);
  graph(1, 6, curveBand);
  lab(buf, F("active. press")); line(7, buf);
  waitPress();
}
bool needBlank() { if (BL.valid) return false; msg(F("Blank first!"), F("Menu > Blank")); idle(1500); return true; }
void calibrateMenu() {
  if (needBlank()) return;
  int8_t s = listMenu(F("CURVE SLOT"), 3, slotItem); if (s < 0) return;
  if (s != calSlot || calActive < 0) pn = 0;
  calSlot = s; calActive = s; static uint8_t sel = 2;
  while (true) {
    int8_t i = listMenu(F("CALIBRATE"), 7, calItem, sel); if (i < 0) return; sel = i;
    Curve& k = C[calSlot];
    if (i == 0) { k.name = (k.name + 1) % 4; saveCurves(); }
    else if (i == 1) { k.unit = (k.unit + 1) % 5; saveCurves(); }
    else if (i == 2) addStandard();
    else if (i == 3) { if (pn) pn--; }
    else if (i == 4) fitAndShow();
    else if (i == 5) { if (k.used) { S.curve = calSlot + 1; saveSettings(); msg(F("Curve active"), F("")); idle(400); return; } }
    else { k.used = 0; saveCurves(); if (S.curve == calSlot + 1) { S.curve = 0; saveSettings(); } pn = 0; }
  }
}

const uint8_t NP = 64; int16_t pts[NP]; uint8_t np = 0; int16_t kLo, kHi;
int16_t kY(uint8_t i) { return 47 - (long)(pts[i] - kLo) * 46 / (kHi - kLo); }
uint8_t kinBand(uint8_t x, uint8_t band) {
  uint8_t i = x / 2; if (i >= np) return 0;
  return spanBits(kY(i ? i - 1 : 0), kY(i), band);
}
void kinetics() {
  if (needBlank()) return;
  msg(F("Insert sample"), F("press=start"));
  while (true) { Ev e = button(); if (e == EV_PRESS) break; if (e == EV_HOLD) return; }
  float st = 0, sa = 0, stt = 0, sta = 0; uint16_t n = 0; np = 0;
  unsigned long t0 = millis(), dur = (unsigned long)S.kinDur * 60000UL;
  Serial.println(F("KIN_START,t_s,A")); clearAll();
  while (millis() - t0 < dur) {
    unsigned long tNext = t0 + (unsigned long)n * S.kinInt * 1000UL;
    while ((long)(millis() - tNext) < 0) if (button() == EV_HOLD) goto done;
    measure();
    if (!measDone) { if (button() == EV_HOLD) goto done; continue; }
    float t = (millis() - t0) / 1000.0; n++;
    if (!isnan(A)) { st += t; sa += A; stt += t * t; sta += t * A; }
    if (np == NP) { memmove(pts, pts + 1, (NP - 1) * 2); np--; }
    pts[np++] = isnan(A) ? 0 : A * 1000;
    Serial.print(F("KIN,")); csv(f2s(t, 1)); Serial.println(f2s(A, 4));
    float d = n * stt - st * st, rate = (n > 1 && d != 0) ? (n * sta - st * sa) / d * 60 : NAN;
    kLo = 32767; kHi = -32768; for (uint8_t i = 0; i < np; i++) { kLo = min(kLo, pts[i]); kHi = max(kHi, pts[i]); }
    if (kHi - kLo < 10) { kHi += 5; kLo -= 5; }
    lab(buf, F("A ")); ap(buf, f2s(A, 3)); ap(buf, " "); ap(buf, f2s(rate, 4)); line(0, buf, true);
    graph(1, 6, kinBand);
    apP(apI(apP(apI(lab(buf, F("")), t), F("/")), S.kinDur * 60), F("s hold")); line(7, buf);
  }
done:
  Serial.println(F("KIN_END")); beep(2000, 150);
  lineP(7, F("done - press")); waitPress();
}

void diagnostics() {
  forceRef = true;
  clearAll(); lineP(0, F("DIAG hold=back"), true);
  while (button() == EV_NONE) {
    measure();
    lab(buf, F("S ")); ap(buf, f2s(sOn * 1000, sOn < 0.1 ? 2 : 1)); apP(buf, F("/")); ap(buf, f2s(sOff * 1000, sOff < 0.1 ? 2 : 1)); apP(buf, F("mV")); line(2, buf);
    lab(buf, F("R ")); ap(buf, f2s(rOn * 1000, rOn < 0.1 ? 2 : 1)); apP(buf, F("/")); ap(buf, f2s(rOff * 1000, rOff < 0.1 ? 2 : 1)); apP(buf, F("mV")); line(3, buf);
    apI(apP(apI(lab(buf, F("gain S x")), GAINX[gS]), F(" R x")), GAINX[gR]); line(4, buf);
    ap(lab(buf, F("S/R ")), ref > 0 ? f2s(sig / ref, 4) : "--"); line(5, buf);
    ap(lab(buf, F("blank ")), BL.valid ? f2s(BL.s0 * 1000, 2) : "--"); line(6, buf);
    lineP(7, F("mV: LED on/off"));
  }
  forceRef = false; resetStability();
}
void noiseTest() {
  if (needBlank()) return;
  msg(F("Insert cuvette"), F("press = start")); waitPress();
  const uint8_t N = 30; float s = 0, ss = 0; clearAll(); lineP(0, F("NOISE TEST"), true);
  uint8_t i = 0;
  while (i < N) {
    measure();
    if (button() == EV_HOLD) return;
    if (!measDone || isnan(A)) { lineP(5, F("no signal hold=x")); continue; }
    s += A; ss += A * A; i++;
    apP(apI(lab(buf, F("")), i), F(" / 30")); line(3, buf); bar(5, i / (float)N);
  }
  float mean = s / N, sd = sqrt(max(0.0f, (ss - s * s / N) / (N - 1)));
  Serial.print(F("NOISE,")); csv(f2s(mean, 5)); Serial.println(f2s(sd, 5));
  clearAll(); lineP(0, F("NOISE RESULT"), true);
  ap(lab(buf, F("mean A ")), f2s(mean, 4)); line(2, buf);
  ap(lab(buf, F("SD     ")), f2s(sd, 5)); line(3, buf);
  ap(lab(buf, F("LOD A  ")), f2s(3.3 * sd, 4)); line(4, buf);
  if (S.curve && C[S.curve - 1].used) { ap(lab(buf, F("LOD c  ")), f2s(3.3 * sd / C[S.curve - 1].m, 3)); line(5, buf); }

  lineP(6, sd <= 0.002 ? F("Precision STRONG") : sd <= 0.005 ? F("Precision GOOD") : sd <= 0.015 ? F("Precision WEAK") : F("Precision BAD"));
  if (fabs(mean) > 0.1) lineP(1, F("LOD: use blank!"));
  lineP(7, F("press")); beep(2000, 100); waitPress();
}
void historyView() {
  if (!hN) { msg(F("No readings yet"), F("")); idle(900); return; }
  uint8_t i = 0; bool redraw = true; clearAll(); lineP(0, F("HISTORY turn"), true);
  while (true) {
    int8_t c = turns(); if (c) { i = constrain(i + c, 0, hN - 1); redraw = true; }
    if (button() != EV_NONE) return;
    if (redraw) {
      apP(apI(apP(apI(lab(buf, F("#")), hist[i].id), F("  (")), i + 1), F(")")); line(1, buf);
      bigLine(3, f2s(hist[i].a, 3), F("Abs"));
      buf[0] = 0; if (!isnan(hist[i].c)) ap(lab(buf, F("conc ")), f2s(hist[i].c, 3)); line(6, buf);
      redraw = false;
    }
  }
}
void toolItem(uint8_t i, char* o) {
  switch (i) { case 0: lab(o, F("Diagnostics")); break; case 1: lab(o, F("Noise/LOD test")); break; default: lab(o, F("Clear history")); }
}
void toolsMenu() {
  while (true) {
    int8_t i = listMenu(F("TOOLS"), 3, toolItem); if (i < 0) return;
    if (i == 0) diagnostics(); else if (i == 1) noiseTest();
    else { hN = 0; msg(F("History cleared"), F("")); idle(700); }
  }
}

ISR(PCINT2_vect) { isrEnc(); if (!swHit && digitalRead(PIN_SW) == LOW) { swHit = true; swHitAt = millis(); } }
void powerOff() {
  msg(F("Powering off"), F("press to wake")); idle(900);
  if (digitalRead(PIN_SW) == LOW) lineP(7, F("release knob..."));
  digitalWrite(PIN_LED, LOW); oled.setPowerSave(1);
  Serial.println(F("STANDBY")); Serial.flush();
  while (digitalRead(PIN_SW) == LOW) {} delay(50);
  PCMSK2 = _BV(PIN_SW); PCIFR |= _BV(PCIF2); PCICR |= _BV(PCIE2);
  set_sleep_mode(SLEEP_MODE_PWR_DOWN); sleep_enable(); sleep_cpu();
  sleep_disable();
  lastAB = (digitalRead(PIN_A) << 1) | digitalRead(PIN_B); encOn(); qsteps = 0;
  while (digitalRead(PIN_SW) == LOW) {} delay(50); pending = EV_NONE; swDown = false; swHit = false;
  oled.setPowerSave(0); Serial.println(F("WAKE"));
  msg(F("Awake"), F("reblank if moved")); idle(800);
  resetStability(); lastActivity = millis();
}

jmp_buf homeJmp; bool homeReady = false;
void goHome() {
  digitalWrite(PIN_LED, LOW); forceRef = false; inShutdown = false;
  swDown = digitalRead(PIN_SW) == LOW; holdSent = true; swUpAt = millis();
  noInterrupts(); swHit = false; interrupts(); pending = EV_NONE;
  if (homeReady) longjmp(homeJmp, 1);
}
bool released() { if (digitalRead(PIN_SW) == LOW) return false; delay(30); return digitalRead(PIN_SW) == HIGH; }
void holdShutdown() {
  inShutdown = true; digitalWrite(PIN_LED, LOW);
  clearAll(); lineP(0, F("SHUT DOWN"), true); lineP(2, F("keep holding to")); lineP(3, F("shut down"));
  for (uint8_t s = 5; s > 0; s--) {
    char n[2] = {char('0' + s), 0}; oled.draw2x2String(7, 5, n); beep(1500, 30);
    unsigned long t = millis();
    while (millis() - t < 1000) if (released()) { msg(F("Cancelled"), F("")); delay(600); goHome(); return; }
  }
  powerOff();
  goHome();
}
void mainItem(uint8_t i, char* o) {
  switch (i) {
    case 0: lab(o, F("Calibrate curves")); break; case 1: lab(o, F("History")); break;
    case 2: lab(o, F("Tools")); break;            case 3: lab(o, F("Settings")); break;
    default: lab(o, F("Power off"));
  }
}
void mainMenu() {
  static uint8_t sel = 0;
  while (true) {
    int8_t i = listMenu(F("MENU v2.3 hold=x"), 5, mainItem, sel); if (i < 0) break; sel = i;
    if (i == 0) calibrateMenu(); else if (i == 1) historyView(); else if (i == 2) toolsMenu();
    else if (i == 3) settingsMenu(); else { powerOff(); break; }
  }
  clearAll(); resetStability();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT); pinMode(PIN_BUZ, OUTPUT);
  pinMode(PIN_A, INPUT_PULLUP); pinMode(PIN_B, INPUT_PULLUP); pinMode(PIN_SW, INPUT_PULLUP);
  lastAB = (digitalRead(PIN_A) << 1) | digitalRead(PIN_B);
  encOn();
  loadAll();
  loadCal();
  bool ok = ads.begin(0x48);
  oled.begin(); oled.setFont(u8x8_font_chroma48medium8_r); oled.setContrast(S.contrast * 25);
  clearAll(); oled.draw2x2String(0, 0, "COLORI-"); oled.draw2x2String(0, 2, "METER");
  lineP(4, F("openbench.")); lineP(5, F("oesimracing.com"));
  lineP(6, calOwn ? F("v2.3 own cal") : F("v2.3 std cal")); lineP(7, ok ? F("sensor OK") : F("NO SENSOR!"));
  Serial.println(F("OpenBench CM1 v2.3 DATA,id,t_s,A,%T,conc,S_mV,R_mV"));
  delay(2000);
  if (!ok) while (true) {}
  ads.setDataRate(RATE_ADS1115_250SPS);
  if (S.gainMode) gS = gR = S.gainMode - 1;
  unsigned long t0 = millis(), total = S.warmup * 10000UL;
  if (total) {
    digitalWrite(PIN_LED, HIGH); clearAll(); lineP(0, F("WARMING UP LED"), true); lineP(7, F("press = skip"));
    while (millis() - t0 < total && button() == EV_NONE) {
      apP(apI(lab(buf, F("")), (total - (millis() - t0)) / 1000), F(" s left")); line(3, buf);
      bar(5, (millis() - t0) / (float)total); idle(250);
    }
    digitalWrite(PIN_LED, LOW);
  }
  doBlank();
  clearAll(); lastActivity = millis();
}

void loop() {
  static bool dirty = true;
  if (setjmp(homeJmp)) { clearAll(); resetStability(); dirty = true; }
  homeReady = true;
  int8_t c = turns(); if (c) { qa = (qa + NQA * 4 + c) % NQA; drawBar(); }
  if (dirty) { drawHome(); dirty = false; }
  Ev e = button();
  if (e == EV_PRESS) {
    switch (qa) {
      case 0: doRead(); break;
      case 1: doBlank(); break;
      case 2: addStandard(); break;
      case 3: fitAndShow(); break;
      case 4: kinetics(); break;
      case 5: liveView(); break;
      case 6: homeView = (homeView + 1) % 3; break;
      default: mainMenu();
    }
    clearAll(); resetStability(); dirty = true;
  }
  if (e == EV_HOLD) { mainMenu(); dirty = true; }
  if (S.autoOff && millis() - lastActivity > AUTOOFF[S.autoOff] * 60000UL) { powerOff(); clearAll(); dirty = true; }
}
