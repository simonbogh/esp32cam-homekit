// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Simon Bøgh

#pragma once

#include <Arduino.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>

// SRTP / SRTCP with AES_CM_128_HMAC_SHA1_80 (RFC 3711), the HomeKit default crypto suite.
class SrtpContext {
 public:
  ~SrtpContext();
  bool begin(const uint8_t masterKey[16], const uint8_t masterSalt[14]);
  // Encrypts an RTP packet in place and appends the 10-byte auth tag. buf must have len + 10 bytes.
  size_t protectRtp(uint8_t *buf, size_t len);
  // Encrypts an RTCP packet in place and appends E|index (4 bytes) + auth tag (10 bytes).
  size_t protectRtcp(uint8_t *buf, size_t len);

 private:
  struct Keys {
    uint8_t salt[14];
    mbedtls_aes_context aes;
    mbedtls_md_context_t hmac;
  };
  Keys rtp_{}, rtcp_{};
  bool ready_ = false;
  uint32_t roc_ = 0;
  uint16_t lastSeq_ = 0;
  bool haveSeq_ = false;
  uint32_t rtcpIndex_ = 0;

  bool initKeys(Keys &k, const uint8_t masterKey[16], const uint8_t masterSalt[14], uint8_t labelBase);
  void crypt(Keys &k, uint32_t ssrc, uint64_t index, uint8_t *data, size_t len);
  void tag(Keys &k, const uint8_t *data, size_t len, const uint8_t *extra, size_t extraLen, uint8_t out[10]);
};

// RFC 6184 H.264 packetizer (single NAL unit + FU-A), sends via a callback.
class H264Packetizer {
 public:
  using SendFn = void (*)(uint8_t *pkt, size_t len, void *ctx);  // pkt has 14 spare bytes for SRTP
  void begin(uint8_t payloadType, uint32_t ssrc, size_t maxPayload);
  // Packetizes one Annex-B access unit with the given 90 kHz timestamp.
  void sendAccessUnit(const uint8_t *annexB, size_t len, uint32_t timestamp, SendFn send, void *ctx);
  uint32_t packets() const { return packets_; }
  uint32_t octets() const { return octets_; }
  uint32_t ssrc() const { return ssrc_; }
  uint32_t lastTimestamp() const { return lastTs_; }

 private:
  uint8_t pt_ = 99;
  uint32_t ssrc_ = 0;
  size_t maxPayload_ = 1200;
  uint16_t seq_ = 0;
  uint32_t packets_ = 0, octets_ = 0, lastTs_ = 0;
  uint8_t buf_[1500 + 32];

  void sendNal(const uint8_t *nal, size_t len, bool lastNalOfAu, uint32_t ts, SendFn send, void *ctx);
  size_t header(bool marker, uint32_t ts);
};
