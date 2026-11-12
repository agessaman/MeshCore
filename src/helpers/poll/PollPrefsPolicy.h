#pragma once

// Pure checks for repeater-poll settings. Shared by /mqtt.json repair and the
// poll CLI so a value accepted on one path is accepted on the other. No Arduino.

#include "../MQTTPrefsStorage.h"
#include "PollTypes.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace poll {

static_assert(kMaxTargets == MQTT_POLL_MAX_TARGETS, "poll target capacity drift");
static_assert(kPubKeySize == MQTT_POLL_PUBKEY_SIZE, "poll key size drift");
static_assert(static_cast<int>(PWMODE_AUTO) == MQTT_POLL_PW_AUTO, "poll pw mode drift");
static_assert(static_cast<int>(PWMODE_BLANK) == MQTT_POLL_PW_BLANK, "poll pw mode drift");
static_assert(static_cast<int>(PWMODE_HELLO) == MQTT_POLL_PW_HELLO, "poll pw mode drift");

enum class TargetHex : uint8_t { Bad, Empty, Ok };

enum class TargetEdit : uint8_t {
  Ok = 0,
  BadKey,
  Duplicate,
  Full,
  NotFound,
  Ambiguous,
  BadMode,
};

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

static inline bool pollStatusHoursValid(int hours) {
  return hours == 1 || hours == 3 || hours == 6 || hours == 12 || hours == 24;
}

static inline bool pollUrlHasScheme(const char* url) {
  if (!url) return false;
  return strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0;
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
// the build's runtime slot count).
static inline bool pollCanEnable(const char* url, int slot, int consent, int slot_max) {
  if (!pollUrlHasScheme(url) || url[0] == '\0') return false;
  if (slot < 1 || slot > slot_max) return false;
  if (consent == 0) return false;
  return true;
}

static inline const char* pwModeName(uint8_t mode) {
  switch (mode) {
    case MQTT_POLL_PW_BLANK: return "blank";
    case MQTT_POLL_PW_HELLO: return "hello";
    default: return "auto";
  }
}

static inline bool parsePwMode(const char* text, uint8_t* mode) {
  if (!text || !mode) return false;
  size_t len = strlen(text);
  while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\r' || text[len - 1] == '\n')) {
    len--;
  }
  if (len == 4 && memcmp(text, "auto", 4) == 0) { *mode = MQTT_POLL_PW_AUTO; return true; }
  if (len == 5 && memcmp(text, "blank", 5) == 0) { *mode = MQTT_POLL_PW_BLANK; return true; }
  if (len == 5 && memcmp(text, "hello", 5) == 0) { *mode = MQTT_POLL_PW_HELLO; return true; }
  return false;
}

static inline bool packTargets(uint8_t targets[][MQTT_POLL_PUBKEY_SIZE], uint8_t* pw) {
  if (!targets || !pw) return false;
  uint8_t packed[MQTT_POLL_MAX_TARGETS][MQTT_POLL_PUBKEY_SIZE];
  uint8_t packed_pw[MQTT_POLL_MAX_TARGETS];
  memset(packed, 0, sizeof(packed));
  memset(packed_pw, 0, sizeof(packed_pw));
  int w = 0;
  for (int r = 0; r < MQTT_POLL_MAX_TARGETS; r++) {
    if (targetIsEmpty(targets[r])) continue;
    memcpy(packed[w], targets[r], MQTT_POLL_PUBKEY_SIZE);
    packed_pw[w] = pw[r];
    w++;
  }
  const bool moved = memcmp(targets, packed, sizeof(packed)) != 0 ||
                     memcmp(pw, packed_pw, sizeof(packed_pw)) != 0;
  memcpy(targets, packed, sizeof(packed));
  memcpy(pw, packed_pw, sizeof(packed_pw));
  return moved;
}

// Full 64-hex key only. Fills the first hole and resets that entry's pw mode.
static inline TargetEdit addTarget(uint8_t targets[][MQTT_POLL_PUBKEY_SIZE], uint8_t* pw,
                                   const char* hex) {
  if (!targets || !pw) return TargetEdit::BadKey;
  uint8_t key[MQTT_POLL_PUBKEY_SIZE];
  if (parseTargetHex(hex, key, nullptr) != TargetHex::Ok) return TargetEdit::BadKey;
  int slot = -1;
  for (int i = 0; i < MQTT_POLL_MAX_TARGETS; i++) {
    if (targetIsEmpty(targets[i])) {
      if (slot < 0) slot = i;
      continue;
    }
    if (memcmp(targets[i], key, MQTT_POLL_PUBKEY_SIZE) == 0) return TargetEdit::Duplicate;
  }
  if (slot < 0) return TargetEdit::Full;
  memcpy(targets[slot], key, MQTT_POLL_PUBKEY_SIZE);
  pw[slot] = MQTT_POLL_PW_AUTO;
  return TargetEdit::Ok;
}

// 64-hex key or a unique hex prefix of one configured target.
static inline TargetEdit findTargetPrefix(const uint8_t targets[][MQTT_POLL_PUBKEY_SIZE],
                                          const char* prefix, int* index) {
  if (index) *index = -1;
  if (!targets || !prefix || prefix[0] == '\0') return TargetEdit::BadKey;
  size_t n = strlen(prefix);
  if (n > static_cast<size_t>(MQTT_POLL_PUBKEY_SIZE * 2)) return TargetEdit::BadKey;
  for (size_t i = 0; i < n; i++) {
    if (hexValue(prefix[i]) < 0) return TargetEdit::BadKey;
  }
  int found = -1;
  char formatted[65];
  for (int i = 0; i < MQTT_POLL_MAX_TARGETS; i++) {
    if (targetIsEmpty(targets[i])) continue;
    formatTargetHex(targets[i], formatted);
    bool match = true;
    for (size_t k = 0; k < n; k++) {
      char c = prefix[k];
      if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
      if (c != formatted[k]) { match = false; break; }
    }
    if (!match) continue;
    if (found >= 0) return TargetEdit::Ambiguous;
    found = i;
  }
  if (found < 0) return TargetEdit::NotFound;
  if (index) *index = found;
  return TargetEdit::Ok;
}

static inline TargetEdit removeTarget(uint8_t targets[][MQTT_POLL_PUBKEY_SIZE], uint8_t* pw,
                                      const char* hex_or_prefix) {
  if (!targets || !pw) return TargetEdit::BadKey;
  int index = -1;
  const TargetEdit found = findTargetPrefix(targets, hex_or_prefix, &index);
  if (found != TargetEdit::Ok) return found;
  memset(targets[index], 0, MQTT_POLL_PUBKEY_SIZE);
  pw[index] = MQTT_POLL_PW_AUTO;
  packTargets(targets, pw);
  return TargetEdit::Ok;
}

static inline void clearTargets(uint8_t targets[][MQTT_POLL_PUBKEY_SIZE], uint8_t* pw) {
  if (targets) memset(targets, 0, MQTT_POLL_MAX_TARGETS * MQTT_POLL_PUBKEY_SIZE);
  if (pw) memset(pw, 0, MQTT_POLL_MAX_TARGETS);
}

static inline TargetEdit setTargetPwMode(uint8_t targets[][MQTT_POLL_PUBKEY_SIZE], uint8_t* pw,
                                         const char* hex_or_prefix, uint8_t mode) {
  if (!targets || !pw) return TargetEdit::BadKey;
  if (mode > MQTT_POLL_PW_HELLO) return TargetEdit::BadMode;
  int index = -1;
  const TargetEdit found = findTargetPrefix(targets, hex_or_prefix, &index);
  if (found != TargetEdit::Ok) return found;
  pw[index] = mode;
  return TargetEdit::Ok;
}

// Spaced form matches `1: C8C8C858 (auto), 2: ...`. Eight fully spaced entries
// do not fit a 160-byte CLI reply, so a tighter join is used when needed.
static inline bool formatTargetList(const uint8_t targets[][MQTT_POLL_PUBKEY_SIZE],
                                    const uint8_t* pw, char* buf, size_t buflen) {
  if (!buf || buflen == 0) return false;
  buf[0] = '\0';
  if (!targets || !pw) return false;

  int slots[MQTT_POLL_MAX_TARGETS];
  int count = 0;
  for (int i = 0; i < MQTT_POLL_MAX_TARGETS; i++) {
    if (!targetIsEmpty(targets[i])) slots[count++] = i;
  }
  if (count == 0) {
    if (buflen < 7) return false;
    memcpy(buf, "> none", 7);
    return true;
  }

  char body[192];
  for (int tight = 0; tight < 2; tight++) {
    size_t used = 0;
    body[0] = '\0';
    auto append = [&](const char* s) -> bool {
      const size_t n = strlen(s);
      if (used + n >= sizeof(body)) return false;
      memcpy(body + used, s, n);
      used += n;
      body[used] = '\0';
      return true;
    };
    if (!append("> ")) return false;
    bool ok = true;
    for (int n = 0; n < count; n++) {
      const int slot = slots[n];
      char prefix[9];
      char formatted[65];
      formatTargetHex(targets[slot], formatted);
      for (int c = 0; c < 8; c++) {
        char h = formatted[c];
        if (h >= 'a' && h <= 'f') h = static_cast<char>(h - 'a' + 'A');
        prefix[c] = h;
      }
      prefix[8] = '\0';
      char piece[32];
      if (tight) {
        snprintf(piece, sizeof(piece), "%d:%s (%s)", n + 1, prefix, pwModeName(pw[slot]));
      } else {
        snprintf(piece, sizeof(piece), "%d: %s (%s)", n + 1, prefix, pwModeName(pw[slot]));
      }
      if (n > 0 && !append(tight ? "," : ", ")) { ok = false; break; }
      if (!append(piece)) { ok = false; break; }
    }
    if (!ok) continue;
    if (used + 1 <= buflen) {
      memcpy(buf, body, used + 1);
      return true;
    }
  }
  return false;
}

}  // namespace poll
