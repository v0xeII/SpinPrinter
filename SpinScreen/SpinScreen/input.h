#pragma once
#include <stdint.h>

#include "ui.h"

#include "board.h"

void input_init(const Board& b);  // the joystick on the board's pins (board.h)
Key input_poll(uint32_t nowMs);  // call often; returns K_NONE when nothing happened
void input_debug(char* out, int n);  // one line for the "JOY" serial command (joystick state)
