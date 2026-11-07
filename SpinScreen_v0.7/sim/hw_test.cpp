// Hardware settings test: the CONFIG line (hw.cpp) parses, checks and round-trips.
//   ./hw_test     exit code 1 on any failure
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "hw.h"

static int fails = 0;
static void expect(bool ok, const char* what) {
  printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) fails++;
}
static const char* apply(HwConfig& c, const char* line) {
  const char* e = hw_apply(c, line);
  return e ? e : hw_check(c);
}

int main() {
  HwConfig d;
  hw_defaults(d);
  expect(!hw_check(d), "config.h's defaults pass the checks");
  int w, h;
  hw_screen_size(d, w, h);
  expect(w == 320 && h == 170, "default screen 320x170");

  // every panel: size after rotation, landscape on the rectangular ones
  struct { const char* p; int rot, w, h; } sizes[] = {
      {"st7789_170x320", 0, 320, 170}, {"st7789_240x240", 0, 240, 240}, {"st7789_240x320", 2, 320, 240},
      {"ili9341", 1, 320, 240},        {"st7735", 3, 160, 128},         {"gc9a01", 1, 240, 240},
  };
  for (auto& s : sizes) {
    HwConfig c = d;
    char line[80], what[120];
    snprintf(line, sizeof line, "panel=%s rot=%d", s.p, s.rot);
    const char* e = apply(c, line);
    hw_screen_size(c, w, h);
    snprintf(what, sizeof what, "%s -> %dx%d%s", line, w, h, e ? e : "");
    expect(!e && w == s.w && h == s.h, what);
  }
  HwConfig c = d;
  expect(!apply(c, "panel=gc9a01") && hw_panel(c.panel).round, "gc9a01 is round");
  expect(hw_invert(c) == 1 && hw_bgr(c) == 1, "gc9a01: inverted, BGR");
  c = d;
  apply(c, "panel=ili9341");
  expect(hw_invert(c) == 0 && hw_bgr(c) == 1, "ili9341: not inverted, BGR");
  expect(!apply(c, "invert=1 bgr=0") && hw_invert(c) == 1 && hw_bgr(c) == 0, "invert / bgr by hand");
  expect(!apply(c, "invert=auto") && hw_invert(c) == 0, "invert=auto");

  // inputs and their pin names
  c = d;
  expect(!apply(c, "input=encoder clk=4 dt=5 sw=6") && c.inA == 4 && c.inB == 5 && c.inC == 6, "encoder pins");
  expect(!apply(c, "input=joystick vrx=1 vry=2 sw=6 step=40 dir=41") && c.input == IN_JOYSTICK, "joystick pins");
  c = d;
  expect(apply(c, "input=joystick vrx=43 vry=44 sw=6") != nullptr, "joystick on non-ADC pins: refused");

  // checks
  c = d;
  const char* e = apply(c, "dc=12");
  expect(e && strstr(e, "twice"), "a pin used twice is refused");
  c = d;
  e = apply(c, "scl=20");
  expect(e && strstr(e, "cannot"), "GPIO20 (USB) is refused");
  c = d;
  e = apply(c, "scl=27");
  expect(e != nullptr, "GPIO27 (flash) is refused");
  c = d;
  expect(apply(c, "panel=st9999") != nullptr, "unknown panel");
  c = d;
  expect(apply(c, "colour=red") != nullptr, "unknown key");
  c = d;
  expect(apply(c, "rot=7") != nullptr, "rot out of range");
  c = d;
  expect(apply(c, "pi_rx=-1") != nullptr, "pi_rx without pi_tx");
  c = d;
  expect(!apply(c, "cs=-1 blk=-1 res=-1 en=-1"), "optional pins can be -1");

  // presets
  c = d;
  expect(!apply(c, "preset=breadboard"), "preset=breadboard");
  hw_screen_size(c, w, h);
  expect(w == 320 && h == 240 && c.input == IN_JOYSTICK && c.scl == 43 && c.piRx == -1 && c.step == 5,
         "breadboard: 320x240, joystick on 13/11/12, screen on TX/RX, USB only");
  expect(!apply(c, "preset=wires panel=st7789_170x320"), "preset=wires");
  expect(c.scl == 12 && c.input == IN_BUTTONS && c.piRx == 44, "wires: the README's wiring");
  HwConfig b = d;
  apply(b, "preset=breadboard spi=27");
  expect(b.spiMhz == 27 && b.scl == 43, "keys after a preset win");

  // round trip: what CONFIG? prints is a CONFIG line that gives the same settings
  const char* lines[] = {"preset=breadboard", "panel=gc9a01 input=encoder clk=4 dt=5 sw=6 enc_detent=2 offx=3",
                         "panel=st7735 rot=3 invert=1 bgr=0 spi=27 input=buttons up=4 down=5 ok=6"};
  for (const char* l : lines) {
    HwConfig x = d, y = d;
    apply(x, l);
    char out[440];
    hw_format(x, out, sizeof out);
    const char* err = apply(y, out);
    expect(!err && !memcmp(&x, &y, sizeof x), l);
    if (err) printf("      %s\n      %s\n", out, err);
  }
  char out[440];
  int n = hw_format(d, out, sizeof out);
  printf("\nCONFIG %s  (%d chars)\n", out, n);
  expect(n < 400, "the line fits the board's line buffer");
  printf("%s\n", fails ? "FAILED" : "all ok");
  return fails ? 1 : 0;
}
