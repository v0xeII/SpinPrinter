// The board's wiring (pins, screen and joystick settings) as values, not only as config.h macros.
//
// The board keeps its wiring in its own flash (NVS, key "board"). Which wiring is used at start:
//  * your own build (Arduino IDE, arduino-cli): the pins in config.h. They are also saved, so the
//    board remembers them;
//  * a release build (made by GitHub, -DSPIN_RELEASE=1): the wiring the board has saved. So a
//    release can go onto any board over the air from the Raspberry and keep its pins. A board that
//    has saved nothing yet (a new one) takes the release's config.h.
#pragma once
#include <stdint.h>
#include <stdio.h>

#include "config.h"

struct Board {
  uint8_t tftScl, tftSda, tftRes, tftDc, tftCs, tftBlk;
  uint8_t rotation, spiMhz, spiMode, invert, bgr;
  uint8_t joyX, joyY, joySw, swapXY, invY, invX;
  uint8_t step, dir, en;
};

static inline Board board_compiled() {
  return Board{PIN_TFT_SCL, PIN_TFT_SDA, PIN_TFT_RES, PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_BLK,
               TFT_ROTATION, TFT_SPI_MHZ, TFT_SPI_MODE, TFT_INVERT, TFT_BGR,
               PIN_JOY_X, PIN_JOY_Y, PIN_JOY_SW, JOY_SWAP_XY, JOY_INVERT_Y, JOY_INVERT_X,
               PIN_STEP, PIN_DIR, PIN_EN};
}

// "43,44,1,..." in the order of PIN_SIG in ui.cpp (the Raspberry compares the two)
static inline void board_sig(const Board& b, char* out, size_t n) {
  snprintf(out, n, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", b.tftScl, b.tftSda, b.tftRes,
           b.tftDc, b.tftCs, b.tftBlk, b.rotation, b.spiMhz, b.spiMode, b.invert, b.bgr, b.joyX, b.joyY, b.joySw,
           b.swapXY, b.invY, b.invX, b.step, b.dir, b.en);
}

#ifdef SPIN_RELEASE
#define SPIN_BUILD "release"
#else
#define SPIN_BUILD "local"
#endif
