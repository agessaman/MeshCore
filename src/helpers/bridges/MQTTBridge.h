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
 * - Built-in presets for LetsMesh Analyzer (US/EU), MeshMapper, MeshRank, Waev, CascadiaMesh
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
  // Connection slot - each slot holds one MQTT connection
  struct MQTTSlot {
    PsychicMqttClient* client;
    const MQTTPresetDef* preset;    // Points to MQTT_PRESETS[] entry, nullptr for custom/none
    bool enabled;                   // true when preset is not "none"
    bool connected;                 // Updated in callbacks
    bool initial_connect_done;      // True after first connect() call

    // JWT auth state (used by preset JWT slots and custom slots with audience set)
    char* auth_token;               // PSRAM-allocated, AUTH_TOKEN_SIZE bytes
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

    // Last error (stored for CLI diagnostics — serial-free debugging)
    int32_t last_tls_err;           // esp_tls_last_esp_err (0 = no error)
    int32_t last_tls_stack_err;     // mbedTLS stack error
    int last_sock_errno;            // socket errno
    unsigned long last_error_time;  // millis() of last error
    uint32_t disconnect_count;      // Number of disconnect callbacks since boot
    unsigned long first_disconnect_time; // millis() of first disconnect after boot
  };

  static const size_t AUTH_TOKEN_SIZE = 768;
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
  bool _tx_enabled;
  unsigned long _last_status_publish;
  unsigned long _status_interval;

  // Packet queue for offline scenarios
  // NOTE: We store a full copy of the packet (not a pointer) because the
  // Dispatcher frees packets back to the static pool immediately after logRx()
  // returns. Storing only a pointer would be a use-after-free.
  struct QueuedPacket {
    mesh::Packet packet_copy;  // ~258 bytes, full value copy
    unsigned long timestamp;
    bool is_tx;
    float snr;
    float rssi;
#if defined(BOARD_HAS_PSRAM)
    // Store raw radio data with each packet to avoid it being overwritten.
    // On non-PSRAM boards, raw hex is reconstructed from packet_copy.writeTo()
    // at publish time to save ~2.5KB of heap (256 bytes × MAX_QUEUE_SIZE).
    uint8_t raw_data[256];
    int raw_len;
    bool has_raw_data;
#endif
  };

  #if defined(BOARD_HAS_PSRAM)
  static const int MAX_QUEUE_SIZE = 50;
  #else
  static const int MAX_QUEUE_SIZE = 10;
  #endif

  // FreeRTOS queue for thread-safe packet queuing
  #ifdef ESP_PLATFORM
  QueueHandle_t _packet_queue_handle;
  TaskHandle_t _mqtt_task_handle;
  SemaphoreHandle_t _raw_data_mutex;  // Mutex for raw radio data
  // PSRAM-backed task stack; TCB kept in internal RAM
  StackType_t* _mqtt_task_stack;     // nullptr if using dynamic task creation
  StaticTask_t _mqtt_task_tcb;
  // PSRAM-backed packet queue storage
  uint8_t* _packet_queue_storage;    // nullptr if using dynamic queue
  StaticQueue_t _packet_queue_struct;
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

  // Timezone handling
  Timezone* _timezone;

  // Raw radio data storage (PSRAM when BOARD_HAS_PSRAM)
  static const size_t LAST_RAW_DATA_SIZE = 256;
  uint8_t* _last_raw_data;
  int _last_raw_len;
  float _last_snr;
  float _last_rssi;
  unsigned long _last_raw_timestamp;

  // Pre-allocated JSON publish buffer (PSRAM when available, allocated once in begin())
  static const size_t PUBLISH_JSON_BUFFER_SIZE = 2048;
  char* _publish_json_buffer;
  static const size_t STATUS_JSON_BUFFER_SIZE = 768;
  char* _status_json_buffer;

  // Memory pressure monitoring
  unsigned long _last_memory_check;
  int _skipped_publishes;  // Count of skipped publishes due to memory pressure
  unsigned long _last_fragmentation_recovery;  // Throttle: 5 min between recovery runs
  unsigned long _fragmentation_pressure_since;  // 0 = not under pressure
  unsigned long _last_critical_check_run;  // Throttle: run unified check at most every 60s

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

  // Recovery: restart ESP after prolonged total failure
  unsigned long _all_tripped_since;  // 0 = not all tripped

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
  void setupSlot(int index);           // Create/destroy client for a slot based on its preset
  void teardownSlot(int index);        // Disconnect and free slot resources
  void maintainSlotConnections();      // Maintain all slot connections (token renewal, reconnect)
  void maintainSlotConnection(int index, unsigned long now_millis, unsigned long current_time, bool time_synced, bool& reconnect_attempted, bool& teardown_attempted);
  bool createSlotAuthToken(int index); // Create/renew JWT token for a slot
  bool publishToSlot(int index, const char* topic, const char* payload, bool retained = false);
  bool publishToAllSlots(const char* topic, const char* payload, bool retained = false);
  void publishStatusToSlot(int index);
  void updateCachedConnectionStatus();

  #ifdef ESP_PLATFORM
  void runCriticalMemoryCheckAndRecovery();
  #endif
  void recreateMqttClientsForFragmentationRecovery();
  void processPacketQueue();
  bool publishStatus();  // Returns true if status was successfully published
  bool handleWiFiConnection(unsigned long now);

  // FreeRTOS task function (runs on Core 0)
  #ifdef ESP_PLATFORM
  static void mqttTask(void* parameter);
  void mqttTaskLoop();  // Main loop for MQTT task
  void initializeWiFiInTask();  // WiFi initialization moved to task
  #endif
  void publishPacket(mesh::Packet* packet, bool is_tx,
                     const uint8_t* raw_data = nullptr, int raw_len = 0,
                     float snr = 0.0f, float rssi = 0.0f);
  void publishRaw(mesh::Packet* packet);
  void queuePacket(mesh::Packet* packet, bool is_tx);
  void dequeuePacket();
  bool isAnySlotConnected();
  void syncTimeWithNTP();
  void refreshNTP();  // Lightweight periodic NTP refresh (non-blocking)
  Timezone* createTimezoneFromString(const char* tz_string);
  void checkConfigurationMismatch();
  bool isIATAValid() const;
  bool isSlotReady(int index, char* reason_buf = nullptr, size_t reason_size = 0) const;

  void optimizeMqttClientConfig(PsychicMqttClient* client, bool needs_large_buffer = false);
  void getClientVersion(char* buffer, size_t buffer_size) const;
  void logMemoryStatus();

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
  static void formatMqttStatusReply(char* buf, size_t bufsize, const NodePrefs* prefs);
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
