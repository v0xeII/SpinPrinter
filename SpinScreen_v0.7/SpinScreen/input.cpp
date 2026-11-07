// Buttons or rotary encoder -> UP / DOWN / OK / BACK (long press OK).
#ifdef ARDUINO
#include "input.h"

#include <Arduino.h>

#include "config.h"
#include "soc/gpio_reg.h"

static const uint32_t DEBOUNCE = 25, LONG = 650, REPEAT_DELAY = 420, REPEAT_EVERY = 110;

struct Btn {
  int pin;
  bool down = false, raw = false, longFired = false;
  uint32_t changed = 0, pressedAt = 0, lastRepeat = 0;
};

#if INPUT_MODE == INPUT_ENCODER
static Btn ok{PIN_ENC_SW};
#else
static Btn ok{PIN_BTN_OK};
#endif

// returns true on a debounced edge
static bool update(Btn& b, uint32_t now) {
  bool r = digitalRead(b.pin) == LOW;
  if (r != b.raw) { b.raw = r; b.changed = now; }
  if (b.raw != b.down && now - b.changed >= DEBOUNCE) {
    b.down = b.raw;
    if (b.down) { b.pressedAt = now; b.lastRepeat = now; b.longFired = false; }
    return true;
  }
  return false;
}

// OK: short = OK on release, long = BACK while held
static Key okKey(uint32_t now) {
  bool edge = update(ok, now);
  if (ok.down && !ok.longFired && now - ok.pressedAt >= LONG) { ok.longFired = true; return K_BACK; }
  if (edge && !ok.down && !ok.longFired) return K_OK;
  return K_NONE;
}

#if INPUT_MODE == INPUT_BUTTONS
static Btn up{PIN_BTN_UP}, dn{PIN_BTN_DOWN};

static Key arrowKey(Btn& b, Key k, uint32_t now) {
  bool edge = update(b, now);
  if (edge && b.down) return k;
  if (b.down && now - b.pressedAt > REPEAT_DELAY && now - b.lastRepeat > REPEAT_EVERY) {
    b.lastRepeat = now;
    return k;
  }
  return K_NONE;
}

void input_init() {
  pinMode(PIN_BTN_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_DOWN, INPUT_PULLUP);
  pinMode(PIN_BTN_OK, INPUT_PULLUP);
}

Key input_poll(uint32_t now) {
  Key k;
  if ((k = arrowKey(up, K_UP, now)) != K_NONE) return k;
  if ((k = arrowKey(dn, K_DOWN, now)) != K_NONE) return k;
  return okKey(now);
}

#else  // INPUT_ENCODER
static volatile int encCount = 0;
static volatile uint8_t encState = 0;
static int encUsed = 0;

static void IRAM_ATTR encIsr() {
  static const DRAM_ATTR int8_t T[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  uint32_t in = REG_READ(GPIO_IN_REG);  // pins 0..31, safe inside an interrupt
  uint8_t s = (((in >> PIN_ENC_A) & 1) << 1) | ((in >> PIN_ENC_B) & 1);
  encState = ((encState << 2) | s) & 0x0F;
  encCount += T[encState];
}

void input_init() {
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encIsr, CHANGE);
}

Key input_poll(uint32_t now) {
  int c = encCount;
  if (c - encUsed >= ENC_STEPS_PER_DETENT) { encUsed += ENC_STEPS_PER_DETENT; return K_DOWN; }
  if (encUsed - c >= ENC_STEPS_PER_DETENT) { encUsed -= ENC_STEPS_PER_DETENT; return K_UP; }
  return okKey(now);
}
#endif
#endif
