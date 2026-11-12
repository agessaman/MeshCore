#pragma once

// The repeater poller's flash record: polling identity, request-tag high-water
// mark, the saved CoreScope gate, and per-target admin stop and learned guest
// password. Pure codec (no Arduino, no heap); the file I/O lives with the
// poller. Little-endian, versioned, CRC32-checked. A record from a newer
// firmware is reported as such so the caller can leave it untouched.
//
// Layout: magic u32, version u16, body_len u16, body, crc32 u32 over all
// preceding bytes.

#include "PollPolicy.h"
#include "PollTypes.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace poll {

static const uint32_t kStateMagic = 0x54535052UL;   // "RPST"
static const uint16_t kStateVersion = 1;
static const uint8_t  kPrvKeySize = 64;
static const size_t   kStateHeaderSize = 8;
static const size_t   kStateTargetSize = kPubKeySize + 2;
static const size_t   kStateBodySizeV1 = 4 + 1 + kPubKeySize + kPrvKeySize + 4 + 4 + 5 +
                                         (size_t)kMaxTargets * kStateTargetSize;
static const size_t   kStateRecordSize = kStateHeaderSize + kStateBodySizeV1 + 4;

struct StoredTarget {
  uint8_t pub[kPubKeySize];   // all zero = empty
  bool    admin_seen;
  bool    admin_removed;
  uint8_t learned_pw;         // LearnedPassword
};

struct StateRecord {
  uint32_t seq;               // bumped on every write; the highest valid copy wins
  bool     has_key;
  uint8_t  pub[kPubKeySize];
  uint8_t  prv[kPrvKeySize];
  uint32_t key_created_at;
  uint32_t tag_high_water;    // last request tag or login timestamp handed out
  SavedGate gate;
  StoredTarget targets[kMaxTargets];
};

enum StateLoad : uint8_t {
  STATE_OK = 0,
  STATE_EMPTY,     // no bytes: first boot
  STATE_CORRUPT,   // truncated, bad magic, bad CRC, or impossible fields
  STATE_NEWER,     // valid, but written by a newer firmware: do not overwrite
};

static inline const char* stateLoadName(uint8_t r) {
  switch (r) {
    case STATE_OK: return "ok";
    case STATE_EMPTY: return "empty";
    case STATE_NEWER: return "newer";
    default: return "corrupt";
  }
}

static inline void stateClear(StateRecord& r) { memset(&r, 0, sizeof(r)); }

// IEEE 802.3 (zlib) CRC32, bitwise: the record is small and written rarely.
static inline uint32_t stateCrc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFUL;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (uint8_t k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320UL & (0UL - (c & 1UL)));
  }
  return ~c;
}

static inline void statePut16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static inline void statePut32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static inline uint16_t stateGet16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static inline uint32_t stateGet32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Returns the bytes written (kStateRecordSize), or 0 when cap is too small.
static inline size_t encodeState(const StateRecord& r, uint8_t* out, size_t cap) {
  if (!out || cap < kStateRecordSize) return 0;
  uint8_t* p = out;
  statePut32(p, kStateMagic);
  statePut16(p + 4, kStateVersion);
  statePut16(p + 6, (uint16_t)kStateBodySizeV1);
  p += kStateHeaderSize;
  statePut32(p, r.seq);
  p += 4;
  *p++ = r.has_key ? 1 : 0;
  memcpy(p, r.pub, kPubKeySize);
  p += kPubKeySize;
  memcpy(p, r.prv, kPrvKeySize);
  p += kPrvKeySize;
  statePut32(p, r.key_created_at);
  p += 4;
  statePut32(p, r.tag_high_water);
  p += 4;
  *p++ = (uint8_t)((r.gate.blocked ? 1 : 0) | (r.gate.version_disabled ? 2 : 0) | (r.gate.paused ? 4 : 0));
  statePut32(p, r.gate.paused_until);
  p += 4;
  for (uint8_t i = 0; i < kMaxTargets; i++) {
    const StoredTarget& t = r.targets[i];
    memcpy(p, t.pub, kPubKeySize);
    p += kPubKeySize;
    *p++ = (uint8_t)((t.admin_seen ? 1 : 0) | (t.admin_removed ? 2 : 0));
    *p++ = t.learned_pw <= LEARNED_HELLO ? t.learned_pw : (uint8_t)LEARNED_NONE;
  }
  statePut32(p, stateCrc32(out, (size_t)(p - out)));
  p += 4;
  return (size_t)(p - out);
}

static inline uint8_t decodeState(const uint8_t* in, size_t len, StateRecord* out) {
  if (!out) return STATE_CORRUPT;
  stateClear(*out);
  if (!in || len == 0) return STATE_EMPTY;
  if (len < kStateHeaderSize + 4) return STATE_CORRUPT;
  if (stateGet32(in) != kStateMagic) return STATE_CORRUPT;
  const uint16_t version = stateGet16(in + 4);
  const size_t body_len = stateGet16(in + 6);
  const size_t total = kStateHeaderSize + body_len + 4;
  if (len != total) return STATE_CORRUPT;
  if (stateGet32(in + total - 4) != stateCrc32(in, total - 4)) return STATE_CORRUPT;
  if (version > kStateVersion) return STATE_NEWER;
  if (version != kStateVersion || body_len != kStateBodySizeV1) return STATE_CORRUPT;

  const uint8_t* p = in + kStateHeaderSize;
  out->seq = stateGet32(p);
  p += 4;
  const uint8_t key_flags = *p++;
  if (key_flags > 1) return STATE_CORRUPT;
  out->has_key = key_flags == 1;
  memcpy(out->pub, p, kPubKeySize);
  p += kPubKeySize;
  memcpy(out->prv, p, kPrvKeySize);
  p += kPrvKeySize;
  out->key_created_at = stateGet32(p);
  p += 4;
  out->tag_high_water = stateGet32(p);
  p += 4;
  const uint8_t gate = *p++;
  if (gate > 7) return STATE_CORRUPT;
  out->gate.blocked = (gate & 1) != 0;
  out->gate.version_disabled = (gate & 2) != 0;
  out->gate.paused = (gate & 4) != 0;
  out->gate.paused_until = stateGet32(p);
  p += 4;
  for (uint8_t i = 0; i < kMaxTargets; i++) {
    StoredTarget& t = out->targets[i];
    memcpy(t.pub, p, kPubKeySize);
    p += kPubKeySize;
    const uint8_t flags = *p++;
    const uint8_t learned = *p++;
    if (flags > 3 || learned > LEARNED_HELLO) return STATE_CORRUPT;
    t.admin_seen = (flags & 1) != 0;
    t.admin_removed = (flags & 2) != 0;
    t.learned_pw = learned;
  }
  return STATE_OK;
}

// Which copy to use when several files may exist (primary, temp, backup after
// an interrupted swap). A newer-firmware copy anywhere wins so nothing is
// overwritten; otherwise the highest seq among valid copies. Returns the
// chosen index, or -1 when no copy is usable; *result says why.
static inline int pickState(const uint8_t* results, const StateRecord* records, uint8_t n,
                            uint8_t* result) {
  int best = -1;
  bool any_corrupt = false;
  for (uint8_t i = 0; i < n; i++) {
    if (results[i] == STATE_NEWER) {
      if (result) *result = STATE_NEWER;
      return -1;
    }
    if (results[i] == STATE_CORRUPT) any_corrupt = true;
    if (results[i] != STATE_OK) continue;
    if (best < 0 || records[i].seq > records[best].seq) best = i;
  }
  if (result) *result = best >= 0 ? STATE_OK : (any_corrupt ? STATE_CORRUPT : STATE_EMPTY);
  return best;
}

}  // namespace poll
