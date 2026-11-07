#pragma once
#include <stdint.h>

#include "hw.h"

// Starts the screen chosen in hw (any of hw.h's panels). false: no RAM or a bad pin.
bool display_init(const HwConfig& hw);
int display_width();   // after rotation: 320x170, 320x240, 240x240, 160x128
int display_height();
uint16_t* display_buffer();  // draw the next frame here
// Send the frame. Only the parts that changed since the last frame go over SPI.
// Returns the number of bytes sent (for the stats on the About screen).
uint32_t display_push();
bool display_double_buffered();
void display_backlight(int pct);
