#pragma once
#include <stdint.h>

#include "ui.h"

void input_init();
Key input_poll(uint32_t nowMs);  // call often; returns K_NONE when nothing happened
void input_debug(char* out, int n);  // one line for the "JOY" serial command (joystick state)
