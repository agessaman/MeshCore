#pragma once

#include "Mesh.h"
#include <helpers/IdentityStore.h>
#include <helpers/SensorManager.h>
#include <helpers/ClientACL.h>
#include <helpers/RegionMap.h>

#if defined(WITH_RS232_BRIDGE) || defined(WITH_ESPNOW_BRIDGE)
#define WITH_BRIDGE
#endif

#define ADVERT_LOC_NONE       0
#define ADVERT_LOC_SHARE      1
#define ADVERT_LOC_PREFS      2

#define LOOP_DETECT_OFF       0
#define LOOP_DETECT_MINIMAL   1
#define LOOP_DETECT_MODERATE  2
#define LOOP_DETECT_STRICT    3

#define RETRY_PRESET_INFRA    0
#define RETRY_PRESET_ROOFTOP  1
#define RETRY_PRESET_MOBILE   2
#define RETRY_PRESET_CUSTOM   0xFF

#define DIRECT_RETRY_INFRA_BASE_MS      275
#define DIRECT_RETRY_INFRA_COUNT          4
#define DIRECT_RETRY_INFRA_STEP_MS      150
#define DIRECT_RETRY_INFRA_MARGIN_X4     60

#define DIRECT_RETRY_ROOFTOP_BASE_MS    175
#define DIRECT_RETRY_ROOFTOP_COUNT       15
#define DIRECT_RETRY_ROOFTOP_STEP_MS    100
#define DIRECT_RETRY_ROOFTOP_MARGIN_X4   20

#define DIRECT_RETRY_MOBILE_BASE_MS     175
#define DIRECT_RETRY_MOBILE_COUNT        15
#define DIRECT_RETRY_MOBILE_STEP_MS      50
#define DIRECT_RETRY_MOBILE_MARGIN_X4     0
#define DIRECT_RETRY_RECENT_DEFAULT       1

#define FLOOD_RETRY_INFRA_COUNT           1
#define FLOOD_RETRY_INFRA_MAX_PATH        1

#define FLOOD_RETRY_ROOFTOP_COUNT         3
#define FLOOD_RETRY_ROOFTOP_MAX_PATH      2

#define FLOOD_RETRY_MOBILE_COUNT         15
#define FLOOD_RETRY_MOBILE_MAX_PATH       1
#define FLOOD_RETRY_ADVERT_DEFAULT        0

#define BATTERY_ALERT_LOW_PERCENT_DEFAULT       20
#define BATTERY_ALERT_CRITICAL_PERCENT_DEFAULT  10

#ifndef FLOOD_RETRY_PREFIX_SLOTS
  #define FLOOD_RETRY_PREFIX_SLOTS        8
#endif
#ifndef FLOOD_RETRY_PREFIX_LEN
  #define FLOOD_RETRY_PREFIX_LEN          3
#endif
#ifndef FLOOD_RETRY_BRIDGE_BUCKETS
  #define FLOOD_RETRY_BRIDGE_BUCKETS      6
#endif
#ifndef FLOOD_RETRY_BUCKET_PREFIXES
  #define FLOOD_RETRY_BUCKET_PREFIXES     17
#endif
#ifndef FLOOD_RETRY_IGNORE_PREFIXES
  #define FLOOD_RETRY_IGNORE_PREFIXES     8
#endif
#ifndef FLOOD_RETRY_LIST_PREFIXES
  #define FLOOD_RETRY_LIST_PREFIXES       ((FLOOD_RETRY_IGNORE_PREFIXES > FLOOD_RETRY_BUCKET_PREFIXES) ? FLOOD_RETRY_IGNORE_PREFIXES : FLOOD_RETRY_BUCKET_PREFIXES)
#endif
#ifndef FLOOD_RETRY_LIST_TEXT_MAX
  #define FLOOD_RETRY_LIST_TEXT_MAX       (FLOOD_RETRY_LIST_PREFIXES * FLOOD_RETRY_PREFIX_LEN * 2 + FLOOD_RETRY_LIST_PREFIXES)
#endif
#ifndef COMMON_CLI_TMP_LEN
  #define COMMON_CLI_TMP_LEN              ((FLOOD_RETRY_LIST_TEXT_MAX > (PRV_KEY_SIZE * 2 + 4)) ? FLOOD_RETRY_LIST_TEXT_MAX : (PRV_KEY_SIZE * 2 + 4))
#endif

#define DIRECT_RETRY_CR4_MIN_SNR_X4_DEFAULT  40
#define DIRECT_RETRY_CR5_MIN_SNR_X4_DEFAULT  30
#define DIRECT_RETRY_CR7_MIN_SNR_X4_DEFAULT  10
#define DIRECT_RETRY_CR8_MAX_SNR_X4_DEFAULT  10

#define DIRECT_RETRY_PREFS_MAGIC_0  0xD1
#define DIRECT_RETRY_PREFS_MAGIC_1  0x52

struct NodePrefs { // persisted to file
  float airtime_factor;
  char node_name[32];
  double node_lat, node_lon;
  char password[16];
  float freq;
  int8_t tx_power_dbm;
  uint8_t disable_fwd;
  uint8_t advert_interval;       // minutes / 2
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
  // Bridge settings
  uint8_t bridge_enabled; // boolean
  uint16_t bridge_delay;  // milliseconds (default 500 ms)
  uint8_t bridge_pkt_src; // 0 = logTx, 1 = logRx (default logTx)
  uint32_t bridge_baud;   // 9600, 19200, 38400, 57600, 115200 (default 115200)
  uint8_t bridge_channel; // 1-14 (ESP-NOW only)
  char bridge_secret[16]; // for XOR encryption of bridge packets (ESP-NOW only)
  // Power setting
  uint8_t powersaving_enabled; // boolean
  uint8_t reboot_interval; // hours, 0-255 (default 0=disable)
  // Gps settings
  uint8_t gps_enabled;
  uint32_t gps_interval; // in seconds
  uint8_t advert_loc_policy;
  uint32_t discovery_mod_timestamp;
  float adc_multiplier;
  char owner_info[120];
  uint8_t rx_boosted_gain; // power settings
  uint8_t radio_fem_rxgain; // LoRa FEM RX gain setting
  uint8_t path_hash_mode;   // which path mode to use when sending
  uint8_t loop_detect;
  uint8_t cad_enabled;      // hardware Channel Activity Detection before TX (boolean)
  uint8_t retry_preset;
  uint8_t direct_retry_attempts;
  uint16_t direct_retry_base_ms;
  uint16_t direct_retry_step_ms;
  uint16_t direct_retry_snr_margin_x4;
  int8_t direct_retry_cr4_snr_x4;
  int8_t direct_retry_cr5_snr_x4;
  int8_t direct_retry_cr7_snr_x4;
  int8_t direct_retry_cr8_snr_x4;
  uint8_t direct_retry_enabled;
  uint8_t direct_retry_cr_enabled;
  uint8_t direct_retry_prefs_magic[2];
  uint8_t flood_retry_attempts;
  uint8_t flood_retry_max_path;
  uint8_t flood_retry_prefixes[FLOOD_RETRY_PREFIX_SLOTS][FLOOD_RETRY_PREFIX_LEN];
  uint8_t flood_retry_bridge_enabled;
  uint8_t flood_retry_bridge_buckets[FLOOD_RETRY_BRIDGE_BUCKETS][FLOOD_RETRY_BUCKET_PREFIXES][FLOOD_RETRY_PREFIX_LEN];
  uint8_t flood_retry_ignore_prefixes[FLOOD_RETRY_IGNORE_PREFIXES][FLOOD_RETRY_PREFIX_LEN];
  uint8_t flood_retry_advert_enabled;
  uint8_t battery_alert_enabled;
  uint8_t battery_alert_low_percent;
  uint8_t battery_alert_critical_percent;
  uint8_t direct_retry_recent_enabled;
};

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
  virtual void formatPacketStatsReply(char *reply) = 0;
  virtual void formatRecentRepeatersReply(char *reply, int page) {
    (void)page;
    if (reply != NULL) reply[0] = 0;
  }
  virtual bool setRecentRepeater(const uint8_t* prefix, uint8_t prefix_len, int8_t snr_x4) {
    (void)prefix;
    (void)prefix_len;
    (void)snr_x4;
    return false;
  }
  virtual void clearRecentRepeaters() {
  }
  virtual mesh::LocalIdentity& getSelfId() = 0;
  virtual void saveIdentity(const mesh::LocalIdentity& new_id) = 0;
  virtual void clearStats() = 0;
  virtual void applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) = 0;
  virtual void addScheduledRadioParams(bool temporary, float freq, float bw, uint8_t sf, uint8_t cr,
                                       uint32_t start_time, uint32_t end_time, char* reply) {
    (void)temporary;
    (void)freq;
    (void)bw;
    (void)sf;
    (void)cr;
    (void)start_time;
    (void)end_time;
    strcpy(reply, "Error: unsupported");
  }
  virtual void formatScheduledRadioParams(bool temporary, const char* selector, char* reply) {
    (void)temporary;
    (void)selector;
    strcpy(reply, "Error: unsupported");
  }
  virtual void deleteScheduledRadioParams(bool temporary, const char* selector, char* reply) {
    (void)temporary;
    (void)selector;
    strcpy(reply, "Error: unsupported");
  }

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

  virtual bool setRxBoostedGain(bool enable) {
    return false; // CommonCLI reports unsupported if not overridden by wrapper
  };
};

class CommonCLI {
  mesh::RTCClock* _rtc;
  NodePrefs* _prefs;
  CommonCLICallbacks* _callbacks;
  mesh::MainBoard* _board;
  SensorManager* _sensors;
  RegionMap* _region_map;
  ClientACL* _acl;
  char tmp[COMMON_CLI_TMP_LEN];

  mesh::RTCClock* getRTCClock() { return _rtc; }
  void savePrefs();
  void loadPrefsInt(FILESYSTEM* _fs, const char* filename);

  void handleRegionCmd(char* command, char* reply);
  void handleGetCmd(uint32_t sender_timestamp, char* command, char* reply);
  void handleSetCmd(uint32_t sender_timestamp, char* command, char* reply);
  void handleDelCmd(char* command, char* reply);

public:
  CommonCLI(mesh::MainBoard& board, mesh::RTCClock& rtc, SensorManager& sensors, RegionMap& region_map, ClientACL& acl, NodePrefs* prefs, CommonCLICallbacks* callbacks)
      : _board(&board), _rtc(&rtc), _sensors(&sensors), _region_map(&region_map), _acl(&acl), _prefs(prefs), _callbacks(callbacks) { }

  void loadPrefs(FILESYSTEM* _fs);
  void savePrefs(FILESYSTEM* _fs);
  void handleCommand(uint32_t sender_timestamp, char* command, char* reply);
  uint8_t buildAdvertData(uint8_t node_type, uint8_t* app_data);
};
