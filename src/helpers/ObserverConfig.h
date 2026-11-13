#pragma once

// Observer configuration (MQTT/WiFi/timezone/SNMP/alert) persisted to /mqtt.json,
// plus the observer CLI that edits it. Independent of any firmware's NodePrefs so
// repeater, room server and companion builds share one implementation, and one
// /mqtt.json format, which is what lets a node be reflashed between roles.

#ifdef WITH_MQTT_BRIDGE

#include <Mesh.h>
#include <helpers/IdentityStore.h>     // FILESYSTEM
#include <helpers/MQTTPrefsStorage.h>
#include <stddef.h>
#include <stdint.h>

namespace MQTTPrefsAtomicStore {
class LegacyUpgradeGate;
}

// Observer settings captured from the trailing block of an old-format /com_prefs
// (fork firmware that predates the NodePrefs -> MQTTPrefs split). loadPrefsInt()
// fills this in when it detects the old file layout; loadMQTTPrefs() then applies
// the values one-time if the loaded /mqtt_prefs predates the appended observer
// fields, so SNMP/watchdog/alert config survives the firmware upgrade.
struct LegacyObserverTail {
  bool valid = false;
  uint8_t snmp_enabled;
  char snmp_community[24];
  uint8_t radio_watchdog_minutes;
  uint8_t alert_enabled;
  char alert_psk_hex[33];
  uint16_t alert_wifi_minutes;
  uint16_t alert_mqtt_minutes;
  uint16_t alert_min_interval_min;
  char alert_hashtag[24];
  char alert_region[31];
};

// Host hooks the observer CLI needs. Defaults are "not supported".
class ObserverCallbacks {
public:
  virtual ~ObserverCallbacks() {}

  virtual const char* getObserverNodeName() = 0;
  // Persist the host's own prefs (ota_channel lives there).
  virtual void saveNodePrefs() = 0;
  virtual bool saveObserverPrefs() = 0;
  virtual const char* getFirmwareVer() = 0;

  virtual void restartBridge() {}
  virtual void restartBridgeSlot(int slot) { (void)slot; restartBridge(); }
  virtual bool beginDeferredOtaUpdate() { return false; }
  virtual bool isDeferredOtaUpdatePending() { return false; }
  virtual bool syncMqttNtp() { return false; }
  virtual bool isMqttBridgeRunning() { return false; }
  virtual bool startWebConfig(bool force_ap, char* reply) { (void)force_ap; (void)reply; return false; }
  virtual bool stopWebConfig(char* reply) { (void)reply; return false; }
  virtual bool runMqttNtpDiag(char* reply, size_t reply_size, bool verbose) {
    (void)reply; (void)reply_size; (void)verbose;
    return false;
  }
  virtual void onAlertConfigChanged() {}
  virtual bool sendAlertText(const char* text) { (void)text; return false; }
};

class ObserverConfig {
  mesh::MainBoard* _board;
  ObserverCallbacks* _callbacks;
  uint8_t* _ota_channel;
  MQTTPrefs _mqtt_prefs;
  // Points at a per-command snapshot only while an observer setter is running.
  // persistObserverPrefs() uses it to undo RAM mutations when flash commit fails.
  const MQTTPrefs* _observer_prefs_rollback = nullptr;
  LegacyObserverTail _legacy_tail;
  // /mqtt.json is newer, corrupt, or temporarily unreadable. The in-memory prefs
  // run on defaults and saveMQTTPrefs() must not overwrite the source file.
  bool _mqtt_prefs_hold = false;
  // A failed publish could not be undone, so the next boot may still come up
  // with the value that was refused. persistObserverPrefs() must not call that
  // a rollback. Latched for the boot: the artifact left behind also makes every
  // later transaction fail to begin, so the condition cannot clear itself.
  bool _observer_save_indeterminate = false;

  bool persistObserverPrefs(char* reply);

public:
  // ota_channel: the host's persisted OTA release channel selector.
  ObserverConfig(mesh::MainBoard* board, ObserverCallbacks* callbacks, uint8_t* ota_channel)
      : _board(board), _callbacks(callbacks), _ota_channel(ota_channel) { }

  void loadMQTTPrefs(FILESYSTEM* fs, MQTTPrefsAtomicStore::LegacyUpgradeGate* legacy_upgrade);
  bool saveMQTTPrefs(FILESYSTEM* fs);

  MQTTPrefs* prefs() { return &_mqtt_prefs; }
  const MQTTPrefs* prefs() const { return &_mqtt_prefs; }
  bool isHeld() const { return _mqtt_prefs_hold; }
  LegacyObserverTail& legacyTail() { return _legacy_tail; }

  // Persist a change the host already made to prefs(). On failure the prefs are
  // restored from `rollback` and `reply` explains whether flash was undone too.
  bool persistWithRollback(const MQTTPrefs* rollback, char* reply);

  // Each returns true if it recognized (handled) the command, or false to fall
  // through to the host's own parser.
  bool handleSetCmd(uint32_t sender_timestamp, const char* config, char* reply);
  bool handleGetCmd(uint32_t sender_timestamp, const char* config, char* reply);
  bool handleCommand(uint32_t sender_timestamp, char* command, char* reply);
};

#endif  // WITH_MQTT_BRIDGE
