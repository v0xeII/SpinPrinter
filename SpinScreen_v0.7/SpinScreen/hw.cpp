// Hardware settings: defaults from config.h, the CONFIG line, checks. Portable (PC tests too).
#include "hw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

static const PanelInfo PANELS[PANEL_TYPES] = {
    {"?", "?", 240, 320, false, 1, 0},
    {"st7789_170x320", "ST7789 1.9\"", 170, 320, false, 1, 0},
    {"st7789_240x240", "ST7789 1.3\"", 240, 240, false, 1, 0},
    {"st7789_240x320", "ST7789 2.4\"", 240, 320, false, 1, 0},
    {"ili9341", "ILI9341", 240, 320, false, 0, 1},
    {"st7735", "ST7735 1.8\"", 128, 160, false, 0, 1},
    {"gc9a01", "GC9A01", 240, 240, true, 1, 1},
};
const PanelInfo& hw_panel(int p) { return PANELS[p > 0 && p < PANEL_TYPES ? p : 0]; }
static const char* INPUTS[IN_TYPES] = {"?", "buttons", "encoder", "joystick"};

void hw_defaults(HwConfig& c) {
  c.panel = TFT_PANEL;
  c.rot = TFT_ROTATION;
  c.spiMhz = TFT_SPI_MHZ;
  c.spiMode = TFT_SPI_MODE;
  c.invert = c.bgr = c.offX = c.offY = HW_AUTO;
  c.scl = PIN_TFT_SCL; c.sda = PIN_TFT_SDA; c.res = PIN_TFT_RES; c.dc = PIN_TFT_DC; c.cs = PIN_TFT_CS; c.blk = PIN_TFT_BLK;
  c.input = INPUT_MODE;
  if (c.input == IN_ENCODER) { c.inA = PIN_ENC_A; c.inB = PIN_ENC_B; c.inC = PIN_ENC_SW; }
  else if (c.input == IN_JOYSTICK) { c.inA = PIN_JOY_X; c.inB = PIN_JOY_Y; c.inC = PIN_JOY_SW; }
  else { c.inA = PIN_BTN_UP; c.inB = PIN_BTN_DOWN; c.inC = PIN_BTN_OK; }
  c.joyGnd = PIN_JOY_GND; c.joyVcc = PIN_JOY_VCC;
  c.joySwap = JOY_SWAP_XY; c.joyInvX = JOY_INVERT_X; c.joyInvY = JOY_INVERT_Y;
  c.encDetent = ENC_STEPS_PER_DETENT;
  c.step = PIN_STEP; c.dir = PIN_DIR; c.en = PIN_EN;
  c.piRx = PIN_PI_RX; c.piTx = PIN_PI_TX;
}

// Two ready-made sets of pins (the two layouts of config.h), so one word sets a whole board.
static const char* PRESETS[][2] = {
    {"wires", "scl=12 sda=11 res=8 dc=9 cs=10 blk=7 input=buttons up=4 down=5 ok=6 joy_gnd=-1 joy_vcc=-1 "
              "step=1 dir=2 en=13 pi_rx=44 pi_tx=43"},
    {"breadboard", "panel=st7789_240x320 rot=1 spi=40 mode=3 scl=43 sda=44 res=1 dc=2 cs=3 blk=4 input=joystick "
                   "vrx=13 vry=11 sw=12 joy_gnd=-1 joy_vcc=-1 step=5 dir=6 en=7 pi_rx=-1 pi_tx=-1"},
};

enum Kind { K_INT, K_PIN, K_AUTO };  // K_AUTO: a number or "auto"
struct CfgKey { const char* name; int HwConfig::*f; Kind kind; int lo, hi; };
static const CfgKey KEYS[] = {
    {"rot", &HwConfig::rot, K_INT, 0, 3},
    {"spi", &HwConfig::spiMhz, K_INT, 1, 80},
    {"mode", &HwConfig::spiMode, K_INT, 0, 3},
    {"invert", &HwConfig::invert, K_AUTO, 0, 1},
    {"bgr", &HwConfig::bgr, K_AUTO, 0, 1},
    {"offx", &HwConfig::offX, K_AUTO, 0, 200},
    {"offy", &HwConfig::offY, K_AUTO, 0, 200},
    {"scl", &HwConfig::scl, K_PIN, 0, 48},
    {"sda", &HwConfig::sda, K_PIN, 0, 48},
    {"res", &HwConfig::res, K_PIN, -1, 48},
    {"dc", &HwConfig::dc, K_PIN, 0, 48},
    {"cs", &HwConfig::cs, K_PIN, -1, 48},
    {"blk", &HwConfig::blk, K_PIN, -1, 48},
    // the three input pins have a name for each kind of input
    {"up", &HwConfig::inA, K_PIN, 0, 48},
    {"down", &HwConfig::inB, K_PIN, 0, 48},
    {"ok", &HwConfig::inC, K_PIN, 0, 48},
    {"clk", &HwConfig::inA, K_PIN, 0, 48},
    {"dt", &HwConfig::inB, K_PIN, 0, 48},
    {"vrx", &HwConfig::inA, K_PIN, 0, 48},
    {"vry", &HwConfig::inB, K_PIN, 0, 48},
    {"sw", &HwConfig::inC, K_PIN, 0, 48},
    {"joy_gnd", &HwConfig::joyGnd, K_PIN, -1, 48},
    {"joy_vcc", &HwConfig::joyVcc, K_PIN, -1, 48},
    {"joy_swap", &HwConfig::joySwap, K_INT, 0, 1},
    {"joy_invx", &HwConfig::joyInvX, K_INT, 0, 1},
    {"joy_invy", &HwConfig::joyInvY, K_INT, 0, 1},
    {"enc_detent", &HwConfig::encDetent, K_INT, 1, 4},
    {"step", &HwConfig::step, K_PIN, 0, 48},
    {"dir", &HwConfig::dir, K_PIN, 0, 48},
    {"en", &HwConfig::en, K_PIN, -1, 48},
    {"pi_rx", &HwConfig::piRx, K_PIN, -1, 48},
    {"pi_tx", &HwConfig::piTx, K_PIN, -1, 48},
};

static int findName(const char* const* names, int n, const char* v) {
  for (int i = 1; i < n; i++)
    if (!strcmp(names[i], v)) return i;
  return 0;
}

const char* hw_apply(HwConfig& c, const char* line) {
  static char err[64];
  char buf[400];
  strncpy(buf, line, sizeof buf - 1);
  buf[sizeof buf - 1] = 0;
  for (char* p = buf; *p;) {  // (no strtok: a preset applies itself recursively)
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    char* tok = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = 0;
    char* eq = strchr(tok, '=');
    if (!eq) { snprintf(err, sizeof err, "no value: %s", tok); return err; }
    *eq = 0;
    const char *k = tok, *v = eq + 1;
    if (!strcmp(k, "preset")) {
      int i = 0, n = sizeof PRESETS / sizeof PRESETS[0];
      while (i < n && strcmp(PRESETS[i][0], v)) i++;
      if (i == n) { snprintf(err, sizeof err, "unknown preset %s", v); return err; }
      const char* e = hw_apply(c, PRESETS[i][1]);
      if (e) return e;
      continue;
    }
    if (!strcmp(k, "panel")) {
      int p = 1;
      while (p < PANEL_TYPES && strcmp(PANELS[p].key, v)) p++;
      if (p == PANEL_TYPES) { snprintf(err, sizeof err, "unknown panel %s", v); return err; }
      c.panel = p;
      continue;
    }
    if (!strcmp(k, "input")) {
      int i = findName(INPUTS, IN_TYPES, v);
      if (!i) { snprintf(err, sizeof err, "unknown input %s", v); return err; }
      c.input = i;
      continue;
    }
    const CfgKey* key = nullptr;
    for (auto& kk : KEYS)
      if (!strcmp(kk.name, k)) key = &kk;
    if (!key) { snprintf(err, sizeof err, "unknown key %s", k); return err; }
    if (key->kind == K_AUTO && !strcmp(v, "auto")) { c.*(key->f) = HW_AUTO; continue; }
    char* end;
    long x = strtol(v, &end, 10);
    if (!*v || *end || x < key->lo || x > key->hi) {
      snprintf(err, sizeof err, "bad value %s=%s (%d..%d)", k, v, key->lo, key->hi);
      return err;
    }
    c.*(key->f) = (int)x;
  }
  return nullptr;
}

// GPIOs an ESP32-S3 module can use: 0..21 and 33..48, not 19 / 20 (the USB port the Pi and the
// installer talk over). 22..25 do not exist, 26..32 are the flash.
static bool gpioOk(int p) { return p >= 0 && p <= 48 && p != 19 && p != 20 && !(p >= 22 && p <= 32); }

const char* hw_check(const HwConfig& c) {
  static char err[80];
  if (c.panel < 1 || c.panel >= PANEL_TYPES) return "no panel";
  if (c.input < 1 || c.input >= IN_TYPES) return "no input";
  struct { const char* name; int pin; bool need; } pins[] = {
      {"scl", c.scl, true}, {"sda", c.sda, true}, {"res", c.res, false}, {"dc", c.dc, true},
      {"cs", c.cs, false}, {"blk", c.blk, false},
      {c.input == IN_BUTTONS ? "up" : (c.input == IN_ENCODER ? "clk" : "vrx"), c.inA, true},
      {c.input == IN_BUTTONS ? "down" : (c.input == IN_ENCODER ? "dt" : "vry"), c.inB, true},
      {c.input == IN_BUTTONS ? "ok" : "sw", c.inC, true},
      {"joy_gnd", c.input == IN_JOYSTICK ? c.joyGnd : -1, false},
      {"joy_vcc", c.input == IN_JOYSTICK ? c.joyVcc : -1, false},
      {"step", c.step, true}, {"dir", c.dir, true}, {"en", c.en, false},
      {"pi_rx", c.piRx, false}, {"pi_tx", c.piTx, false},
  };
  const int n = sizeof pins / sizeof pins[0];
  for (int i = 0; i < n; i++) {
    if (pins[i].pin < 0) {
      if (pins[i].need) { snprintf(err, sizeof err, "%s needs a pin", pins[i].name); return err; }
      continue;
    }
    if (!gpioOk(pins[i].pin)) { snprintf(err, sizeof err, "%s: GPIO%d cannot be used", pins[i].name, pins[i].pin); return err; }
    for (int j = 0; j < i; j++)
      if (pins[j].pin == pins[i].pin) {
        snprintf(err, sizeof err, "GPIO%d twice: %s and %s", pins[i].pin, pins[j].name, pins[i].name);
        return err;
      }
  }
  if ((c.piRx < 0) != (c.piTx < 0)) return "pi_rx and pi_tx: both pins or both -1";
  // the joystick's potentiometers need an ADC pin (ADC1: GPIO1..10, ADC2: GPIO11..20)
  if (c.input == IN_JOYSTICK && (c.inA < 1 || c.inA > 20 || c.inB < 1 || c.inB > 20))
    return "vrx and vry need ADC pins (GPIO1..20)";
  return nullptr;
}

int hw_format(const HwConfig& c, char* out, int n) {
  char inv[8], bgr[8], ox[8], oy[8];
  auto au = [](char* b, int v) { if (v == HW_AUTO) strcpy(b, "auto"); else snprintf(b, 8, "%d", v); };
  au(inv, c.invert); au(bgr, c.bgr); au(ox, c.offX); au(oy, c.offY);
  const char *a = "up", *b = "down", *k = "ok";
  if (c.input == IN_ENCODER) { a = "clk"; b = "dt"; k = "sw"; }
  if (c.input == IN_JOYSTICK) { a = "vrx"; b = "vry"; k = "sw"; }
  int len = snprintf(out, n,
                     "panel=%s rot=%d spi=%d mode=%d invert=%s bgr=%s offx=%s offy=%s scl=%d sda=%d res=%d dc=%d cs=%d "
                     "blk=%d input=%s %s=%d %s=%d %s=%d",
                     hw_panel(c.panel).key, c.rot, c.spiMhz, c.spiMode, inv, bgr, ox, oy, c.scl, c.sda, c.res, c.dc, c.cs,
                     c.blk, INPUTS[c.input > 0 && c.input < IN_TYPES ? c.input : 0], a, c.inA, b, c.inB, k, c.inC);
  if (len < n && c.input == IN_JOYSTICK)
    len += snprintf(out + len, n - len, " joy_gnd=%d joy_vcc=%d joy_swap=%d joy_invx=%d joy_invy=%d", c.joyGnd,
                    c.joyVcc, c.joySwap, c.joyInvX, c.joyInvY);
  if (len < n && c.input == IN_ENCODER) len += snprintf(out + len, n - len, " enc_detent=%d", c.encDetent);
  if (len < n)
    len += snprintf(out + len, n - len, " step=%d dir=%d en=%d pi_rx=%d pi_tx=%d", c.step, c.dir, c.en, c.piRx, c.piTx);
  return len;
}

int hw_rotation(const HwConfig& c) {
  const PanelInfo& p = hw_panel(c.panel);
  int r = c.rot & 3;
  if (p.w != p.h) r |= 1;  // the UI is landscape: 0 -> 1, 2 -> 3
  return r;
}
void hw_screen_size(const HwConfig& c, int& w, int& h) {
  const PanelInfo& p = hw_panel(c.panel);
  bool swap = hw_rotation(c) & 1;
  w = swap ? p.h : p.w;
  h = swap ? p.w : p.h;
}
int hw_invert(const HwConfig& c) { return c.invert == HW_AUTO ? hw_panel(c.panel).invert : c.invert; }
int hw_bgr(const HwConfig& c) { return c.bgr == HW_AUTO ? hw_panel(c.panel).bgr : c.bgr; }
