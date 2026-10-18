// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Simon Bøgh

#include "motion.h"

#include <atomic>
#include <esp_heap_caps.h>
#include <jpeg_decoder.h>

#include "camera.h"
#include "config.h"

namespace Motion {

static constexpr int MAX_W = 1600 / 8;  // 1/8 scale of the largest OV2640 frame
static constexpr int MAX_H = 1200 / 8;
static constexpr size_t WORK_BUF_SIZE = 4096;  // ROM tjpgd (CONFIG_JD_USE_ROM) needs ~3.1 kB; keep it in fast internal RAM

static std::atomic<bool> motion{false};
static volatile float lastPct = 0;
static volatile uint32_t lastCheckMs = 0;

static void motionTask(void *) {
  uint8_t *rgb = (uint8_t *)heap_caps_malloc(MAX_W * MAX_H * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  uint8_t *work = (uint8_t *)heap_caps_malloc(WORK_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint8_t *bg = (uint8_t *)heap_caps_calloc(MAX_W * MAX_H, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rgb || !work || !bg) {
    Serial.println("[motion] buffer allocation failed, motion detection disabled");
    vTaskDelete(nullptr);
    return;
  }

  bool haveBg = false;
  int consecutive = 0;
  uint32_t lastMotionMs = 0;
  uint32_t lastSeq = 0;
  uint16_t bgW = 0, bgH = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(MOTION_INTERVAL_MS));
    const uint32_t now = millis();

    if (motion && now - lastMotionMs > MOTION_HOLD_MS) motion = false;

    FramePtr f = Camera::latest();
    if (!f || f->seq == lastSeq) continue;
    lastSeq = f->seq;

    const uint32_t t0 = millis();
    esp_jpeg_image_cfg_t cfg = {};
    cfg.indata = f->buf;
    cfg.indata_size = f->len;
    cfg.outbuf = rgb;
    cfg.outbuf_size = MAX_W * MAX_H * 3;
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;
    cfg.out_scale = JPEG_IMAGE_SCALE_1_8;
    cfg.advanced.working_buffer = work;
    cfg.advanced.working_buffer_size = WORK_BUF_SIZE;
    esp_jpeg_image_output_t out = {};
    if (esp_jpeg_decode(&cfg, &out) != ESP_OK) continue;  // truncated/corrupt frame: skip

    const int n = out.width * out.height;
    const bool settling = (now - Camera::lastReconfigMs()) < MOTION_SETTLE_MS;
    if (!haveBg || settling || out.width != bgW || out.height != bgH) {
      for (int i = 0; i < n; i++) {
        const uint8_t *p = rgb + 3 * i;
        bg[i] = (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8;
      }
      haveBg = true;
      bgW = out.width;
      bgH = out.height;
      consecutive = 0;
      continue;
    }

    int changed = 0;
    for (int i = 0; i < n; i++) {
      const uint8_t *p = rgb + 3 * i;
      const int y = (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8;
      const int d = y - bg[i];
      if (d > MOTION_PIXEL_DELTA || d < -MOTION_PIXEL_DELTA) changed++;
      bg[i] += d / 8;  // slow running background
    }

    const float pct = changed * 100.0f / n;
    lastPct = pct;
    lastCheckMs = millis() - t0;

    if (pct >= MOTION_LIGHT_PCT) {  // global change (lights, auto-exposure): re-learn background
      haveBg = false;
      consecutive = 0;
      continue;
    }

    consecutive = pct >= MOTION_TRIGGER_PCT ? consecutive + 1 : 0;
    if (consecutive >= 2) {
      if (!motion) Serial.printf("[motion] detected (%.1f%% changed)\n", pct);
      motion = true;
      lastMotionMs = now;
    }
  }
}

void begin() {
  // Idle priority: motion detection only uses spare CPU on core 0, so it backs off on its own while a
  // HomeKit live stream decodes large frames there (and never starves the idle task / task watchdog).
  xTaskCreatePinnedToCore(motionTask, "motion", 4096, nullptr, tskIDLE_PRIORITY, nullptr, 0);
}

bool detected() { return motion; }
float changedPct() { return lastPct; }
uint32_t checkMs() { return lastCheckMs; }

}  // namespace Motion
