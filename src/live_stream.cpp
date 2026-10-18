// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Simon Bøgh

#include "live_stream.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>

#include <atomic>

#include "camera.h"
#include "config.h"
#include "h264_pipe.h"
#include "rtp_srtp.h"

namespace LiveStream {

enum class State { IDLE, PREPARED, STREAMING };

static State state = State::IDLE;
static Endpoint endpoint;
static Prepared prepared;
static Params params;
static int videoSock = -1, audioSock = -1;
static std::atomic<bool> stopRequested{false};
static std::atomic<bool> statusChanged{false};
static std::atomic<uint16_t> bitrateKbps{0};

// Stream profiles: camera JPEG size while streaming -> H.264 size (decode at 1:1, cost scales with pixels).
struct Profile {
  framesize_t source;
  uint16_t width, height;
  uint8_t maxFps;
  const char *name;
};
static const Profile PROFILES[] = {
    {FRAMESIZE_QQVGA, 160, 120, 15, "160x120"},
    {FRAMESIZE_HQVGA, 240, 176, 10, "240x176"},
    {FRAMESIZE_QVGA, 320, 240, 8, "320x240"},
    {FRAMESIZE_HVGA, 480, 320, 4, "480x320"},
    {FRAMESIZE_VGA, 640, 480, 2, "640x480"},
};
static constexpr int NUM_PROFILES = sizeof(PROFILES) / sizeof(PROFILES[0]);
static std::atomic<int> activeProfile{-1};  // loaded from NVS on first use, default STREAM_PROFILE

static int profileIndex() {
  if (activeProfile < 0) {
    Preferences prefs;
    prefs.begin("livestream", false);  // read-write: creates the namespace on first boot (no NOT_FOUND log)
    const int saved = prefs.getInt("profile", STREAM_PROFILE);
    prefs.end();
    activeProfile = saved >= 0 && saved < NUM_PROFILES ? saved : STREAM_PROFILE;
  }
  return activeProfile;
}

// stats (written by the stream task)
static volatile float statFps = 0, statKbps = 0, statEncodeMs = 0;
static volatile uint32_t statFrames = 0, statPackets = 0, statRxAgeMs = 0;

static SemaphoreHandle_t lock() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();
  return m;
}

static int openUdp(uint16_t *port) {
  const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) return -1;
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = 0;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  socklen_t alen = sizeof(a);
  if (bind(s, (sockaddr *)&a, sizeof(a)) != 0 || getsockname(s, (sockaddr *)&a, &alen) != 0) {
    close(s);
    return -1;
  }
  *port = ntohs(a.sin_port);
  return s;
}

static void closeSockets() {
  if (videoSock >= 0) close(videoSock);
  if (audioSock >= 0) close(audioSock);
  videoSock = audioSock = -1;
}

// Returns true if any datagram from the controller was waiting (RTCP receiver reports keep the session alive).
static bool drain(int sock) {
  if (sock < 0) return false;
  uint8_t buf[256];
  bool got = false;
  while (recv(sock, buf, sizeof(buf), MSG_DONTWAIT) > 0) got = true;
  return got;
}

struct SendCtx {
  SrtpContext *srtp;
  int sock;
  sockaddr_in dst;
  uint32_t bytes;
  uint32_t errors;
};

static void sendPacket(uint8_t *pkt, size_t len, void *ctx) {
  SendCtx *c = (SendCtx *)ctx;
  const size_t n = c->srtp->protectRtp(pkt, len);
  for (int attempt = 0; attempt < 5; attempt++) {
    if (sendto(c->sock, pkt, n, 0, (sockaddr *)&c->dst, sizeof(c->dst)) == (int)n) {
      c->bytes += n;
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(2));  // lwIP out of buffers: back off briefly
  }
  c->errors++;
}

static void sendSenderReport(SendCtx &c, const H264Packetizer &pk) {
  uint8_t pkt[28 + 14];
  const int64_t us = esp_timer_get_time();
  const uint32_t ntpSec = (uint32_t)(us / 1000000) + 2208988800u;  // uptime-based NTP, only relative use
  const uint32_t ntpFrac = (uint32_t)(((us % 1000000) << 32) / 1000000);
  const uint32_t rtpTs = (uint32_t)(us * 9 / 100);
  const uint32_t words[] = {pk.ssrc(), ntpSec, ntpFrac, rtpTs, pk.packets(), pk.octets()};
  pkt[0] = 0x80;
  pkt[1] = 200;  // SR
  pkt[2] = 0;
  pkt[3] = 6;  // length in 32-bit words minus one
  for (int i = 0; i < 6; i++) {
    pkt[4 + 4 * i] = words[i] >> 24;
    pkt[5 + 4 * i] = words[i] >> 16;
    pkt[6 + 4 * i] = words[i] >> 8;
    pkt[7 + 4 * i] = words[i];
  }
  const size_t n = c.srtp->protectRtcp(pkt, 28);
  sendto(c.sock, pkt, n, 0, (sockaddr *)&c.dst, sizeof(c.dst));
}

// Two-stage pipeline: decodeTask (core 0) turns camera JPEGs into I420 buffers, streamTask (core 1)
// encodes, packetizes and sends them. Buffers circulate through freeQ -> fullQ -> freeQ.
struct Slot {
  uint8_t *i420;
  uint32_t ts90;  // 90 kHz RTP timestamp of the capture
};

struct Pipeline {
  Profile profile;
  int fps;
  QueueHandle_t freeQ, fullQ;
  std::atomic<bool> decoderDone{false};
};

static void decodeTask(void *arg) {
  Pipeline *pl = (Pipeline *)arg;
  const uint32_t intervalUs = 1000000 / pl->fps;
  int64_t nextUs = esp_timer_get_time();
  uint32_t lastSeq = 0;
  while (!stopRequested) {
    const int64_t now = esp_timer_get_time();
    if (now < nextUs) {
      vTaskDelay(pdMS_TO_TICKS((nextUs - now) / 1000 + 1));
      continue;
    }
    nextUs += intervalUs;
    if (now - nextUs > 2 * (int64_t)intervalUs) nextUs = now + intervalUs;  // fell behind: don't burst

    Slot slot;
    if (xQueueReceive(pl->freeQ, &slot, pdMS_TO_TICKS(200)) != pdTRUE) continue;  // encoder busy: drop frame
    FramePtr f = Camera::waitNext(lastSeq, 500);
    if (f) lastSeq = f->seq;
    const bool ok = f && f->width == pl->profile.width &&  // else: stale frame from before the resolution switch
                    H264::jpegToI420(*f, 0, slot.i420, pl->profile.width, pl->profile.height);
    if (ok) {
      slot.ts90 = (uint32_t)(f->timestampUs * 9 / 100);
      xQueueSend(pl->fullQ, &slot, 0);
    } else {
      xQueueSend(pl->freeQ, &slot, 0);
    }
    // When decoding is slower than the frame interval nothing above blocks (at 640x480 one decode is
    // ~600 ms of CPU); always yield so IDLE0 (task watchdog) and Wi-Fi/lwIP on core 0 get CPU time.
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  pl->decoderDone = true;
  vTaskDelete(nullptr);
}

static void streamTask(void *) {
  xSemaphoreTake(lock(), portMAX_DELAY);
  const Params p = params;
  const Endpoint ep = endpoint;
  const Prepared pr = prepared;
  const int vsock = videoSock, asock = audioSock;
  xSemaphoreGive(lock());

  Pipeline pl;
  pl.profile = PROFILES[profileIndex()];
  pl.fps = p.fps && p.fps < pl.profile.maxFps ? p.fps : pl.profile.maxFps;
  const int w = pl.profile.width, h = pl.profile.height;
  bitrateKbps = p.maxBitrateKbps ? p.maxBitrateKbps : 300;

  Serial.printf("[live] start: controller %s:%u, asked %ux%u@%u %u kbps PT %u MTU %u -> sending %dx%d@%d\n",
                ep.controllerIp, ep.controllerVideoPort, p.width, p.height, p.fps, p.maxBitrateKbps, p.payloadType,
                p.mtu, w, h, pl.fps);

  const framesize_t previousSize = Camera::frameSize();
  Camera::setFrameSize(pl.profile.source);

  SrtpContext *srtp = new SrtpContext();
  H264Packetizer *pk = new H264Packetizer();
  H264::Encoder *enc = new H264::Encoder();
  pl.freeQ = xQueueCreate(2, sizeof(Slot));
  pl.fullQ = xQueueCreate(2, sizeof(Slot));
  bool ok = pl.freeQ && pl.fullQ && srtp->begin(ep.videoKey, ep.videoSalt) &&
            enc->begin(w, h, STREAM_GOP, bitrateKbps * 1000 / 8 / pl.fps);
  for (int i = 0; ok && i < 2; i++) {
    Slot slot = {H264::allocI420(w, h), 0};
    ok = slot.i420 != nullptr;
    if (ok) xQueueSend(pl.freeQ, &slot, 0);
  }

  SendCtx ctx = {srtp, vsock, {}, 0, 0};
  ctx.dst.sin_family = AF_INET;
  ctx.dst.sin_port = htons(ep.controllerVideoPort);
  ok = ok && inet_pton(AF_INET, ep.controllerIp, &ctx.dst.sin_addr) == 1;
  // Leave room for RTP header (12), FU-A header (2), SRTP tag (10) inside the controller's MTU.
  pk->begin(p.payloadType, pr.videoSsrc, (p.mtu ? p.mtu : 1378) - 24);

  ok = ok && xTaskCreatePinnedToCore(decodeTask, "livedecode", 8192, &pl, 1, nullptr, 0) == pdPASS;
  if (!ok) {
    Serial.println("[live] setup failed (memory / address / SRTP)");
    pl.decoderDone = true;
  }

  uint32_t frameIndex = 0, windowFrames = 0, windowBytes = 0;
  int64_t lastRxUs = esp_timer_get_time(), lastSrUs = 0, windowStartUs = esp_timer_get_time();
  uint32_t lastLogMs = millis();

  while (ok && !stopRequested) {
    if (drain(vsock) | drain(asock)) lastRxUs = esp_timer_get_time();
    statRxAgeMs = (esp_timer_get_time() - lastRxUs) / 1000;
    if (statRxAgeMs > STREAM_TIMEOUT_MS) {
      Serial.println("[live] no packets from controller, ending stream");
      break;
    }

    Slot slot;
    if (xQueueReceive(pl.fullQ, &slot, pdMS_TO_TICKS(100)) != pdTRUE) continue;

    enc->setFrameBytes(bitrateKbps * 1000 / 8 / pl.fps);
    const bool key = STREAM_GOP == 1 || frameIndex % (pl.fps * STREAM_KEY_INTERVAL_S) == 0;
    const uint8_t *bits = nullptr;
    const int64_t e0 = esp_timer_get_time();
    const int n = enc->encode(slot.i420, key, &bits);
    statEncodeMs = (esp_timer_get_time() - e0) / 1000.0f;
    xQueueSend(pl.freeQ, &slot, 0);
    if (n <= 0) continue;

    H264::patchSps((uint8_t *)bits, n);  // encoder-owned scratch; only SPS header bytes change
    const uint32_t bytesBefore = ctx.bytes;
    pk->sendAccessUnit(bits, n, slot.ts90, sendPacket, &ctx);
    frameIndex++;
    windowFrames++;
    windowBytes += ctx.bytes - bytesBefore;

    if (esp_timer_get_time() - lastSrUs > 1000000) {
      sendSenderReport(ctx, *pk);
      lastSrUs = esp_timer_get_time();
    }

    const int64_t win = esp_timer_get_time() - windowStartUs;
    if (win >= 2000000) {
      statFps = windowFrames * 1e6f / win;
      statKbps = windowBytes * 8e3f / win;
      windowFrames = windowBytes = 0;
      windowStartUs = esp_timer_get_time();
    }
    statFrames = frameIndex;
    statPackets = pk->packets();
    if (millis() - lastLogMs >= 5000) {
      lastLogMs = millis();
      Serial.printf("[live] %u frames, %.1f fps, %.0f kbit/s, encode %.0f ms, %u pkts, send errors %u, rx %u ms ago\n",
                    (unsigned)frameIndex, statFps, statKbps, statEncodeMs, (unsigned)pk->packets(),
                    (unsigned)ctx.errors, (unsigned)statRxAgeMs);
    }
  }

  stopRequested = true;  // also stops the decoder after a timeout/error exit
  while (!pl.decoderDone) vTaskDelay(pdMS_TO_TICKS(10));
  Slot slot;
  while (pl.freeQ && xQueueReceive(pl.freeQ, &slot, 0) == pdTRUE) heap_caps_free(slot.i420);
  while (pl.fullQ && xQueueReceive(pl.fullQ, &slot, 0) == pdTRUE) heap_caps_free(slot.i420);
  if (pl.freeQ) vQueueDelete(pl.freeQ);
  if (pl.fullQ) vQueueDelete(pl.fullQ);
  delete enc;
  delete pk;
  delete srtp;
  H264::releaseDecodeBuffers();  // up to ~1 MB of PSRAM at 640x480
  Camera::setFrameSize(previousSize);

  xSemaphoreTake(lock(), portMAX_DELAY);
  closeSockets();
  state = State::IDLE;
  xSemaphoreGive(lock());
  statFps = 0;
  statKbps = 0;
  statusChanged = true;
  Serial.printf("[live] stopped after %u frames\n", (unsigned)frameIndex);
  vTaskDelete(nullptr);
}

bool setProfile(int index) {
  if (index < 0 || index >= NUM_PROFILES) return false;
  activeProfile = index;
  Preferences prefs;  // remembered across reboots
  prefs.begin("livestream", false);
  prefs.putInt("profile", index);
  prefs.end();
  Serial.printf("[live] profile %d: %s (saved; applies at the next live view)\n", index, PROFILES[index].name);
  return true;
}

bool prepare(const Endpoint &ep, Prepared *out) {
  // A stream that is shutting down gets a moment to finish before we call ourselves busy.
  for (int i = 0; i < 20 && state == State::STREAMING && stopRequested; i++) vTaskDelay(pdMS_TO_TICKS(50));

  xSemaphoreTake(lock(), portMAX_DELAY);
  if (state == State::STREAMING) {
    xSemaphoreGive(lock());
    return false;
  }
  closeSockets();  // replace a prepared-but-never-started session
  Prepared pr = {};
  videoSock = openUdp(&pr.localVideoPort);
  audioSock = openUdp(&pr.localAudioPort);
  if (videoSock < 0 || audioSock < 0) {
    closeSockets();
    state = State::IDLE;
    xSemaphoreGive(lock());
    return false;
  }
  pr.videoSsrc = esp_random();
  pr.audioSsrc = esp_random();
  endpoint = ep;
  prepared = pr;
  state = State::PREPARED;
  xSemaphoreGive(lock());
  *out = pr;
  return true;
}

bool start(const uint8_t sessionId[16], const Params &p) {
  xSemaphoreTake(lock(), portMAX_DELAY);
  if (state != State::PREPARED || memcmp(sessionId, endpoint.sessionId, 16) != 0) {
    xSemaphoreGive(lock());
    return false;
  }
  params = p;
  stopRequested = false;
  state = State::STREAMING;
  xSemaphoreGive(lock());

  if (xTaskCreatePinnedToCore(streamTask, "livestream", 16384, nullptr, 2, nullptr, 1) != pdPASS) {
    xSemaphoreTake(lock(), portMAX_DELAY);
    closeSockets();
    state = State::IDLE;
    xSemaphoreGive(lock());
    return false;
  }
  statusChanged = true;
  return true;
}

void reconfigure(const uint8_t sessionId[16], const Params &p) {
  if (state == State::STREAMING && memcmp(sessionId, endpoint.sessionId, 16) == 0 && p.maxBitrateKbps) {
    bitrateKbps = p.maxBitrateKbps;
    Serial.printf("[live] reconfigure: %u kbps\n", p.maxBitrateKbps);
  }
}

void stop(const uint8_t sessionId[16]) {
  xSemaphoreTake(lock(), portMAX_DELAY);
  if (memcmp(sessionId, endpoint.sessionId, 16) == 0) {
    if (state == State::STREAMING) {
      stopRequested = true;  // the task cleans up and reports the state change
    } else if (state == State::PREPARED) {
      closeSockets();
      state = State::IDLE;
    }
  }
  xSemaphoreGive(lock());
}

void stopAll() {
  xSemaphoreTake(lock(), portMAX_DELAY);
  if (state == State::STREAMING) {
    stopRequested = true;
  } else if (state == State::PREPARED) {
    closeSockets();
    state = State::IDLE;
  }
  xSemaphoreGive(lock());
}

bool streaming() { return state == State::STREAMING; }

bool takeStatusChanged() { return statusChanged.exchange(false); }

size_t statsJson(char *buf, size_t size) {
  return snprintf(buf, size,
                  "{\"active\":%s,\"profile\":\"%s\",\"fps\":%.1f,\"kbps\":%.0f,\"encode_ms\":%.0f,\"frames\":%u}",
                  streaming() ? "true" : "false", PROFILES[profileIndex()].name, statFps, statKbps, statEncodeMs,
                  (unsigned)statFrames);
}

}  // namespace LiveStream
