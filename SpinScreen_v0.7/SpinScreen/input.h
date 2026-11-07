#pragma once
#include <stdint.h>

#include "ui.h"

void input_init();
Key input_poll(uint32_t nowMs);  // call often; returns K_NONE when nothing happened
