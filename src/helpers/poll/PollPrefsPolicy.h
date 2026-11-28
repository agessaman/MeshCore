#pragma once

// Pure checks for repeater-poll settings. Shared by /mqtt.json repair and the
// poll CLI so a value accepted on one path is accepted on the other. No Arduino.

#include "PollPrefsValidation.h"
#include "PollTypes.h"
#include "PollWipe.h"

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
// Custom passwords are prefs-only: PasswordMode has no value for mode 3 yet.
static_assert(MQTT_POLL_PW_CUSTOM > MQTT_POLL_PW_HELLO, "poll custom pw mode drift");

enum class TargetEdit : uint8_t {
  Ok = 0,
  BadKey,
  Duplicate,
  Full,
  NotFound,
  Ambiguous,
  BadMode,
  BadPassword,
};

static inline const char* pwModeName(uint8_t mode) {
  switch (mode) {
    case MQTT_POLL_PW_BLANK: return "blank";
    case MQTT_POLL_PW_HELLO: return "hello";
    case MQTT_POLL_PW_CUSTOM: return "custom";
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

// Passwords move with their targets; an empty entry keeps none.
static inline bool packTargets(uint8_t targets[][MQTT_POLL_PUBKEY_SIZE], uint8_t* pw,
                               char pass[][kPwBufSize]) {
  if (!targets || !pw || !pass) return false;
  uint8_t packed[MQTT_POLL_MAX_TARGETS][MQTT_POLL_PUBKEY_SIZE];
  uint8_t packed_pw[MQTT_POLL_MAX_TARGETS];
  char packed_pass[MQTT_POLL_MAX_TARGETS][kPwBufSize];
  memset(packed, 0, sizeof(packed));
  memset(packed_pw, 0, sizeof(packed_pw));
  memset(packed_pass, 0, sizeof(packed_pass));
  int w = 0;
  for (int r = 0; r < MQTT_POLL_MAX_TARGETS; r++) {
    if (targetIsEmpty(targets[r])) continue;
    memcpy(packed[w], targets[r], MQTT_POLL_PUBKEY_SIZE);
    packed_pw[w] = pw[r];
    memcpy(packed_pass[w], pass[r], kPwBufSize);
    w++;
  }
  const bool moved = memcmp(targets, packed, sizeof(packed)) != 0 ||
                     memcmp(pw, packed_pw, sizeof(packed_pw)) != 0 ||
                     memcmp(pass, packed_pass, sizeof(packed_pass)) != 0;
  memcpy(targets, packed, sizeof(packed));
  memcpy(pw, packed_pw, sizeof(packed_pw));
  memcpy(pass, packed_pass, sizeof(packed_pass));
  detail::wipe(packed_pass, sizeof(packed_pass));
  return moved;
}

// Full 64-hex key only. Fills the first hole and resets that entry's pw mode.
static inline TargetEdit addTarget(uint8_t targets[][MQTT_POLL_PUBKEY_SIZE], uint8_t* pw,
                                   char pass[][kPwBufSize], const char* hex) {
  if (!targets || !pw || !pass) return TargetEdit::BadKey;
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
  memset(pass[slot], 0, kPwBufSize);
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
                                      char pass[][kPwBufSize], const char* hex_or_prefix) {
  if (!targets || !pw || !pass) return TargetEdit::BadKey;
  int index = -1;
  const TargetEdit found = findTargetPrefix(targets, hex_or_prefix, &index);
  if (found != TargetEdit::Ok) return found;
  memset(targets[index], 0, MQTT_POLL_PUBKEY_SIZE);
  pw[index] = MQTT_POLL_PW_AUTO;
  memset(pass[index], 0, kPwBufSize);
  packTargets(targets, pw, pass);
  return TargetEdit::Ok;
}

static inline void clearTargets(uint8_t targets[][MQTT_POLL_PUBKEY_SIZE], uint8_t* pw,
                                char pass[][kPwBufSize]) {
  if (targets) memset(targets, 0, MQTT_POLL_MAX_TARGETS * MQTT_POLL_PUBKEY_SIZE);
  if (pw) memset(pw, 0, MQTT_POLL_MAX_TARGETS);
  if (pass) memset(pass, 0, MQTT_POLL_MAX_TARGETS * kPwBufSize);
}

// auto, blank, or hello only; any stored custom password is zeroed.
static inline TargetEdit setTargetPwMode(uint8_t targets[][MQTT_POLL_PUBKEY_SIZE], uint8_t* pw,
                                         char pass[][kPwBufSize], const char* hex_or_prefix,
                                         uint8_t mode) {
  if (!targets || !pw || !pass) return TargetEdit::BadKey;
  if (mode > MQTT_POLL_PW_HELLO) return TargetEdit::BadMode;
  int index = -1;
  const TargetEdit found = findTargetPrefix(targets, hex_or_prefix, &index);
  if (found != TargetEdit::Ok) return found;
  pw[index] = mode;
  memset(pass[index], 0, kPwBufSize);
  return TargetEdit::Ok;
}

// Stores a custom password and switches the target to custom mode.
static inline TargetEdit setTargetPassword(uint8_t targets[][MQTT_POLL_PUBKEY_SIZE], uint8_t* pw,
                                           char pass[][kPwBufSize], const char* hex_or_prefix,
                                           const char* password) {
  if (!targets || !pw || !pass) return TargetEdit::BadKey;
  if (!pollPasswordValid(password)) return TargetEdit::BadPassword;
  int index = -1;
  const TargetEdit found = findTargetPrefix(targets, hex_or_prefix, &index);
  if (found != TargetEdit::Ok) return found;
  memset(pass[index], 0, kPwBufSize);
  memcpy(pass[index], password, strlen(password));
  pw[index] = MQTT_POLL_PW_CUSTOM;
  return TargetEdit::Ok;
}

// Drops a target's custom password (back to auto); false if it had none.
static inline bool discardTargetPassword(uint8_t* pw, char pass[][kPwBufSize], int index) {
  if (!pw || !pass || index < 0 || index >= MQTT_POLL_MAX_TARGETS) return false;
  const bool had = pw[index] == MQTT_POLL_PW_CUSTOM;
  if (had) pw[index] = MQTT_POLL_PW_AUTO;
  memset(pass[index], 0, kPwBufSize);
  return had;
}

// Spaced form matches `1: C8C8C858 (auto), 2: ...`. Eight fully spaced entries
// do not fit a 160-byte CLI reply, so tighter joins are used when needed.
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
  for (int tight = 0; tight < 3; tight++) {
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
      if (tight == 2) {
        snprintf(piece, sizeof(piece), "%d:%s(%s)", n + 1, prefix, pwModeName(pw[slot]));
      } else if (tight) {
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
