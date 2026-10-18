#pragma once

#include <Arduino.h>
#include <esp_camera.h>
#include <memory>

// One captured JPEG frame, copied out of the driver's frame buffer into PSRAM so that
// any number of readers (/jpg, /stream, HomeKit snapshots, motion) can hold it.
struct Frame {
  uint8_t *buf = nullptr;
  size_t len = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t seq = 0;
  int64_t timestampUs = 0;
  ~Frame();
};

using FramePtr = std::shared_ptr<Frame>;

namespace Camera {

bool begin();
FramePtr latest();
FramePtr waitNext(uint32_t lastSeq, uint32_t timeoutMs);  // next frame newer than lastSeq, or nullptr
bool setXclk(int mhz);
int xclkMhz();
bool setFrameSize(framesize_t size);  // runtime JPEG resolution change (e.g. smaller while H.264 streaming)
framesize_t frameSize();
float captureFps();
uint32_t captureErrors();
uint32_t lastReconfigMs();  // millis() of last XCLK change (motion detector settles after it)

}  // namespace Camera
