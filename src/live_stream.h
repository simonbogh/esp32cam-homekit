#pragma once

#include <Arduino.h>

// One HomeKit live-video session: H.264 (minih264e) -> RTP -> SRTP over UDP to the controller.
namespace LiveStream {

struct Endpoint {
  uint8_t sessionId[16];
  char controllerIp[40];
  uint16_t controllerVideoPort;
  uint16_t controllerAudioPort;
  uint8_t videoKey[16];
  uint8_t videoSalt[14];
};

struct Prepared {
  uint16_t localVideoPort;
  uint16_t localAudioPort;
  uint32_t videoSsrc;
  uint32_t audioSsrc;
};

struct Params {
  uint8_t payloadType = 99;
  uint16_t width = 320, height = 240;  // what the controller asked for (we encode the active profile's size)
  uint8_t fps = 15;
  uint16_t maxBitrateKbps = 300;
  uint16_t mtu = 1378;
};

bool prepare(const Endpoint &ep, Prepared *out);  // false = busy (another session is streaming)
bool start(const uint8_t sessionId[16], const Params &p);
void reconfigure(const uint8_t sessionId[16], const Params &p);
void stop(const uint8_t sessionId[16]);
void stopAll();  // e.g. camera switched off in Apple Home
bool streaming();
bool setProfile(int index);  // 0..4 = 160x120, 240x176, 320x240, 480x320, 640x480 (next stream start)
bool takeStatusChanged();  // true once after the streaming state changed (for StreamingStatus events)
size_t statsJson(char *buf, size_t size);

}  // namespace LiveStream
