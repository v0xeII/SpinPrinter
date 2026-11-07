#pragma once
#include <stdint.h>

#include "config.h"  // DISPLAY_W / DISPLAY_H come from the panel chosen there

bool display_init();
uint16_t* display_buffer();  // draw the next frame here
// Send the frame. Only the parts that changed since the last frame go over SPI.
// Returns the number of bytes sent (for the stats on the About screen).
uint32_t display_push();
bool display_double_buffered();
void display_backlight(int pct);
