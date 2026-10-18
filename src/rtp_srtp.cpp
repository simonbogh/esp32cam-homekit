// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Simon Bøgh

#include "rtp_srtp.h"

// ---------------------------------------------------------------------------------------------
// SRTP (RFC 3711), AES_CM_128_HMAC_SHA1_80

static constexpr size_t AUTH_TAG_LEN = 10;

SrtpContext::~SrtpContext() {
  if (!ready_) return;
  for (Keys *k : {&rtp_, &rtcp_}) {
    mbedtls_aes_free(&k->aes);
    mbedtls_md_free(&k->hmac);
  }
}

// AES-CM PRF of RFC 3711 4.3.1 (key derivation rate 0): keystream of AES(master, (salt ^ label<<48) * 2^16).
static void deriveKey(const uint8_t masterKey[16], const uint8_t masterSalt[14], uint8_t label, uint8_t *out,
                      size_t len) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, masterKey, 128);
  uint8_t iv[16] = {0};
  memcpy(iv, masterSalt, 14);
  iv[7] ^= label;
  uint8_t stream[16];
  size_t off = 0;
  memset(out, 0, len);
  mbedtls_aes_crypt_ctr(&aes, len, &off, iv, stream, out, out);
  mbedtls_aes_free(&aes);
}

bool SrtpContext::initKeys(Keys &k, const uint8_t masterKey[16], const uint8_t masterSalt[14], uint8_t labelBase) {
  uint8_t encKey[16], authKey[20];
  deriveKey(masterKey, masterSalt, labelBase + 0, encKey, sizeof(encKey));
  deriveKey(masterKey, masterSalt, labelBase + 1, authKey, sizeof(authKey));
  deriveKey(masterKey, masterSalt, labelBase + 2, k.salt, sizeof(k.salt));

  mbedtls_aes_init(&k.aes);
  mbedtls_md_init(&k.hmac);
  return mbedtls_aes_setkey_enc(&k.aes, encKey, 128) == 0 &&
         mbedtls_md_setup(&k.hmac, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1) == 0 &&
         mbedtls_md_hmac_starts(&k.hmac, authKey, sizeof(authKey)) == 0;
}

bool SrtpContext::begin(const uint8_t masterKey[16], const uint8_t masterSalt[14]) {
  ready_ = initKeys(rtp_, masterKey, masterSalt, 0) && initKeys(rtcp_, masterKey, masterSalt, 3);
  roc_ = 0;
  haveSeq_ = false;
  rtcpIndex_ = 0;
  return ready_;
}

// IV = (k_s * 2^16) XOR (SSRC * 2^64) XOR (index * 2^16)
void SrtpContext::crypt(Keys &k, uint32_t ssrc, uint64_t index, uint8_t *data, size_t len) {
  uint8_t iv[16] = {0};
  memcpy(iv, k.salt, 14);
  for (int i = 0; i < 4; i++) iv[4 + i] ^= (uint8_t)(ssrc >> (24 - 8 * i));
  for (int i = 0; i < 6; i++) iv[8 + i] ^= (uint8_t)(index >> (40 - 8 * i));
  uint8_t stream[16];
  size_t off = 0;
  mbedtls_aes_crypt_ctr(&k.aes, len, &off, iv, stream, data, data);
}

void SrtpContext::tag(Keys &k, const uint8_t *data, size_t len, const uint8_t *extra, size_t extraLen,
                      uint8_t out[10]) {
  uint8_t mac[20];
  mbedtls_md_hmac_reset(&k.hmac);
  mbedtls_md_hmac_update(&k.hmac, data, len);
  if (extraLen) mbedtls_md_hmac_update(&k.hmac, extra, extraLen);
  mbedtls_md_hmac_finish(&k.hmac, mac);
  memcpy(out, mac, AUTH_TAG_LEN);
}

size_t SrtpContext::protectRtp(uint8_t *buf, size_t len) {
  if (!ready_ || len < 12) return 0;
  const uint16_t seq = (buf[2] << 8) | buf[3];
  const uint32_t ssrc = ((uint32_t)buf[8] << 24) | (buf[9] << 16) | (buf[10] << 8) | buf[11];
  if (haveSeq_ && seq < lastSeq_ && lastSeq_ - seq > 0x8000) roc_++;  // sequence number wrapped
  lastSeq_ = seq;
  haveSeq_ = true;

  const size_t hdr = 12 + 4 * (buf[0] & 0x0f);  // CSRCs (none from us, but be correct)
  crypt(rtp_, ssrc, ((uint64_t)roc_ << 16) | seq, buf + hdr, len - hdr);
  const uint8_t roc[4] = {(uint8_t)(roc_ >> 24), (uint8_t)(roc_ >> 16), (uint8_t)(roc_ >> 8), (uint8_t)roc_};
  tag(rtp_, buf, len, roc, sizeof(roc), buf + len);
  return len + AUTH_TAG_LEN;
}

size_t SrtpContext::protectRtcp(uint8_t *buf, size_t len) {
  if (!ready_ || len < 8) return 0;
  const uint32_t ssrc = ((uint32_t)buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7];
  const uint32_t index = rtcpIndex_++ & 0x7fffffff;
  crypt(rtcp_, ssrc, index, buf + 8, len - 8);
  const uint32_t eIndex = 0x80000000u | index;  // E flag: payload encrypted
  buf[len + 0] = eIndex >> 24;
  buf[len + 1] = eIndex >> 16;
  buf[len + 2] = eIndex >> 8;
  buf[len + 3] = eIndex;
  len += 4;
  tag(rtcp_, buf, len, nullptr, 0, buf + len);
  return len + AUTH_TAG_LEN;
}

// ---------------------------------------------------------------------------------------------
// H.264 RTP packetizer (RFC 6184)

void H264Packetizer::begin(uint8_t payloadType, uint32_t ssrc, size_t maxPayload) {
  pt_ = payloadType;
  ssrc_ = ssrc;
  maxPayload_ = maxPayload > sizeof(buf_) - 32 ? sizeof(buf_) - 32 : maxPayload;
  seq_ = (uint16_t)esp_random();
  packets_ = octets_ = 0;
}

size_t H264Packetizer::header(bool marker, uint32_t ts) {
  buf_[0] = 0x80;  // V=2
  buf_[1] = (marker ? 0x80 : 0) | (pt_ & 0x7f);
  buf_[2] = seq_ >> 8;
  buf_[3] = seq_;
  buf_[4] = ts >> 24;
  buf_[5] = ts >> 16;
  buf_[6] = ts >> 8;
  buf_[7] = ts;
  buf_[8] = ssrc_ >> 24;
  buf_[9] = ssrc_ >> 16;
  buf_[10] = ssrc_ >> 8;
  buf_[11] = ssrc_;
  seq_++;
  return 12;
}

void H264Packetizer::sendNal(const uint8_t *nal, size_t len, bool lastNalOfAu, uint32_t ts, SendFn send, void *ctx) {
  if (len == 0) return;
  if (len <= maxPayload_) {  // single NAL unit packet
    size_t n = header(lastNalOfAu, ts);
    memcpy(buf_ + n, nal, len);
    n += len;
    packets_++;
    octets_ += len;
    send(buf_, n, ctx);
    return;
  }

  // FU-A fragmentation
  const uint8_t indicator = (nal[0] & 0xe0) | 28;
  const uint8_t type = nal[0] & 0x1f;
  const uint8_t *p = nal + 1;
  size_t left = len - 1;
  bool first = true;
  while (left) {
    const size_t chunk = left > maxPayload_ - 2 ? maxPayload_ - 2 : left;
    const bool last = chunk == left;
    size_t n = header(last && lastNalOfAu, ts);
    buf_[n++] = indicator;
    buf_[n++] = (first ? 0x80 : 0) | (last ? 0x40 : 0) | type;
    memcpy(buf_ + n, p, chunk);
    n += chunk;
    packets_++;
    octets_ += chunk + 2;
    send(buf_, n, ctx);
    p += chunk;
    left -= chunk;
    first = false;
  }
}

// Returns the offset of the next Annex-B start code at or after pos, or len. *scLen = start code length.
static size_t findStartCode(const uint8_t *d, size_t len, size_t pos, size_t *scLen) {
  for (size_t i = pos; i + 3 <= len; i++) {
    if (d[i] == 0 && d[i + 1] == 0) {
      if (d[i + 2] == 1) {
        *scLen = 3;
        return i;
      }
      if (i + 4 <= len && d[i + 2] == 0 && d[i + 3] == 1) {
        *scLen = 4;
        return i;
      }
    }
  }
  *scLen = 0;
  return len;
}

void H264Packetizer::sendAccessUnit(const uint8_t *d, size_t len, uint32_t ts, SendFn send, void *ctx) {
  lastTs_ = ts;
  size_t sc = 0;
  size_t start = findStartCode(d, len, 0, &sc);
  while (start < len) {
    const size_t nalStart = start + sc;
    size_t nextSc = 0;
    const size_t next = findStartCode(d, len, nalStart, &nextSc);
    sendNal(d + nalStart, next - nalStart, next >= len, ts, send, ctx);
    start = next;
    sc = nextSc;
  }
}
