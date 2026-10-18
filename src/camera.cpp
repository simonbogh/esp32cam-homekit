// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Simon Bøgh

#include "camera.h"

#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <atomic>
#include <freertos/semphr.h>

#include "config.h"

Frame::~Frame() {
  heap_caps_free(buf);
}

namespace Camera {

static SemaphoreHandle_t camMutex;    // guards the sensor/driver (capture vs. XCLK change)
static SemaphoreHandle_t frameMutex;  // guards latestFrame
static FramePtr latestFrame;
static volatile int currentXclkMhz = CAM_XCLK_MHZ;
static volatile float fps = 0;
static std::atomic<uint32_t> errors{0};
static volatile uint32_t reconfigMs = 0;

static void captureTask(void *) {
  uint32_t seq = 0;
  uint32_t windowStart = millis();
  uint32_t windowFrames = 0;

  for (;;) {
    xSemaphoreTake(camMutex, portMAX_DELAY);
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      xSemaphoreGive(camMutex);
      errors++;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    auto frame = std::make_shared<Frame>();
    frame->buf = (uint8_t *)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frame->buf) {
      memcpy(frame->buf, fb->buf, fb->len);
      frame->len = fb->len;
      frame->width = fb->width;
      frame->height = fb->height;
      frame->seq = ++seq;
      frame->timestampUs = esp_timer_get_time();
    }
    esp_camera_fb_return(fb);
    xSemaphoreGive(camMutex);

    if (!frame->buf) {
      errors++;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    xSemaphoreTake(frameMutex, portMAX_DELAY);
    latestFrame = frame;
    xSemaphoreGive(frameMutex);

    windowFrames++;
    const uint32_t now = millis();
    if (now - windowStart >= 1000) {
      fps = windowFrames * 1000.0f / (now - windowStart);
      windowFrames = 0;
      windowStart = now;
    }
    vTaskDelay(1);  // let lower-priority tasks on this core run
  }
}

bool begin() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = CAM_XCLK_MHZ * 1000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = CAM_FRAME_SIZE;
  config.jpeg_quality = CAM_JPEG_QUALITY;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (!psramFound()) {
    Serial.println("[cam] PSRAM not found - this firmware needs the 4 MB PSRAM of the ESP32-CAM");
    return false;
  }

  const esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[cam] init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  Serial.printf("[cam] sensor PID 0x%02x, %s JPEG q=%d, XCLK %d MHz\n", s->id.PID,
                CAM_FRAME_SIZE == FRAMESIZE_VGA ? "VGA" : "custom", CAM_JPEG_QUALITY, CAM_XCLK_MHZ);

  camMutex = xSemaphoreCreateMutex();
  frameMutex = xSemaphoreCreateMutex();
  reconfigMs = millis();
  xTaskCreatePinnedToCore(captureTask, "capture", 4096, nullptr, 2, nullptr, 1);
  return true;
}

FramePtr latest() {
  if (!frameMutex) return nullptr;
  xSemaphoreTake(frameMutex, portMAX_DELAY);
  FramePtr f = latestFrame;
  xSemaphoreGive(frameMutex);
  return f;
}

FramePtr waitNext(uint32_t lastSeq, uint32_t timeoutMs) {
  const uint32_t start = millis();
  for (;;) {
    FramePtr f = latest();
    if (f && f->seq != lastSeq) return f;
    if (millis() - start >= timeoutMs) return nullptr;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool setXclk(int mhz) {
  if (mhz < 4 || mhz > 24) return false;
  sensor_t *s = esp_camera_sensor_get();
  if (!s || !s->set_xclk) return false;

  xSemaphoreTake(camMutex, portMAX_DELAY);
  const int rc = s->set_xclk(s, LEDC_TIMER_0, mhz);
  // drop frames that were exposed with the old clock
  for (int i = 0; i < 2; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }
  xSemaphoreGive(camMutex);

  if (rc != 0) return false;
  currentXclkMhz = mhz;
  reconfigMs = millis();
  return true;
}

bool setFrameSize(framesize_t size) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return false;
  if (size == (framesize_t)s->status.framesize) return true;

  xSemaphoreTake(camMutex, portMAX_DELAY);
  const int rc = s->set_framesize(s, size);
  for (int i = 0; i < 2; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }
  xSemaphoreGive(camMutex);

  reconfigMs = millis();
  return rc == 0;
}

framesize_t frameSize() {
  sensor_t *s = esp_camera_sensor_get();
  return s ? (framesize_t)s->status.framesize : CAM_FRAME_SIZE;
}

int xclkMhz() { return currentXclkMhz; }
float captureFps() { return fps; }
uint32_t captureErrors() { return errors; }
uint32_t lastReconfigMs() { return reconfigMs; }

}  // namespace Camera
