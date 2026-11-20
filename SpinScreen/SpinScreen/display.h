#pragma once
#include <stdint.h>

#include "board.h"

// Starts the ST7789 on the board's pins (board.h). false: no RAM or the SPI did not start.
bool display_init(const Board& b);
int display_width();   // 320 (landscape, config.h TFT_ROTATION)
int display_height();
uint16_t* display_buffer();  // draw the next frame here
// Send the frame. Only the parts that changed since the last frame go over SPI.
// Returns the number of bytes sent (for the stats on the About screen).
uint32_t display_push();
bool display_double_buffered();
void display_backlight(int pct);
