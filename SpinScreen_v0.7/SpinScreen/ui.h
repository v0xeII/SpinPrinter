// SpinScreen UI: screens, menus, animations. Portable (ESP32 + PC simulator).
#pragma once
#include <stdint.h>

#include "gfx.h"

enum Key { K_NONE, K_UP, K_DOWN, K_OK, K_BACK };

struct Settings {
  float rpm;      // vial speed
  int lang;       // 0 = русский, 1 = English
  int bright;     // 10..100 %
  int micro;      // driver microsteps
  int stepsRev;   // motor full steps per revolution
  float accel;    // rpm per second
  int dirInv;     // 1 = counter-clockwise
  float gear;     // motor turns per vial turn
  int dark;       // dim the screen while printing
  int axo;        // show Spinny the axolotl
  int axoColor;   // colour morph (spinny.h MORPH_*)
};

// What the UI needs from the hardware. main.cpp fills this in; the simulator fakes it.
struct UiHal {
  void (*send)(const char* line);          // one line to the Raspberry Pi
  void (*spin)(bool on, float rpm);        // motor on/off + target speed
  void (*brightness)(int pct);             // backlight
  void (*save)(const Settings& s);         // persist settings
  float (*motorRpm)();                     // current (ramping) speed
  double (*motorAngle)();                  // vial angle, degrees
  void (*motorConfig)(const Settings& s);  // steps / microsteps / ramp / direction changed
  void (*motorZero)();
  void (*motorMove)(double deg, float rpm);  // exact turn ("1 оборот" test)
};

void ui_init(const UiHal& hal, const Settings& s, uint32_t nowMs);
void ui_key(Key k, uint32_t nowMs);
void ui_line(const char* line, uint32_t nowMs);  // a text line received from the Pi
int ui_frame(Canvas& c, uint32_t nowMs, float subMs = 0);  // subMs: fraction of a millisecond, for even motion         // draw one frame; returns the frame rate it wants next (60/30/15)
void ui_stats(float frameMs, float kbPerFrame);  // for the About screen
