// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Simon Bøgh

#pragma once

#include <Arduino.h>

namespace Web {

void begin();            // starts both HTTP servers (call once Wi-Fi is up)
float streamFps();       // MJPEG frames/s sent to the most recent /stream client
int streamClients();
size_t statsJson(char *buf, size_t size);

}  // namespace Web
