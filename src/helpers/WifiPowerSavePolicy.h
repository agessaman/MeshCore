#pragma once

#include <stdint.h>
#include <string.h>

// The single mapping between the stored `wifi.powersave` preference, its CLI
// name, and the IDF power-save mode.
//
// Pure lookup so host tests can hold startup, reconnect, CLI and web config to
// one table: they used to map the same stored value differently, so a node set
// to `min` silently ran with power save off after its first reconnect while
// `get wifi.powersave` still said min.
//
// Stored values are fleet state — never renumber them. The product default is
// `none`, which is a *default* (MQTTDefaults.h), not a reinterpretation of an
// operator's explicit `min`.
namespace WifiPowerSavePolicy {

enum StoredValue : uint8_t {
  kMin  = 0,   // WIFI_PS_MIN_MODEM
  kNone = 1,   // WIFI_PS_NONE  (default)
  kMax  = 2,   // WIFI_PS_MAX_MODEM
};

// Mirrors wifi_ps_type_t. MQTTBridge.cpp static_asserts these against the SDK.
enum Mode : uint8_t {
  kModeNone     = 0,
  kModeMinModem = 1,
  kModeMaxModem = 2,
};

// Anything outside the known range reads as the default rather than as the
// lowest-numbered mode, so a corrupt byte cannot silently enable modem sleep.
static inline Mode modeFor(uint8_t stored) {
  switch (stored) {
    case kMin:  return kModeMinModem;
    case kMax:  return kModeMaxModem;
    case kNone: return kModeNone;
    default:    return kModeNone;
  }
}

static inline const char* nameFor(uint8_t stored) {
  switch (stored) {
    case kMin:  return "min";
    case kMax:  return "max";
    case kNone: return "none";
    default:    return "none";
  }
}

// Parses a CLI argument, which may be followed by trailing text (the observer
// setters take the rest of the command line). Returns false and leaves *out
// untouched for anything else.
static inline bool parseName(const char* value, uint8_t* out) {
  if (value == nullptr || out == nullptr) return false;
  static const struct { const char* name; uint8_t stored; } kNames[] = {
    { "min",  kMin  },
    { "none", kNone },
    { "max",  kMax  },
  };
  for (unsigned i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
    const size_t len = strlen(kNames[i].name);
    if (strncmp(value, kNames[i].name, len) == 0 &&
        (value[len] == '\0' || value[len] == ' ')) {
      *out = kNames[i].stored;
      return true;
    }
  }
  return false;
}

} // namespace WifiPowerSavePolicy
