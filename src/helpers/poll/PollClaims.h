#pragma once

// Claim v2: tells CoreScope which repeaters this observer can reach, by what
// route and under which key. The payload matches the plugin's json.dumps call
// (default separators, ensure_ascii), so CoreScope sees the same bytes.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "PollTypes.h"

namespace poll {

struct ClaimNode {
  uint8_t node[kPubKeySize];
  uint8_t reply_hops;     // 0 direct, 1 one hop
  bool    has_via;        // false -> JSON null
  uint8_t via[kPubKeySize];
  bool    personal_key;   // informational; the claim always says "shared"
  bool    has_snr;
  int16_t snr_x4;         // neighbor SNR in quarter dB, printed to 0.1
};

static const uint32_t kClaimEverySec = 30UL * 60UL;
static const uint32_t kClaimRetrySec = 60;

// Python's json.dumps gets no separators= argument here, so it emits ", " and
// ": ", and its default ensure_ascii escapes non-ASCII. out holds 2 * len + 1.
static inline void toHexLower(char* out, const uint8_t* data, size_t len) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = kHex[data[i] >> 4];
    out[2 * i + 1] = kHex[data[i] & 0x0f];
  }
  out[2 * len] = '\0';
}

static inline void toHexUpper(char* out, const uint8_t* data, size_t len) {
  static const char kHex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = kHex[data[i] >> 4];
    out[2 * i + 1] = kHex[data[i] & 0x0f];
  }
  out[2 * len] = '\0';
}

// Unsigned difference that reads a zero or future stamp as "just now".
static inline uint32_t elapsedSec(uint32_t now, uint32_t then) {
  return now >= then ? now - then : 0;
}

namespace detail {

// Bounded append buffer: refuses the whole write rather than truncating, so a
// short buffer can never publish a half-built payload.
struct Out {
  char*  buf;
  size_t cap;
  size_t len;
  bool   ok;

  Out(char* b, size_t c) : buf(b), cap(c), len(0), ok(b != nullptr && c > 0) {
    if (ok) buf[0] = '\0';
  }

  void raw(const char* s, size_t n) {
    if (!ok) return;
    if (n >= cap - len) {
      ok = false;
      return;
    }
    memcpy(buf + len, s, n);
    len += n;
    buf[len] = '\0';
  }

  void lit(const char* s) { raw(s, strlen(s)); }
  void ch(char c) { raw(&c, 1); }
};

static inline void writeUint(Out& o, uint32_t v) {
  char tmp[10];
  uint8_t n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (n) o.ch(tmp[--n]);
}

static inline void write2(Out& o, uint32_t v) {
  o.ch((char)('0' + (v / 10) % 10));
  o.ch((char)('0' + v % 10));
}

static inline void write4(Out& o, uint32_t v) {
  o.ch((char)('0' + (v / 1000) % 10));
  o.ch((char)('0' + (v / 100) % 10));
  o.ch((char)('0' + (v / 10) % 10));
  o.ch((char)('0' + v % 10));
}

static inline void hex(Out& o, const uint8_t* data, size_t len, bool upper) {
  static const char kLower[] = "0123456789abcdef";
  static const char kUpper[] = "0123456789ABCDEF";
  const char* kHex = upper ? kUpper : kLower;
  for (size_t i = 0; i < len; i++) {
    o.ch(kHex[data[i] >> 4]);
    o.ch(kHex[data[i] & 0x0f]);
  }
}

static inline void uEscape(Out& o, uint32_t cp) {
  static const char kHex[] = "0123456789abcdef";
  char tmp[6] = {'\\', 'u', kHex[(cp >> 12) & 0x0f], kHex[(cp >> 8) & 0x0f],
                 kHex[(cp >> 4) & 0x0f], kHex[cp & 0x0f]};
  o.raw(tmp, sizeof(tmp));
}

// UTF-8 code point at p. Malformed input consumes one byte and reports false.
static inline bool utf8Next(const uint8_t*& p, uint32_t& cp) {
  const uint8_t b = *p;
  size_t need;
  if (b < 0x80) {
    cp = b;
    p += 1;
    return true;
  } else if (b >= 0xc2 && b <= 0xdf) {
    need = 2;
    cp = b & 0x1f;
  } else if (b >= 0xe0 && b <= 0xef) {
    need = 3;
    cp = b & 0x0f;
  } else if (b >= 0xf0 && b <= 0xf4) {
    need = 4;
    cp = b & 0x07;
  } else {
    p += 1;
    return false;
  }
  for (size_t i = 1; i < need; i++) {
    if (p[i] == 0 || (p[i] & 0xc0) != 0x80) {
      p += 1;
      return false;
    }
    cp = (cp << 6) | (p[i] & 0x3f);
  }
  p += need;
  if ((need == 3 && cp < 0x800) || (need == 4 && cp < 0x10000) || cp > 0x10ffff) {
    return false;
  }
  return true;
}

static inline void jsonString(Out& o, const char* s) {
  o.ch('"');
  const uint8_t* p = (const uint8_t*)(s ? s : "");
  while (*p) {
    const uint8_t b = *p;
    if (b < 0x20 || b == 0x7f) {
      switch (b) {
        case '\b': o.lit("\\b"); break;
        case '\f': o.lit("\\f"); break;
        case '\n': o.lit("\\n"); break;
        case '\r': o.lit("\\r"); break;
        case '\t': o.lit("\\t"); break;
        default: uEscape(o, b); break;
      }
      p++;
    } else if (b == '"') {
      o.lit("\\\"");
      p++;
    } else if (b == '\\') {
      o.lit("\\\\");
      p++;
    } else if (b < 0x80) {
      o.ch((char)b);
      p++;
    } else {
      uint32_t cp = 0;
      if (!utf8Next(p, cp)) {
        o.ch((char)b);  // malformed input: keep the byte rather than invent one
      } else if (cp <= 0xffff) {
        uEscape(o, cp);
      } else {
        cp -= 0x10000;
        uEscape(o, 0xd800 + (cp >> 10));
        uEscape(o, 0xdc00 + (cp & 0x3ff));
      }
    }
  }
  o.ch('"');
}

// Quarter dB -> tenths, rounded half to even like Python round(x, 1) on the
// exactly representable n / 4.
static inline void jsonSnr(Out& o, int16_t snr_x4) {
  const int32_t n = snr_x4;
  int32_t t;
  if ((n & 1) == 0) {
    t = (n * 5) / 2;
  } else {
    const int32_t floorTenths = (n * 5 - 1) / 2;
    t = (floorTenths & 1) ? floorTenths + 1 : floorTenths;
  }
  if (t < 0) {
    o.ch('-');
    t = -t;
  }
  char digits[6];
  uint8_t nd = 0;
  uint32_t ip = (uint32_t)(t / 10);
  do {
    digits[nd++] = (char)('0' + ip % 10);
    ip /= 10;
  } while (ip);
  while (nd) o.ch(digits[--nd]);
  o.ch('.');
  o.ch((char)('0' + t % 10));
}

static inline void jsonTimestamp(Out& o, uint32_t epoch) {
  const uint32_t days = epoch / 86400u;
  const uint32_t sod = epoch % 86400u;
  const int64_t z = (int64_t)days + 719468;
  const int64_t era = z / 146097;
  const uint32_t doe = (uint32_t)(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int32_t y = (int32_t)yoe + (int32_t)(era * 400);
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  const uint32_t d = doy - (153 * mp + 2) / 5 + 1;
  const uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  const uint32_t year = (uint32_t)(y + (m <= 2 ? 1 : 0));
  write4(o, year);
  o.ch('-');
  write2(o, m);
  o.ch('-');
  write2(o, d);
  o.ch('T');
  write2(o, sod / 3600);
  o.ch(':');
  write2(o, (sod / 60) % 60);
  o.ch(':');
  write2(o, sod % 60);
  o.ch('Z');
}

}  // namespace detail

static inline size_t buildClaimJson(char* out, size_t cap, const char* origin,
                                    const uint8_t observer_pub[kPubKeySize],
                                    uint32_t now_epoch, const char* plugin_tag,
                                    const char* policy_version, bool community,
                                    const ClaimNode* nodes, uint8_t count) {
  if (!out || cap == 0 || !observer_pub) return 0;
  if (count > kMaxTargets || (count > 0 && !nodes)) return 0;

  detail::Out o(out, cap);
  o.lit("{\"v\": 2, \"origin\": ");
  detail::jsonString(o, origin);
  o.lit(", \"origin_id\": \"");
  detail::hex(o, observer_pub, kPubKeySize, true);
  o.lit("\", \"timestamp\": \"");
  detail::jsonTimestamp(o, now_epoch);
  o.lit("\", \"plugin\": ");
  detail::jsonString(o, plugin_tag);
  o.lit(", \"policy_version\": ");
  detail::jsonString(o, policy_version);
  o.lit(community ? ", \"community\": true, \"nodes\": [" : ", \"community\": false, \"nodes\": [");
  for (uint8_t i = 0; i < count; i++) {
    const ClaimNode& n = nodes[i];
    if (i) o.lit(", ");
    o.lit("{\"node_id\": \"");
    detail::hex(o, n.node, kPubKeySize, false);
    o.lit("\", \"reply_hops\": ");
    detail::writeUint(o, n.reply_hops);
    o.lit(", \"via\": ");
    if (n.has_via) {
      o.ch('"');
      detail::hex(o, n.via, kPubKeySize, false);
      o.ch('"');
    } else {
      o.lit("null");
    }
    o.lit(", \"key\": \"shared\", \"snr\": ");
    if (n.has_snr) {
      detail::jsonSnr(o, n.snr_x4);
    } else {
      o.lit("null");
    }
    o.ch('}');
  }
  o.lit("]}");
  return o.ok ? o.len : 0;
}

// Sort key for the signature: the fields CoreScope compares, not the key name
// or SNR, so a route or hop change is what triggers a fresh claim.
static inline int claimNodeCmp(const ClaimNode& a, const ClaimNode& b) {
  const int c = memcmp(a.node, b.node, kPubKeySize);
  if (c != 0) return c;
  if (a.reply_hops != b.reply_hops) return a.reply_hops < b.reply_hops ? -1 : 1;
  if (a.has_via != b.has_via) return a.has_via ? 1 : -1;
  if (!a.has_via) return 0;
  return memcmp(a.via, b.via, kPubKeySize);
}

static inline uint32_t claimSignature(const ClaimNode* nodes, uint8_t count, bool community) {
  if (count > kMaxTargets || (count > 0 && !nodes)) return 0;
  uint8_t order[kMaxTargets];
  for (uint8_t i = 0; i < count; i++) order[i] = i;
  for (uint8_t i = 1; i < count; i++) {
    const uint8_t v = order[i];
    int j = i - 1;
    while (j >= 0 && claimNodeCmp(nodes[order[j]], nodes[v]) > 0) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = v;
  }

  uint32_t h = 2166136261u;
  for (uint8_t i = 0; i < count; i++) {
    const ClaimNode& n = nodes[order[i]];
    for (uint8_t k = 0; k < kPubKeySize; k++) {
      h = (h ^ n.node[k]) * 16777619u;
    }
    h = (h ^ n.reply_hops) * 16777619u;
    h = (h ^ (n.has_via ? 1u : 0u)) * 16777619u;
    if (n.has_via) {
      for (uint8_t k = 0; k < kPubKeySize; k++) {
        h = (h ^ n.via[k]) * 16777619u;
      }
    }
    h = (h ^ 0xffu) * 16777619u;
  }
  h = (h ^ (community ? 1u : 0u)) * 16777619u;
  return h;
}

struct ClaimTiming {
  uint32_t sent_at;
  uint32_t tried_at;
  uint32_t signature;
  bool     sent_once;
  bool     last_community;
};

// Every attempt starts the 60 s retry clock, as claims.py does, so a route
// change right after a publish waits for it too.
static inline bool claimDue(const ClaimTiming& t, uint32_t signature, bool community,
                            uint32_t now) {
  if (!community && !(t.sent_once && t.last_community)) return false;  // off, and already said so
  const bool changed = signature != t.signature || community != t.last_community;
  if (!changed && elapsedSec(now, t.sent_at) < kClaimEverySec) return false;
  if (elapsedSec(now, t.tried_at) < kClaimRetrySec) return false;
  return true;
}

static inline void claimSent(ClaimTiming& t, uint32_t signature, bool community, uint32_t now) {
  t.sent_at = now;
  t.tried_at = now;
  t.signature = signature;
  t.sent_once = true;
  t.last_community = community;
}

static inline void claimFailed(ClaimTiming& t, uint32_t now) {
  t.tried_at = now;
}

}  // namespace poll
