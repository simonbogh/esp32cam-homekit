// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Simon Bøgh

#pragma once

namespace HomeKitCamera {

// Creates the HomeKit accessory: Camera RTP Stream Management (snapshots via POST /resource,
// live video via LiveStream when LIVE_STREAM=1) plus a Motion Sensor. Call after homeSpan.begin().
void begin();

}  // namespace HomeKitCamera
