// Hardware settings chosen at run time: which screen, how it is wired, which buttons, motor pins.
// One firmware for every board. They are stored on the board (NVS, as one "key=value ..." line)
// and set over serial with CONFIG (or from the web installer). Until something is stored, the
// values from config.h apply. Portable (no Arduino): the PC tests use it too.
#pragma once
#include <stdint.h>

enum PanelType {
  PANEL_ST7789_170x320 = 1,  // 1.9" wide
  PANEL_ST7789_240x240,      // 1.3" / 1.54" square
  PANEL_ST7789_240x320,      // 2.0" / 2.4" / 2.8"
  PANEL_ILI9341,             // 2.4" / 2.8" / 3.2", 240x320
  PANEL_ST7735,              // 1.8", 128x160
  PANEL_GC9A01,              // 1.28" round, 240x240
  PANEL_TYPES
};
enum InputType { IN_BUTTONS = 1, IN_ENCODER, IN_JOYSTICK, IN_TYPES };

static const int HW_AUTO = -999;  // invert / bgr / offx / offy: take the panel's usual value

struct HwConfig {
  int panel, rot, spiMhz, spiMode, invert, bgr, offX, offY;
  int scl, sda, res, dc, cs, blk;
  int input;
  int inA, inB, inC;  // buttons: up, down, ok · encoder: clk, dt, sw · joystick: vrx, vry, sw
  int joyGnd, joyVcc, joySwap, joyInvX, joyInvY, encDetent;
  int step, dir, en;
  int piRx, piTx;
};

struct PanelInfo {
  const char* key;   // name in CONFIG ("st7789_240x320")
  const char* name;  // for people ("ST7789 2.4\"")
  int w, h;          // the controller's own (portrait) size
  bool round;
  int invert, bgr;   // usual values
};
const PanelInfo& hw_panel(int panel);

void hw_defaults(HwConfig& c);  // the values from config.h
// Apply "key=value key=value ..." to c. Returns nullptr when all went well, else an error
// ("unknown key foo"). Keys: see hw.cpp (KEYS) or the README.
const char* hw_apply(HwConfig& c, const char* line);
// Is this a configuration the board can run? nullptr = yes, else what is wrong.
const char* hw_check(const HwConfig& c);
// c as one "key=value ..." line (every key, the input pins under their names for this input)
int hw_format(const HwConfig& c, char* out, int n);

// screen size after rotation (landscape for the rectangular panels)
void hw_screen_size(const HwConfig& c, int& w, int& h);
int hw_rotation(const HwConfig& c);  // 0..3, odd = landscape on the rectangular panels
int hw_invert(const HwConfig& c);
int hw_bgr(const HwConfig& c);
