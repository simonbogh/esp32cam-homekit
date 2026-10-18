// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Simon Bøgh

#pragma once

#include <Arduino.h>

namespace Motion {

void begin();
bool detected();
float changedPct();      // % of changed pixels in the last check
uint32_t checkMs();      // duration of the last decode + compare

}  // namespace Motion
