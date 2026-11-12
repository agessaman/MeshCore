#pragma once

// CoreScope reply bodies into the fixed structs of PollControlTypes.h. Field
// rules follow the plugin: a wrong-typed field is ignored and keeps its default,
// never fatal. A body that is not a JSON object is rejected. Pure: no Arduino,
// no clock reads; the caller picks the allocator (PSRAM on the device).

#include <ArduinoJson.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "PollControlTypes.h"

namespace poll {

// Deep enough for every shape CoreScope sends, shallow enough to bound junk.
static const uint8_t kControlJsonNesting = 6;

namespace cjson {

static inline int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Exactly 2 * n hex digits, either case, nothing else.
static inline bool decodeHex(const char* s, uint8_t* out, size_t n) {
  if (!s) return false;
  for (size_t i = 0; i < n; i++) {
    const int hi = hexDigit(s[2 * i]);
    if (hi < 0) return false;
    const int lo = hexDigit(s[2 * i + 1]);
    if (lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return s[2 * n] == '\0';
}

static inline bool decodeKey(const char* s, uint8_t out[kPubKeySize]) {
  return decodeHex(s, out, kPubKeySize);
}

// Python's int(): bools are 0/1, floats truncate, strings must be a whole
// integer with optional surrounding whitespace and sign. Anything else fails.
static inline bool readInt(JsonVariantConst v, int64_t* out) {
  if (v.isNull()) return false;
  if (v.is<bool>()) {
    *out = v.as<bool>() ? 1 : 0;
    return true;
  }
  if (v.is<double>()) {
    double d = v.as<double>();
    if (d != d) return false;
    if (d > 9.0e18) d = 9.0e18;
    if (d < -9.0e18) d = -9.0e18;
    *out = (int64_t)d;  // truncates toward zero, as int() does
    return true;
  }
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!s) return false;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    bool neg = false;
    if (*s == '+' || *s == '-') {
      neg = (*s == '-');
      s++;
    }
    if (*s < '0' || *s > '9') return false;
    int64_t val = 0;
    while (*s >= '0' && *s <= '9') {
      if (val < (int64_t)900000000000000000LL) val = val * 10 + (*s - '0');
      s++;
    }
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s != '\0') return false;
    *out = neg ? -val : val;
    return true;
  }
  return false;
}

static inline uint32_t satU32(int64_t v) {
  if (v < 0) return 0;
  if (v > (int64_t)UINT32_MAX) return UINT32_MAX;
  return (uint32_t)v;
}

static inline uint8_t satU8(int64_t v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return (uint8_t)v;
}

// Python truthiness for the scalar shapes CoreScope could send.
static inline bool truthy(JsonVariantConst v) {
  if (v.isNull()) return false;
  if (v.is<bool>()) return v.as<bool>();
  if (v.is<double>()) return v.as<double>() != 0.0;
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    return s && s[0] != '\0';
  }
  if (v.is<JsonArrayConst>()) return v.as<JsonArrayConst>().size() > 0;
  if (v.is<JsonObjectConst>()) return v.as<JsonObjectConst>().size() > 0;
  return false;
}

static inline void copyText(JsonVariantConst v, char* out, size_t cap) {
  if (cap == 0) return;
  out[0] = '\0';
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!s) return;
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
  } else if (!v.is<bool>() && v.is<long>()) {
    // str(number) for a numeric version label.
    char tmp[24];
    const long x = v.as<long>();
    unsigned long u = x < 0 ? 0UL - (unsigned long)x : (unsigned long)x;
    size_t i = sizeof(tmp);
    tmp[--i] = '\0';
    do {
      tmp[--i] = (char)('0' + (u % 10));
      u /= 10;
    } while (u && i > 1);
    if (x < 0) tmp[--i] = '-';
    size_t n = strlen(tmp + i);
    if (n >= cap) n = cap - 1;
    memcpy(out, tmp + i, n);
    out[n] = '\0';
  }
}

// Present with any value, null included: containsKey is deprecated in v7.
static inline bool hasKey(JsonObjectConst o, const char* name) {
  for (JsonPairConst kv : o) {
    if (strcmp(kv.key().c_str(), name) == 0) return true;
  }
  return false;
}

// nullptr means the library's default allocator.
static inline ArduinoJson::Allocator* pickAllocator(ArduinoJson::Allocator* alloc) {
  return alloc ? alloc : ArduinoJson::detail::DefaultAllocator::instance();
}

static inline void policyFilter(JsonObject f) {
  static const char* const kFields[] = {
    "direct_s", "one_hop_s", "grace_s", "max_requests", "version", "paused",
    "paused_until", "min_plugin_version", "version_ok", "observer_blocked",
  };
  for (size_t i = 0; i < sizeof(kFields) / sizeof(kFields[0]); i++) f[kFields[i]] = true;
  f["excluded_nodes"] = true;
  f["node_windows"] = true;
}

static inline bool deserializeObject(JsonDocument& doc, const char* body, size_t len,
                                     JsonDocument& filter) {
  if (!body || len == 0) return false;
  const DeserializationError err =
      deserializeJson(doc, body, len, DeserializationOption::Filter(filter),
                      DeserializationOption::NestingLimit(kControlJsonNesting));
  if (err) return false;
  if (doc.overflowed()) return false;
  return doc.is<JsonObjectConst>();
}

static inline void numberField(JsonObjectConst o, const char* name, uint32_t* field) {
  int64_t v = 0;
  if (readInt(o[name], &v)) *field = satU32(v);
}

}  // namespace cjson

namespace cjson {

static inline bool keyIn(const uint8_t* key, const uint8_t (*keys)[kPubKeySize], uint8_t n) {
  for (uint8_t i = 0; keys && i < n; i++) {
    if (memcmp(keys[i], key, kPubKeySize) == 0) return true;
  }
  return false;
}

}  // namespace cjson

// Server fields on top of policyDefaults; nothing is clamped here. When the lists hold more than
// fits, entries for targets[] are kept first (they always fit) and the list is marked truncated.
static inline void parsePolicyObject(JsonObjectConst o, Policy& p,
                                     const uint8_t (*targets)[kPubKeySize] = nullptr,
                                     uint8_t target_count = 0) {
  policyDefaults(p);
  cjson::numberField(o, "direct_s", &p.direct_s);
  cjson::numberField(o, "one_hop_s", &p.one_hop_s);
  cjson::numberField(o, "grace_s", &p.grace_s);
  int64_t v = 0;
  if (cjson::readInt(o["max_requests"], &v)) p.max_requests = cjson::satU8(v);

  cjson::copyText(o["version"], p.version, sizeof(p.version));
  p.paused = o["paused"].is<bool>() && o["paused"].as<bool>();
  JsonVariantConst until = o["paused_until"];
  if (until.is<const char*>()) {
    uint32_t t = 0;
    if (parseIsoUtc(until.as<const char*>(), &t)) p.paused_until = t;
  }

  if (target_count > kMaxTargets) target_count = kMaxTargets;
  JsonArrayConst ex = o["excluded_nodes"].as<JsonArrayConst>();
  for (uint8_t pass = 0; pass < 2; pass++) {
    for (JsonVariantConst item : ex) {
      uint8_t key[kPubKeySize];
      if (!item.is<const char*>() || !cjson::decodeKey(item.as<const char*>(), key)) continue;
      if (cjson::keyIn(key, targets, target_count) != (pass == 0)) continue;
      if (policyExcluded(p, key)) continue;  // the plugin keeps a set
      if (p.excluded_count >= kMaxExcluded) {
        p.excluded_truncated = true;
        continue;
      }
      memcpy(p.excluded[p.excluded_count++], key, kPubKeySize);
    }
  }

  JsonObjectConst nw = o["node_windows"].as<JsonObjectConst>();
  for (uint8_t pass = 0; pass < 2; pass++) {
    for (JsonPairConst kv : nw) {
      uint8_t key[kPubKeySize];
      if (!cjson::decodeKey(kv.key().c_str(), key)) continue;
      if (cjson::keyIn(key, targets, target_count) != (pass == 0)) continue;
      int64_t w = 0;
      if (!cjson::readInt(kv.value(), &w) || w <= 0) continue;
      bool dup = false;
      for (uint8_t i = 0; i < p.node_window_count; i++) {
        if (memcmp(p.node_windows[i].node, key, kPubKeySize) == 0) dup = true;
      }
      if (dup) continue;
      if (p.node_window_count >= kMaxNodeWindows) {
        p.windows_truncated = true;
        continue;
      }
      memcpy(p.node_windows[p.node_window_count].node, key, kPubKeySize);
      p.node_windows[p.node_window_count].window_s = cjson::satU32(w);
      p.node_window_count++;
    }
  }

  cjson::copyText(o["min_plugin_version"], p.min_plugin_version, sizeof(p.min_plugin_version));
  if (cjson::hasKey(o, "version_ok")) {
    JsonVariantConst ok = o["version_ok"];
    p.has_version_ok = true;
    p.version_ok = !(ok.is<bool>() && !ok.as<bool>());  // anything but literal false
  }
  if (cjson::hasKey(o, "observer_blocked")) {
    JsonVariantConst b = o["observer_blocked"];
    p.observer_blocked = b.is<bool>() && b.as<bool>();
  }
}

// A /policy reply. False: not a JSON object (p is left at the defaults).
static inline bool parsePolicyReply(const char* body, size_t len, Policy& p,
                                    ArduinoJson::Allocator* alloc = nullptr,
                                    const uint8_t (*targets)[kPubKeySize] = nullptr,
                                    uint8_t target_count = 0) {
  policyDefaults(p);
  JsonDocument filter(cjson::pickAllocator(alloc));
  cjson::policyFilter(filter.to<JsonObject>());
  JsonDocument doc(cjson::pickAllocator(alloc));
  if (!cjson::deserializeObject(doc, body, len, filter)) return false;
  parsePolicyObject(doc.as<JsonObjectConst>(), p, targets, target_count);
  return true;
}

static inline void ownerEntryFrom(JsonObjectConst e, OwnerEntry& out) {
  memset(&out, 0, sizeof(out));
  out.present = true;
  out.standby_rank = 1;
  JsonVariantConst role = e["role"];
  if (role.isNull()) {
    out.role = OWNER_ROLE_ABSENT;
  } else if (role.is<const char*>()) {
    out.role = ownerRoleFromText(role.as<const char*>());
    if (out.role == OWNER_ROLE_ABSENT) out.role = OWNER_ROLE_UNKNOWN;  // "" is not None
  } else {
    out.role = OWNER_ROLE_UNKNOWN;
  }
  out.mine = cjson::truthy(e["mine"]);
  out.excluded = cjson::truthy(e["excluded"]);
  int64_t v = 0;
  if (cjson::truthy(e["standby_rank"]) && cjson::readInt(e["standby_rank"], &v)) {
    out.standby_rank = v < 1 ? 1 : cjson::satU8(v);
  }
  if (cjson::readInt(e["owner_hops"], &v)) out.owner_hops = cjson::satU8(v);
  JsonVariantConst ts = e["latest_ts"];
  if (ts.is<const char*>()) {
    uint32_t t = 0;
    if (parseIsoUtc(ts.as<const char*>(), &t)) out.latest_ts = t;
  }
}

// An /owners reply. Returns false when the body is not a JSON object. owners[]
// is index-aligned with targets; *nodes_ok says the reply had a "nodes" object;
// *has_policy says a valid owners envelope carried a policy, parsed into *policy.
static inline bool parseOwnersReply(const char* body, size_t len,
                                    const uint8_t (*targets)[kPubKeySize], uint8_t target_count,
                                    OwnerEntry owners[kMaxTargets], bool* nodes_ok,
                                    Policy* policy, bool* has_policy,
                                    ArduinoJson::Allocator* alloc = nullptr) {
  if (target_count > kMaxTargets) target_count = kMaxTargets;
  memset(owners, 0, sizeof(OwnerEntry) * kMaxTargets);
  if (nodes_ok) *nodes_ok = false;
  if (has_policy) *has_policy = false;
  if (policy) policyDefaults(*policy);

  JsonDocument filter(cjson::pickAllocator(alloc));
  JsonObject entry = filter["nodes"]["*"].to<JsonObject>();
  entry["role"] = true;
  entry["mine"] = true;
  entry["excluded"] = true;
  entry["standby_rank"] = true;
  entry["owner_hops"] = true;
  entry["latest_ts"] = true;
  if (policy) cjson::policyFilter(filter["policy"].to<JsonObject>());

  JsonDocument doc(cjson::pickAllocator(alloc));
  if (!cjson::deserializeObject(doc, body, len, filter)) return false;
  JsonObjectConst root = doc.as<JsonObjectConst>();

  JsonObjectConst nodes = root["nodes"].as<JsonObjectConst>();
  if (!nodes.isNull()) {
    if (nodes_ok) *nodes_ok = true;
    for (JsonPairConst kv : nodes) {
      uint8_t key[kPubKeySize];
      if (!cjson::decodeKey(kv.key().c_str(), key)) continue;
      JsonObjectConst e = kv.value().as<JsonObjectConst>();
      if (e.isNull()) continue;  // a non-dict entry counts as no entry
      for (uint8_t i = 0; i < target_count; i++) {
        if (owners[i].present || memcmp(targets[i], key, kPubKeySize) != 0) continue;
        ownerEntryFrom(e, owners[i]);
      }
    }
  }

  if (policy && !nodes.isNull()) {
    JsonObjectConst p = root["policy"].as<JsonObjectConst>();
    if (!p.isNull()) {
      parsePolicyObject(p, *policy, targets, target_count);
      if (has_policy) *has_policy = true;
    }
  }
  return true;
}

// A POST /api/guest-keys reply: true only for {"ok": true}.
static inline bool parseRegistrationReply(const char* body, size_t len,
                                          ArduinoJson::Allocator* alloc = nullptr) {
  JsonDocument filter(cjson::pickAllocator(alloc));
  filter["ok"] = true;
  JsonDocument doc(cjson::pickAllocator(alloc));
  if (!cjson::deserializeObject(doc, body, len, filter)) return false;
  JsonVariantConst ok = doc["ok"];
  return ok.is<bool>() && ok.as<bool>();
}

// A /shared-guest/key reply. *prv_len is 32 (a seed) or 64 (expanded); the
// caller turns a seed into the expanded form and checks the pair. The caller
// must wipe prv after use.
static inline bool parseSharedKeyReply(const char* body, size_t len, uint8_t pub[kPubKeySize],
                                       uint8_t prv[64], uint8_t* prv_len,
                                       ArduinoJson::Allocator* alloc = nullptr) {
  *prv_len = 0;
  JsonDocument filter(cjson::pickAllocator(alloc));
  filter["public_key"] = true;
  filter["private_key"] = true;
  JsonDocument doc(cjson::pickAllocator(alloc));
  if (!cjson::deserializeObject(doc, body, len, filter)) return false;
  const char* pub_hex = doc["public_key"].is<const char*>() ? doc["public_key"].as<const char*>() : nullptr;
  const char* prv_hex = doc["private_key"].is<const char*>() ? doc["private_key"].as<const char*>() : nullptr;
  if (!pub_hex || !prv_hex) return false;
  if (!cjson::decodeKey(pub_hex, pub)) return false;
  const size_t n = strlen(prv_hex);
  bool ok = false;
  if (n == 64) ok = cjson::decodeHex(prv_hex, prv, 32);
  else if (n == 128) ok = cjson::decodeHex(prv_hex, prv, 64);
  if (!ok) {
    memset(prv, 0, 64);
    return false;
  }
  *prv_len = (uint8_t)(n / 2);
  return true;
}

}  // namespace poll
