#pragma once

#include <Arduino.h>

#ifndef SOFT_I2C_SCL
#define SOFT_I2C_SCL A4
#endif
#ifndef SOFT_I2C_SDA
#define SOFT_I2C_SDA A5
#endif

#define GAIN_TWOTHIRDS 0x0000
#define GAIN_ONE 0x0200
#define GAIN_TWO 0x0400
#define GAIN_FOUR 0x0600
#define GAIN_EIGHT 0x0800
#define GAIN_SIXTEEN 0x0A00
#define RATE_ADS1115_128SPS 0x0080
#define RATE_ADS1115_250SPS 0x00A0
#define RATE_ADS1115_475SPS 0x00C0

class SoftADS1115 {
 public:
  bool begin(uint8_t addr = 0x48) {
    _addr = addr;
    release(SOFT_I2C_SCL);
    release(SOFT_I2C_SDA);
    delayMicroseconds(50);
    busClear();

    start();
    bool ack = writeByte(_addr << 1);
    stop();
    return ack;
  }
  void setGain(uint16_t g) { _gain = g; }
  void setDataRate(uint16_t r) { _rate = r; }

  int16_t readADC_SingleEnded(uint8_t ch) {
    if (ch > 3) return 0;
    uint16_t cfg = 0x8000
                   | (uint16_t)(4 + ch) << 12
                   | _gain | 0x0100
                   | _rate | 0x0003;
    writeReg(0x01, cfg);

    unsigned long t0 = millis();
    while (!(readReg(0x01) & 0x8000)) {
      if (millis() - t0 > 20) break;
    }
    return (int16_t)readReg(0x00);
  }

  float computeVolts(int16_t counts) {
    float fs;
    switch (_gain) {
      case GAIN_TWOTHIRDS: fs = 6.144; break;
      case GAIN_ONE: fs = 4.096; break;
      case GAIN_TWO: fs = 2.048; break;
      case GAIN_FOUR: fs = 1.024; break;
      case GAIN_EIGHT: fs = 0.512; break;
      default: fs = 0.256; break;
    }
    return counts * (fs / 32768.0);
  }

 private:
  uint8_t _addr = 0x48;
  uint16_t _gain = GAIN_TWOTHIRDS;
  uint16_t _rate = RATE_ADS1115_128SPS;
  static const uint8_t HALF = 5;

  static void release(uint8_t p) { pinMode(p, INPUT); }
  static void pullLow(uint8_t p) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }

  void sclHigh() {
    release(SOFT_I2C_SCL);
    unsigned long t0 = micros();
    while (!digitalRead(SOFT_I2C_SCL) && micros() - t0 < 1000) {}
    delayMicroseconds(HALF);
  }
  void sclLow() { pullLow(SOFT_I2C_SCL); delayMicroseconds(HALF); }

  void busClear() {
    for (uint8_t i = 0; i < 9 && !digitalRead(SOFT_I2C_SDA); i++) { sclLow(); sclHigh(); }
    stop();
  }
  void start() {
    release(SOFT_I2C_SDA); sclHigh();
    pullLow(SOFT_I2C_SDA); delayMicroseconds(HALF);
    sclLow();
  }
  void stop() {
    pullLow(SOFT_I2C_SDA); delayMicroseconds(HALF);
    sclHigh();
    release(SOFT_I2C_SDA); delayMicroseconds(HALF);
  }
  bool writeByte(uint8_t b) {
    for (uint8_t i = 0; i < 8; i++) {
      if (b & 0x80) release(SOFT_I2C_SDA); else pullLow(SOFT_I2C_SDA);
      b <<= 1;
      sclHigh(); sclLow();
    }
    release(SOFT_I2C_SDA);
    sclHigh();
    bool ack = !digitalRead(SOFT_I2C_SDA);
    sclLow();
    return ack;
  }
  uint8_t readByte(bool ack) {
    uint8_t b = 0;
    release(SOFT_I2C_SDA);
    for (uint8_t i = 0; i < 8; i++) {
      sclHigh();
      b = (b << 1) | (digitalRead(SOFT_I2C_SDA) ? 1 : 0);
      sclLow();
    }
    if (ack) pullLow(SOFT_I2C_SDA); else release(SOFT_I2C_SDA);
    sclHigh(); sclLow();
    release(SOFT_I2C_SDA);
    return b;
  }
  void writeReg(uint8_t reg, uint16_t v) {
    start();
    writeByte(_addr << 1);
    writeByte(reg);
    writeByte(v >> 8);
    writeByte(v & 0xFF);
    stop();
  }
  uint16_t readReg(uint8_t reg) {
    start();
    writeByte(_addr << 1);
    writeByte(reg);
    stop();
    start();
    writeByte((_addr << 1) | 1);
    uint8_t hi = readByte(true);
    uint8_t lo = readByte(false);
    stop();
    return ((uint16_t)hi << 8) | lo;
  }
};
