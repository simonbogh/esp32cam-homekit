// HomeKit camera accessory on top of HomeSpan.
// HomeSpan has no camera service, so the HAP Camera RTP Stream Management service (UUID 110)
// and its TLV8 characteristics are declared here as custom types with Apple's short UUIDs.
// TLV tags follow HAP-NodeJS src/lib/camera/RTPStreamManagement.ts.

#include "hk_camera.h"

#include <HomeSpan.h>
#include <sodium.h>  // HAP.h needs libsodium constants
#include <HAP.h>
#include <WiFi.h>

#include <atomic>

#include "camera.h"
#include "config.h"
#include "live_stream.h"
#include "motion.h"

CUSTOM_SERV(CameraRTPStreamManagement, 110);
CUSTOM_CHAR_TLV8(SupportedVideoStreamConfiguration, 114, PR);
CUSTOM_CHAR_TLV8(SupportedAudioStreamConfiguration, 115, PR);
CUSTOM_CHAR_TLV8(SupportedRTPConfiguration, 116, PR);
CUSTOM_CHAR_TLV8(SelectedRTPStreamConfiguration, 117, PR + PW);
CUSTOM_CHAR_TLV8(SetupEndpoints, 118, PR + PW);
CUSTOM_CHAR_TLV8(StreamingStatus, 120, PR + EV);

namespace {

std::atomic<bool> cameraActive{true};  // HAP Active on the stream service: Home's camera "Off" mode writes 0

enum StreamingStatusValue : uint8_t { STATUS_AVAILABLE = 0, STATUS_IN_USE = 1, STATUS_UNAVAILABLE = 2 };
enum SetupStatus : uint8_t { SETUP_SUCCESS = 0, SETUP_BUSY = 1, SETUP_ERROR = 2 };

// TLV8::add(tag, uint64_t) uses the fewest bytes; HAP wants fixed 2-byte width/height.
void add16(TLV8 &t, uint8_t tag, uint16_t v) {
  const uint8_t b[2] = {uint8_t(v & 0xff), uint8_t(v >> 8)};
  t.add(tag, 2, b);
}

void add32(TLV8 &t, uint8_t tag, uint32_t v) {
  const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
  t.add(tag, 4, b);
}

// Copies a TLV item's bytes into dst if it has exactly len bytes.
bool getBytes(const TLV8 &t, uint8_t tag, void *dst, size_t len) {
  auto it = t.find(tag);
  if (it == t.end() || it->getLen() != len) return false;
  memcpy(dst, it->get(), len);
  return true;
}

template <class T>
T getNum(const TLV8 &t, uint8_t tag, T def = 0) {
  auto it = t.find(tag);
  return it == t.end() ? def : it->getVal<T>();
}

TLV8 sub(const TLV8 &t, uint8_t tag) {
  TLV8 out;
  auto it = t.find(tag);
  if (it != t.end()) out.unpack(it);
  return out;
}

// Same capabilities as HAP-NodeJS / homebridge-camera-ffmpeg defaults (known to be accepted by Home hubs).
// iOS picks one; we always send our own (smaller) Constrained Baseline stream, which the SPS describes.
TLV8 videoConfig() {
  TLV8 params;
  for (uint8_t profile = 0; profile < 3; profile++) {  // constrained baseline, main, high
    if (profile) params.add(0);                         // list separator
    params.add(1, (uint64_t)profile);
  }
  for (uint8_t level = 0; level < 3; level++) {  // 3.1, 3.2, 4.0
    if (level) params.add(0);
    params.add(2, (uint64_t)level);
  }
  params.add(3, (uint64_t)0);  // packetization: non-interleaved

  TLV8 codec;
  codec.add(1, (uint64_t)0);  // codec: H.264
  codec.add(2, params);
  const uint16_t res[][3] = {{320, 180, 30},  {320, 240, 15},   {320, 240, 30},  {480, 270, 30},
                             {480, 360, 30},  {640, 360, 30},   {640, 480, 30},  {1280, 720, 30},
                             {1280, 960, 30}, {1920, 1080, 30}, {1600, 1200, 30}};
  for (size_t i = 0; i < sizeof(res) / sizeof(res[0]); i++) {
    TLV8 attr;
    add16(attr, 1, res[i][0]);
    add16(attr, 2, res[i][1]);
    attr.add(3, (uint64_t)res[i][2]);
    if (i) codec.add(0);  // list separator
    codec.add(3, attr);
  }

  TLV8 top;
  top.add(1, codec);
  return top;
}

TLV8 audioConfig() {
  // No microphone, but iOS only starts a stream if an audio codec it accepts is offered.
  TLV8 top;
  const uint8_t sampleRates[] = {1, 2};  // 16 kHz, 24 kHz
  for (size_t i = 0; i < sizeof(sampleRates); i++) {
    TLV8 params;
    params.add(1, (uint64_t)1);               // channels
    params.add(2, (uint64_t)0);               // variable bit rate
    params.add(3, (uint64_t)sampleRates[i]);  // sample rate
    TLV8 codec;
    codec.add(1, (uint64_t)3);  // Opus
    codec.add(2, params);
    if (i) top.add(0);
    top.add(1, codec);
  }
  top.add(2, (uint64_t)0);  // comfort noise: no
  return top;
}

TLV8 rtpConfig() {
  TLV8 t;
  t.add(2, (uint64_t)0);  // SRTP crypto suite AES_CM_128_HMAC_SHA1_80
  return t;
}

TLV8 streamingStatus(uint8_t status) {
  TLV8 t;
  t.add(1, (uint64_t)status);
  return t;
}

// TLV tags (HAP-NodeJS RTPStreamManagement.ts)
enum : uint8_t { SE_SESSION_ID = 1, SE_STATUS = 2, SE_ADDRESS = 3, SE_VIDEO_SRTP = 4, SE_AUDIO_SRTP = 5,
                 SE_VIDEO_SSRC = 6, SE_AUDIO_SSRC = 7 };
enum : uint8_t { ADDR_VERSION = 1, ADDR_IP = 2, ADDR_VIDEO_PORT = 3, ADDR_AUDIO_PORT = 4 };
enum : uint8_t { SRTP_SUITE = 1, SRTP_KEY = 2, SRTP_SALT = 3 };
enum : uint8_t { SEL_SESSION_CONTROL = 1, SEL_VIDEO = 2 };
enum : uint8_t { CTRL_SESSION_ID = 1, CTRL_COMMAND = 2 };
enum : uint8_t { CMD_END = 0, CMD_START = 1, CMD_SUSPEND = 2, CMD_RESUME = 3, CMD_RECONFIGURE = 4 };
enum : uint8_t { VID_ATTRIBUTES = 3, VID_RTP = 4 };
enum : uint8_t { RTP_PAYLOAD_TYPE = 1, RTP_SSRC = 2, RTP_MAX_BITRATE = 3, RTP_RTCP_INTERVAL = 4, RTP_MTU = 5 };

struct CamStream : Service::CameraRTPStreamManagement {
  SpanCharacteristic *setup;
  SpanCharacteristic *selected;
  SpanCharacteristic *status;
  SpanCharacteristic *active;

  CamStream() : Service::CameraRTPStreamManagement() {
    new Characteristic::SupportedVideoStreamConfiguration(videoConfig());
    new Characteristic::SupportedAudioStreamConfiguration(audioConfig());
    new Characteristic::SupportedRTPConfiguration(rtpConfig());

    // Initial values as in HAP-NodeJS: SelectedRTPStreamConfiguration = SUSPEND, SetupEndpoints = ERROR.
    TLV8 ctrl, sel0, se0;
    ctrl.add(CTRL_COMMAND, (uint64_t)CMD_SUSPEND);
    sel0.add(SEL_SESSION_CONTROL, ctrl);
    se0.add(SE_STATUS, (uint64_t)SETUP_ERROR);
    selected = new Characteristic::SelectedRTPStreamConfiguration(sel0);
    setup = new Characteristic::SetupEndpoints(se0);
    status = new Characteristic::StreamingStatus(
        streamingStatus(!LIVE_STREAM && DECLINE_MODE == 2 ? STATUS_UNAVAILABLE : STATUS_AVAILABLE));

    // Apple Home switches the camera on/off (Off / Detect Activity / Stream) through this characteristic;
    // without it the Home hub reports the camera as "No Response". Persisted across reboots.
    active = new Characteristic::Active(1, true);
    cameraActive = active->getVal() != 0;
  }

  // iOS writes the controller's address + SRTP keys, then reads back ours.
  void handleSetupEndpoints() {
    TLV8 req;
    setup->getNewTLV(req);
    TLV8 resp;
    uint8_t sessionId[16];
    const bool haveId = getBytes(req, SE_SESSION_ID, sessionId, 16);
    if (haveId) resp.add(SE_SESSION_ID, 16, sessionId);

#if LIVE_STREAM
    if (!cameraActive) {
      resp.add(SE_STATUS, (uint64_t)SETUP_ERROR);
      Serial.println("[hk] SetupEndpoints: camera is switched off in Apple Home");
      setup->setTLV(resp, false);
      return;
    }
    LiveStream::Endpoint ep = {};
    TLV8 addr = sub(req, SE_ADDRESS);
    TLV8 vsrtp = sub(req, SE_VIDEO_SRTP);
    TLV8 asrtp = sub(req, SE_AUDIO_SRTP);
    auto ip = addr.find(ADDR_IP);
    const bool valid = haveId && getNum<uint8_t>(addr, ADDR_VERSION) == 0 && ip != addr.end() &&
                       ip->getLen() < sizeof(ep.controllerIp) && getNum<uint8_t>(vsrtp, SRTP_SUITE) == 0 &&
                       getBytes(vsrtp, SRTP_KEY, ep.videoKey, 16) && getBytes(vsrtp, SRTP_SALT, ep.videoSalt, 14);
    LiveStream::Prepared pr = {};
    if (valid) {
      memcpy(ep.sessionId, sessionId, 16);
      memcpy(ep.controllerIp, ip->get(), ip->getLen());
      ep.controllerVideoPort = getNum<uint16_t>(addr, ADDR_VIDEO_PORT);
      ep.controllerAudioPort = getNum<uint16_t>(addr, ADDR_AUDIO_PORT);
    }
    if (!valid) {
      resp.add(SE_STATUS, (uint64_t)SETUP_ERROR);
      Serial.println("[hk] SetupEndpoints: unsupported request (needs IPv4 + AES_CM_128_HMAC_SHA1_80)");
    } else if (!LiveStream::prepare(ep, &pr)) {
      resp.add(SE_STATUS, (uint64_t)SETUP_BUSY);
      Serial.println("[hk] SetupEndpoints: busy (already streaming)");
    } else {
      resp.add(SE_STATUS, (uint64_t)SETUP_SUCCESS);
      TLV8 ours;
      ours.add(ADDR_VERSION, (uint64_t)0);
      ours.add(ADDR_IP, WiFi.localIP().toString().c_str());
      add16(ours, ADDR_VIDEO_PORT, pr.localVideoPort);
      add16(ours, ADDR_AUDIO_PORT, pr.localAudioPort);
      resp.add(SE_ADDRESS, ours);
      resp.add(SE_VIDEO_SRTP, vsrtp);  // we send with the controller's key/salt, so echo them
      resp.add(SE_AUDIO_SRTP, asrtp);
      add32(resp, SE_VIDEO_SSRC, pr.videoSsrc);
      add32(resp, SE_AUDIO_SSRC, pr.audioSsrc);
      Serial.printf("[hk] SetupEndpoints: controller %s:%u, our video port %u\n", ep.controllerIp,
                    ep.controllerVideoPort, pr.localVideoPort);
    }
#else
    resp.add(SE_STATUS, (uint64_t)SETUP_BUSY);  // live video disabled: decline politely
    Serial.println("[hk] live view requested -> declined (snapshots only)");
#endif
    setup->setTLV(resp, false);
  }

  void handleSelectedConfiguration() {
#if LIVE_STREAM
    TLV8 sel;
    selected->getNewTLV(sel);
    TLV8 ctrl = sub(sel, SEL_SESSION_CONTROL);
    uint8_t sessionId[16];
    if (!getBytes(ctrl, CTRL_SESSION_ID, sessionId, 16)) return;
    const uint8_t cmd = getNum<uint8_t>(ctrl, CTRL_COMMAND, 0xff);

    LiveStream::Params p;
    TLV8 video = sub(sel, SEL_VIDEO);
    TLV8 attr = sub(video, VID_ATTRIBUTES);
    TLV8 rtp = sub(video, VID_RTP);
    p.width = getNum<uint16_t>(attr, 1, p.width);
    p.height = getNum<uint16_t>(attr, 2, p.height);
    p.fps = getNum<uint8_t>(attr, 3, p.fps);
    p.payloadType = getNum<uint8_t>(rtp, RTP_PAYLOAD_TYPE, p.payloadType);
    p.maxBitrateKbps = getNum<uint16_t>(rtp, RTP_MAX_BITRATE, p.maxBitrateKbps);
    p.mtu = getNum<uint16_t>(rtp, RTP_MTU, p.mtu);

    switch (cmd) {
      case CMD_START:
        if (!LiveStream::start(sessionId, p)) Serial.println("[hk] start for unknown/unprepared session");
        break;
      case CMD_RECONFIGURE:
        LiveStream::reconfigure(sessionId, p);
        break;
      case CMD_END:
        Serial.println("[hk] stream end requested");
        LiveStream::stop(sessionId);
        break;
      default:  // suspend/resume: keep streaming, iOS ends the session when it is done
        Serial.printf("[hk] stream command %u ignored\n", cmd);
        break;
    }
#endif
  }

  boolean update() override {
    if (active->updated()) {
      cameraActive = active->getNewVal() != 0;
      Serial.printf("[hk] camera %s by Apple Home\n", cameraActive ? "switched on" : "switched off");
#if LIVE_STREAM
      if (!cameraActive) LiveStream::stopAll();
#endif
    }
    if (setup->updated()) handleSetupEndpoints();
    if (selected->updated()) handleSelectedConfiguration();
    return true;
  }

  void loop() override {
#if LIVE_STREAM
    if (LiveStream::takeStatusChanged())
      status->setTLV(streamingStatus(LiveStream::streaming() ? STATUS_IN_USE : STATUS_AVAILABLE));
#endif
  }
};

struct CamMotion : Service::MotionSensor {
  SpanCharacteristic *motion;

  CamMotion() : Service::MotionSensor() { motion = new Characteristic::MotionDetected(false); }

  void loop() override {
    const bool m = Motion::detected();
    if (motion->getVal<bool>() != m) motion->setVal(m);
  }
};

}  // namespace

// Called by the patched HomeSpan HAP server for POST /resource on a verified (encrypted) session.
// Body is JSON like {"aid":1,"resource-type":"image","image-width":640,"image-height":480}.
int hs_resource_handler(HAPClient *hc, const char *json, size_t len) {
  FramePtr f = cameraActive ? Camera::latest() : nullptr;  // no snapshots while switched off in Home
  hapOut.setHapClient(hc);
  if (!f) {
    hapOut << "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
    hapOut.flush();
    return -1;
  }
  const uint32_t t0 = millis();
  hapOut << "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: " << f->len << "\r\n\r\n";
  hapOut.write((const char *)f->buf, f->len);
  hapOut.flush();
  Serial.printf("[hk] snapshot %ux%u %u bytes sent in %lu ms (req: %.*s)\n", f->width, f->height, (unsigned)f->len,
                (unsigned long)(millis() - t0), (int)len, json);
  return 0;
}

namespace HomeKitCamera {

void begin() {
  char serial[16];
  snprintf(serial, sizeof(serial), "%012llX", ESP.getEfuseMac());

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name(HK_DISPLAY_NAME);
  new Characteristic::Manufacturer("AI-Thinker");
  new Characteristic::Model("ESP32-CAM OV2640");
  new Characteristic::SerialNumber(serial);
  new Characteristic::FirmwareRevision(FW_VERSION);
  // Layout of known-working HomeKit cameras (HAP-NodeJS, esp32-homekit-camera): primary stream
  // management service plus a Microphone service (required by the HAP IP-camera profile; we have no mic).
  (new CamStream())->setPrimary();
  new Service::Microphone();
  new Characteristic::Mute(1);
  new Characteristic::Volume(0);
  new CamMotion();
}

}  // namespace HomeKitCamera
