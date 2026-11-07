#pragma once
#include <stdint.h>

#include "hw.h"
#include "ui.h"

void input_init(const HwConfig& hw);  // buttons, encoder or joystick, on the pins in hw
Key input_poll(uint32_t nowMs);  // call often; returns K_NONE when nothing happened
void input_debug(char* out, int n);  // one line for the "JOY" serial command (joystick state)
