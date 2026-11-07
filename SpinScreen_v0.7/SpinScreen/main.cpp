// setup() and loop() live here (not in the .ino) so the Arduino IDE needs no code generation.
#include <Arduino.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include "config.h"
#include "display.h"
#include "gfx.h"
#include "input.h"
#include "motor.h"
#include "ui.h"

// the UI runs in loop(); give it room (default is 8 KB)
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

static Preferences prefs;

static MotorConfig toMotor(const Settings& s) {
  return MotorConfig{s.stepsRev, s.micro, s.gear, s.accel, s.dirInv != 0};
}
// The Pi can be on the USB cable or on three wires (UART: GPIO44 RX, GPIO43 TX). Both are
// listened to all the time; answers go where the Pi's PING last came from (before the first
// PING: to both). So either way works, and switching from one to the other needs nothing.
struct Link {
  Stream* io;
  char buf[128];
  int n;
  bool junk;  // this line had bytes no Pi sends (a floating wire): dropped
};
static Link links[2] = {{&Serial, {0}, 0, false}, {&Serial0, {0}, 0, false}};
// PIN_PI_RX -1: the UART pins are used by something else (breadboard layout), USB only
static const int NLINKS = PIN_PI_RX >= 0 ? 2 : 1;
static volatile int active = -1;  // index into links, -1 = not known yet

// one write() per line, so lines never interleave
static void sendLine(const char* line) {
  char b[160];
  int n = snprintf(b, sizeof b - 1, "%s\n", line);
  if (n > (int)sizeof b - 2) n = sizeof b - 2;
  for (int i = 0; i < NLINKS; i++)
    if (active < 0 || active == i) links[i].io->write((const uint8_t*)b, n);
}
// Lines from the UI go out through the serial task. A USB write can block for ~100 ms when the
// computer at the other end is not reading (port not opened); in the UI loop that froze the
// screen and made Spinny jump. Queued, the UI never waits: if nobody reads, lines are dropped.
static QueueHandle_t txq;
struct Line;
static void hSend(const char* line);
static void hSpin(bool on, float rpm) { motor_set(on, rpm); }
static void hBright(int pct) { display_backlight(pct); }
static void hSave(const Settings& s) {
  prefs.putFloat("rpm", s.rpm);
  prefs.putUChar("lang", s.lang);
  prefs.putUChar("bright", s.bright);
  prefs.putUShort("micro", s.micro);
  prefs.putUShort("steps", s.stepsRev);
  prefs.putFloat("accel", s.accel);
  prefs.putUChar("dir", s.dirInv);
  prefs.putFloat("gear", s.gear);
  prefs.putUChar("dark", s.dark);
  prefs.putUChar("axo", s.axo);
  prefs.putUChar("axoColor", s.axoColor);
}
static float hRpm() { return motor_rpm(); }
static double hAngle() { return motor_angle(); }
static void hCfg(const Settings& s) { motor_config(toMotor(s)); }
static void hZero() { motor_zero(); }
static void hMove(double deg, float rpm) { motor_move(deg, rpm); }

// ---- the Pi link runs in its own task so angle queries are answered within ~1 ms,
//      even while a frame is being drawn. Everything else is handed to the UI loop.
static QueueHandle_t lines;
struct Line { char s[128]; };
static void hSend(const char* line) {
  Line l;
  strncpy(l.s, line, sizeof l.s);
  l.s[sizeof l.s - 1] = 0;
  if (txq) xQueueSend(txq, &l, 0);
}

// a whole line from link k: answer the time-critical ones here, queue the rest for the UI
static void handleLine(int k, const char* buf) {
  if (!strcmp(buf, "PING")) active = k;  // the Pi is on this one
  if (!strcmp(buf, "POS")) {  // time-critical: answer right here
    char out[64];
    snprintf(out, sizeof out, "POS %.4f %.4f %d", motor_angle(), (double)motor_rpm(), motor_at_speed() ? 1 : 0);
    sendLine(out);
    return;
  } else if (!strcmp(buf, "ZERO")) {
    motor_zero();
    sendLine("ZEROED");
    return;
  } else if (!strcmp(buf, "JOY")) {  // joystick state, for wiring checks
    char out[200];
    input_debug(out, sizeof out);
    sendLine(out);
    return;
  } else if (!strncmp(buf, "SETANGLE ", 9)) {
    motor_set_angle(atof(buf + 9));
    sendLine("ANGLE");
    return;
  }
  Line l;
  strncpy(l.s, buf, sizeof l.s);
  l.s[sizeof l.s - 1] = 0;
  xQueueSend(lines, &l, 0);
}

static void serialTask(void*) {
  for (;;) {
    Line out;
    while (xQueueReceive(txq, &out, 0) == pdTRUE) sendLine(out.s);
    for (int k = 0; k < NLINKS; k++) {
      Link& L = links[k];
      while (L.io->available()) {
        uint8_t ch = (uint8_t)L.io->read();
        if (ch == '\r') continue;
        if (ch != '\n') {
          if (ch < 0x20 && ch != '\t') L.junk = true;
          if (L.n < (int)sizeof(L.buf) - 1) L.buf[L.n++] = (char)ch;
          continue;
        }
        L.buf[L.n] = 0;
        bool ok = !L.junk && L.n > 0;
        L.n = 0;
        L.junk = false;
        if (ok) handleLine(k, L.buf);
      }
    }
    vTaskDelay(1);
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
#if PIN_PI_RX >= 0
  Serial0.begin(SERIAL_BAUD, SERIAL_8N1, PIN_PI_RX, PIN_PI_TX);  // the three-wire link
  gpio_pullup_en((gpio_num_t)PIN_PI_RX);  // no wire on RX: idle high, not noise
#endif
  prefs.begin("spinscreen", false);
  Settings s;
  s.rpm = prefs.getFloat("rpm", RPM_DEFAULT);
  s.lang = prefs.getUChar("lang", 0);
  s.bright = prefs.getUChar("bright", 80);
  s.micro = prefs.getUShort("micro", MOTOR_MICROSTEPS);
  s.stepsRev = prefs.getUShort("steps", MOTOR_STEPS_PER_REV);
  s.accel = prefs.getFloat("accel", MOTOR_ACCEL_RPM_S);
  s.dirInv = prefs.getUChar("dir", 0);
  s.gear = prefs.getFloat("gear", MOTOR_GEAR);
  s.dark = prefs.getUChar("dark", 1);
  s.axo = prefs.getUChar("axo", 1);
  s.axoColor = prefs.getUChar("axoColor", 0);

  motor_init(toMotor(s));
  input_init();
  if (!display_init()) {
    while (true) {
      Serial.println("ERR display init failed");
      delay(1000);
    }
  }
  UiHal hal{hSend, hSpin, hBright, hSave, hRpm, hAngle, hCfg, hZero, hMove};
  ui_init(hal, s, millis());
  lines = xQueueCreate(16, sizeof(Line));
  txq = xQueueCreate(24, sizeof(Line));
  xTaskCreatePinnedToCore(serialTask, "serial", 4096, nullptr, 5, nullptr, 0);
  sendLine("READY SpinScreen " FW_VERSION);
}

void loop() {
  Line l;
  while (xQueueReceive(lines, &l, 0) == pdTRUE) ui_line(l.s, millis());
  bool pressed = false;
  for (Key k; (k = input_poll(millis())) != K_NONE;) {
    ui_key(k, millis());
    pressed = true;
  }

  // Frame pacing on an exact microsecond grid: every frame is the same distance apart, and the
  // animation time is the planned frame time, so motion is perfectly even (millisecond pacing
  // alternates 16 and 17 ms, which the eye sees as a slight shimmer).
  //  * the UI asks for 60 fps, or 30 when the screen is completely still;
  //  * if this board cannot draw a frame in time, it settles on a steady 30 fps instead of
  //    stuttering between the two (and goes back to 60 when frames are cheap again).
  static uint64_t nextUs = 0;
  static int wantFps = 60, capFps = 60;
  static float workMs = 8;
  uint64_t t = esp_timer_get_time();
  if (nextUs == 0) nextUs = t;
  if (pressed && nextUs > t + 16667) nextUs = t;  // calm 30 fps: answer a key press right away
  if (t < nextUs) {
    uint32_t left = (uint32_t)(nextUs - t);
    if (left > 1500) {  // plenty of time: let the CPU sleep a tick
      delay(1);
      return;
    }
    delayMicroseconds(left);
    t = nextUs;
  }
  int fps = wantFps < capFps ? wantFps : capFps;
  uint32_t period = 1000000u / fps;
  uint64_t frameT = nextUs;
  if (t > nextUs + period) frameT = t;  // fell far behind (e.g. flash write): start a new grid
  nextUs = frameT + period;

  uint32_t t0 = micros();
  Canvas c(DISPLAY_W, DISPLAY_H, display_buffer());
  wantFps = ui_frame(c, (uint32_t)(frameT / 1000), (frameT % 1000) / 1000.f);
  uint32_t t1 = micros();
  uint32_t bytes = display_push();
  uint32_t t2 = micros();
  workMs += ((t2 - t0) / 1000.f - workMs) * 0.05f;
  if (capFps == 60 && workMs > 14.5f) capFps = 30;
  else if (capFps == 30 && workMs < 9.f) capFps = 60;
  (void)t1;
  ui_stats((t2 - t0) / 1000.f, bytes / 1024.f);
}
