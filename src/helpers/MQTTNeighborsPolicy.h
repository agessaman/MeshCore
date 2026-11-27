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

static inline bool mqttNeighborsSlotConfigured(const char* preset) {
  return preset && preset[0] != '\0' && strcmp(preset, MQTT_PRESET_NONE) != 0;
}

// Whether a snapshot goes to slot `slot`, from the stored prefs. A slot's own
// mqttN.neighbors on/off decides for that slot; auto follows mqtt.neighbors on,
// or in auto the preset opt-in. Global off stops periodic snapshots on every
// slot. Manual discover.scopes snapshots reach every slot not set off.
static inline bool mqttNeighborsSlotWants(const MQTTPrefs* prefs, int slot, bool periodic) {
  if (!prefs || slot < 0 || slot >= MQTT_PREFS_SLOT_COUNT) return false;
  const char* preset = prefs->mqtt_slot_preset[slot];
  if (!mqttNeighborsSlotConfigured(preset)) return false;
  const uint8_t own = prefs->mqtt_slot_neighbors[slot];
  if (own == MQTT_NEIGHBORS_OFF) return false;
  if (!periodic) return true;
  const uint8_t global = prefs->mqtt_neighbors_enabled;
  if (global == MQTT_NEIGHBORS_OFF) return false;
  if (own == MQTT_NEIGHBORS_ON) return true;
  if (own != MQTT_NEIGHBORS_AUTO) return false;  // corrupt: fail safe
  if (global == MQTT_NEIGHBORS_ON) return true;
  return global == MQTT_NEIGHBORS_AUTO && mqttPresetNameWantsNeighbors(preset);
}

// Bit i set = slot i receives the snapshot. Computed on the mesh side and handed
// to the bridge with the snapshot, so the MQTT task never reads prefs.
static inline uint8_t mqttNeighborsSlotMask(const MQTTPrefs* prefs, int slot_count, bool periodic) {
  if (slot_count > MQTT_PREFS_SLOT_COUNT) slot_count = MQTT_PREFS_SLOT_COUNT;
  uint8_t mask = 0;
  for (int i = 0; i < slot_count; i++) {
    if (mqttNeighborsSlotWants(prefs, i, periodic)) mask |= static_cast<uint8_t>(1u << i);
  }
  return mask;
}

// Whether the periodic neighbor cycle should run: some slot would receive it.
static inline bool mqttNeighborsCollectionEnabled(const MQTTPrefs* prefs, int slot_count) {
  return mqttNeighborsSlotMask(prefs, slot_count, true) != 0;
}
