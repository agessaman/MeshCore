#pragma once

#include "MeshCore.h"
#include "helpers/bridges/BridgeBase.h"
#include <PsychicMqttClient.h>
#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Timezone.h>
#include "helpers/JWTHelper.h"
#include "helpers/MQTTPresets.h"

#ifdef WITH_SNMP
class MeshSNMPAgent;  // Forward declaration
#endif

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#endif

#if defined(MQTT_DEBUG) && defined(ARDUINO)
  #include <Arduino.h>
  // USB CDC-aware debug macros: only print if Serial is ready (non-blocking check)
  // Serial.availableForWrite() returns bytes available in write buffer (>0 means ready)
  // This prevents hangs when USB CDC isn't ready yet (e.g., ESP32-S3 native USB)
  #define MQTT_DEBUG_PRINT(F, ...) do { if (Serial.availableForWrite() > 0) { Serial.printf("MQTT: " F, ##__VA_ARGS__); } } while(0)
  #define MQTT_DEBUG_PRINTLN(F, ...) do { if (Serial.availableForWrite() > 0) { Serial.printf("MQTT: " F "\n", ##__VA_ARGS__); } } while(0)
#else
  #define MQTT_DEBUG_PRINT(...) {}
  #define MQTT_DEBUG_PRINTLN(...) {}
#endif

#ifdef WITH_MQTT_BRIDGE

/**
 * @brief Bridge implementation using MQTT protocol for packet transport
 *
 * This bridge enables mesh packet transport over MQTT, allowing repeaters to
 * uplink packet data to multiple MQTT brokers for monitoring and analysis.
 *
 * Features:
 * - Up to 6 configurable MQTT connection slots (5 active with PSRAM, 2 without)
 * - Built-in presets for LetsMesh Analyzer (US/EU), MeshMapper, MeshRank, Waev, CascadiaMesh, DutchMeshcore(#1/#2)
 * - Custom broker support with username/password auth
 * - JWT authentication with Ed25519 device signing
 * - Automatic reconnection with exponential backoff
 * - JSON message formatting for status, packets, and raw data
 * - Packet queuing during connection issues
 *
 * Configuration:
 * - Define WITH_MQTT_BRIDGE to enable this bridge
 * - Configure slots via: set mqtt1.preset <name>, set mqtt2.preset <name>, etc.
 * - Available presets: analyzer-us, analyzer-eu, meshmapper, custom, none
 */
class MQTTBridge : public BridgeBase {
private:
  static const size_t AUTH_TOKEN_SIZE = 768;

  // Connection slot - each slot holds one MQTT connection
  struct MQTTSlot {
    PsychicMqttClient* client;
    const MQTTPresetDef* preset;    // Points to MQTT_PRESETS[] entry, nullptr for custom/none
    bool enabled;                   // true when preset is not "none"
    bool connected;                 // Updated in callbacks
    bool initial_connect_done;      // True after first connect() call

    // JWT auth state (used by preset JWT slots and custom slots with audience set)
    // Inline buffer avoids per-reconnect heap alloc/free churn (fragmentation source).
    char auth_token[AUTH_TOKEN_SIZE]; // empty string = no valid token
    unsigned long token_expires_at;
    unsigned long last_token_renewal;

    // Custom broker settings (only used when preset_name is "custom")
    char host[64];
    uint16_t port;
    char username[32];
    char password[64];
    char audience[64];              // JWT audience for custom JWT slots (empty = username/password)
    char broker_uri[128];           // Persistent URI for custom slots (avoids dangling pointer)

    // Reconnect backoff
    uint8_t reconnect_backoff;      // 0..4 index into backoff table
    uint8_t max_backoff_failures;   // consecutive failures at max backoff level
    bool circuit_breaker_tripped;   // true = stop reconnecting until reconfigured
    unsigned long last_reconnect_attempt;
    unsigned long last_log_time;    // Throttle disconnect log messages
    unsigned long last_deferred_log_ms; // Throttle "connect deferred" log spam (Phase 1)

    // Last error (stored for CLI diagnostics — serial-free debugging)
    int32_t last_tls_err;           // esp_tls_last_esp_err (0 = no error)
    int32_t last_tls_stack_err;     // mbedTLS stack error
    int last_sock_errno;            // socket errno
    unsigned long last_error_time;  // millis() of last error
    uint32_t disconnect_count;      // Number of disconnect callbacks since boot
    unsigned long first_disconnect_time; // millis() of first disconnect after boot
  };

  MQTTSlot _slots[RUNTIME_MQTT_SLOTS];

  // JWT username shared across all JWT-auth slots (same device identity)
  char _jwt_username[70];  // Format: v1_{UPPERCASE_PUBLIC_KEY}

  // Message configuration
  char _origin[32];
  char _iata[8];
  char _device_id[65];  // Device public key (hex string)
  char _firmware_version[64];  // Firmware version string
  char _board_model[64];  // Board model string
  char _build_date[32];  // Build date string
  bool _status_enabled;
  bool _packets_enabled;
  bool _raw_enabled;
  bool _rx_enabled;
  uint8_t _tx_mode;  // 0=off, 1=all TX, 2=self-advert only
  unsigned long _last_status_publish;
  unsigned long _status_interval;

  // Packet queue for offline scenarios
  // NOTE: We store a full copy of the packet (not a pointer) because the
  // Dispatcher frees packets back to the static pool immediately after logRx()
  // returns. Storing only a pointer would be a use-after-free.
  struct QueuedPacket {
    mesh::Packet packet_copy;  // ~258 bytes, full value copy
    unsigned long timestamp;
    unsigned long next_retry_ms;  // Earliest millis() when a failed send may be retried
    uint8_t retry_attempts;       // Bounded resend attempts for transient QoS0 publish failures
    bool is_tx;
    float snr;
    float rssi;
    // Raw radio bytes embedded at enqueue time (Core 1), never shared across cores.
    // On non-PSRAM boards the queue is smaller (6 items) to offset the per-item cost.
    uint8_t raw_data[256];
    uint8_t raw_len;
    bool has_raw_data;
  };

  #if defined(BOARD_HAS_PSRAM)
  static const int MAX_QUEUE_SIZE = 50;
  #else
  // Reduced from 10: raw_data[256] adds ~256 bytes/item; 6×543 ≈ 3.3 KB vs old 10×282 ≈ 2.8 KB.
  // Net increase is <500 bytes; _last_raw_data (256 bytes) is eliminated to offset further.
  static const int MAX_QUEUE_SIZE = 6;
  #endif

  // FreeRTOS queue for thread-safe packet queuing
  #ifdef ESP_PLATFORM
  QueueHandle_t _packet_queue_handle;
  TaskHandle_t _mqtt_task_handle;
  // PSRAM-backed task stack; TCB kept in internal RAM
  StackType_t* _mqtt_task_stack;     // nullptr if using dynamic task creation
  StaticTask_t _mqtt_task_tcb;
  // Packet queue storage: PSRAM heap on PSRAM boards, inline array on non-PSRAM boards.
  // Using xQueueCreateStatic with inline storage eliminates a separate heap allocation.
  uint8_t* _packet_queue_storage;
  StaticQueue_t _packet_queue_struct;
  #if !defined(BOARD_HAS_PSRAM)
  uint8_t _packet_queue_inline[MAX_QUEUE_SIZE * sizeof(QueuedPacket)];
  #endif
  #else
  // Fallback to circular buffer for non-ESP32 platforms
  QueuedPacket _packet_queue[MAX_QUEUE_SIZE];
  int _queue_head;
  int _queue_tail;
  #endif
  int _queue_count;  // Protected by queue operations or mutex

  // NTP time sync
  WiFiUDP _ntp_udp;
  NTPClient _ntp_client;
  unsigned long _last_ntp_sync;
  bool _ntp_synced;
  bool _ntp_sync_pending;  // Flag to trigger NTP sync from loop() instead of event handler
  bool _slots_setup_done;  // Deferred: slots set up after NTP sync
  int _max_active_slots;   // Runtime limit: 5 with PSRAM, 2 without

  // Pending slot reconfigure: set from CLI (Core 1), processed by MQTT task (Core 0)
  volatile bool _slot_reconfigure_pending[RUNTIME_MQTT_SLOTS];

  // Timezone handling.
  // _timezone_storage is inline class storage (zero heap) that is reconfigured
  // via setRules() whenever the preferred timezone string changes. _timezone
  // is a stable alias pointer to &_timezone_storage so existing call sites
  // that accept a Timezone* keep working without modification.
  Timezone _timezone_storage;
  Timezone* _timezone;

  // Core 1-only staging: written by storeRawRadioData(), consumed by queuePacket().
  // No mutex needed — both call sites run on Core 1 in guaranteed sequence.
  static const size_t LAST_RAW_DATA_SIZE = 256;
  uint8_t _staged_raw[LAST_RAW_DATA_SIZE];
  int     _staged_raw_len   = 0;
  float   _staged_snr       = 0.0f;
  float   _staged_rssi      = 0.0f;
  bool    _staged_raw_valid = false;

  // Core 0-owned copy of the most recent raw data — written only by processPacketQueue()
  // on Core 0, read only by publishStatus() on Core 0. No mutex required.
  // On PSRAM boards: heap pointer into PSRAM. On non-PSRAM: inline array in class object.
  static const size_t LAST_RAW_DATA_SIZE_MEMBER = 256;  // mirrors LAST_RAW_DATA_SIZE
  #if defined(BOARD_HAS_PSRAM)
  uint8_t* _last_raw_data;
  #else
  uint8_t  _last_raw_data[LAST_RAW_DATA_SIZE_MEMBER];
  #endif
  int _last_raw_len;
  float _last_snr;
  float _last_rssi;
  unsigned long _last_raw_timestamp;

  // JSON publish/status serialization buffers — reused for every publish (no alloc/free churn).
  // On PSRAM boards: heap pointer into PSRAM to save internal heap. On non-PSRAM: inline in
  // class object so these allocations don't interleave with large TLS buffers at startup.
  static const size_t PUBLISH_JSON_BUFFER_SIZE = 2048;
  static const size_t STATUS_JSON_BUFFER_SIZE = 768;
  #if defined(BOARD_HAS_PSRAM)
  char* _publish_json_buffer;
  char* _status_json_buffer;
  #else
  char _publish_json_buffer[PUBLISH_JSON_BUFFER_SIZE];
  char _status_json_buffer[STATUS_JSON_BUFFER_SIZE];
  #endif

  // JSON document scratch space — inline StaticJsonDocument keeps the pool off the 8 KB MQTT
  // task stack and eliminates two separate heap allocations (fragmentation reduction).
  StaticJsonDocument<PUBLISH_JSON_BUFFER_SIZE> _packet_json_doc;
  StaticJsonDocument<STATUS_JSON_BUFFER_SIZE>  _status_json_doc;

  // Memory pressure monitoring (per-publish skip; see publishPacket()).
  // The broader fragmentation-recovery machinery was removed in Phase 4 of
  // the MQTT memory-defrag work — persistent MQTT clients no longer churn
  // the heap, so gray-zone / critical-restart trackers are unnecessary.
  unsigned long _last_memory_check;
  int _skipped_publishes;  // Exposed via SNMP; count of publishes skipped when max_alloc is too low

  // Status publish retry tracking
  unsigned long _last_status_retry;  // Track last retry attempt (separate from successful publish)
  static const unsigned long STATUS_RETRY_INTERVAL = 30000; // Retry every 30 seconds if failed

  // Device identity for JWT token creation
  mesh::LocalIdentity *_identity;

  // Cached connection status (updated in callbacks to avoid redundant checks)
  bool _cached_has_connected_slots;

  // Queue staleness tracking
  unsigned long _queue_disconnected_since;  // 0 = has connected slots
  static const unsigned long QUEUE_STALE_MS = 300000UL; // Flush queue after 5 min disconnected

#ifdef WITH_SNMP
  MeshSNMPAgent* _snmp_agent;
#endif

  // Throttle logging
  unsigned long _last_no_broker_log;
  static const unsigned long NO_BROKER_LOG_INTERVAL = 30000; // Log every 30 seconds max
  static const unsigned long SLOT_LOG_INTERVAL = 30000; // Log every 30 seconds max
  unsigned long _last_config_warning; // Throttle configuration mismatch warnings
  static const unsigned long CONFIG_WARNING_INTERVAL = 300000; // Log every 5 minutes max

  // WiFi connection state and exponential backoff
  unsigned long _last_wifi_check;
  wl_status_t _last_wifi_status;
  bool _wifi_status_initialized;
  unsigned long _wifi_disconnected_time;  // 0 when connected
  unsigned long _last_wifi_reconnect_attempt;
  uint8_t _wifi_reconnect_backoff_attempt;  // 0..5 → 15s, 30s, 60s, 120s, 300s; reset on connect
  unsigned long _last_slot_reconnect_ms;   // guards against concurrent TLS handshakes (15 s inter-slot gap)

  // Optional pointers for collecting stats internally (set by mesh if available)
  mesh::Dispatcher* _dispatcher;  // For air times and errors
  mesh::Radio* _radio;             // For noise floor
  mesh::MainBoard* _board;         // For battery voltage
  mesh::MillisecondClock* _ms;    // For uptime

  // Topic building
  enum MQTTMessageType { MSG_STATUS, MSG_PACKETS, MSG_RAW };
  bool buildTopicForSlot(int index, MQTTMessageType type, char* topic_buf, size_t buf_size);
  bool substituteTopicTemplate(const char* tmpl, MQTTMessageType type, int slot_index, char* buf, size_t buf_size);

  // Internal methods - slot management
  // Lifetime model (Phase 1 of MQTT memory-defrag):
  // - initSlotClients() allocates one PsychicMqttClient per slot and registers
  //   its persistent callbacks. Runs once per bridge lifetime in begin().
  // - destroySlotClients() disconnects and deletes each client. Runs once in end().
  // - setupSlot() configures an already-allocated client (server, credentials,
  //   CA) and calls connect(). Safe to call multiple times to reconfigure.
  // - teardownSlot() only disconnects — it never deletes the client. Leaves
  //   the mbedTLS/transport state ready for a subsequent setupSlot().
  // This avoids delete/new cycles that shed ~40 KB of mbedTLS buffers per
  // reconfigure and fragment the internal heap on non-PSRAM boards.
  void initSlotClients();              // Allocate persistent clients + register callbacks (once)
  void destroySlotClients();           // Delete all persistent clients (shutdown only)
  void setupSlot(int index);           // Configure and connect the slot's existing client
  void teardownSlot(int index);        // Disconnect the slot's client (keeps the object alive)
  void maintainSlotConnections();      // Maintain all slot connections (token renewal, reconnect)
  void maintainSlotConnection(int index, unsigned long now_millis, unsigned long current_time, bool time_synced, bool& reconnect_attempted, bool& teardown_attempted);
  bool createSlotAuthToken(int index); // Create/renew JWT token for a slot
  bool publishToSlot(int index, const char* topic, const char* payload, bool retained = false, uint8_t qos = 0);
  bool publishToAllSlots(const char* topic, const char* payload, bool retained = false, uint8_t qos = 0);
  void publishStatusToSlot(int index);
  void updateCachedConnectionStatus();

  void processPacketQueue();
  bool publishStatus();  // Returns true if status was successfully published
  bool handleWiFiConnection(unsigned long now);

  // FreeRTOS task function (runs on Core 0)
  #ifdef ESP_PLATFORM
  static void mqttTask(void* parameter);
  void mqttTaskLoop();  // Main loop for MQTT task
  void initializeWiFiInTask();  // WiFi initialization moved to task
  #endif
  bool publishPacket(mesh::Packet* packet, bool is_tx,
                     const uint8_t* raw_data = nullptr, int raw_len = 0,
                     float snr = 0.0f, float rssi = 0.0f);
  bool publishRaw(mesh::Packet* packet);
  void queuePacket(mesh::Packet* packet, bool is_tx);
  void dequeuePacket();
  bool isAnySlotConnected();
  void syncTimeWithNTP();
  void refreshNTP();  // Lightweight periodic NTP refresh (non-blocking)
  // Populates dst_out/std_out with TimeChangeRules for the given IANA or
  // abbreviation string. Returns false if the string is not recognized
  // (callers should fall back to UTC). Zero-allocation.
  static bool timezoneRulesFromString(const char* tz_string, TimeChangeRule& dst_out, TimeChangeRule& std_out);
  void checkConfigurationMismatch();
  bool isIATAValid() const;
  bool isSlotReady(int index, char* reason_buf = nullptr, size_t reason_size = 0) const;

  void optimizeMqttClientConfig(PsychicMqttClient* client, bool needs_large_buffer = false);
  void getClientVersion(char* buffer, size_t buffer_size) const;
  void logMemoryStatus();
  void refreshOriginFromPrefs();

public:
  MQTTBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc, mesh::LocalIdentity *identity);

  void begin() override;
  void end() override;
  void loop() override;
  void onPacketReceived(mesh::Packet *packet) override;
  void sendPacket(mesh::Packet *packet) override;

  /**
   * Configure a slot with a preset name. Call this when the user runs
   * "set mqttN.preset <name>". Handles teardown of old connection and
   * setup of new one.
   *
   * @param slot_index Slot index (0-2)
   * @param preset_name Preset name: "analyzer-us", "analyzer-eu", "meshmapper", "custom", "none"
   */
  void setSlotPreset(int slot_index, const char* preset_name);
  void applySlotPreset(int slot_index, const char* preset_name);

  /**
   * Configure custom broker settings for a slot. Only applies when the
   * slot's preset is "custom".
   *
   * @param slot_index Slot index (0-2)
   * @param host Broker hostname
   * @param port Broker port
   * @param username MQTT username (empty for anonymous)
   * @param password MQTT password (empty for anonymous)
   */
  void setSlotCustomBroker(int slot_index, const char* host, uint16_t port,
                           const char* username, const char* password);

  void setOrigin(const char* origin);
  void setIATA(const char* iata);
  void setDeviceID(const char* device_id);
  void setFirmwareVersion(const char* firmware_version);
  void setBoardModel(const char* board_model);
  void setBuildDate(const char* build_date);
  void storeRawRadioData(const uint8_t* raw_data, int len, float snr, float rssi);
  void setMessageTypes(bool status, bool packets, bool raw);
  int getConnectedBrokers() const;
  int getQueueSize() const;
  bool isReady() const;

  static unsigned long getWifiConnectedAtMillis();
  /** Resolved origin for MQTT JSON: node_name when mqtt_origin is empty, else mqtt_origin (with quote stripping). */
  static void getEffectiveMqttOrigin(const NodePrefs* prefs, char* buf, size_t buf_size);
  static void formatMqttStatusReply(char* buf, size_t bufsize, const NodePrefs* prefs);
  /** True when WiFi is set and at least one MQTT slot can run (preset + custom host if needed). */
  static bool isConfigValid(const NodePrefs* prefs);
  static void formatSlotDiagReply(char* buf, size_t bufsize, int slot_index);
  static uint8_t getLastWifiDisconnectReason();
  static unsigned long getLastWifiDisconnectTime();
  static const char* wifiReasonStr(uint8_t reason);
  static const char* tlsErrorStr(int32_t err);

  void setStatsSources(mesh::Dispatcher* dispatcher, mesh::Radio* radio,
                       mesh::MainBoard* board, mesh::MillisecondClock* ms);

#ifdef WITH_SNMP
  void setSNMPAgent(MeshSNMPAgent* agent) { _snmp_agent = agent; }
#endif
};

#endif
