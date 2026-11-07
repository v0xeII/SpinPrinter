// Buttons, rotary encoder or joystick -> UP / DOWN / OK / BACK (long press OK).
#ifdef ARDUINO
#include "input.h"

#include <Arduino.h>

#include "config.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"

static const uint32_t DEBOUNCE = 25, LONG = 650, REPEAT_DELAY = 420, REPEAT_EVERY = 110;

struct Btn {
  int pin;
  bool down = false, raw = false, longFired = false;
  uint32_t changed = 0, pressedAt = 0, lastRepeat = 0;
};

#if INPUT_MODE == INPUT_ENCODER
static Btn ok{PIN_ENC_SW};
#elif INPUT_MODE == INPUT_JOYSTICK
static Btn ok{PIN_JOY_SW};
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

#elif INPUT_MODE == INPUT_JOYSTICK
// Two potentiometers read by the ADC (0..4095, centre about 2048 at 3.3 V).
// A tilt counts past ON from the centre and is released below OFF (hysteresis, no flicker).
// Up / down repeat while held, like the buttons; right = OK, left = BACK, once per tilt.
// A pin with no stick on it floats and reads noise; such an axis is found at power-up and
// switched off, and every tilt must be seen on JOY_AGREE reads in a row, so noise never
// turns into phantom presses.
static const int JOY_ON = 1100, JOY_OFF = 600;
static const uint32_t JOY_READ_EVERY = 10;  // ms: the ADC is not read on every loop
static const int JOY_AGREE = 3;             // 3 reads = 30 ms
struct Axis {
  int pin, centre = 2048;
  bool ok = false;
  int dir = 0, cand = 0, candN = 0;
  int lo = 0, hi = 0, mn = 0, mx = 0, last = -1;  // what the power-up check saw; last read
};
static Axis ax{PIN_JOY_X}, ay{PIN_JOY_Y};
static uint32_t lastRead = 0, yAt = 0, yRep = 0;

static int tilt(int v, int centre, int prev) {
  int d = v - centre;
  if (prev && d * prev > JOY_OFF) return prev;  // still held the same way
  return d > JOY_ON ? 1 : (d < -JOY_ON ? -1 : 0);
}

// true when the new direction has held for JOY_AGREE reads in a row
static bool settle(Axis& a, int raw) {
  if (raw == a.dir) { a.candN = 0; return false; }
  if (raw == a.cand) a.candN++;
  else { a.cand = raw; a.candN = 1; }
  if (a.candN < JOY_AGREE) return false;
  a.dir = raw;
  a.candN = 0;
  return true;
}

// Is a potentiometer really on this pin? Pulled down and then up, a floating pin follows the
// pull across the whole range; a pot moves much less (a 100 kΩ pot still under ~1500 of
// 4095, so the limit is generous). Then the centre, from reads
// spread over 200 ms: a pin that wanders or sits at a rail has no stick on it either.
static void probeAxis(Axis& a, bool invert) {
  gpio_num_t g = (gpio_num_t)a.pin;
  analogRead(a.pin);  // attaches the ADC to the pin first
  gpio_pullup_dis(g);
  gpio_pulldown_en(g);
  delay(3);
  int lo = analogRead(a.pin);
  gpio_pulldown_dis(g);
  gpio_pullup_en(g);
  delay(3);
  int hi = analogRead(a.pin);
  gpio_pullup_dis(g);
  delay(3);
  long sum = 0;
  int mn = 4095, mx = 0;
  for (int i = 0; i < 40; i++) {
    int v = analogRead(a.pin);
    sum += v;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
    delay(5);
  }
  int c = sum / 40;
  a.centre = invert ? 4095 - c : c;
  a.lo = lo, a.hi = hi, a.mn = mn, a.mx = mx;
  a.ok = hi - lo < 3000 && mx - mn < 400 && c > 300 && c < 3800;
}

// which physical axis is "up/down" and which is "OK/back"
#if JOY_SWAP_XY
static Axis& axUD = ax;
static Axis& axLR = ay;
#else
static Axis& axUD = ay;
static Axis& axLR = ax;
#endif

static int joyVal(Axis& a, bool invert) {
  int v = analogRead(a.pin);
  a.last = v;
  return invert ? 4095 - v : v;
}

// "JOY" over serial: is each axis on, its centre, what the power-up check saw, the last read
void input_debug(char* out, int n) {
  snprintf(out, n, "JOY ud pin%d ok%d c%d pull%d..%d spread%d..%d now%d | lr pin%d ok%d c%d pull%d..%d spread%d..%d now%d | sw%d",
           axUD.pin, axUD.ok, axUD.centre, axUD.lo, axUD.hi, axUD.mn, axUD.mx, axUD.last,
           axLR.pin, axLR.ok, axLR.centre, axLR.lo, axLR.hi, axLR.mn, axLR.mx, axLR.last,
           digitalRead(PIN_JOY_SW));
}

void input_init() {
#if PIN_JOY_GND >= 0  // the stick powered straight from two pins (breadboard layout)
  pinMode(PIN_JOY_GND, OUTPUT);
  digitalWrite(PIN_JOY_GND, LOW);
#endif
#if PIN_JOY_VCC >= 0
  pinMode(PIN_JOY_VCC, OUTPUT);
  digitalWrite(PIN_JOY_VCC, HIGH);
#endif
  delay(5);
  pinMode(PIN_JOY_SW, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);  // the full 0..3.3 V range
  // the centre: the stick must be left alone at power-up
  probeAxis(axUD, JOY_INVERT_Y);
  probeAxis(axLR, JOY_INVERT_X);
}

Key input_poll(uint32_t now) {
  if (now - lastRead >= JOY_READ_EVERY) {
    lastRead = now;
    if (axUD.ok && settle(axUD, tilt(joyVal(axUD, JOY_INVERT_Y), axUD.centre, axUD.dir)) && axUD.dir) {
      yAt = yRep = now;
      return axUD.dir < 0 ? K_UP : K_DOWN;
    }
    if (axLR.ok && settle(axLR, tilt(joyVal(axLR, JOY_INVERT_X), axLR.centre, axLR.dir))) {
      if (axLR.dir > 0) return K_OK;
      if (axLR.dir < 0) return K_BACK;
    }
  }
  if (axUD.ok && axUD.dir && now - yAt > REPEAT_DELAY && now - yRep > REPEAT_EVERY) {
    yRep = now;
    return axUD.dir < 0 ? K_UP : K_DOWN;
  }
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
#if INPUT_MODE != INPUT_JOYSTICK
void input_debug(char* out, int n) { snprintf(out, n, "JOY none"); }
#endif
#endif
