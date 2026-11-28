#pragma once

#include "../MQTTPrefsStorage.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace poll {

static const size_t kPwBufSize = MQTT_POLL_PW_MAX_LEN + 1;

enum class TargetHex : uint8_t { Bad, Empty, Ok };

static inline int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static inline bool targetIsEmpty(const uint8_t key[MQTT_POLL_PUBKEY_SIZE]) {
  if (!key) return true;
  for (int i = 0; i < MQTT_POLL_PUBKEY_SIZE; i++) {
    if (key[i] != 0) return false;
  }
  return true;
}

// Exactly 64 hex digits, or "" (Empty). All-zero decodes as Empty so it is not
// stored as a configured target. *folded_upper is set when A-F was present.
static inline TargetHex parseTargetHex(const char* hex, uint8_t out[MQTT_POLL_PUBKEY_SIZE],
                                       bool* folded_upper) {
  if (folded_upper) *folded_upper = false;
  if (!out) return TargetHex::Bad;
  memset(out, 0, MQTT_POLL_PUBKEY_SIZE);
  if (!hex || hex[0] == '\0') return TargetHex::Empty;
  for (int i = 0; i < MQTT_POLL_PUBKEY_SIZE * 2; i++) {
    if (hex[i] == '\0') return TargetHex::Bad;
  }
  if (hex[MQTT_POLL_PUBKEY_SIZE * 2] != '\0') return TargetHex::Bad;
  bool upper = false;
  bool any = false;
  for (int i = 0; i < MQTT_POLL_PUBKEY_SIZE; i++) {
    int hi = hexValue(hex[i * 2]);
    int lo = hexValue(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return TargetHex::Bad;
    if (hex[i * 2] >= 'A' && hex[i * 2] <= 'F') upper = true;
    if (hex[i * 2 + 1] >= 'A' && hex[i * 2 + 1] <= 'F') upper = true;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
    if (out[i] != 0) any = true;
  }
  if (folded_upper) *folded_upper = upper;
  if (!any) {
    memset(out, 0, MQTT_POLL_PUBKEY_SIZE);
    return TargetHex::Empty;
  }
  return TargetHex::Ok;
}

static inline void formatTargetHex(const uint8_t key[MQTT_POLL_PUBKEY_SIZE], char out[65]) {
  static const char kHex[] = "0123456789abcdef";
  if (!out) return;
  if (!key) {
    out[0] = '\0';
    return;
  }
  for (int i = 0; i < MQTT_POLL_PUBKEY_SIZE; i++) {
    out[i * 2] = kHex[key[i] >> 4];
    out[i * 2 + 1] = kHex[key[i] & 0x0f];
  }
  out[64] = '\0';
}

static inline bool pollIntervalValid(int minutes) {
  return minutes >= static_cast<int>(MQTT_POLL_INTERVAL_MIN) &&
         minutes <= static_cast<int>(MQTT_POLL_INTERVAL_MAX);
}

static inline bool pollLimitValid(int limit) {
  return limit >= 1 && limit <= static_cast<int>(MQTT_POLL_MAX_TARGETS);
}

static inline bool pollStatusHoursValid(int hours) {
  return hours == 1 || hours == 3 || hours == 6 || hours == 12 || hours == 24;
}

static inline bool pollUrlHasScheme(const char* url) {
  if (!url) return false;
  return strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0;
}

// True only for an https:// URL, scheme compared without regard to case.
static inline bool pollUrlIsHttps(const char* url) {
  if (!url) return false;
  static const char kScheme[] = "https://";
  for (size_t i = 0; i < sizeof(kScheme) - 1; i++) {
    char c = url[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != kScheme[i]) return false;
  }
  return true;
}

// 1 to 15 bytes of printable ASCII without spaces (0x21..0x7E).
static inline bool pollPasswordValid(const char* password) {
  if (!password || password[0] == '\0') return false;
  size_t len = 0;
  for (; password[len] != '\0'; len++) {
    if (len >= MQTT_POLL_PW_MAX_LEN) return false;
    const unsigned char c = static_cast<unsigned char>(password[len]);
    if (c < 0x21 || c > 0x7E) return false;
  }
  return true;
}

// Mode in range and a password stored exactly when the mode is custom. Repairs
// either side to auto with a zeroed password; returns true when it changed.
static inline bool normalizeTargetPassword(uint8_t* mode, char password[kPwBufSize]) {
  if (!mode || !password) return false;
  bool changed = false;
  if (*mode > MQTT_POLL_PW_CUSTOM) {
    *mode = MQTT_POLL_PW_AUTO;
    changed = true;
  }
  if (*mode == MQTT_POLL_PW_CUSTOM) {
    if (memchr(password, '\0', kPwBufSize) != nullptr && pollPasswordValid(password)) {
      const size_t len = strlen(password);
      for (size_t i = len + 1; i < kPwBufSize; i++) {
        if (password[i] != '\0') {
          password[i] = '\0';
          changed = true;
        }
      }
      return changed;
    }
    *mode = MQTT_POLL_PW_AUTO;
    changed = true;
  }
  for (size_t i = 0; i < kPwBufSize; i++) {
    if (password[i] != '\0') changed = true;
  }
  memset(password, 0, kPwBufSize);
  return changed;
}

// Empty is fine. A trailing slash is removed. Anything else that does not
// start with http:// or https:// is cleared. *changed reports either edit.
static inline bool normalizePollUrl(char* url, bool* changed) {
  if (changed) *changed = false;
  if (!url) return false;
  if (url[0] == '\0') return true;
  size_t len = strlen(url);
  while (len > 0 && url[len - 1] == '/') {
    url[--len] = '\0';
    if (changed) *changed = true;
  }
  if (url[0] != '\0' && pollUrlHasScheme(url)) return true;
  if (url[0] != '\0') {
    url[0] = '\0';
    if (changed) *changed = true;
  }
  return false;
}

// slot_max is the highest 1-based slot this check accepts (prefs capacity or
// the build's runtime slot count). Slot 0 is allowed: the policy's broker hosts may pick one.
// A blank URL is local mode, which needs a slot. Consent is not needed: it gates only the key upload.
static inline bool pollCanEnable(const char* url, int slot, int slot_max) {
  if (slot < 0 || slot > slot_max) return false;
  if (url && url[0] == '\0') return slot >= 1;
  return pollUrlHasScheme(url);
}

}  // namespace poll
