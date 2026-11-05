#pragma once

#include "MQTTPresetPolicy.h"
#include "MQTTPrefsStorage.h"

#include <stdint.h>
#include <string.h>

// mqtt.neighbors modes, held in MQTTPrefs::mqtt_neighbors_enabled. /mqtt.json
// stores the mode under neighbors.mode and keeps neighbors.enabled as the 0/1
// older firmware reads (1 only for on); see NeighborPrefs in
// MQTTPrefsSerializer.h. A file without "mode" maps enabled 0 to auto, so nodes
// that never touched the setting pick up per-preset collection on upgrade.
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

// Whether a snapshot published under this mode goes to a slot with this preset
// (nullptr = custom). Manual snapshots are published as on, reaching every slot.
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
