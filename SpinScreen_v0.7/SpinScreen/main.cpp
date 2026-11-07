// setup() and loop() live here (not in the .ino) so the Arduino IDE needs no code generation.
#include <Arduino.h>
#include <Preferences.h>
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
// one write() per line, so lines from the two tasks never interleave
static void sendLine(const char* line) {
  char b[160];
  int n = snprintf(b, sizeof b - 1, "%s\n", line);
  if (n > (int)sizeof b - 2) n = sizeof b - 2;
  Serial.write((const uint8_t*)b, n);
}
static void hSend(const char* line) { sendLine(line); }
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

// ---- USB serial runs in its own task so angle queries are answered within ~1 ms,
//      even while a frame is being drawn. Everything else is handed to the UI loop.
static QueueHandle_t lines;
struct Line { char s[128]; };

static void serialTask(void*) {
  char buf[128];
  int n = 0;
  for (;;) {
    while (Serial.available()) {
      char ch = (char)Serial.read();
      if (ch == '\r') continue;
      if (ch != '\n') {
        if (n < (int)sizeof(buf) - 1) buf[n++] = ch;
        continue;
      }
      buf[n] = 0;
      n = 0;
      if (!strcmp(buf, "POS")) {  // time-critical: answer right here
        char out[64];
        snprintf(out, sizeof out, "POS %.4f %.4f %d", motor_angle(), (double)motor_rpm(), motor_at_speed() ? 1 : 0);
        sendLine(out);
        continue;
      } else if (!strcmp(buf, "ZERO")) {
        motor_zero();
        sendLine("ZEROED");
        continue;
      } else if (!strncmp(buf, "SETANGLE ", 9)) {
        motor_set_angle(atof(buf + 9));
        sendLine("ANGLE");
        continue;
      }
      Line l;
      strncpy(l.s, buf, sizeof l.s);
      l.s[sizeof l.s - 1] = 0;
      xQueueSend(lines, &l, 0);
    }
    vTaskDelay(1);
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
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
  xTaskCreatePinnedToCore(serialTask, "serial", 4096, nullptr, 5, nullptr, 0);
  Serial.println("READY SpinScreen " FW_VERSION);
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
