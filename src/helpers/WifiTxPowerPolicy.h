#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The single mapping between the stored `wifi.txpower` preference, its CLI
// form, and the quarter-dBm argument of esp_wifi_set_max_tx_power().
//
// Stored values are whole dBm, with 0 meaning "use the build's
// MQTT_WIFI_TX_POWER". Every write saves this field, so storing the build value
// itself would pin it on every node and a later firmware could no longer move a
// board's default. Keep 0 as the sentinel; never renumber it.
//
// The radio quantizes the request differently per chip and IDF version, so no
// table lives here: callers read back esp_wifi_get_max_tx_power() and report
// what was actually applied.
namespace WifiTxPowerPolicy {

static const uint8_t kBoardDefault = 0;
static const uint8_t kMinDbm = 2;
static const uint8_t kMaxDbm = 20;
// esp_wifi_set_max_tx_power() rejects anything outside [8, 84].
static const int8_t kMinQdbm = 8;
static const int8_t kMaxQdbm = 84;
// Arduino's WIFI_POWER_11dBm, used when a build sets no MQTT_WIFI_TX_POWER.
static const int8_t kFallbackQdbm = 44;

static inline bool isValidStored(int32_t stored) {
  return stored == kBoardDefault || (stored >= kMinDbm && stored <= kMaxDbm);
}

// A build default outside the SDK range (e.g. WIFI_POWER_MINUS_1dBm) would be
// rejected outright and leave the radio at full power, so clamp it instead.
static inline int8_t clampQdbm(int32_t qdbm) {
  if (qdbm < kMinQdbm) return kMinQdbm;
  if (qdbm > kMaxQdbm) return kMaxQdbm;
  return (int8_t)qdbm;
}

// Corrupt stored values read as the board default rather than as some power.
static inline int8_t qdbmFor(uint8_t stored, int32_t build_default_qdbm) {
  if (stored == kBoardDefault || !isValidStored(stored)) {
    return clampQdbm(build_default_qdbm);
  }
  return (int8_t)(stored * 4);
}

// Accepts `default`, `0` or a whole number of dBm in [kMinDbm, kMaxDbm],
// optionally followed by trailing text (the observer setters take the rest of
// the command line). Returns false and leaves *out untouched for anything else.
static inline bool parse(const char* value, uint8_t* out) {
  if (value == nullptr || out == nullptr) return false;
  if (strncmp(value, "default", 7) == 0 && (value[7] == '\0' || value[7] == ' ')) {
    *out = kBoardDefault;
    return true;
  }
  if (value[0] < '0' || value[0] > '9') return false;
  char* end = nullptr;
  long dbm = strtol(value, &end, 10);
  if (end == value || (*end != '\0' && *end != ' ')) return false;
  if (!isValidStored(dbm)) return false;
  *out = (uint8_t)dbm;
  return true;
}

// "11", "16.5", "8.25": quarter-dBm as the shortest exact decimal.
static inline void formatQdbm(char* out, size_t out_size, int8_t qdbm) {
  if (out == nullptr || out_size == 0) return;
  const int whole = qdbm / 4;
  switch (qdbm % 4) {
    case 1:  snprintf(out, out_size, "%d.25", whole); break;
    case 2:  snprintf(out, out_size, "%d.5", whole); break;
    case 3:  snprintf(out, out_size, "%d.75", whole); break;
    default: snprintf(out, out_size, "%d", whole); break;
  }
}

} // namespace WifiTxPowerPolicy
