// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Simon Bøgh

#pragma once

#define FW_VERSION "1.0.0"

// AI-Thinker ESP32-CAM pin map
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27

#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

#ifndef CAM_XCLK_MHZ
#define CAM_XCLK_MHZ 12
#endif

#ifndef DECLINE_MODE
#define DECLINE_MODE 1
#endif

// Network
#define HOST_NAME_BASE "esp32cam"         // -> esp32cam.local
#define HK_DISPLAY_NAME "ESP32 Cam"
#define WEB_PORT 81                       // index, /jpg, /stats, /control
#define STREAM_PORT 82                    // MJPEG /stream
#define WIFI_TX_POWER WIFI_POWER_13dBm    // lower TX power reduces current spikes / brownouts on this board

// Camera
#define CAM_FRAME_SIZE FRAMESIZE_VGA
#define CAM_JPEG_QUALITY 12

// Motion detection
#define MOTION_INTERVAL_MS 200
#define MOTION_PIXEL_DELTA 25             // luma change that counts as a changed pixel
#define MOTION_TRIGGER_PCT 2              // % of changed pixels that counts as motion
#define MOTION_LIGHT_PCT 50               // % of changed pixels treated as a lighting change
#define MOTION_HOLD_MS 10000
#define MOTION_SETTLE_MS 8000             // auto-exposure needs a few seconds after boot / XCLK change

#define STATS_INTERVAL_MS 10000

// HomeKit live video (experimental): JPEG -> I420 -> H.264 (minih264e) -> RTP/SRTP.
// Set LIVE_STREAM 0 to fall back to declining live view (snapshots only).
#ifndef LIVE_STREAM
#define LIVE_STREAM 1
#endif
#ifndef STREAM_PROFILE
#define STREAM_PROFILE 2                    // default until changed via @p / /control?profile= (saved in NVS)
                                            // 0=160x120 1=240x176 2=320x240 3=480x320 4=640x480
#endif
#define STREAM_GOP 1                        // 1 = every frame is an IDR (robust to packet loss)
#define STREAM_KEY_INTERVAL_S 1             // IDR interval when STREAM_GOP > 1
#define STREAM_TIMEOUT_MS 30000             // end the stream if the controller sends nothing
