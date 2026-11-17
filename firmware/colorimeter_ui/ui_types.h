#pragma once
#include <Arduino.h>
enum Ev : uint8_t { EV_NONE, EV_PRESS, EV_HOLD };
typedef void (*ItemFn)(uint8_t i, char* out);
typedef uint8_t (*BandFn)(uint8_t x, uint8_t band);
struct Rec { uint16_t id; float a, c, t; };
