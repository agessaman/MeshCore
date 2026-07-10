#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <RTClib.h>
#include <CayenneLPP.h>
#include <target.h>

#ifndef MESH_ENABLE_RECENT_REPEATERS
  #define MESH_ENABLE_RECENT_REPEATERS  1
#endif

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
#endif

#ifdef WITH_RS232_BRIDGE
#include "helpers/bridges/RS232Bridge.h"
#define WITH_BRIDGE
#endif

#ifdef WITH_ESPNOW_BRIDGE
#include "helpers/bridges/ESPNowBridge.h"
#define WITH_BRIDGE
#endif

#ifdef WITH_MQTT_BRIDGE
#include "helpers/bridges/MQTTBridge.h"
#define WITH_BRIDGE
#endif

#ifdef WITH_SNMP
#include "helpers/SNMPAgent.h"
#endif

#include <helpers/AdvertDataHelpers.h>
#include <helpers/AlertReporter.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/ClientACL.h>
#include <helpers/CommonCLI.h>
#include <helpers/IdentityStore.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/StatsFormatHelper.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/RegionMap.h>
#include "RateLimiter.h"


struct RepeaterStats {
  uint16_t batt_milli_volts;
  uint16_t curr_tx_queue_len;
  int16_t  noise_floor;
  int16_t  last_rssi;
  uint32_t n_packets_recv;
  uint32_t n_packets_sent;
  uint32_t total_air_time_secs;
  uint32_t total_up_time_secs;
  uint32_t n_sent_flood, n_sent_direct;
  uint32_t n_recv_flood, n_recv_direct;
  uint16_t err_events;                // was 'n_full_events'
  int16_t  last_snr;   // x 4
  uint16_t n_direct_dups, n_flood_dups;
  uint32_t total_rx_air_time_secs;
  uint32_t n_recv_errors;
};

#ifndef MAX_CLIENTS
  #define MAX_CLIENTS           32
#endif

struct NeighbourInfo {
  mesh::Identity id;
  uint32_t advert_timestamp;
  uint32_t heard_timestamp;
  int8_t snr; // multiplied by 4, user should divide to get float value
};

#ifndef FIRMWARE_BUILD_DATE
  #define FIRMWARE_BUILD_DATE   "6 Jun 2026"
#endif

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION   "v1.16.0"
#endif

#define FIRMWARE_ROLE "repeater"

#define PACKET_LOG_FILE  "/packet_log"

#ifndef MAX_SCHEDULED_RADIO_SETTINGS_PER_TYPE
  #define MAX_SCHEDULED_RADIO_SETTINGS_PER_TYPE 3
#endif

#define MAX_SCHEDULED_RADIO_SETTINGS (MAX_SCHEDULED_RADIO_SETTINGS_PER_TYPE * 2)

class MyMesh : public mesh::Mesh, public CommonCLICallbacks {
  struct ScheduledRadioSetting {
    bool active;
    bool temporary;
    bool started;
    float freq;
    float bw;
    uint8_t sf;
    uint8_t cr;
    uint32_t start_time;
    uint32_t end_time;
  };

  FILESYSTEM* _fs;
  uint32_t last_millis;
  uint64_t uptime_millis;
  unsigned long next_local_advert, next_flood_advert;
  unsigned long next_battery_alert_check;
  unsigned long last_battery_alert_sent;
  bool battery_alert_sent;
  bool _logging;
  NodePrefs _prefs;
  ClientACL  acl;
  CommonCLI _cli;
  uint8_t reply_data[MAX_PACKET_PAYLOAD];
  uint8_t reply_path[MAX_PATH_SIZE];
  int8_t  reply_path_len;
  uint8_t reply_path_hash_size;
  TransportKeyStore key_store;
  RegionMap region_map, temp_map;
  RegionEntry* load_stack[8];
  RegionEntry* recv_pkt_region;
  TransportKey default_scope;
  RateLimiter discover_limiter, anon_limiter;
  struct FloodRetryBridgeState {
    uint8_t key[MAX_HASH_SIZE];
    uint8_t source_bucket;
    uint8_t target_mask;
    uint8_t heard_mask;
    uint8_t progress_marker;
    bool active;
  };
  struct FloodChannelBlockEntry {
    bool active;
    uint8_t key_len;
    uint8_t max_hops;
    uint8_t hash_prefix[FLOOD_CHANNEL_BLOCK_PREFIX_LEN];
    uint8_t secret[PUB_KEY_SIZE];
    char name[FLOOD_CHANNEL_BLOCK_NAME_LEN];
  };
  mutable FloodRetryBridgeState flood_retry_bridge_states[MAX_FLOOD_RETRY_SLOTS];
  FloodChannelBlockEntry flood_channel_blocks[FLOOD_CHANNEL_BLOCK_SLOTS];
  uint32_t pending_discover_tag;
  unsigned long pending_discover_until;
  bool region_load_active;
  unsigned long dirty_contacts_expiry;
#if MAX_NEIGHBOURS
  NeighbourInfo neighbours[MAX_NEIGHBOURS];
#endif
  CayenneLPP telemetry;
  unsigned long _ota_update_at = 0;  // deferred `ota update` fire time (0 = none scheduled)
  float active_bw;  // live BW, including temporary radio overrides
  uint8_t active_sf;  // live SF, including temporary radio overrides
  uint8_t active_cr;   // live CR, including temporary radio overrides
  ScheduledRadioSetting scheduled_radio_settings[MAX_SCHEDULED_RADIO_SETTINGS];
  int  matching_peer_indexes[MAX_CLIENTS];
#if defined(WITH_RS232_BRIDGE)
  RS232Bridge bridge;
#elif defined(WITH_ESPNOW_BRIDGE)
  ESPNowBridge bridge;
#elif defined(WITH_MQTT_BRIDGE)
  MQTTBridge* bridge;
#endif
#ifdef WITH_SNMP
  MeshSNMPAgent _snmp_agent;
#endif
#ifdef WITH_MQTT_BRIDGE
  AlertReporter _alerter;
#endif

  bool extractDirectRetryPrefix(const mesh::Packet* packet, uint8_t* prefix, uint8_t& prefix_len) const;
  int8_t getDirectRetryMinSNRX4() const;
  uint8_t getDirectRetryCodingRateForSNR(int8_t snr_x4) const;
  uint8_t getDirectRetryConfiguredMaxAttempts() const;
  uint32_t getDirectRetryAttemptStepMillis() const;
  bool hasFloodRetryPrefixes() const;
  bool floodRetryPrefixMatches(const mesh::Packet* packet) const;
  bool floodRetryLastHopMatches(const mesh::Packet* packet) const;
  bool floodRetryPrefixIgnored(const uint8_t* prefix, uint8_t prefix_len) const;
  uint8_t floodRetryEffectivePathLength(const mesh::Packet* packet, uint8_t max_hops = 0xFF) const;
  bool floodRetryPrefixFresh(const uint8_t* prefix, uint8_t prefix_len) const;
  int floodRetryBucketForPrefix(const uint8_t* prefix, uint8_t prefix_len, bool require_fresh,
                                bool include_other) const;
  int floodRetryBucketForPathHop(const uint8_t* prefix, uint8_t prefix_len, uint8_t hop,
                                 uint8_t progress_marker) const;
  int floodRetrySourceBucket(const mesh::Packet* packet) const;
  uint8_t floodRetryBridgeTargetMask(uint8_t source_bucket) const;
  uint8_t floodRetryBridgeHeardMask(const mesh::Packet* packet, uint8_t source_bucket,
                                    uint8_t progress_marker) const;
  FloodRetryBridgeState* floodRetryBridgeStateFor(const mesh::Packet* packet, bool create) const;
  void clearFloodRetryBridgeState(const mesh::Packet* packet);
  void refreshFloodRetryHeardRecent(const mesh::Packet* packet);
  void formatFloodRetryPath(char* dest, size_t dest_len, const mesh::Packet* packet) const;
  bool formatFloodRetryHeard(char* dest, size_t dest_len, const mesh::Packet* packet) const;
  void putNeighbour(const mesh::Identity& id, uint32_t timestamp, float snr);
  uint8_t handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood);
  uint8_t handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  uint8_t handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  uint8_t handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  int handleRequest(ClientInfo* sender, uint32_t sender_timestamp, uint8_t* payload, size_t payload_len);
  mesh::Packet* createSelfAdvert();
  bool sendRepeatersFloodText(const char* text);
  void checkBatteryAlert();
  void printRecentRepeatersSerial();

  File openAppend(const char* fname);
  bool isLooped(const mesh::Packet* packet, const uint8_t max_counters[]);
  void applyRadioParams(float freq, float bw, uint8_t sf, uint8_t cr);
  void applySavedRadioParams();
  void processScheduledRadioSettings();
  bool isMillisTimerDue(unsigned long timestamp) const;
  void loadFloodChannelBlocks();
  bool saveFloodChannelBlocks();
  void seedDefaultFloodChannelBlocks();
  void clearFloodChannelBlockEntry(FloodChannelBlockEntry& entry);
  void deriveFloodChannelBlockPrefix(const uint8_t* secret, uint8_t key_len,
                                     uint8_t prefix[FLOOD_CHANNEL_BLOCK_PREFIX_LEN]) const;
  uint8_t resolveFloodChannelBlockHops(uint8_t max_hops) const;
  bool floodChannelBlockHopApplies(const mesh::Packet* packet, uint8_t max_hops) const;
  bool floodChannelDataHopApplies(const mesh::Packet* packet) const;
  bool floodChannelBlockMatches(const FloodChannelBlockEntry& entry, const mesh::Packet* packet) const;
  bool shouldBlockFloodChannelForward(const mesh::Packet* packet) const;
  int findFloodChannelBlockBySelector(const char* selector) const;
  int findFloodChannelBlockSlot(const uint8_t prefix[FLOOD_CHANNEL_BLOCK_PREFIX_LEN], const char* name) const;
  void formatFloodChannelBlockDetail(char* reply, int idx) const;
  bool hasScheduledRadioWorkDue() const;
  uint32_t limitSleepToMillisTimer(unsigned long timestamp, uint32_t sleep_secs) const;
  uint32_t limitSleepToRtcTime(uint32_t timestamp, uint32_t sleep_secs) const;
  uint32_t limitSleepToScheduledRadioWork(uint32_t sleep_secs) const;
  bool hasStartedScheduledTempRadio() const;
  int findFreeScheduledRadioSlot() const;
  int countScheduledRadioSettings(bool temporary) const;
  int findScheduledRadioSettingByIndex(bool temporary, int wanted) const;
  int getScheduledRadioSettingIndex(bool temporary, int slot_idx) const;
  bool scheduledRadioConflicts(bool temporary, uint32_t start_time, uint32_t end_time) const;
  void clearScheduledRadioSetting(int idx, bool restore_if_started);
  void formatScheduledRadioDuration(char* dest, size_t dest_len, uint32_t target_time) const;
  void formatRadioParamTuple(char* dest, size_t dest_len, const ScheduledRadioSetting& setting) const;
  void formatScheduledRadioSetting(char* reply, int setting_idx, int display_idx) const;

protected:
  float getAirtimeBudgetFactor() const override {
    return _prefs.airtime_factor;
  }

  bool allowPacketForward(const mesh::Packet* packet) override;
  const char* getLogDateTime() override;
  void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override;

  void logRx(mesh::Packet* pkt, int len, float score) override;
  void logTx(mesh::Packet* pkt, int len) override;
  void logTxFail(mesh::Packet* pkt, int len) override;
  int calcRxDelay(float score, uint32_t air_time) const override;

  uint32_t getRetransmitDelay(const mesh::Packet* packet) override;
  uint32_t getDirectRetransmitDelay(const mesh::Packet* packet) override;
  uint8_t getDefaultTxCodingRate() const override { return active_cr; }
  bool allowDirectRetry(const mesh::Packet* packet, const uint8_t* next_hop_hash, uint8_t next_hop_hash_len) const override;
  bool maybeShortCircuitDirect(mesh::Packet* packet) override;
  void configureDirectRetryPacket(mesh::Packet* retry, const mesh::Packet* original, uint8_t retry_attempt) override;
  uint32_t getDirectRetryEchoDelay(const mesh::Packet* packet) const override;
  uint8_t getDirectRetryMaxAttempts(const mesh::Packet* packet) const override;
  uint32_t getDirectRetryAttemptDelay(const mesh::Packet* packet, uint8_t attempt_idx) override;
  void onDirectRetryEvent(const char* event, const mesh::Packet* packet, uint32_t delay_millis, uint8_t retry_attempt,
                          const uint8_t* target_hash = NULL, uint8_t target_hash_len = 0,
                          int16_t payload_type = -1) override;
  void onDirectRetryFailed(const uint8_t* next_hop_hash, uint8_t next_hop_hash_len) override;
  void onDirectRetrySucceeded(const uint8_t* next_hop_hash, uint8_t next_hop_hash_len, int8_t snr_x4) override;
  bool allowFloodRetry(const mesh::Packet* packet) const override;
  void onFloodRetryEvent(const char* event, const mesh::Packet* packet, uint32_t delay_millis, uint8_t retry_attempt) override;
  bool hasFloodRetryTargetPrefix(const mesh::Packet* packet) const override;
  uint8_t getFloodRetryMaxPathLength(const mesh::Packet* packet) const override;
  uint8_t getFloodRetryMaxAttempts(const mesh::Packet* packet) const override;
  bool isFloodRetryEchoTarget(const mesh::Packet* packet, uint8_t progress_marker) const override;

  int getInterferenceThreshold() const override {
    return _prefs.interference_threshold;
  }
  bool getCADEnabled() const override {
    return _prefs.cad_enabled;
  }
  int getAGCResetInterval() const override {
    return ((int)_prefs.agc_reset_interval) * 4000;   // milliseconds
  }
#ifdef WITH_MQTT_BRIDGE
  uint32_t getRadioWatchdogMillis() const override {
    return ((uint32_t)_cli.getObserverPrefs()->radio_watchdog_minutes) * 60000UL;
  }
#endif
  uint8_t getExtraAckTransmitCount() const override {
    return _prefs.multi_acks;
  }

#if ENV_INCLUDE_GPS == 1
  void applyGpsPrefs() {
    sensors.setSettingValue("gps", _prefs.gps_enabled?"1":"0");
  }
#endif

  mesh::DispatcherAction onRecvPacket(mesh::Packet* pkt) override;

  void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const mesh::Identity& sender, uint8_t* data, size_t len) override;
  int searchPeersByHash(const uint8_t* hash) override;
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;
  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp, const uint8_t* app_data, size_t app_data_len);
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) override;
  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path, uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onControlDataRecv(mesh::Packet* packet) override;

  void sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size);
  void sendClientReply(ClientInfo* client, mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size);

public:
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables);

  void begin(FILESYSTEM* fs);
  void sendNodeDiscoverReq();
  const char* getFirmwareVer() override { return FIRMWARE_VERSION; }
  const char* getBuildDate() override { return FIRMWARE_BUILD_DATE; }
  const char* getRole() override { return FIRMWARE_ROLE; }
  const char* getNodeName() { return _prefs.node_name; }
  NodePrefs* getNodePrefs() {
    return &_prefs;
  }

  void savePrefs() override {
    _cli.savePrefs(_fs);
  }

  void sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size);

  // CommonCLICallbacks
  void applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) override;

#ifdef WITH_MQTT_BRIDGE
  void onAlertConfigChanged() override { _alerter.onConfigChanged(); }
  bool sendAlertText(const char* text) override { return _alerter.sendText(text); }
#endif
  bool resolveAlertScope(TransportKey& dest) override;
  void addScheduledRadioParams(bool temporary, float freq, float bw, uint8_t sf, uint8_t cr,
                               uint32_t start_time, uint32_t end_time, char* reply) override;
  void formatScheduledRadioParams(bool temporary, const char* selector, char* reply) override;
  void deleteScheduledRadioParams(bool temporary, const char* selector, char* reply) override;
  bool formatFileSystem() override;
  void sendSelfAdvertisement(int delay_millis, bool flood) override;
  void updateAdvertTimer() override;
  void updateFloodAdvertTimer() override;

  void setLoggingOn(bool enable) override { _logging = enable; }

  void eraseLogFile() override {
    _fs->remove(PACKET_LOG_FILE);
  }

  void dumpLogFile() override;
  void setTxPower(int8_t power_dbm) override;
  void formatNeighborsReply(char *reply) override;
  void removeNeighbor(const uint8_t* pubkey, int key_len) override;
  void formatStatsReply(char *reply) override;
  void formatRadioStatsReply(char *reply) override;
  void formatRadioDiagReply(char *reply) override;
  void formatPacketStatsReply(char *reply) override;
  void formatRecentRepeatersReply(char *reply, int page) override;
  bool setRecentRepeater(const uint8_t* prefix, uint8_t prefix_len, int8_t snr_x4) override;
  void clearRecentRepeaters() override;
  void startRegionsLoad() override;
  bool saveRegions() override;
  void onDefaultRegionChanged(const RegionEntry* r) override;
  void setFloodChannelBlock(int index, const uint8_t* secret, uint8_t key_len,
                            const char* name, uint8_t max_hops, char* reply) override;
  void formatFloodChannelBlocks(const char* selector, char* reply) override;
  void deleteFloodChannelBlock(const char* selector, char* reply) override;

  mesh::LocalIdentity& getSelfId() override { return self_id; }

  void saveIdentity(const mesh::LocalIdentity& new_id) override;
  void clearStats() override;

  void handleCommand(uint32_t sender_timestamp, ClientInfo* sender, char* command, char* reply);
  void handleCommand(uint32_t sender_timestamp, char* command, char* reply) {
    handleCommand(sender_timestamp, NULL, command, reply);
  }
  void loop();
  uint32_t getPowerSaveSleepSeconds(uint32_t max_secs) const;

#if defined(WITH_BRIDGE)
  void setBridgeState(bool enable) override {
    if (!bridge) {
#ifdef WITH_MQTT_BRIDGE
      bridge = new MQTTBridge(&_prefs, _cli.getObserverPrefs(), _mgr, getRTCClock(), &self_id);
#endif
      if (!bridge) return;
    }
    if (enable == bridge->isRunning()) return;
    if (enable)
    {
      // Set device metadata before starting bridge (same as in begin())
      char device_id[65];
      mesh::LocalIdentity self_id = getSelfId();
      mesh::Utils::toHex(device_id, self_id.pub_key, PUB_KEY_SIZE);
      bridge->setDeviceID(device_id);
      bridge->setFirmwareVersion(getFirmwareVer());
      bridge->setBoardModel(_cli.getBoard()->getManufacturerName());
      bridge->setBuildDate(getBuildDate());
#ifdef WITH_MQTT_BRIDGE
      bridge->setStatsSources(this, _radio, _cli.getBoard(), _ms);
#endif
      bridge->begin();
#ifdef WITH_MQTT_BRIDGE
      _alerter.setBridge(bridge);
#endif
    }
    else
    {
      bridge->end();
#ifdef WITH_MQTT_BRIDGE
      _alerter.setBridge(nullptr);
#endif
    }
  }

  void restartBridge() override {
    if (!bridge || !bridge->isRunning()) return;
    bridge->end();
    // Set device metadata before restarting bridge (same as in begin())
    char device_id[65];
    mesh::LocalIdentity self_id = getSelfId();
    mesh::Utils::toHex(device_id, self_id.pub_key, PUB_KEY_SIZE);
    bridge->setDeviceID(device_id);
    bridge->setFirmwareVersion(getFirmwareVer());
    bridge->setBoardModel(_cli.getBoard()->getManufacturerName());
    bridge->setBuildDate(getBuildDate());
#ifdef WITH_MQTT_BRIDGE
    bridge->setStatsSources(this, _radio, _cli.getBoard(), _ms);
#endif
    bridge->begin();
  }

  void restartBridgeSlot(int slot) override {
#ifdef WITH_MQTT_BRIDGE
    if (!bridge || !bridge->isRunning()) return;
    bridge->setSlotPreset(slot, _cli.getObserverPrefs()->mqtt_slot_preset[slot]);
#else
    (void)slot;
#endif
  }

  // Schedule the pull-OTA flash to run from loop() in ~2.5 s, leaving time for the
  // "Beginning update..." CLI reply (CLI_REPLY_DELAY_MILLIS = 600 ms) to transmit
  // before the flash blocks the loop and reboots.
  bool beginDeferredOtaUpdate() override {
    _ota_update_at = millis() + 2500;
    if (_ota_update_at == 0) _ota_update_at = 1;  // 0 means "none"
    return true;
  }

  int getQueueSize() override {
    return bridge ? bridge->getQueueSize() : 0;
  }

  bool isMqttBridgeRunning() override {
    return bridge && bridge->isRunning();
  }

  bool syncMqttNtp() override {
    if (!bridge || !bridge->isRunning()) return false;
    // Marshal onto the MQTT task (Core 0); this runs on the CLI thread (Core 1).
    return bridge->requestForcedNtpSync();
  }

  bool runMqttNtpDiag(char* reply, size_t reply_size, bool verbose) override {
    if (!bridge || !bridge->isRunning()) return false;
    return bridge->ntpDiag(reply, reply_size, verbose);
  }
#endif

  // To check if there is pending work
  bool hasPendingWork() const;

  bool setRxBoostedGain(bool enable) override;

};
