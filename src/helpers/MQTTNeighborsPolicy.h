#pragma once

#include "MQTTPresetPolicy.h"
#include "MQTTPrefsStorage.h"

#include <stdint.h>
#include <string.h>

// mqtt.neighbors modes, stored in MQTTPrefs::mqtt_neighbors_enabled.
//
// 0 was "off" before auto existed. It is reused as auto so the existing fleet,
// which never touched the setting, picks up per-preset collection on upgrade.
// Explicit off moves to 2, which older firmware repairs to 0 — off there too —
// so a rollback never turns collection on.
enum MQTTNeighborsMode : uint8_t {
  MQTT_NEIGHBORS_AUTO = 0,  // collect and publish only for presets that opt in
  MQTT_NEIGHBORS_ON = 1,    // collect, and publish to every connected slot
  MQTT_NEIGHBORS_OFF = 2,   // no periodic collection; manual discover.scopes still publishes
};

static const uint8_t MQTT_NEIGHBORS_MODE_MAX = MQTT_NEIGHBORS_OFF;

static inline const char* mqttNeighborsModeName(uint8_t mode) {
  switch (mode) {
    case MQTT_NEIGHBORS_AUTO: return "auto";
    case MQTT_NEIGHBORS_ON: return "on";
    case MQTT_NEIGHBORS_OFF: return "off";
    default: return "auto";
  }
}

// Parses "auto", "on", or "off", allowing trailing whitespace. Returns false
// and leaves *mode alone for anything else.
static inline bool mqttNeighborsParseMode(const char* text, uint8_t* mode) {
  if (!text || !mode) return false;
  size_t len = strlen(text);
  while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\r' || text[len - 1] == '\n')) len--;
  if (len == 4 && memcmp(text, "auto", 4) == 0) { *mode = MQTT_NEIGHBORS_AUTO; return true; }
  if (len == 2 && memcmp(text, "on", 2) == 0)   { *mode = MQTT_NEIGHBORS_ON; return true; }
  if (len == 3 && memcmp(text, "off", 3) == 0)  { *mode = MQTT_NEIGHBORS_OFF; return true; }
  return false;
}

// Whether a periodic snapshot goes to a slot with this preset (nullptr = custom).
// Off has no periodic snapshots; manual ones bypass this and reach every slot.
static inline bool mqttNeighborsSlotSelected(uint8_t mode, const MQTTPresetDef* preset) {
  if (mode == MQTT_NEIGHBORS_ON) return true;
  if (mode == MQTT_NEIGHBORS_AUTO) return mqttPresetWantsNeighbors(preset);
  return false;
}

// Whether the periodic neighbor cycle should run, from the stored slot presets.
static inline bool mqttNeighborsCollectionEnabled(const MQTTPrefs* prefs, int slot_count) {
  if (!prefs) return false;
  if (prefs->mqtt_neighbors_enabled == MQTT_NEIGHBORS_ON) return true;
  if (prefs->mqtt_neighbors_enabled != MQTT_NEIGHBORS_AUTO) return false;
  if (slot_count > MQTT_PREFS_SLOT_COUNT) slot_count = MQTT_PREFS_SLOT_COUNT;
  for (int i = 0; i < slot_count; i++) {
    if (mqttPresetNameWantsNeighbors(prefs->mqtt_slot_preset[i])) return true;
  }
  return false;
}
