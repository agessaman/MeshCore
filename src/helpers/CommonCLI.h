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
  uint8_t flood_max_unscoped;
  uint8_t flood_max_advert;
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

  uint8_t loop_detect;

  // Restored from upstream (dropped by the 22eb9b87 revert). Persisted at the same
  // /com_prefs offsets upstream uses (293, 294) so the file stays upstream-aligned.
  uint8_t radio_fem_rxgain;  // LoRa FEM RX-gain (LNA); default on. Hardware driving is
                             // wired per-board in the FEM-restore change; persisted here.
  uint8_t cad_enabled;       // hardware Channel Activity Detection before TX; default off

  // NOTE: observer settings (MQTT/WiFi/timezone/SNMP/alert) were moved out of
  // NodePrefs into MQTTPrefs (persisted to /mqtt_prefs) so this struct stays
  // aligned with upstream. See struct MQTTPrefs below.
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

  // Per-slot extended fields
  char mqtt_slot_token[MAX_MQTT_SLOTS][48];    // Per-slot token (e.g., MeshRank account token)
  char mqtt_slot_topic[MAX_MQTT_SLOTS][96];    // Per-slot custom topic template (custom preset only)
  char mqtt_slot_audience[MAX_MQTT_SLOTS][64];  // JWT audience (non-empty enables JWT auth for custom slots)

  uint8_t mqtt_rx_enabled;       // Enable RX packet uplinking (default: on)
  char mqtt_ntp_server[64];      // Custom NTP server; empty = pool.ntp.org

  // Observer non-MQTT settings (moved out of NodePrefs so this file stays aligned
  // with upstream). New fields are appended here so a shorter /mqtt_prefs payload
  // from an earlier v1 firmware still loads; the missing tail keeps its default.
  uint8_t snmp_enabled;            // boolean
  char snmp_community[24];         // community string (default "public")
  uint8_t radio_watchdog_minutes;  // 0=disabled, 1-120 minutes (observer-only radio recovery)
  uint8_t alert_enabled;           // 0 = off (default)
  char alert_psk_hex[33];          // 32 hex chars + null; empty = alerts disabled
  uint16_t alert_wifi_minutes;     // WiFi-down threshold (0 = disabled), default 30
  uint16_t alert_mqtt_minutes;     // MQTT-down threshold (0 = disabled), default 240
  uint16_t alert_min_interval_min; // min minutes between same-fault alerts, default 60
  char alert_hashtag[24];          // readback for `get alert.hashtag` (legacy width — see alert_hashtag_ext)
  char alert_region[31];           // optional region override; empty = default_scope

  // Wider hashtag readback, appended (fields above are at frozen offsets).
  // Sized to match the companion app's 31-char channel-name limit (incl '#').
  // When set, this is authoritative; alert_hashtag keeps a truncated mirror so
  // older firmware still shows something sensible after a downgrade.
  char alert_hashtag_ext[32];
};

// /mqtt_prefs is written with an 8-byte header so the format is self-describing.
// Files with no header are legacy (versionless) and detected by size in loadMQTTPrefs.
// The magic leads with a non-ASCII byte so it can never collide with the first
// bytes of a legacy file, whose payload starts with the mqtt_origin string.
static const uint8_t MQTT_PREFS_MAGIC[4] = {0xF5, 'M', 'Q', 'P'};
static const uint16_t MQTT_PREFS_VERSION = 1;  // bump when the MQTTPrefs payload layout changes incompatibly

struct MQTTPrefsHeader {
  uint8_t  magic[4];    // MQTT_PREFS_MAGIC
  uint16_t version;     // MQTT_PREFS_VERSION
  uint16_t payload_len; // sizeof(MQTTPrefs) at write time (sanity / forward-compat)
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

// Versionless 6-slot layout as shipped on mqtt-bridge-implementation-flex (the
// several-thousand-device deployed fleet). This is the current MQTTPrefs minus the
// observer tail, and it still carries the now-removed `_legacy_*` fields mid-struct.
// loadMQTTPrefs reads a headerless file of this size into this struct, then
// field-copies (dropping `_legacy_*`) into the compact versioned MQTTPrefs.
struct Legacy6SlotMQTTPrefs {
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
  char mqtt_slot_preset[MAX_MQTT_SLOTS][24];
  char mqtt_slot_host[MAX_MQTT_SLOTS][64];
  uint16_t mqtt_slot_port[MAX_MQTT_SLOTS];
  char mqtt_slot_username[MAX_MQTT_SLOTS][32];
  char mqtt_slot_password[MAX_MQTT_SLOTS][64];
  char mqtt_owner_public_key[65];
  char mqtt_email[64];
  uint8_t _legacy_analyzer_us_enabled;
  uint8_t _legacy_analyzer_eu_enabled;
  char _legacy_mqtt_server[64];
  uint16_t _legacy_mqtt_port;
  char _legacy_mqtt_username[32];
  char _legacy_mqtt_password[64];
  char mqtt_slot_token[MAX_MQTT_SLOTS][48];
  char mqtt_slot_topic[MAX_MQTT_SLOTS][96];
  char mqtt_slot_audience[MAX_MQTT_SLOTS][64];
  uint8_t mqtt_rx_enabled;
  char mqtt_ntp_server[64];
};

// The legacy layouts above describe files already written to the deployed fleet's
// flash, so their sizes are frozen forever — loadMQTTPrefs() tells the eras apart
// by file size and reads each file as a raw struct dump. These asserts pin the
// layouts on every target toolchain; if one fires, the compiler (or an edit to a
// legacy struct or MAX_MQTT_SLOTS) has changed a layout and fleet files would be
// read at wrong offsets.
static_assert(sizeof(MQTTPrefsHeader) == 8, "versioned /mqtt_prefs header must stay 8 bytes");
static_assert(sizeof(OldMQTTPrefs) == 472, "frozen pre-slot /mqtt_prefs layout changed");
static_assert(sizeof(ThreeSlotMQTTPrefs) == 1464, "frozen 3-slot /mqtt_prefs layout changed");
static_assert(sizeof(Legacy6SlotMQTTPrefs) == 2904, "frozen deployed-fleet /mqtt_prefs layout changed");

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

  // Schedule a pull-OTA firmware update to run shortly (from the app loop), after
  // the "Beginning update..." CLI reply has been transmitted. Deferred because the
  // flash blocks the loop and then reboots, so it can't run inline with the reply.
  // Returns true if scheduled. Default: not supported.
  virtual bool beginDeferredOtaUpdate() {
    return false;
  };

  virtual int getQueueSize() {
    return 0; // no op by default
  };

  virtual bool syncMqttNtp() {
    return false; // WITH_MQTT_BRIDGE builds override
  };

  virtual bool isMqttBridgeRunning() {
    return false;
  };

  // Probe all configured NTP servers for connectivity (verbose=serial console gets a
  // detailed table; otherwise reply gets a compact "<server> ok|fail" list).
  virtual bool runMqttNtpDiag(char* reply, size_t reply_size, bool verbose) {
    return false; // WITH_MQTT_BRIDGE builds override
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
  LegacyObserverTail _legacy_tail;
  // /mqtt_prefs carries a version newer than this firmware understands (a downgrade).
  // The in-memory prefs run on defaults and saveMQTTPrefs() must not overwrite the
  // file, or the first `set` command would destroy the newer config.
  bool _mqtt_prefs_hold = false;
#endif
  bool _com_prefs_needs_upgrade = false;  // old-format /com_prefs detected; rewrite once after load

  // Provisioning (/provision defaults package) — implementation lives in the
  // fork-owned CommonCLI_Provision.cpp. _fs is cached by loadPrefs() so the
  // provision command family can reach the filesystem without re-plumbing
  // every handleCommand call site. No persisted struct layout is involved.
  FILESYSTEM* _fs = nullptr;
  bool _prov_capture = false;             // serial 'provision begin' paste-capture active
  bool _prov_capture_got_header = false;  // first non-blank captured line validated
  uint16_t _prov_capture_lines = 0;
  uint32_t _prov_capture_bytes = 0;

  bool handleProvisionCommand(uint32_t sender_timestamp, char* command, char* reply);
  void provisionCaptureLine(const char* line, char* reply);
  void runProvisionFile(uint32_t sender_timestamp, char* reply);

  mesh::RTCClock* getRTCClock() { return _rtc; }
  void savePrefs();
  void loadPrefsInt(FILESYSTEM* _fs, const char* filename);
#ifdef WITH_MQTT_BRIDGE
  void loadMQTTPrefs(FILESYSTEM* fs);
  void saveMQTTPrefs(FILESYSTEM* fs);
#endif

  void handleRegionCmd(char* command, char* reply);
  void handleGetCmd(uint32_t sender_timestamp, char* command, char* reply);
  void handleSetCmd(uint32_t sender_timestamp, char* command, char* reply);

  // Observer/MQTT/WiFi/timezone/alert/SNMP CLI handling lives in the fork-owned
  // CommonCLI_Observer.cpp to keep these branches out of the upstream-tracked
  // CommonCLI.cpp. Each returns true if it recognized (handled) the command, or
  // false to fall through to the base get/set parsing.
  bool handleObserverSetCmd(uint32_t sender_timestamp, const char* config, char* reply);
  // After 'set wifi.ssid/pwd': kick the MQTT bridge so the new credentials take
  // effect immediately (previously WiFi only came up on the next reboot).
  void applyWifiCredsChange(char* reply);
  bool handleObserverGetCmd(uint32_t sender_timestamp, const char* config, char* reply);
  // Observer-only top-level commands (ota check/update, tls.bundletest, alert test)
  // also live in CommonCLI_Observer.cpp; returns true if it handled the command.
  bool handleObserverCommand(uint32_t sender_timestamp, char* command, char* reply);

public:
  CommonCLI(mesh::MainBoard& board, mesh::RTCClock& rtc, SensorManager& sensors, RegionMap& region_map, ClientACL& acl, NodePrefs* prefs, CommonCLICallbacks* callbacks)
      : _board(&board), _rtc(&rtc), _sensors(&sensors), _region_map(&region_map), _acl(&acl), _prefs(prefs), _callbacks(callbacks) { }

  void loadPrefs(FILESYSTEM* _fs);
  void savePrefs(FILESYSTEM* _fs);
  void handleCommand(uint32_t sender_timestamp, char* command, char* reply);

  // Provisioning hooks (see CommonCLI_Provision.cpp / PROVISIONING.md):
  // provisionCaptureActive() — apps must route serial lines straight to
  // handleCommand while true, bypassing app-level command intercepts, so the
  // lines are captured to /provision instead of executed.
  // autoApplyProvisionFile() — boot-time hook; runs /provision with serial
  // privileges unless the /provision_done marker exists. Returns true if the
  // file ran (reply holds the summary) and the caller should reboot.
  bool provisionCaptureActive() const { return _prov_capture; }
  bool autoApplyProvisionFile(char* reply);
  mesh::MainBoard* getBoard() { return _board; }
  uint8_t buildAdvertData(uint8_t node_type, uint8_t* app_data);
#ifdef WITH_MQTT_BRIDGE
  // Observer config (MQTT/WiFi/timezone/SNMP/alert), persisted to /mqtt_prefs.
  // Exposed so the app can hand it to MQTTBridge/AlertReporter, which read these
  // fields directly (they no longer live in NodePrefs).
  MQTTPrefs* getObserverPrefs() const { return const_cast<MQTTPrefs*>(&_mqtt_prefs); }
#endif
};
