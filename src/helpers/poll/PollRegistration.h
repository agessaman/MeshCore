#pragma once

// Registering this observer's own polling key with CoreScope. The repeater key
// signs the message; CoreScope keeps the private half to decrypt overheard
// replies. The body matches requests' json.dumps of the plugin's dict order.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "PollClaims.h"  // hex writers and the bounded append buffer
#include "PollPolicy.h"
#include "PollTypes.h"

namespace poll {

static const uint32_t kReregisterEverySec = 6UL * 3600UL;
static const uint32_t kUnsupportedRetrySec = 6UL * 3600UL;
static const uint32_t kFailedRetrySec = 5UL * 60UL;

// "corescope-guest-key/v1|<observer>|<polling>|<timestamp>", all hex lower.
static inline size_t buildRegistrationMessage(char* out, size_t cap,
                                              const uint8_t observer_pub[kPubKeySize],
                                              const uint8_t poll_pub[kPubKeySize],
                                              uint32_t timestamp) {
  if (!out || cap == 0 || !observer_pub || !poll_pub) return 0;
  detail::Out o(out, cap);
  o.lit("corescope-guest-key/v1|");
  detail::hex(o, observer_pub, kPubKeySize, false);
  o.ch('|');
  detail::hex(o, poll_pub, kPubKeySize, false);
  o.ch('|');
  detail::writeUint(o, timestamp);
  return o.ok ? o.len : 0;
}

static inline size_t buildRegistrationBody(char* out, size_t cap,
                                           const uint8_t observer_pub[kPubKeySize],
                                           const uint8_t poll_pub[kPubKeySize],
                                           const uint8_t* poll_prv, size_t prv_len,
                                           uint32_t timestamp,
                                           const uint8_t signature[64]) {
  if (!out || cap == 0 || !observer_pub || !poll_pub || !signature) return 0;
  if (!poll_prv || prv_len == 0) return 0;
  // The repeater's own key only ever signs; it is never handed to CoreScope.
  if (memcmp(observer_pub, poll_pub, kPubKeySize) == 0) return 0;

  detail::Out o(out, cap);
  o.lit("{\"observer_id\": \"");
  detail::hex(o, observer_pub, kPubKeySize, false);
  o.lit("\", \"public_key\": \"");
  detail::hex(o, poll_pub, kPubKeySize, false);
  o.lit("\", \"private_key\": \"");
  detail::hex(o, poll_prv, prv_len, false);
  o.lit("\", \"timestamp\": ");
  detail::writeUint(o, timestamp);
  o.lit(", \"signature\": \"");
  detail::hex(o, signature, 64, false);
  o.lit("\"}");
  return o.ok ? o.len : 0;
}

struct RegistrationState {
  bool     ok;            // CoreScope accepted the own key
  bool     unsupported;   // 404: this CoreScope has no per-observer keys
  uint32_t ok_at;
  uint32_t tried_at;
  int16_t  last_status;   // HTTP status, 0 = no answer
};

static inline bool registrationDue(const RegistrationState& s, uint32_t now) {
  if (s.tried_at == 0) return true;  // never tried
  uint32_t retry = kFailedRetrySec;
  if (s.ok) retry = kReregisterEverySec;
  else if (s.unsupported) retry = kUnsupportedRetrySec;
  return elapsedSec(now, s.tried_at) >= retry;
}

// Only a 200 with {"ok": true} refreshes the key, and only a 404 rules the
// feature out; any other failure keeps the earlier answer until it lapses.
static inline void registrationResult(RegistrationState& s, int http_status, bool body_ok,
                                      uint32_t now) {
  s.tried_at = now;
  s.last_status = (int16_t)http_status;
  if (http_status == 200 && body_ok) {
    s.ok = true;
    s.unsupported = false;
    s.ok_at = now;
  } else if (http_status == 404) {
    s.ok = false;
    s.unsupported = true;
  }
}

enum KeyChoice : uint8_t { KEY_NONE = 0, KEY_OWN, KEY_SHARED };

static_assert(KEY_NONE == 0, "claimMayList treats 0 as no polling key");

// CoreScope drops a key 24 h after its last registration, so stop using it then.
static inline uint8_t chooseKey(const RegistrationState& s, bool have_shared_key, uint32_t now) {
  if (s.ok && elapsedSec(now, s.ok_at) < kDay) return KEY_OWN;
  return have_shared_key ? KEY_SHARED : KEY_NONE;
}

// How readings are delivered, from the latest policy answer of this boot; zero until one arrives.
enum ReadingMode : uint8_t { MODE_UNKNOWN = 0, MODE_ENCRYPTED, MODE_JSON, MODE_TELEMETRY };

static inline uint8_t modeFromFormat(uint8_t format) {
  if (format == READING_JSON) return MODE_JSON;
  if (format == READING_TELEMETRY) return MODE_TELEMETRY;
  return MODE_ENCRYPTED;
}

static inline bool modeIsJson(uint8_t mode) { return mode == MODE_JSON || mode == MODE_TELEMETRY; }

// A policy 200 sets the mode from its field, a 404 selects encrypted, and a failure keeps it.
static inline uint8_t modeAfterPolicy(uint8_t mode, bool policy_ok, int16_t policy_status,
                                      uint8_t format) {
  if (policy_ok) return modeFromFormat(format);
  if (policy_status == 404) return MODE_ENCRYPTED;
  return mode;
}

// Registration and the shared key belong to the encrypted path, once a policy answer chose it.
static inline bool keyExchangeAllowed(uint8_t mode) { return mode == MODE_ENCRYPTED; }

// The private key is offered for upload only with consent, and never in a JSON mode.
static inline bool registrationWanted(bool due, bool consent, uint8_t mode) {
  return due && consent && !modeIsJson(mode);
}

// Leaving a JSON mode voids an earlier registration: the key is registered again before use.
static inline bool registrationVoided(uint8_t from, uint8_t to) {
  return modeIsJson(from) && !modeIsJson(to);
}

// A JSON mode needs no consent for the polling key, which then never leaves the device.
static inline bool identityAllowed(bool consent, uint8_t mode) { return consent || modeIsJson(mode); }

// In a JSON mode the own key is usable at once; before any policy answer no key is.
static inline uint8_t chooseKeyForMode(uint8_t mode, bool has_own, const RegistrationState& s,
                                       bool have_shared_key, uint32_t now) {
  if (mode == MODE_UNKNOWN) return KEY_NONE;
  if (modeIsJson(mode)) return has_own ? KEY_OWN : KEY_NONE;
  return chooseKey(s, have_shared_key, now);
}

}  // namespace poll
