#pragma once

#include <Arduino.h>

#include "camera.h"

struct H264E_persist_tag;
struct H264E_scratch_tag;

namespace H264 {

uint8_t *allocI420(int w, int h);  // 16-byte aligned PSRAM buffer for a w x h I420 frame
void releaseDecodeBuffers();       // frees jpegToI420's RGB buffer (call when a session ends)

// Decodes a JPEG frame at 1/2^scaleShift size into a planar I420 buffer of w x h (w, h even).
// Timings in microseconds are returned through decodeUs / convertUs when non-null.
bool jpegToI420(const Frame &f, int scaleShift, uint8_t *i420, int w, int h, uint32_t *decodeUs = nullptr,
                uint32_t *convertUs = nullptr);

class Encoder {
 public:
  ~Encoder() { end(); }
  bool begin(int width, int height, int gop, int frameBytes, int speed = 10);
  void end();
  void setFrameBytes(int frameBytes) { frameBytes_ = frameBytes; }
  // Encodes one I420 frame; returns Annex-B bytes in *out (owned by the encoder) or -1 on error.
  int encode(uint8_t *i420, bool forceKeyFrame, const uint8_t **out);
  int width() const { return width_; }
  int height() const { return height_; }

 private:
  H264E_persist_tag *enc_ = nullptr;
  H264E_scratch_tag *scratch_ = nullptr;
  int width_ = 0, height_ = 0, gop_ = 1, frameBytes_ = 0, speed_ = 10;
};

// Rewrites SPS profile/constraint/level bytes in an Annex-B buffer to Constrained Baseline level 3.1
// (minih264e writes Baseline level 1.0; HomeKit negotiates profile 0 = constrained baseline, level 0 = 3.1).
void patchSps(uint8_t *annexB, size_t len);

// Serial "@b": measures JPEG decode, RGB->I420 and H.264 encode times at 160x120 and 320x240.
void runBenchmark();

}  // namespace H264
