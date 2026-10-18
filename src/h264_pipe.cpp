// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Simon Bøgh

#include "h264_pipe.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <jpeg_decoder.h>

#include "minih264e.h"

namespace H264 {

static constexpr size_t JPEG_WORK_SIZE = 4096;  // ROM tjpgd (CONFIG_JD_USE_ROM) needs ~3.1 kB
static constexpr size_t ALIGN = 16;              // minih264e asserts 8-byte aligned planes/buffers

static void *psramAlloc(size_t n) { return heap_caps_aligned_alloc(ALIGN, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

// Allocates in internal RAM when there is comfortable headroom (much faster than PSRAM), else PSRAM.
static void *fastAlloc(size_t n, bool *internal) {
  const size_t headroom = 48 * 1024;
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) > n + headroom) {
    void *p = heap_caps_aligned_alloc(ALIGN, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) {
      if (internal) *internal = true;
      return p;
    }
  }
  if (internal) *internal = false;
  return psramAlloc(n);
}

uint8_t *allocI420(int w, int h) { return (uint8_t *)psramAlloc((size_t)w * h * 3 / 2); }

static uint8_t *rgb = nullptr;  // decode buffers, grown on demand (single user: stream decoder or benchmark)
static size_t rgbSize = 0;
static uint8_t *work = nullptr;

void releaseDecodeBuffers() {
  heap_caps_free(rgb);
  rgb = nullptr;
  rgbSize = 0;
}

bool jpegToI420(const Frame &f, int scaleShift, uint8_t *i420, int w, int h, uint32_t *decodeUs,
                uint32_t *convertUs) {

  const int dw = f.width >> scaleShift, dh = f.height >> scaleShift;
  const size_t need = (size_t)dw * dh * 3;
  if (need > rgbSize) {
    heap_caps_free(rgb);
    rgb = (uint8_t *)psramAlloc(need);
    rgbSize = rgb ? need : 0;
  }
  if (!work) work = (uint8_t *)heap_caps_malloc(JPEG_WORK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!rgb || !work) return false;

  const int64_t t0 = esp_timer_get_time();
  esp_jpeg_image_cfg_t cfg = {};
  cfg.indata = f.buf;
  cfg.indata_size = f.len;
  cfg.outbuf = rgb;
  cfg.outbuf_size = rgbSize;
  cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;
  cfg.out_scale = (esp_jpeg_image_scale_t)scaleShift;  // 0..3 == SCALE_0 .. SCALE_1_8
  cfg.advanced.working_buffer = work;
  cfg.advanced.working_buffer_size = JPEG_WORK_SIZE;
  esp_jpeg_image_output_t out = {};
  if (esp_jpeg_decode(&cfg, &out) != ESP_OK) return false;
  const int64_t t1 = esp_timer_get_time();

  // RGB888 -> I420 (BT.601 limited range), cropping/clamping to w x h; 2x2 chroma average.
  uint8_t *Y = i420, *U = i420 + w * h, *V = U + (w / 2) * (h / 2);
  for (int y = 0; y < h; y += 2) {
    const int sy0 = y < out.height ? y : out.height - 1;
    const int sy1 = y + 1 < out.height ? y + 1 : out.height - 1;
    const uint8_t *r0 = rgb + (size_t)sy0 * out.width * 3;
    const uint8_t *r1 = rgb + (size_t)sy1 * out.width * 3;
    uint8_t *y0 = Y + y * w, *y1 = y0 + w;
    uint8_t *u = U + (y / 2) * (w / 2), *v = V + (y / 2) * (w / 2);
    for (int x = 0; x < w; x += 2) {
      const int sx0 = (x < out.width ? x : out.width - 1) * 3;
      const int sx1 = (x + 1 < out.width ? x + 1 : out.width - 1) * 3;
      const uint8_t *p[4] = {r0 + sx0, r0 + sx1, r1 + sx0, r1 + sx1};
      int rs = 0, gs = 0, bs = 0;
      for (int i = 0; i < 4; i++) {
        const int r = p[i][0], g = p[i][1], b = p[i][2];
        const uint8_t yy = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        if (i == 0) y0[x] = yy;
        else if (i == 1) y0[x + 1] = yy;
        else if (i == 2) y1[x] = yy;
        else y1[x + 1] = yy;
        rs += r;
        gs += g;
        bs += b;
      }
      rs >>= 2;
      gs >>= 2;
      bs >>= 2;
      u[x / 2] = (uint8_t)(((-38 * rs - 74 * gs + 112 * bs + 128) >> 8) + 128);
      v[x / 2] = (uint8_t)(((112 * rs - 94 * gs - 18 * bs + 128) >> 8) + 128);
    }
  }
  const int64_t t2 = esp_timer_get_time();
  if (decodeUs) *decodeUs = (uint32_t)(t1 - t0);
  if (convertUs) *convertUs = (uint32_t)(t2 - t1);
  return true;
}

bool Encoder::begin(int width, int height, int gop, int frameBytes, int speed) {
  end();
  H264E_create_param_t cp = {};
  cp.width = width;
  cp.height = height;
  cp.gop = gop;
  // Sizes that are not multiples of 16 need const_input (an extra internal frame copy); otherwise let the
  // encoder use the input as scratch, which saves a frame of PSRAM (matters at 640x480).
  cp.const_input_flag = ((width | height) & 15) ? 1 : 0;
  cp.fine_rate_control_flag = 0;
  cp.vbv_size_bytes = 0;
  int persistSize = 0, scratchSize = 0;
  if (H264E_sizeof(&cp, &persistSize, &scratchSize) != H264E_STATUS_SUCCESS) return false;

  bool scratchInternal = false;
  enc_ = (H264E_persist_t *)psramAlloc(persistSize);
  scratch_ = (H264E_scratch_t *)fastAlloc(scratchSize, &scratchInternal);
  if (!enc_ || !scratch_ || H264E_init(enc_, &cp) != H264E_STATUS_SUCCESS) {
    end();
    return false;
  }
  width_ = width;
  height_ = height;
  gop_ = gop;
  frameBytes_ = frameBytes;
  speed_ = speed;
  Serial.printf("[h264] encoder %dx%d gop=%d: persist %d B (PSRAM), scratch %d B (%s)\n", width, height, gop,
                persistSize, scratchSize, scratchInternal ? "internal" : "PSRAM");
  return true;
}

void Encoder::end() {
  heap_caps_free(enc_);
  heap_caps_free(scratch_);
  enc_ = nullptr;
  scratch_ = nullptr;
}

int Encoder::encode(uint8_t *i420, bool forceKeyFrame, const uint8_t **out) {
  if (!enc_) return -1;
  H264E_run_param_t rp = {};
  rp.encode_speed = speed_;
  rp.frame_type = forceKeyFrame ? H264E_FRAME_TYPE_KEY : H264E_FRAME_TYPE_DEFAULT;
  // minih264e's rate control gives intra frames of an intra-only stream (gop 1) a budget of desired/4
  // (nominal P size is 0 there), so scale the request to land on the target size.
  rp.desired_frame_bytes = gop_ == 1 ? frameBytes_ * 4 : frameBytes_;
  rp.qp_min = 10;
  rp.qp_max = 51;

  H264E_io_yuv_t yuv;
  yuv.yuv[0] = i420;
  yuv.yuv[1] = i420 + width_ * height_;
  yuv.yuv[2] = yuv.yuv[1] + (width_ / 2) * (height_ / 2);
  yuv.stride[0] = width_;
  yuv.stride[1] = yuv.stride[2] = width_ / 2;

  unsigned char *coded = nullptr;
  int size = 0;
  if (H264E_encode(enc_, scratch_, &rp, &yuv, &coded, &size) != H264E_STATUS_SUCCESS) return -1;
  *out = coded;
  return size;
}

void patchSps(uint8_t *d, size_t len) {
  for (size_t i = 0; i + 7 < len; i++) {
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] & 0x1f) == 7) {
      d[i + 4] = 66;    // profile_idc: baseline
      d[i + 5] = 0xe0;  // constraint_set0/1/2: constrained baseline
      d[i + 6] = 31;    // level_idc 3.1
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Benchmark

struct BenchCase {
  framesize_t src;
  int w, h, scaleShift, gop, frameBytes;
};

static const char *srcName(framesize_t fs) {
  switch (fs) {
    case FRAMESIZE_VGA: return "VGA";
    case FRAMESIZE_QVGA: return "QVGA";
    case FRAMESIZE_QQVGA: return "QQVGA";
    default: return "?";
  }
}

static void benchTask(void *) {
  static const BenchCase cases[] = {
      {FRAMESIZE_VGA, 160, 120, 2, 30, 2500},   {FRAMESIZE_QVGA, 160, 120, 1, 30, 2500},
      {FRAMESIZE_QQVGA, 160, 120, 0, 30, 2500}, {FRAMESIZE_QQVGA, 160, 120, 0, 1, 4000},
      {FRAMESIZE_QVGA, 320, 240, 0, 30, 6000},  {FRAMESIZE_VGA, 320, 240, 1, 30, 6000},
  };
  constexpr int FRAMES = 20;
  const framesize_t original = Camera::frameSize();

  Serial.println("\n[bench] camera JPEG -> I420 -> H.264 (minih264e plain C, speed 10, VBR target)");
  for (const auto &c : cases) {
    Camera::setFrameSize(c.src);
    Encoder enc;
    uint8_t *i420 = allocI420(c.w, c.h);
    if (!i420 || !enc.begin(c.w, c.h, c.gop, c.frameBytes)) {
      Serial.println("[bench] allocation failed");
      heap_caps_free(i420);
      continue;
    }
    uint64_t decSum = 0, convSum = 0, encSum = 0, bytesSum = 0, keyBytes = 0;
    int n = 0;
    uint32_t lastSeq = 0;
    const uint32_t t0 = millis();
    while (n < FRAMES && millis() - t0 < 60000) {
      FramePtr f = Camera::waitNext(lastSeq, 1000);
      if (!f) continue;
      lastSeq = f->seq;
      if ((f->width >> c.scaleShift) < c.w) continue;  // frame from before the size switch
      uint32_t dUs = 0, cUs = 0;
      if (!jpegToI420(*f, c.scaleShift, i420, c.w, c.h, &dUs, &cUs)) continue;
      const uint8_t *out = nullptr;
      const int64_t e0 = esp_timer_get_time();
      const int bytes = enc.encode(i420, n == 0, &out);
      const int64_t e1 = esp_timer_get_time();
      if (bytes < 0) {
        Serial.println("[bench] encode failed");
        break;
      }
      if (n == 0) keyBytes = bytes;
      decSum += dUs;
      convSum += cUs;
      encSum += (e1 - e0);
      bytesSum += bytes;
      n++;
    }
    if (n) {
      const float dec = decSum / 1000.0f / n, conv = convSum / 1000.0f / n, encMs = encSum / 1000.0f / n;
      Serial.printf("[bench] %-5s->%3dx%-3d gop=%-2d decode %6.1f  convert %5.1f  encode %5.1f ms"
                    " => serial %4.1f fps, pipelined %4.1f fps | %4u B/frame, key %4u B, cam %4.1f fps\n",
                    srcName(c.src), c.w, c.h, c.gop, dec, conv, encMs, 1000.0f / (dec + conv + encMs),
                    1000.0f / max(dec + conv, encMs), (unsigned)(bytesSum / n), (unsigned)keyBytes,
                    Camera::captureFps());
    }
    enc.end();
    heap_caps_free(i420);
  }
  Camera::setFrameSize(original);
  Serial.printf("[bench] done. free heap %u, min %u\n\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
  vTaskDelete(nullptr);
}

void runBenchmark() {
  // Same core as the future stream task; the camera driver task and Wi-Fi live on core 0.
  xTaskCreatePinnedToCore(benchTask, "h264bench", 16384, nullptr, 1, nullptr, 1);
}

}  // namespace H264
