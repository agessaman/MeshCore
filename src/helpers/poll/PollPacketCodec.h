#pragma once

// Plaintext builders and reply pre-filter for guest-key polling. Encryption and
// the MAC stay with the caller (Utils::encryptThenMAC / MACThenDecrypt). No
// Arduino, mesh types, heap, or crypto here.

#include "PollTypes.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace poll {

// Numeric values of PAYLOAD_TYPE_RESPONSE and PAYLOAD_TYPE_PATH in src/Packet.h.
static const uint8_t kPayloadTypeResponse = 0x01;
static const uint8_t kPayloadTypePath = 0x08;
static_assert(kPayloadTypeResponse == 0x01, "Packet.h PAYLOAD_TYPE_RESPONSE");
static_assert(kPayloadTypePath == 0x08, "Packet.h PAYLOAD_TYPE_PATH");

// CIPHER_MAC_SIZE and CIPHER_BLOCK_SIZE in src/MeshCore.h. A reply shorter than
// the two hashes plus a MAC and one cipher block cannot decrypt.
static const uint8_t kCipherMacSize = 2;
static const uint8_t kCipherBlockSize = 16;
static const size_t kMinCandidatePayload =
    2u + (size_t)kCipherMacSize + (size_t)kCipherBlockSize;

// BaseChatMesh::sendRequest(req_type) plaintext is always this long.
static const size_t kRequestPlainLen = 13;

// sendCommandData: (attempt & 3) | (txt_type << 2), attempt 0. TXT_TYPE_CLI_DATA (1)
// rather than TXT_TYPE_CLI_COMMAND (3): repeaters built before the latter existed
// drop it, and the removal must work on every release.
static const uint8_t kSetPermFlags = (uint8_t)(kTxtTypeCliData << 2);

// createPathReturn with no extra writes this dummy type, then 4 random bytes.
static const uint8_t kPathExtraNone = 0xFF;

// Packet::isValidPathLen rejects a 4-byte hash size and anything past this.
static const uint8_t kMaxPathBytes = 64;

static inline void writeLe32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t readLe32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

// Same acceptance rule as Packet::isValidPathLen. nbytes is the path body.
static inline bool pathBodyLen(uint8_t path_len_byte, uint8_t* nbytes) {
  uint8_t hash_size = (uint8_t)((path_len_byte >> 6) + 1);
  uint8_t hash_count = (uint8_t)(path_len_byte & 63);
  if (hash_size == 4) return false;
  unsigned n = (unsigned)hash_size * (unsigned)hash_count;
  if (n > kMaxPathBytes) return false;
  if (nbytes) *nbytes = (uint8_t)n;
  return true;
}

// timestamp (4 LE) plus the password bytes. Blank is no bytes; "hello" is five.
// Anything else is refused so a real password cannot go out under this key.
static inline size_t buildLoginPlain(uint8_t* out, size_t cap, uint32_t timestamp, uint8_t pw) {
  if (pw != PW_BLANK && pw != PW_HELLO) return 0;
  const char* text = (pw == PW_HELLO) ? passwordText(pw) : "";
  size_t pwlen = 0;
  while (text[pwlen] != 0) pwlen++;
  if (!out || cap < 4u + pwlen) return 0;
  writeLe32(out, timestamp);
  if (pwlen) memcpy(out + 4, text, pwlen);
  return 4u + pwlen;
}

// tag (4 LE), request type, four reserved zeros (the telemetry permission-mask
// byte is the first of them), then the 4 random bytes. Kind must be a request
// type, not the ReqKind enum.
static inline size_t buildRequestPlain(uint8_t* out, size_t cap, uint32_t tag, uint8_t kind,
                                      const uint8_t rnd[4]) {
  if (!out || !rnd || cap < kRequestPlainLen) return 0;
  if (kind != kReqStatus && kind != kReqTelemetry) return 0;
  writeLe32(out, tag);
  out[4] = kind;
  out[5] = 0;
  out[6] = 0;
  out[7] = 0;
  out[8] = 0;
  memcpy(out + 9, rnd, 4);
  return kRequestPlainLen;
}

// PATH plaintext createPathReturn builds when extra_len is 0: path_len byte,
// path bytes, 0xFF, 4 random bytes. path_len_byte 0 is a direct/empty path.
static inline size_t buildTeachPlain(uint8_t* out, size_t cap, const uint8_t* path,
                                    uint8_t path_len_byte, const uint8_t rnd[4]) {
  if (!out || !rnd) return 0;
  uint8_t nbytes = 0;
  if (!pathBodyLen(path_len_byte, &nbytes)) return 0;
  if (nbytes > 0 && !path) return 0;
  size_t n = 1u + (size_t)nbytes + 1u + 4u;
  if (cap < n) return 0;
  out[0] = path_len_byte;
  if (nbytes) memcpy(out + 1, path, nbytes);
  out[1u + nbytes] = kPathExtraNone;
  memcpy(out + 2u + nbytes, rnd, 4);
  return n;
}

// "setperm <64 lowercase hex> 0". fromHex accepts either case; lowercase is what
// the plugin sends and what we always emit. Length excludes a trailing NUL;
// cipher padding supplies the terminator the repeater's C string expects.
static inline size_t buildSetPermPlain(uint8_t* out, size_t cap, uint32_t timestamp,
                                      const uint8_t poll_pub[32]) {
  static const char kHex[] = "0123456789abcdef";
  const size_t n = 5u + 8u + 64u + 2u;
  if (!out || !poll_pub || cap < n) return 0;
  writeLe32(out, timestamp);
  out[4] = kSetPermFlags;
  memcpy(out + 5, "setperm ", 8);
  for (uint8_t i = 0; i < 32; i++) {
    out[13u + (size_t)i * 2u] = (uint8_t)kHex[poll_pub[i] >> 4];
    out[14u + (size_t)i * 2u] = (uint8_t)kHex[poll_pub[i] & 0x0F];
  }
  out[77] = ' ';
  out[78] = '0';
  return n;
}

// Cheap reject before any MAC check. payload[0] is the destination (the polling
// key); payload[1] is the repeater that sent it.
static inline bool isCandidateReply(uint8_t payload_type, const uint8_t* payload, size_t payload_len,
                                    uint8_t poll_hash, uint8_t target_hash) {
  if (payload_type != kPayloadTypeResponse && payload_type != kPayloadTypePath) return false;
  if (!payload || payload_len < kMinCandidatePayload) return false;
  return payload[0] == poll_hash && payload[1] == target_hash;
}

// PATH plaintext is path_len, path bytes, then an extra-type byte whose low
// nibble must be RESPONSE (the high nibble is reserved). inner is the extra.
static inline bool unwrapPathResponse(const uint8_t* plain, size_t len, const uint8_t** inner,
                                     size_t* inner_len) {
  if (!plain || !inner || !inner_len || len < 1) return false;
  uint8_t nbytes = 0;
  if (!pathBodyLen(plain[0], &nbytes)) return false;
  size_t type_at = 1u + (size_t)nbytes;
  if (len <= type_at + 1u) return false;
  if ((plain[type_at] & 0x0F) != kPayloadTypeResponse) return false;
  *inner = plain + type_at + 1u;
  *inner_len = len - (type_at + 1u);
  return true;
}

struct Reply {
  uint32_t tag;
  const uint8_t* data;
  size_t len;
};

static inline bool splitReply(const uint8_t* plain, size_t len, Reply* out) {
  if (!plain || !out || len < 4) return false;
  out->tag = readLe32(plain);
  out->data = plain + 4;
  out->len = len - 4;
  return true;
}

enum ReplyClass : uint8_t {
  REPLY_IGNORE = 0,
  REPLY_DATA,
  REPLY_LOGIN_GUEST,
  REPLY_LOGIN_ADMIN
};

static inline bool tagInList(uint32_t tag, const uint32_t* tags, uint8_t n) {
  if (!tags) return false;
  for (uint8_t i = 0; i < n; i++) {
    if (tags[i] == tag) return true;
  }
  return false;
}

// A poll-tag match is this poll's data reply. A login answer is response code
// 0 and admin code <= 3, four bytes present. Own tags are never logins:
// CayenneLPP voltage `01 74 01 ..` puts 1 in the admin-code byte, and a short
// status reply can pass the shape check on its own.
static inline uint8_t classifyReply(const Reply& r, const TargetState& t, const uint32_t* poll_tags,
                                   uint8_t n_poll_tags, bool expect_login, bool expect_data) {
  bool in_poll = tagInList(r.tag, poll_tags, n_poll_tags);
  if (expect_data && in_poll) return REPLY_DATA;
  if (!expect_login || in_poll || isRecentTag(t, r.tag)) return REPLY_IGNORE;
  if (!r.data || r.len < 4) return REPLY_IGNORE;
  if (r.data[0] != kRespServerLoginOk || r.data[2] > 3) return REPLY_IGNORE;
  return r.data[2] == 1 ? REPLY_LOGIN_ADMIN : REPLY_LOGIN_GUEST;
}

}  // namespace poll
