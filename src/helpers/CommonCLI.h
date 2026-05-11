#pragma once

#include "Mesh.h"
#include <helpers/IdentityStore.h>
#include <helpers/SensorManager.h>
#include <helpers/ClientACL.h>
#include <helpers/MQTTPresets.h>  // For MAX_MQTT_SLOTS (used in NodePrefs struct layout)
#include <helpers/RegionMap.h>

#if defined(WITH_RS232_BRIDGE) || defined(WITH_ESPNOW_BRIDGE) || defined(WITH_MQTT_BRIDGE)
#define WITH_BRIDGE
#endif

#define ADVERT_LOC_NONE       0
#define ADVERT_LOC_SHARE      1
#define ADVERT_LOC_PREFS      2

#define LOOP_DETECT_OFF       0
#define LOOP_DETECT_MINIMAL   1
#define LOOP_DETECT_MODERATE  2
#define LOOP_DETECT_STRICT    3

struct NodePrefs { // persisted to file
  float airtime_factor;
  char node_name[32];
  double node_lat, node_lon;
  char password[16];
  float freq;
  int8_t tx_power_dbm;
  uint8_t disable_fwd;
  uint8_t advert_interval;       // minutes / 2
  uint8_t rx_boosted_gain;       // power settings (file offset 79)
  uint8_t flood_advert_interval; // hours
  float rx_delay_base;
  float tx_delay_factor;
  char guest_password[16];
  float direct_tx_delay_factor;
  uint32_t guard;
  uint8_t sf;
  uint8_t cr;
  uint8_t allow_read_only;
  uint8_t multi_acks;
  float bw;
  uint8_t flood_max;
  uint8_t interference_threshold;
  uint8_t agc_reset_interval; // secs / 4
  uint8_t path_hash_mode;   // which path mode to use when sending
  // Bridge settings
  uint8_t bridge_enabled; // boolean
  uint16_t bridge_delay;  // milliseconds (default 500 ms)
  uint8_t bridge_pkt_src; // 0 = logTx, 1 = logRx (default logRx)
  uint32_t bridge_baud;   // 9600, 19200, 38400, 57600, 115200 (default 115200)
  uint8_t bridge_channel; // 1-14 (ESP-NOW only)
  char bridge_secret[16]; // for XOR encryption of bridge packets (ESP-NOW only)
  // Power setting
  uint8_t powersaving_enabled; // boolean
  // Gps settings
  uint8_t gps_enabled;
  uint32_t gps_interval; // in seconds
  uint8_t advert_loc_policy;
  uint32_t discovery_mod_timestamp;
  float adc_multiplier;
  char owner_info[120];
  // MQTT settings (stored separately in /mqtt_prefs, but kept here for backward compatibility)
  char mqtt_origin[32];     // Device name for MQTT topics
  char mqtt_iata[8];        // IATA code for MQTT topics
  uint8_t mqtt_status_enabled;   // Enable status messages
  uint8_t mqtt_packets_enabled;  // Enable packet messages
  uint8_t mqtt_raw_enabled;      // Enable raw messages
  uint8_t mqtt_tx_enabled;       // TX packet uplinking: 0=off, 1=all, 2=advert (self-originated only)
  uint32_t mqtt_status_interval; // Status publish interval (ms)
  uint8_t mqtt_rx_enabled;       // Enable RX packet uplinking (default: on)

  // WiFi settings
  char wifi_ssid[32];       // WiFi SSID
  char wifi_password[64];  // WiFi password
  uint8_t wifi_power_save; // WiFi power save mode: 0=min, 1=none, 2=max (default: 1=none)
  
  // Timezone settings
  char timezone_string[32]; // Timezone string (e.g., "America/Los_Angeles")
  int8_t timezone_offset;   // Timezone offset in hours (-12 to +14) - fallback
  
  // MQTT slot presets (up to MAX_MQTT_SLOTS, each can be a preset name or "custom"/"none")
  char mqtt_slot_preset[MAX_MQTT_SLOTS][24]; // e.g. "analyzer-us", "meshmapper", "custom", "none"

  // Per-slot custom broker settings (only used when slot preset is "custom")
  char mqtt_slot_host[MAX_MQTT_SLOTS][64];
  uint16_t mqtt_slot_port[MAX_MQTT_SLOTS];
  char mqtt_slot_username[MAX_MQTT_SLOTS][32];
  char mqtt_slot_password[MAX_MQTT_SLOTS][64];

  // Shared MQTT authentication
  char mqtt_owner_public_key[65]; // Owner public key (hex string, same length as repeater public key)
  char mqtt_email[64]; // Owner email address for matching nodes with owners

  // Per-slot extended fields
  char mqtt_slot_token[MAX_MQTT_SLOTS][48];    // Per-slot token (e.g., MeshRank account token)
  char mqtt_slot_topic[MAX_MQTT_SLOTS][96];    // Per-slot custom topic template (custom preset only)
  char mqtt_slot_audience[MAX_MQTT_SLOTS][64]; // JWT audience (non-empty enables JWT auth for custom slots)

  uint8_t loop_detect;

  // SNMP settings (optional, only used when WITH_SNMP is defined)
  uint8_t snmp_enabled;          // boolean: 0=off, 1=on
  char snmp_community[24];       // community string (default "public")
  uint8_t radio_watchdog_minutes; // 0=disabled, 1-120 minutes

  // Fault alert channel (LoRa group-channel "observer status" message on prolonged WiFi/MQTT outage).
  // Sent over the radio (NOT over MQTT) so the alert still works while the MQTT path is broken.
  // All fields are appended at the end of NodePrefs for binary-compatible upgrades.
  uint8_t  alert_enabled;          // 0 = off (default), 1 = on
  char     alert_psk_b64[48];      // base64 PSK; empty = alerts disabled. PUBLIC_GROUP_PSK is rejected.
  uint16_t alert_wifi_minutes;     // WiFi-down threshold in minutes (0 = disabled), default 30
  uint16_t alert_mqtt_minutes;     // MQTT-down threshold in minutes (0 = disabled), default 240 (4 h)
  uint16_t alert_min_interval_min; // min minutes between alerts for the same fault, default 60, floor 60
  // When the operator configures via `set alert.hashtag <name>`, we derive
  // alert_psk_b64 from sha256("#name")[0..15] once and remember the hashtag
  // text here purely for `get alert.hashtag` readback. A subsequent
  // `set alert.psk` clears this field so it doesn't lie about provenance.
  char     alert_hashtag[24];
  // Optional region name (e.g. "us", "eu"); empty = use the repeater's
  // default_scope. Looked up lazily via RegionMap::findByNamePrefix at send
  // time, so the operator can name a region that doesn't exist yet without
  // polluting region_map state. Falls back to default_scope on miss.
  char     alert_region[31];
};

#ifdef WITH_MQTT_BRIDGE
// Old MQTT preferences layout (pre-slot firmware) — used only for migration detection
struct OldMQTTPrefs {
  char mqtt_origin[32];
  char mqtt_iata[8];
  uint8_t mqtt_status_enabled;
  uint8_t mqtt_packets_enabled;
  uint8_t mqtt_raw_enabled;
  uint8_t mqtt_tx_enabled;
  uint32_t mqtt_status_interval;
  char wifi_ssid[32];
  char wifi_password[64];
  uint8_t wifi_power_save;
  char timezone_string[32];
  int8_t timezone_offset;
  char mqtt_server[64];
  uint16_t mqtt_port;
  char mqtt_username[32];
  char mqtt_password[64];
  uint8_t mqtt_analyzer_us_enabled;
  uint8_t mqtt_analyzer_eu_enabled;
  char mqtt_owner_public_key[65];
  char mqtt_email[64];
};

// MQTT preferences stored in separate file to avoid conflicts with upstream NodePrefs changes
struct MQTTPrefs {
  // MQTT settings
  char mqtt_origin[32];     // Device name for MQTT topics
  char mqtt_iata[8];        // IATA code for MQTT topics
  uint8_t mqtt_status_enabled;   // Enable status messages
  uint8_t mqtt_packets_enabled;  // Enable packet messages
  uint8_t mqtt_raw_enabled;      // Enable raw messages
  uint8_t mqtt_tx_enabled;       // Enable TX packet uplinking
  uint32_t mqtt_status_interval; // Status publish interval (ms)

  // WiFi settings
  char wifi_ssid[32];       // WiFi SSID
  char wifi_password[64];  // WiFi password
  uint8_t wifi_power_save; // WiFi power save mode: 0=min, 1=none, 2=max (default: 1=none)

  // Timezone settings
  char timezone_string[32]; // Timezone string (e.g., "America/Los_Angeles")
  int8_t timezone_offset;   // Timezone offset in hours (-12 to +14) - fallback

  // Slot presets (up to MAX_MQTT_SLOTS)
  char mqtt_slot_preset[MAX_MQTT_SLOTS][24]; // e.g. "analyzer-us", "meshmapper", "custom", "none"

  // Per-slot custom broker settings (only used when preset is "custom")
  char mqtt_slot_host[MAX_MQTT_SLOTS][64];
  uint16_t mqtt_slot_port[MAX_MQTT_SLOTS];
  char mqtt_slot_username[MAX_MQTT_SLOTS][32];
  char mqtt_slot_password[MAX_MQTT_SLOTS][64];

  // Shared authentication
  char mqtt_owner_public_key[65]; // Owner public key (hex string)
  char mqtt_email[64]; // Owner email address

  // --- Legacy fields (vestigial, kept for binary compatibility) ---
  // Migration now uses OldMQTTPrefs/ThreeSlotMQTTPrefs structs. These fields are unused
  // but must remain to preserve byte offsets for devices that already saved a new-format /mqtt_prefs file.
  uint8_t _legacy_analyzer_us_enabled;
  uint8_t _legacy_analyzer_eu_enabled;
  char _legacy_mqtt_server[64];
  uint16_t _legacy_mqtt_port;
  char _legacy_mqtt_username[32];
  char _legacy_mqtt_password[64];

  // --- New fields (appended at end for migration safety) ---
  char mqtt_slot_token[MAX_MQTT_SLOTS][48];    // Per-slot token (e.g., MeshRank account token)
  char mqtt_slot_topic[MAX_MQTT_SLOTS][96];    // Per-slot custom topic template (custom preset only)
  char mqtt_slot_audience[MAX_MQTT_SLOTS][64];  // JWT audience (non-empty enables JWT auth for custom slots)

  // --- Appended fields (added after initial 6-slot migration) ---
  uint8_t mqtt_rx_enabled;       // Enable RX packet uplinking (default: on)
};

// 3-slot MQTTPrefs layout — used for migrating from 3-slot to 6-slot format.
// Changing array sizes from [3] to [6] shifts all field offsets, so raw file.read()
// into the new struct would corrupt data. This struct preserves the old binary layout.
struct ThreeSlotMQTTPrefs {
  char mqtt_origin[32];
  char mqtt_iata[8];
  uint8_t mqtt_status_enabled;
  uint8_t mqtt_packets_enabled;
  uint8_t mqtt_raw_enabled;
  uint8_t mqtt_tx_enabled;
  uint32_t mqtt_status_interval;
  char wifi_ssid[32];
  char wifi_password[64];
  uint8_t wifi_power_save;
  char timezone_string[32];
  int8_t timezone_offset;
  char mqtt_slot_preset[3][24];
  char mqtt_slot_host[3][64];
  uint16_t mqtt_slot_port[3];
  char mqtt_slot_username[3][32];
  char mqtt_slot_password[3][64];
  char mqtt_owner_public_key[65];
  char mqtt_email[64];
  uint8_t _legacy_analyzer_us_enabled;
  uint8_t _legacy_analyzer_eu_enabled;
  char _legacy_mqtt_server[64];
  uint16_t _legacy_mqtt_port;
  char _legacy_mqtt_username[32];
  char _legacy_mqtt_password[64];
  char mqtt_slot_token[3][48];
  char mqtt_slot_topic[3][96];
};
#endif

class CommonCLICallbacks {
public:
  virtual void savePrefs() = 0;
  virtual const char* getFirmwareVer() = 0;
  virtual const char* getBuildDate() = 0;
  virtual const char* getRole() = 0;
  virtual bool formatFileSystem() = 0;
  virtual void sendSelfAdvertisement(int delay_millis, bool flood) = 0;
  virtual void updateAdvertTimer() = 0;
  virtual void updateFloodAdvertTimer() = 0;
  virtual void setLoggingOn(bool enable) = 0;
  virtual void eraseLogFile() = 0;
  virtual void dumpLogFile() = 0;
  virtual void setTxPower(int8_t power_dbm) = 0;
  virtual void formatNeighborsReply(char *reply) = 0;
  virtual void removeNeighbor(const uint8_t* pubkey, int key_len) {
    // no op by default
  };
  virtual void formatStatsReply(char *reply) = 0;
  virtual void formatRadioStatsReply(char *reply) = 0;
  virtual void formatRadioDiagReply(char *reply) { strcpy(reply, "Not supported"); }
  virtual void formatPacketStatsReply(char *reply) = 0;
  virtual mesh::LocalIdentity& getSelfId() = 0;
  virtual void saveIdentity(const mesh::LocalIdentity& new_id) = 0;
  virtual void clearStats() = 0;
  virtual void applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) = 0;

  virtual void startRegionsLoad() {
    // no op by default
  }
  virtual bool saveRegions() {
    return false;
  }
  virtual void onDefaultRegionChanged(const RegionEntry* r) {
    // no op by default
  }

  virtual void setBridgeState(bool enable) {
    // no op by default
  };

  virtual void restartBridge() {
    // no op by default
  };

  virtual void restartBridgeSlot(int slot) {
    // Default: fall back to full restart
    restartBridge();
  };

  virtual int getQueueSize() {
    return 0; // no op by default
  };

  virtual void setRxBoostedGain(bool enable) {
    // no op by default
  };

  // Fault-alert channel hooks (see NodePrefs::alert_*). The default no-op
  // implementations keep CLI commands harmless on builds that don't wire up
  // an AlertReporter.
  virtual void onAlertConfigChanged() {
    // no op by default
  }
  virtual bool sendAlertText(const char* /*text*/) {
    return false; // no op by default
  }
  // Resolve the TransportKey scope to use for outgoing fault-alert floods.
  // Implementations should consult NodePrefs::alert_region first (look up via
  // RegionMap), then fall back to the repeater's default_scope, then return
  // false if neither yields a usable key. AlertReporter falls back to an
  // unscoped flood when this returns false.
  virtual bool resolveAlertScope(TransportKey& /*dest*/) {
    return false; // no op by default
  }
};

class CommonCLI {
  mesh::RTCClock* _rtc;
  NodePrefs* _prefs;
  CommonCLICallbacks* _callbacks;
  mesh::MainBoard* _board;
  SensorManager* _sensors;
  RegionMap* _region_map;
  ClientACL* _acl;
  char tmp[PRV_KEY_SIZE*2 + 4];
#ifdef WITH_MQTT_BRIDGE
  MQTTPrefs _mqtt_prefs;
#endif

  mesh::RTCClock* getRTCClock() { return _rtc; }
  void savePrefs();
  void loadPrefsInt(FILESYSTEM* _fs, const char* filename);
#ifdef WITH_MQTT_BRIDGE
  void loadMQTTPrefs(FILESYSTEM* fs);
  void saveMQTTPrefs(FILESYSTEM* fs);
  void syncMQTTPrefsToNodePrefs();
  void syncNodePrefsToMQTTPrefs();
#endif

  void handleRegionCmd(char* command, char* reply);
  void handleGetCmd(uint32_t sender_timestamp, char* command, char* reply);
  void handleSetCmd(uint32_t sender_timestamp, char* command, char* reply);

public:
  CommonCLI(mesh::MainBoard& board, mesh::RTCClock& rtc, SensorManager& sensors, RegionMap& region_map, ClientACL& acl, NodePrefs* prefs, CommonCLICallbacks* callbacks)
      : _board(&board), _rtc(&rtc), _sensors(&sensors), _region_map(&region_map), _acl(&acl), _prefs(prefs), _callbacks(callbacks) { }

  void loadPrefs(FILESYSTEM* _fs);
  void savePrefs(FILESYSTEM* _fs);
  void handleCommand(uint32_t sender_timestamp, char* command, char* reply);
  mesh::MainBoard* getBoard() { return _board; }
  uint8_t buildAdvertData(uint8_t node_type, uint8_t* app_data);
};
