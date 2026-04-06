#include "MQTTBridge.h"
#include "../MQTTMessageBuilder.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Timezone.h>

#ifdef WITH_SNMP
#include "../SNMPAgent.h"
#endif

#ifdef ESP_PLATFORM
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <mbedtls/platform.h>
#endif

// Helper function to strip quotes from strings (both single and double quotes)
static void stripQuotes(char* str, size_t max_len) {
  if (!str || max_len == 0) return;

  size_t len = strlen(str);
  if (len == 0) return;

  // Remove leading quote (single or double)
  if (str[0] == '"' || str[0] == '\'') {
    memmove(str, str + 1, len);
    len--;
  }

  // Remove trailing quote (single or double)
  if (len > 0 && (str[len-1] == '"' || str[len-1] == '\'')) {
    str[len-1] = '\0';
  }
}

// Helper function to check if WiFi credentials are valid
static bool isWiFiConfigValid(const NodePrefs* prefs) {
  // Check if WiFi SSID is configured (not empty)
  if (strlen(prefs->wifi_ssid) == 0) {
    return false;
  }

  // WiFi password can be empty for open networks, so we don't check it

  return true;
}

#ifdef WITH_MQTT_BRIDGE

// Optional embedded CA bundle symbols produced by board_build.embed_files.
// Weak linkage keeps non-bundle builds linkable and allows runtime fallback.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_src_certs_x509_crt_bundle_bin_start") __attribute__((weak));
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_src_certs_x509_crt_bundle_bin_end") __attribute__((weak));

// Track whether the global cert bundle has been loaded into s_crt_bundle.
// Loading must happen exactly once to avoid a use-after-free race when multiple
// TLS slots are set up in sequence (each connect() launches an async task).
static bool s_ca_bundle_loaded = false;

// PSRAM-aware allocation: prefer PSRAM on ESP32 when BOARD_HAS_PSRAM, fallback to internal heap or malloc.
// Use psram_free() for any pointer returned by psram_malloc().
static void* psram_malloc(size_t size) {
  if (size == 0) return nullptr;
#if defined(ESP_PLATFORM) && defined(BOARD_HAS_PSRAM)
  void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (p != nullptr) return p;
  p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL);
  return p;
#else
  return malloc(size);
#endif
}

static void* psram_calloc(size_t n, size_t size) {
  if (n == 0 || size == 0) return nullptr;
#if defined(ESP_PLATFORM) && defined(BOARD_HAS_PSRAM)
  void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
  if (p != nullptr) return p;
  return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL);
#else
  return calloc(n, size);
#endif
}

static void psram_free(void* ptr) {
  if (ptr == nullptr) return;
#if defined(ESP_PLATFORM)
  heap_caps_free(ptr);
#else
  free(ptr);
#endif
}

// Time (millis()) when WiFi was last seen connected; 0 when disconnected. Used for get wifi.status uptime.
static unsigned long s_wifi_connected_at = 0;

// Last WiFi disconnect reason (from ESP-IDF event). Used for get wifi.status diagnostics.
static uint8_t s_wifi_disconnect_reason = 0;
static unsigned long s_wifi_disconnect_time = 0;

#ifdef MQTT_MEMORY_DEBUG
// #region agent log
static void agentLogHeap(const char* location, const char* message, const char* hypothesisId,
                         size_t free_h, size_t max_alloc, unsigned long internal_free, unsigned long spiram_free) {
  char buf[320];
  snprintf(buf, sizeof(buf),
          "{\"sessionId\":\"debug-session\",\"location\":\"%s\",\"message\":\"%s\",\"hypothesisId\":\"%s\","
          "\"data\":{\"free\":%u,\"max_alloc\":%u,\"internal_free\":%lu,\"spiram_free\":%lu},\"timestamp\":%lu}",
          location, message, hypothesisId, (unsigned)free_h, (unsigned)max_alloc, internal_free, spiram_free,
          (unsigned long)millis());
  Serial.println(buf);
}
// #endregion
#endif

// Singleton for formatMqttStatusReply (set in begin(), cleared in end())
static MQTTBridge* s_mqtt_bridge_instance = nullptr;

unsigned long MQTTBridge::getWifiConnectedAtMillis() {
  return s_wifi_connected_at;
}

void MQTTBridge::formatMqttStatusReply(char* buf, size_t bufsize, const NodePrefs* prefs) {
  if (buf == nullptr || bufsize == 0) return;
  const char* msgs = (prefs->mqtt_status_enabled) ? "on" : "off";
  if (s_mqtt_bridge_instance == nullptr || !s_mqtt_bridge_instance->_initialized) {
    snprintf(buf, bufsize, "> msgs: %s (bridge not running)", msgs);
    return;
  }
  MQTTBridge* b = s_mqtt_bridge_instance;

  // Build per-slot status strings (compact format to fit 160-byte reply buffer)
  // Only show configured slots, skip "none" slots
  int q = 0;
#ifdef ESP_PLATFORM
  if (b->_packet_queue_handle != nullptr) {
    q = (int)uxQueueMessagesWaiting(b->_packet_queue_handle);
  }
#else
  q = b->_queue_count;
#endif

  int pos = snprintf(buf, bufsize, "> msgs: %s", msgs);
  for (int i = 0; i < RUNTIME_MQTT_SLOTS && pos < (int)bufsize - 1; i++) {
    const MQTTSlot& slot = b->_slots[i];
    const char* name = nullptr;
    const char* state = nullptr;

    if (!slot.enabled && slot.preset) {
      name = slot.preset->name;
      state = "inactive";
    } else if (!slot.enabled) {
      continue;  // Skip unconfigured slots
    } else if (!b->isSlotReady(i)) {
      name = slot.preset ? slot.preset->name : "custom";
      state = "wait";
    } else if (slot.connected) {
      name = slot.preset ? slot.preset->name : "custom";
      state = "ok";
    } else if (slot.circuit_breaker_tripped) {
      name = slot.preset ? slot.preset->name : "custom";
      state = "fail";
    } else {
      name = slot.preset ? slot.preset->name : "custom";
      state = "disc";
    }
    pos += snprintf(buf + pos, bufsize - pos, ", %d: %s (%s)", i + 1, name, state);
  }
  snprintf(buf + pos, bufsize - pos, ", q:%d", q);
}

uint8_t MQTTBridge::getLastWifiDisconnectReason() { return s_wifi_disconnect_reason; }
unsigned long MQTTBridge::getLastWifiDisconnectTime() { return s_wifi_disconnect_time; }

const char* MQTTBridge::wifiReasonStr(uint8_t reason) {
  switch (reason) {
    case 2:   return "auth expired";
    case 4:   return "assoc timeout";
    case 8:   return "AP disconnected";
    case 15:  return "4-way handshake timeout";
    case 34:  return "AP state mismatch (class 3 frame)";
    case 39:  return "SSID not found";
    case 63:  return "SA query timeout (PMF)";
    case 200: return "signal lost";
    case 201: return "security mismatch";
    case 202: return "auth mode rejected";
    default:  return nullptr;
  }
}

const char* MQTTBridge::tlsErrorStr(int32_t err) {
  switch (err) {
    case 0x8001: return "DNS failed";
    case 0x8002: return "socket error";
    case 0x8004: return "connect refused";
    case 0x8006: return "TLS timeout";
    case 0x800B: return "cert verify failed";
    case 0x8010: return "mbedTLS error";
    default:     return nullptr;
  }
}

void MQTTBridge::formatSlotDiagReply(char* buf, size_t bufsize, int slot_index) {
  if (!buf || bufsize == 0) return;
  if (!s_mqtt_bridge_instance || !s_mqtt_bridge_instance->_initialized) {
    snprintf(buf, bufsize, "> mqtt%d: bridge not running", slot_index + 1);
    return;
  }
  if (slot_index < 0 || slot_index >= RUNTIME_MQTT_SLOTS) {
    snprintf(buf, bufsize, "> invalid slot");
    return;
  }

  MQTTBridge* b = s_mqtt_bridge_instance;
  const MQTTSlot& slot = b->_slots[slot_index];

  // Determine state string
  const char* state;
  if (!slot.enabled && !slot.preset && slot.host[0] == '\0') {
    snprintf(buf, bufsize, "> mqtt%d: not configured", slot_index + 1);
    return;
  } else if (!slot.enabled) {
    state = "inactive";
  } else if (!slot.client) {
    state = "no client";
  } else if (slot.connected) {
    state = "ok";
  } else if (slot.circuit_breaker_tripped) {
    state = "fail";
  } else {
    state = "disc";
  }

  int pos = snprintf(buf, bufsize, "> mqtt%d: %s", slot_index + 1, state);
  if (slot.disconnect_count > 0) {
    pos += snprintf(buf + pos, bufsize - pos, ", dc:%lu", (unsigned long)slot.disconnect_count);
    if (slot.first_disconnect_time > 0) {
      unsigned long first_disc_age_sec = (millis() - slot.first_disconnect_time) / 1000;
      pos += snprintf(buf + pos, bufsize - pos, ", first_disc:%lus", first_disc_age_sec);
    }
  }

  // If connected with no errors, we're done
  if (slot.connected && slot.last_error_time == 0) {
    snprintf(buf + pos, bufsize - pos, ", no errors");
    return;
  }

  // Show last error if we have one
  if (slot.last_error_time > 0) {
    // TLS error with human-friendly description
    if (slot.last_tls_err != 0) {
      const char* desc = tlsErrorStr(slot.last_tls_err);
      if (desc) {
        pos += snprintf(buf + pos, bufsize - pos, ", %s (0x%04X)", desc, (unsigned)slot.last_tls_err);
      } else {
        pos += snprintf(buf + pos, bufsize - pos, ", tls:0x%04X", (unsigned)slot.last_tls_err);
      }
    }
    // mbedTLS stack error (shown as negative hex per convention)
    if (slot.last_tls_stack_err != 0) {
      pos += snprintf(buf + pos, bufsize - pos, ", mbedtls:-0x%04X", (unsigned)(-slot.last_tls_stack_err));
    }
    // Socket errno
    if (slot.last_sock_errno != 0) {
      pos += snprintf(buf + pos, bufsize - pos, ", sock:%d", slot.last_sock_errno);
    }
    // Time ago
    unsigned long ago_sec = (millis() - slot.last_error_time) / 1000;
    if (ago_sec < 60) {
      snprintf(buf + pos, bufsize - pos, ", %lus ago", ago_sec);
    } else if (ago_sec < 3600) {
      snprintf(buf + pos, bufsize - pos, ", %lum ago", ago_sec / 60);
    } else {
      snprintf(buf + pos, bufsize - pos, ", %luh ago", ago_sec / 3600);
    }
  } else if (!slot.connected) {
    snprintf(buf + pos, bufsize - pos, ", no error info");
  }
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
MQTTBridge::MQTTBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc, mesh::LocalIdentity *identity)
    : BridgeBase(prefs, mgr, rtc),
      _queue_count(0),
      _last_status_publish(0), _last_status_retry(0), _status_interval(300000),
      _ntp_client(_ntp_udp, "pool.ntp.org", 0, 60000), _last_ntp_sync(0), _ntp_synced(false), _ntp_sync_pending(false), _slots_setup_done(false), _max_active_slots(RUNTIME_MQTT_SLOTS),
      _timezone(nullptr), _last_raw_len(0), _last_snr(0), _last_rssi(0), _last_raw_timestamp(0),
      _identity(identity),
      _cached_has_connected_slots(false),
      _last_memory_check(0), _skipped_publishes(0), _last_fragmentation_recovery(0),
      _fragmentation_pressure_since(0), _last_critical_check_run(0),
      _last_no_broker_log(0), _queue_disconnected_since(0), _all_tripped_since(0),
      _last_config_warning(0),
      _dispatcher(nullptr), _radio(nullptr), _board(nullptr), _ms(nullptr),
#ifdef WITH_SNMP
      _snmp_agent(nullptr),
#endif
      _last_wifi_check(0), _last_wifi_status(WL_DISCONNECTED), _wifi_status_initialized(false),
      _wifi_disconnected_time(0), _last_wifi_reconnect_attempt(0), _wifi_reconnect_backoff_attempt(0),
      _last_slot_reconnect_ms(0)
#ifdef ESP_PLATFORM
      , _packet_queue_handle(nullptr), _mqtt_task_handle(nullptr), _raw_data_mutex(nullptr),
        _mqtt_task_stack(nullptr), _packet_queue_storage(nullptr)
#else
      , _queue_head(0), _queue_tail(0)
#endif
{
  // Initialize default values
  strncpy(_origin, "MeshCore-Repeater", sizeof(_origin) - 1);
  strncpy(_iata, "XXX", sizeof(_iata) - 1);
  strncpy(_device_id, "DEVICE_ID_PLACEHOLDER", sizeof(_device_id) - 1);
  strncpy(_firmware_version, "unknown", sizeof(_firmware_version) - 1);
  strncpy(_board_model, "unknown", sizeof(_board_model) - 1);
  strncpy(_build_date, "unknown", sizeof(_build_date) - 1);
  _status_enabled = true;
  _packets_enabled = true;
  _raw_enabled = false;
  _rx_enabled = true;
  _tx_mode = 0;

  // Initialize all slots to empty/disabled state
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    memset(&_slots[i], 0, sizeof(MQTTSlot));
    _slots[i].enabled = false;
    _slots[i].client = nullptr;
    _slots[i].preset = nullptr;
    _slots[i].auth_token = nullptr;
    _slots[i].connected = false;
    _slots[i].initial_connect_done = false;
    _slots[i].token_expires_at = 0;
    _slots[i].last_token_renewal = 0;
    _slots[i].reconnect_backoff = 0;
    _slots[i].max_backoff_failures = 0;
    _slots[i].circuit_breaker_tripped = false;
    _slots[i].last_reconnect_attempt = 0;
    _slots[i].last_log_time = 0;
    _slots[i].port = 1883;
    _slot_reconfigure_pending[i] = false;
  }

  // Initialize JWT username
  _jwt_username[0] = '\0';

  // Initialize packet queue (FreeRTOS queue will be created in begin())
  #ifdef ESP_PLATFORM
  // Queue and mutex will be created in begin()
  #else
  // Initialize circular buffer for non-ESP32 platforms
  memset(_packet_queue, 0, sizeof(_packet_queue));
#if defined(BOARD_HAS_PSRAM)
  for (int i = 0; i < MAX_QUEUE_SIZE; i++) {
    _packet_queue[i].has_raw_data = false;
  }
#endif
  #endif

  // Raw radio buffer in PSRAM when available
  _last_raw_data = (uint8_t*)psram_malloc(LAST_RAW_DATA_SIZE);

  // Pre-allocate JSON publish buffer (reused for all publishes to avoid alloc/free churn)
  _publish_json_buffer = (char*)psram_malloc(PUBLISH_JSON_BUFFER_SIZE);
  _status_json_buffer = (char*)psram_malloc(STATUS_JSON_BUFFER_SIZE);
}

// ---------------------------------------------------------------------------
// begin()
// ---------------------------------------------------------------------------
void MQTTBridge::begin() {
  MQTT_DEBUG_PRINTLN("Initializing MQTT Bridge...");

  // PSRAM diagnostic - helps debug memory fragmentation on boards with external RAM
  #ifdef BOARD_HAS_PSRAM
  {
    bool psram_available = psramFound();
    size_t psram_size = 0;
    size_t psram_free = 0;
    if (psram_available) {
      psram_size = ESP.getPsramSize();
      psram_free = ESP.getFreePsram();
    }
    MQTT_DEBUG_PRINTLN("PSRAM: found=%s, size=%u, free=%u",
      psram_available ? "YES" : "NO", psram_size, psram_free);
    if (!psram_available) {
      MQTT_DEBUG_PRINTLN("PSRAM: board has PSRAM flag but psramFound()=false. "
        "Trying explicit psramInit()...");
      bool init_result = psramInit();
      MQTT_DEBUG_PRINTLN("PSRAM: psramInit() returned %s", init_result ? "true" : "false");
      if (init_result) {
        psram_size = ESP.getPsramSize();
        psram_free = ESP.getFreePsram();
        MQTT_DEBUG_PRINTLN("PSRAM: after init - size=%u, free=%u", psram_size, psram_free);
      }
    }
    // Log internal heap for comparison
    MQTT_DEBUG_PRINTLN("PSRAM: internal_free=%u, internal_max_alloc=%u",
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  }
  #else
  MQTT_DEBUG_PRINTLN("PSRAM: not configured for this board (no BOARD_HAS_PSRAM)");
  #endif

  // Limit active slots based on available memory.
  // Each WSS/TLS connection needs ~40KB for mbedTLS buffers.
  // Without PSRAM, even 3 concurrent connections would exhaust internal heap.
  // With PSRAM, cap at 5 for safety (6 configurable but 5 active max).
  #if defined(ESP_PLATFORM) && defined(BOARD_HAS_PSRAM)
  _max_active_slots = psramFound() ? 5 : 2;
  #else
  _max_active_slots = 2;
  #endif
  MQTT_DEBUG_PRINTLN("Max active slots: %d", _max_active_slots);

  // Check if WiFi credentials are configured first
  if (!isWiFiConfigValid(_prefs)) {
    MQTT_DEBUG_PRINTLN("MQTT Bridge initialization skipped - WiFi credentials not configured");
    return;
  }

  // Update origin and IATA from preferences
  strncpy(_origin, _prefs->mqtt_origin, sizeof(_origin) - 1);
  _origin[sizeof(_origin) - 1] = '\0';
  strncpy(_iata, _prefs->mqtt_iata, sizeof(_iata) - 1);
  _iata[sizeof(_iata) - 1] = '\0';

  // Strip quotes from origin and IATA if present
  stripQuotes(_origin, sizeof(_origin));
  stripQuotes(_iata, sizeof(_iata));

  // Convert IATA code to uppercase (IATA codes are conventionally uppercase)
  for (int i = 0; _iata[i]; i++) {
    _iata[i] = toupper(_iata[i]);
  }

  // Update enabled flags from preferences
  _status_enabled = _prefs->mqtt_status_enabled;
  _packets_enabled = _prefs->mqtt_packets_enabled;
  _raw_enabled = _prefs->mqtt_raw_enabled;
  _rx_enabled = _prefs->mqtt_rx_enabled;
  _tx_mode = _prefs->mqtt_tx_enabled;  // 0=off, 1=all, 2=advert
  // Set status interval to 5 minutes (300000 ms), or use preference if set and valid
  if (_prefs->mqtt_status_interval >= 1000 && _prefs->mqtt_status_interval <= 3600000) {
    _status_interval = _prefs->mqtt_status_interval;
  } else {
    // Invalid or uninitialized value - fix it in preferences and use default
    _prefs->mqtt_status_interval = 300000; // Fix the preference value
    _status_interval = 300000; // 5 minutes default
  }

  // Check for configuration mismatch: bridge.source=tx but mqtt.tx=off
  checkConfigurationMismatch();

  MQTT_DEBUG_PRINTLN("Config: Origin=%s, IATA=%s, Device=%s", _origin, _iata, _device_id);

  // Apply slot presets from preferences
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    const char* preset_name = _prefs->mqtt_slot_preset[i];
    if (preset_name[0] != '\0' && strcmp(preset_name, MQTT_PRESET_NONE) != 0) {
      if (strcmp(preset_name, MQTT_PRESET_CUSTOM) == 0) {
        // Custom broker: copy host/port/username/password from prefs
        _slots[i].preset = nullptr;
        strncpy(_slots[i].host, _prefs->mqtt_slot_host[i], sizeof(_slots[i].host) - 1);
        _slots[i].host[sizeof(_slots[i].host) - 1] = '\0';
        if (strlen(_slots[i].host) == 0) {
          MQTT_DEBUG_PRINTLN("MQTT%d: custom preset has no server configured, disabling", i + 1);
          _slots[i].enabled = false;
          continue;
        }
        _slots[i].enabled = true;
        _slots[i].port = _prefs->mqtt_slot_port[i];
        strncpy(_slots[i].username, _prefs->mqtt_slot_username[i], sizeof(_slots[i].username) - 1);
        _slots[i].username[sizeof(_slots[i].username) - 1] = '\0';
        strncpy(_slots[i].password, _prefs->mqtt_slot_password[i], sizeof(_slots[i].password) - 1);
        _slots[i].password[sizeof(_slots[i].password) - 1] = '\0';
        strncpy(_slots[i].audience, _prefs->mqtt_slot_audience[i], sizeof(_slots[i].audience) - 1);
        _slots[i].audience[sizeof(_slots[i].audience) - 1] = '\0';
      } else {
        const MQTTPresetDef* preset = findMQTTPreset(preset_name);
        if (preset) {
          _slots[i].enabled = true;
          _slots[i].preset = preset;
        } else {
          MQTT_DEBUG_PRINTLN("MQTT%d: unknown preset '%s', disabling", i + 1, preset_name);
          _slots[i].enabled = false;
        }
      }
    }
  }

  // Log slot configuration
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    if (_slots[i].enabled) {
      if (_slots[i].preset) {
        MQTT_DEBUG_PRINTLN("MQTT%d: preset=%s", i + 1, _slots[i].preset->name);
      } else {
        MQTT_DEBUG_PRINTLN("MQTT%d: custom=%s:%d", i + 1, _slots[i].host, _slots[i].port);
      }
    } else {
      MQTT_DEBUG_PRINTLN("MQTT%d: none", i + 1);
    }
  }

  #ifdef ESP_PLATFORM
  // Create FreeRTOS queue; use PSRAM storage when available
  #ifdef BOARD_HAS_PSRAM
  _packet_queue_storage = (uint8_t*)psram_malloc(MAX_QUEUE_SIZE * sizeof(QueuedPacket));
  if (_packet_queue_storage != nullptr) {
    _packet_queue_handle = xQueueCreateStatic(MAX_QUEUE_SIZE, sizeof(QueuedPacket), _packet_queue_storage, &_packet_queue_struct);
  } else {
    _packet_queue_handle = nullptr;
  }
  #else
  _packet_queue_storage = nullptr;
  _packet_queue_handle = nullptr;
  #endif
  if (_packet_queue_handle == nullptr) {
    _packet_queue_handle = xQueueCreate(MAX_QUEUE_SIZE, sizeof(QueuedPacket));
  }
  if (_packet_queue_handle == nullptr) {
    MQTT_DEBUG_PRINTLN("Failed to create packet queue!");
    psram_free(_packet_queue_storage);
    _packet_queue_storage = nullptr;
    return;
  }

  // Create mutex for raw radio data protection
  _raw_data_mutex = xSemaphoreCreateMutex();
  if (_raw_data_mutex == nullptr) {
    MQTT_DEBUG_PRINTLN("Failed to create raw data mutex!");
    vQueueDelete(_packet_queue_handle);
    _packet_queue_handle = nullptr;
    return;
  }

  // Create FreeRTOS task for MQTT/WiFi processing on Core 0
  #ifndef MQTT_TASK_CORE
  #define MQTT_TASK_CORE 0
  #endif
  #ifndef MQTT_TASK_STACK_SIZE
  #define MQTT_TASK_STACK_SIZE 8192
  #endif
  #ifndef MQTT_TASK_PRIORITY
  #define MQTT_TASK_PRIORITY 1
  #endif

  // Task stack: use dynamic allocation (internal RAM). PSRAM stack was disabled because it
  // causes resets on some boards (e.g. Heltec V4) when the task runs from PSRAM stack.
  _mqtt_task_stack = nullptr;
  _mqtt_task_handle = nullptr;
  BaseType_t create_result = xTaskCreatePinnedToCore(
    mqttTask,
    "MQTTBridge",
    MQTT_TASK_STACK_SIZE,
    this,
    MQTT_TASK_PRIORITY,
    &_mqtt_task_handle,
    MQTT_TASK_CORE
  );
  if (create_result != pdPASS) _mqtt_task_handle = nullptr;
  if (_mqtt_task_handle == nullptr) {
    MQTT_DEBUG_PRINTLN("Failed to create MQTT task!");
    psram_free(_mqtt_task_stack);
    _mqtt_task_stack = nullptr;
    vQueueDelete(_packet_queue_handle);
    _packet_queue_handle = nullptr;
    psram_free(_packet_queue_storage);
    _packet_queue_storage = nullptr;
    vSemaphoreDelete(_raw_data_mutex);
    _raw_data_mutex = nullptr;
    return;
  }

  MQTT_DEBUG_PRINTLN("MQTT task created on Core %d", MQTT_TASK_CORE);
  #else
  // Non-ESP32: Initialize WiFi directly (no task)
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setAutoConnect(true);
  WiFi.begin(_prefs->wifi_ssid, _prefs->wifi_password);

  // NOTE: Slot setup deferred until after NTP sync in loop()
  #endif

  _initialized = true;
  s_mqtt_bridge_instance = this;
  MQTT_DEBUG_PRINTLN("MQTT Bridge initialized");
}

// ---------------------------------------------------------------------------
// end()
// ---------------------------------------------------------------------------
void MQTTBridge::end() {
  MQTT_DEBUG_PRINTLN("Stopping MQTT Bridge...");
  s_mqtt_bridge_instance = nullptr;

  #ifdef ESP_PLATFORM
  // Delete FreeRTOS task first (it will clean up WiFi/MQTT connections)
  if (_mqtt_task_handle != nullptr) {
    vTaskDelete(_mqtt_task_handle);
    _mqtt_task_handle = nullptr;
  }
  // Free PSRAM task stack
  psram_free(_mqtt_task_stack);
  _mqtt_task_stack = nullptr;

  // Clean up queued packets from FreeRTOS queue
  // Packets are value-copied in the queue, so no external pointers to clean up.
  if (_packet_queue_handle != nullptr) {
    QueuedPacket queued;
    while (xQueueReceive(_packet_queue_handle, &queued, 0) == pdTRUE) {
      _queue_count--;
    }
    vQueueDelete(_packet_queue_handle);
    _packet_queue_handle = nullptr;
  }
  psram_free(_packet_queue_storage);
  _packet_queue_storage = nullptr;

  // Delete mutex
  if (_raw_data_mutex != nullptr) {
    vSemaphoreDelete(_raw_data_mutex);
    _raw_data_mutex = nullptr;
  }
  #else
  // Clean up queued packet references
  // Packets are value-copied in the queue, so no external pointers to clean up.
  for (int i = 0; i < _queue_count; i++) {
    int index = (_queue_head + i) % MAX_QUEUE_SIZE;
    memset(&_packet_queue[index], 0, sizeof(QueuedPacket));
  }

  _queue_count = 0;
  _queue_head = 0;
  _queue_tail = 0;
  memset(_packet_queue, 0, sizeof(_packet_queue));
  #endif

  // Teardown all slots
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    teardownSlot(i);
  }

  // Clean up timezone object to prevent memory leak
  if (_timezone) {
    delete _timezone;
    _timezone = nullptr;
  }

  // Free PSRAM-backed buffers
  psram_free(_last_raw_data);
  _last_raw_data = nullptr;
  psram_free(_publish_json_buffer);
  _publish_json_buffer = nullptr;
  psram_free(_status_json_buffer);
  _status_json_buffer = nullptr;

  _initialized = false;
  _slots_setup_done = false;  // Reset so deferred setup runs again on next begin()
  MQTT_DEBUG_PRINTLN("MQTT Bridge stopped");
}

// ---------------------------------------------------------------------------
// FreeRTOS task entry point
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
void MQTTBridge::mqttTask(void* parameter) {
  MQTTBridge* bridge = static_cast<MQTTBridge*>(parameter);
  if (bridge) {
    bridge->mqttTaskLoop();
  }
  // Task should never return, but if it does, delete itself
  vTaskDelete(nullptr);
}

void MQTTBridge::initializeWiFiInTask() {
  MQTT_DEBUG_PRINTLN("Initializing WiFi in MQTT task...");

  // Initialize WiFi
  WiFi.mode(WIFI_STA);

  // Enable automatic reconnection - ESP32 will handle reconnection automatically
  WiFi.setAutoReconnect(true);
  WiFi.setAutoConnect(true);

  // Set up WiFi event handlers for better diagnostics and immediate disconnection detection
  WiFi.onEvent([this](WiFiEvent_t event, WiFiEventInfo_t info) {
    switch(event) {
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        MQTT_DEBUG_PRINTLN("WiFi connected: %s", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
        // Set flag to trigger NTP sync from loop() instead of doing it here
        if (!_ntp_synced && !_ntp_sync_pending) {
          _ntp_sync_pending = true;
        }
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        s_wifi_disconnect_reason = info.wifi_sta_disconnected.reason;
        s_wifi_disconnect_time = millis();
        MQTT_DEBUG_PRINTLN("WiFi disconnected: reason %d", s_wifi_disconnect_reason);
        break;
      default:
        break;
    }
  });

  WiFi.begin(_prefs->wifi_ssid, _prefs->wifi_password);

  // NOTE: Slot setup is deferred until after NTP sync in mqttTaskLoop().
  // JWT-auth slots need valid timestamps for token creation, and connecting
  // before NTP sync just wastes heap on TLS handshakes that will be rejected.

  MQTT_DEBUG_PRINTLN("WiFi initialization started in task");
}

// ---------------------------------------------------------------------------
// mqttTaskLoop() - main loop running on Core 0
// ---------------------------------------------------------------------------
void MQTTBridge::mqttTaskLoop() {
  // Initialize WiFi first
  initializeWiFiInTask();

  // Wait a bit for WiFi to start connecting
  vTaskDelay(pdMS_TO_TICKS(1000));

  // Main task loop
  #ifdef MQTT_MEMORY_DEBUG
  static unsigned long last_agent_log = 0;
  #endif
  while (true) {
    #ifdef MQTT_MEMORY_DEBUG
    // #region agent log
    unsigned long now_loop = millis();
    if (now_loop - last_agent_log >= 60000) {
      last_agent_log = now_loop;
      size_t free_h = ESP.getFreeHeap();
      size_t max_alloc = ESP.getMaxAllocHeap();
      unsigned long internal_f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      unsigned long spiram_f = 0;
      #ifdef BOARD_HAS_PSRAM
      spiram_f = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      #endif
      agentLogHeap("MQTTBridge.cpp:mqttTaskLoop", "mqtt_loop_60s", "H5", free_h, max_alloc, internal_f, spiram_f);
    }
    // #endregion
    #endif

    unsigned long now = millis();
    bool wifi_just_connected = handleWiFiConnection(now);
    if (wifi_just_connected) {
      // WiFi recovered — reset last_reconnect_attempt for disconnected slots so they
      // retry immediately rather than waiting up to 5 min for backoff timers to expire.
      for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
        if (_slots[i].enabled && _slots[i].initial_connect_done && !_slots[i].connected) {
          _slots[i].last_reconnect_attempt = 0;
        }
      }
    }

    // Check for pending NTP sync (triggered from WiFi event handler)
    if (_ntp_sync_pending && WiFi.status() == WL_CONNECTED) {
      _ntp_sync_pending = false;
      syncTimeWithNTP();
    }

    // Retry NTP every 30s if initial sync failed (slots can't start without valid time)
    if (!_ntp_synced && WiFi.status() == WL_CONNECTED) {
      static unsigned long last_ntp_retry = 0;
      if (now - last_ntp_retry >= 30000) {
        last_ntp_retry = now;
        syncTimeWithNTP();
      }
    }

    // Deferred slot setup: wait until NTP is synced so JWT tokens get valid timestamps.
    // This avoids wasted TLS handshakes that get rejected due to bad token times.
    if (_ntp_synced && !_slots_setup_done) {
      _slots_setup_done = true;

      // Redirect mbedTLS allocations to PSRAM to save ~40KB internal heap per TLS connection.
      // This is critical when running 3 concurrent WSS connections.
      #if defined(BOARD_HAS_PSRAM)
      mbedtls_platform_set_calloc_free(psram_calloc, psram_free);
      MQTT_DEBUG_PRINTLN("mbedTLS allocator redirected to PSRAM");
      #endif

      MQTT_DEBUG_PRINTLN("NTP synced, setting up MQTT slots (max %d active)...", _max_active_slots);
      int active_count = 0;
      for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
        if (_slots[i].enabled) {
          if (active_count >= _max_active_slots) {
            MQTT_DEBUG_PRINTLN("MQTT%d skipped: max active slots (%d) reached (no PSRAM)", i + 1, _max_active_slots);
            _slots[i].enabled = false;  // Disable so other loops skip it
            continue;
          }
          char reason[80];
          if (!isSlotReady(i, reason, sizeof(reason))) {
            MQTT_DEBUG_PRINTLN("MQTT%d not ready — run '%s' to connect", i + 1, reason);
            continue;
          }
          setupSlot(i);
          active_count++;
          // Stagger connections: 5s between slots to avoid simultaneous TLS handshakes
          // which compete for ~40KB internal heap each
          if (i < RUNTIME_MQTT_SLOTS - 1) {
            vTaskDelay(pdMS_TO_TICKS(5000));
          }
        }
      }
    }

    // Process pending slot reconfigures (queued from CLI on Core 1)
    for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
      if (_slot_reconfigure_pending[i]) {
        _slot_reconfigure_pending[i] = false;
        MQTT_DEBUG_PRINTLN("Applying deferred reconfigure for MQTT%d (preset: %s)", i + 1, _prefs->mqtt_slot_preset[i]);
        applySlotPreset(i, _prefs->mqtt_slot_preset[i]);
      }
    }

    // Maintain slot connections (token renewal, reconnect with backoff)
    maintainSlotConnections();

    // Process packet queue
    processPacketQueue();

#ifdef WITH_SNMP
    // SNMP agent loop — process incoming UDP requests
    if (_snmp_agent) {
      if (!_snmp_agent->isRunning() && WiFi.isConnected() && _prefs->snmp_enabled) {
        _snmp_agent->begin(_prefs->snmp_community);
        MQTT_DEBUG_PRINTLN("SNMP agent started on port 161 (community: %s)", _prefs->snmp_community);
      }
      if (_snmp_agent->isRunning()) {
        // Update MQTT stats from this core
        int connected = 0;
        for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
          if (_slots[i].enabled && _slots[i].connected) connected++;
        }
        _snmp_agent->updateMQTTStats(connected, _queue_count, _skipped_publishes);
        _snmp_agent->loop();
      }
    }
#endif

    // Periodic configuration check (throttled to avoid spam)
    checkConfigurationMismatch();

    // Periodic NTP refresh (every hour) — lightweight, non-blocking.
    // Uses async SNTP instead of the heavy syncTimeWithNTP() which blocks Core 0
    // for up to 20+ seconds with DNS lookups, UDP sockets, and retry loops.
    if (WiFi.status() == WL_CONNECTED && now - _last_ntp_sync > 3600000) {
      refreshNTP();
    }

    // Publish status updates (handle millis() overflow correctly)
    if (_status_enabled) {
      bool has_destinations = _cached_has_connected_slots;

      // Early exit if no destinations - skip all the expensive logic below
      if (!has_destinations) {
        if (_last_status_retry != 0) {
          _last_status_retry = 0;
        }
      } else {
        bool should_publish = false;

        // First, check if we need to respect retry interval (prevents spam when publish keeps failing)
        if (_last_status_retry != 0) {
          unsigned long retry_elapsed = (now >= _last_status_retry) ?
                                       (now - _last_status_retry) :
                                       (ULONG_MAX - _last_status_retry + now + 1);
          if (retry_elapsed < STATUS_RETRY_INTERVAL) {
            should_publish = false;
          } else {
            should_publish = true;
          }
        } else {
          if (_last_status_publish == 0) {
            should_publish = true;
          } else {
            unsigned long elapsed = (now >= _last_status_publish) ?
                                   (now - _last_status_publish) :
                                   (ULONG_MAX - _last_status_publish + now + 1);
            should_publish = (elapsed >= _status_interval);
          }
        }

        if (should_publish) {
          if (_last_status_publish != 0) {
            unsigned long elapsed = (now >= _last_status_publish) ?
                                   (now - _last_status_publish) :
                                   (ULONG_MAX - _last_status_publish + now + 1);
            MQTT_DEBUG_PRINTLN("Status publish timer expired (elapsed: %lu ms, interval: %lu ms)", elapsed, _status_interval);
          } else {
            MQTT_DEBUG_PRINTLN("Status publish attempt (first publish or retry)");
          }

          _last_status_retry = now;
          if (publishStatus()) {
            _last_status_publish = now;
            _last_status_retry = 0;
            MQTT_DEBUG_PRINTLN("Status published successfully, next publish in %lu ms", _status_interval);
            // If we're in the hole but just proved connectivity, recover sooner than the dedicated pressure timer
            size_t max_alloc = ESP.getMaxAllocHeap();
            if (max_alloc < 58000 && (now - _last_fragmentation_recovery) > 300000) {
              _last_fragmentation_recovery = now;
              _fragmentation_pressure_since = 0;
              MQTT_DEBUG_PRINTLN("Fragmentation recovery after status (max_alloc=%d)", (int)max_alloc);
              recreateMqttClientsForFragmentationRecovery();
            }
          } else {
            MQTT_DEBUG_PRINTLN("Status publish failed, will retry in %lu ms", STATUS_RETRY_INTERVAL);
          }
        }
      }
    }

    runCriticalMemoryCheckAndRecovery();

    // Update cached connection status periodically (every 5 seconds)
    // This ensures cache stays accurate even if callbacks miss updates
    static unsigned long last_slot_status_update = 0;
    if (now - last_slot_status_update > 5000) {
      updateCachedConnectionStatus();
      last_slot_status_update = now;
    }

    // Adaptive task delay based on work done
    bool has_work = (_queue_count > 0);
    if (!has_work && _status_enabled) {
      if (_last_status_publish == 0 ||
          (now - _last_status_publish >= (_status_interval - 10000))) {
        has_work = true;
      }
    }

    // Adaptive delay: shorter when work pending, longer when idle
    if (has_work) {
      vTaskDelay(pdMS_TO_TICKS(5));
    } else {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}
#endif

// ---------------------------------------------------------------------------
// Slot management
// ---------------------------------------------------------------------------

void MQTTBridge::setupSlot(int index) {
  if (index < 0 || index >= RUNTIME_MQTT_SLOTS) return;
  MQTTSlot& slot = _slots[index];

  if (!slot.enabled) {
    teardownSlot(index);
    return;
  }

  // Don't recreate if already exists
  if (slot.client) return;

  slot.client = new PsychicMqttClient();
  slot.client->setAutoReconnect(false);  // We handle reconnect with our own backoff logic
  bool uses_jwt = (slot.preset && slot.preset->auth_type == MQTT_AUTH_JWT) || slot.audience[0] != '\0';
  optimizeMqttClientConfig(slot.client, uses_jwt);  // sets keepalive (45s PSRAM, 75s non-PSRAM)
  #ifndef MQTT_FORCE_KEEPALIVE_45
  #if defined(BOARD_HAS_PSRAM)
  if (slot.preset && slot.preset->keepalive > 0) {
    slot.client->setKeepAlive(slot.preset->keepalive);  // preset overrides default
  }
  #else
  // Non-PSRAM: keep the longer 75s default to reduce TLS churn.
  // Preset keepalive (55s) is more aggressive than needed behind Cloudflare.
  #endif
  #endif

  // Callbacks (capture index by value)
  slot.client->onConnect([this, index](bool sessionPresent) {
    MQTT_DEBUG_PRINTLN("MQTT%d connected", index + 1);
    _slots[index].connected = true;
    _slots[index].reconnect_backoff = 0;
    _slots[index].max_backoff_failures = 0;
    _slots[index].circuit_breaker_tripped = false;
    _slots[index].last_tls_err = 0;
    _slots[index].last_tls_stack_err = 0;
    _slots[index].last_sock_errno = 0;
    _slots[index].last_error_time = 0;
    updateCachedConnectionStatus();
    publishStatusToSlot(index);
  });
  slot.client->onDisconnect([this, index](bool sessionPresent) {
    MQTT_DEBUG_PRINTLN("MQTT%d disconnected", index + 1);
    _slots[index].disconnect_count++;
    if (_slots[index].first_disconnect_time == 0) {
      _slots[index].first_disconnect_time = millis();
    }
    _slots[index].connected = false;
    updateCachedConnectionStatus();
  });
  slot.client->onError([this, index](esp_mqtt_error_codes error) {
    _slots[index].last_tls_err = error.esp_tls_last_esp_err;
    _slots[index].last_tls_stack_err = error.esp_tls_stack_err;
    _slots[index].last_sock_errno = error.esp_transport_sock_errno;
    _slots[index].last_error_time = millis();
    if (error.esp_tls_last_esp_err != 0 || error.esp_tls_stack_err != 0 || error.esp_transport_sock_errno != 0) {
      MQTT_DEBUG_PRINTLN("MQTT%d error: tls=%d, tls_stack=%d, sock=%d, type=%d",
        index + 1, error.esp_tls_last_esp_err, error.esp_tls_stack_err,
        error.esp_transport_sock_errno, error.error_type);
    } else {
      MQTT_DEBUG_PRINTLN("MQTT%d error: type=%d", index + 1, error.error_type);
    }
  });

  if (slot.preset) {
    // Preset-based slot
    slot.client->setServer(slot.preset->server_url);
    if (slot.preset->ca_cert) {
      slot.client->setCACert(slot.preset->ca_cert);
    }

    // Allocate JWT token buffer if needed
    if (slot.preset->auth_type == MQTT_AUTH_JWT && !slot.auth_token) {
      slot.auth_token = (char*)psram_malloc(AUTH_TOKEN_SIZE);
      if (slot.auth_token) slot.auth_token[0] = '\0';
    }

    // Try to create token and connect (will succeed only if NTP synced)
    if (slot.preset->auth_type == MQTT_AUTH_JWT) {
      createSlotAuthToken(index);
      if (slot.auth_token && strlen(slot.auth_token) > 0) {
        slot.client->setCredentials(_jwt_username, slot.auth_token);
      }
    } else if (slot.preset->auth_type == MQTT_AUTH_USERPASS &&
               slot.preset->userpass_username && slot.preset->userpass_password) {
      slot.client->setCredentials(slot.preset->userpass_username, slot.preset->userpass_password);
    }
  } else {
    // Custom broker slot — build persistent URI
    // If host already has a scheme (mqtt://, mqtts://, ws://, wss://), preserve the full URI
    // (including optional path/query) and only inject :port when the authority has no explicit port.
    // Otherwise, infer protocol from port number.
    bool has_scheme = (strncmp(slot.host, "mqtt://", 7) == 0 ||
                       strncmp(slot.host, "mqtts://", 8) == 0 ||
                       strncmp(slot.host, "ws://", 5) == 0 ||
                       strncmp(slot.host, "wss://", 6) == 0);
    if (has_scheme) {
      const char* authority = strstr(slot.host, "://");
      authority = authority ? authority + 3 : slot.host;
      const char* path = strchr(authority, '/');
      const char* authority_end = path ? path : slot.host + strlen(slot.host);
      bool has_explicit_port = false;

      // Detect host:port in URI authority (IPv6 literals in [addr]:port are supported).
      if (authority < authority_end) {
        if (*authority == '[') {
          const char* close = (const char*)memchr(authority, ']', authority_end - authority);
          if (close && (close + 1) < authority_end && *(close + 1) == ':') {
            has_explicit_port = true;
          }
        } else {
          const char* colon = (const char*)memchr(authority, ':', authority_end - authority);
          if (colon != nullptr) {
            has_explicit_port = true;
          }
        }
      }

      if (has_explicit_port || slot.port == 0) {
        snprintf(slot.broker_uri, sizeof(slot.broker_uri), "%s", slot.host);
      } else {
        const size_t authority_len = (size_t)(authority_end - slot.host);
        snprintf(slot.broker_uri, sizeof(slot.broker_uri), "%.*s:%u%s",
                 (int)authority_len,
                 slot.host,
                 (unsigned)slot.port,
                 path ? path : "");
      }
    } else {
      const char* proto = "mqtt";
      if (slot.port == 8883) {
        proto = "mqtts";
      } else if (slot.port == 443) {
        proto = "wss";
      }
      snprintf(slot.broker_uri, sizeof(slot.broker_uri), "%s://%s:%d", proto, slot.host, slot.port);
    }
    slot.client->setServer(slot.broker_uri);
    MQTT_DEBUG_PRINTLN("MQTT%d custom broker URI: %s (host='%s', port=%u)",
      index + 1, slot.broker_uri, slot.host, (unsigned)slot.port);

    // Custom TLS/WSS slots need a CA bundle for server verification.
    // The bundle is loaded into the global s_crt_bundle exactly once to avoid
    // a use-after-free race: connect() launches an async FreeRTOS task, and
    // calling setCACertBundle() again from a later slot would free the global
    // crts array while a prior slot's TLS handshake may still be reading it.
    bool needs_tls = (strncmp(slot.broker_uri, "mqtts://", 8) == 0 ||
                      strncmp(slot.broker_uri, "wss://", 6) == 0);
    if (needs_tls) {
      if (!s_ca_bundle_loaded) {
        size_t bundle_len = 0;
        if (rootca_crt_bundle_start != nullptr &&
            rootca_crt_bundle_end != nullptr &&
            rootca_crt_bundle_end > rootca_crt_bundle_start) {
          bundle_len = static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start);
        }

        if (bundle_len > 0) {
          MQTT_DEBUG_PRINTLN("MQTT global CA bundle init: embedded bundle (%u bytes)",
            (unsigned)bundle_len);
          // Load the bundle into the global s_crt_bundle via the first client.
          // This is a one-time operation; subsequent clients reuse via attachArduinoCACertBundle.
          slot.client->setCACertBundle(rootca_crt_bundle_start, bundle_len);
          s_ca_bundle_loaded = true;
        } else {
          MQTT_DEBUG_PRINTLN("MQTT%d TLS: no embedded cert bundle available", index + 1);
        }
      } else {
        // Global bundle already loaded — just attach the callback for this client.
        slot.client->attachArduinoCACertBundle(true);
      }
      MQTT_DEBUG_PRINTLN("MQTT%d TLS verify: CA bundle %s", index + 1,
        s_ca_bundle_loaded ? "active" : "unavailable");
    } else {
      MQTT_DEBUG_PRINTLN("MQTT%d custom broker uses non-TLS transport", index + 1);
    }

    // Custom slot authentication: JWT if audience is set, else username/password
    if (slot.audience[0] != '\0') {
      // JWT auth for custom slot — allocate token buffer and create initial token
      if (!slot.auth_token) {
        slot.auth_token = (char*)psram_malloc(AUTH_TOKEN_SIZE);
        if (slot.auth_token) slot.auth_token[0] = '\0';
      }
      createSlotAuthToken(index);
      if (slot.auth_token && strlen(slot.auth_token) > 0) {
        slot.client->setCredentials(_jwt_username, slot.auth_token);
      }
      MQTT_DEBUG_PRINTLN("MQTT%d custom broker using JWT auth (audience: %s)", index + 1, slot.audience);
    } else if (strlen(slot.username) > 0) {
      slot.client->setCredentials(slot.username, slot.password);
    }
  }

  slot.client->connect();
  slot.initial_connect_done = true;
}

void MQTTBridge::teardownSlot(int index) {
  if (index < 0 || index >= RUNTIME_MQTT_SLOTS) return;
  MQTTSlot& slot = _slots[index];

  if (slot.client) {
    if (slot.client->connected()) {
      slot.client->disconnect();
    }
    #ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(50));
    #else
    delay(50);
    #endif
    delete slot.client;
    slot.client = nullptr;
  }

  // Free auth token buffer
  if (slot.auth_token) {
    psram_free(slot.auth_token);
    slot.auth_token = nullptr;
  }

  slot.connected = false;
  slot.initial_connect_done = false;
  slot.broker_uri[0] = '\0';
  slot.token_expires_at = 0;
  slot.last_token_renewal = 0;
  slot.reconnect_backoff = 0;
  slot.max_backoff_failures = 0;
  slot.circuit_breaker_tripped = false;
  slot.last_reconnect_attempt = 0;
  slot.last_log_time = 0;
}

void MQTTBridge::maintainSlotConnections() {
  if (!_identity) return;

  // Check WiFi status first
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now_millis = millis();
  unsigned long current_time = time(nullptr);
  bool time_synced = (current_time >= 1000000000); // After year 2001

  // JWT tokens require valid timestamps
  unsigned long clock_sec = current_time;
  bool clock_looks_set = (clock_sec >= 1735689600);  // 2025-01-01 00:00:00 UTC
  bool can_do_jwt = _ntp_synced || clock_looks_set;

  // Count connected slots to inform reconnect decisions
  int connected_count = 0;
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    if (_slots[i].enabled && _slots[i].connected) connected_count++;
  }

  // Only allow one reconnect attempt per maintenance cycle to avoid
  // multiple simultaneous TLS handshakes blocking the network stack.
  // Time-based guard: block reconnects if any slot reconnected within the last 15 s,
  // ensuring the previous TLS handshake (and its Core-0-expensive completion events)
  // finish before the next slot begins its own handshake.
  const unsigned long RECONNECT_GUARD_MS = 15000UL;
  bool reconnect_attempted_this_cycle = (now_millis - _last_slot_reconnect_ms < RECONNECT_GUARD_MS);
  // Only allow one full teardown+setup per cycle to limit heap fragmentation
  // when multiple slots fail simultaneously
  bool teardown_attempted_this_cycle = false;

  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    if (!_slots[i].enabled || !_slots[i].client) continue;

    // JWT slots need time sync before we can manage tokens
    bool slot_jwt = (_slots[i].preset && _slots[i].preset->auth_type == MQTT_AUTH_JWT) ||
                    (!_slots[i].preset && _slots[i].audience[0] != '\0' && _slots[i].auth_token != nullptr);
    if (slot_jwt && !can_do_jwt) {
      continue;
    }

    maintainSlotConnection(i, now_millis, current_time, time_synced, reconnect_attempted_this_cycle, teardown_attempted_this_cycle);
  }
}

void MQTTBridge::maintainSlotConnection(int index, unsigned long now_millis, unsigned long current_time, bool time_synced, bool& reconnect_attempted, bool& teardown_attempted) {
  MQTTSlot& slot = _slots[index];

  if (slot.connected) {
    slot.reconnect_backoff = 0;
    slot.max_backoff_failures = 0;
  }

  // JWT token renewal (for preset JWT slots and custom slots with audience set)
  bool slot_uses_jwt = (slot.preset && slot.preset->auth_type == MQTT_AUTH_JWT) ||
                       (!slot.preset && slot.audience[0] != '\0' && slot.auth_token != nullptr);
  if (slot_uses_jwt) {
    bool token_needs_renewal = false;
    if (!time_synced) {
      token_needs_renewal = (slot.token_expires_at == 0);
    } else {
      const unsigned long RENEWAL_BUFFER = 60;
      token_needs_renewal = (slot.token_expires_at == 0) ||
                           !(slot.token_expires_at >= 1000000000) ||
                           (current_time >= slot.token_expires_at) ||
                           (current_time >= (slot.token_expires_at - RENEWAL_BUFFER));
    }

    // Throttle renewal attempts to once per minute
    const unsigned long RENEWAL_THROTTLE_MS = 60000;
    bool can_attempt_renewal = (now_millis - slot.last_token_renewal) >= RENEWAL_THROTTLE_MS;

    if (token_needs_renewal && can_attempt_renewal) {
      slot.last_token_renewal = now_millis;

      unsigned long old_token_expires_at = slot.token_expires_at;

      if (createSlotAuthToken(index)) {
        MQTT_DEBUG_PRINTLN("MQTT%d token renewed", index + 1);

        const unsigned long DISCONNECT_THRESHOLD = 60;
        bool old_token_expired_or_imminent = !time_synced ||
                                            (old_token_expires_at == 0) ||
                                            (current_time >= old_token_expires_at) ||
                                            (time_synced && old_token_expires_at >= 1000000000 &&
                                             current_time >= (old_token_expires_at - DISCONNECT_THRESHOLD));

        if (old_token_expired_or_imminent || !slot.client->connected()) {
          // Disconnect + reconnect with fresh credentials, reusing existing client
          // to avoid internal heap leak/fragmentation from destroy/create cycles
          MQTT_DEBUG_PRINTLN("MQTT%d token renewal: reconnecting with fresh credentials", index + 1);
          if (slot.client->connected()) {
            slot.client->disconnect();  // stops the client internally
          }
          slot.client->setCredentials(_jwt_username, slot.auth_token);
          slot.client->connect();  // restart stopped client; reconnect() fails silently on a stopped client
          reconnect_attempted = true;
          _last_slot_reconnect_ms = now_millis;
        } else {
          // Token renewed but old one still valid — just update credentials for next reconnect
          slot.client->setCredentials(_jwt_username, slot.auth_token);
        }
      } else {
        MQTT_DEBUG_PRINTLN("MQTT%d token renewal failed", index + 1);
        slot.token_expires_at = 0;
      }
      return; // Token renewal handled connect; skip backoff logic below
    }
  }

  // Periodic probe for circuit-breaker-tripped slots (recovery from transient outages)
  // Attempts a single reconnect every 30 minutes to see if the server has come back
  if (slot.circuit_breaker_tripped && !reconnect_attempted) {
    static const unsigned long CIRCUIT_BREAKER_PROBE_INTERVAL_MS = 1800000UL; // 30 minutes
    unsigned long probe_elapsed = (now_millis >= slot.last_reconnect_attempt) ?
                                  (now_millis - slot.last_reconnect_attempt) :
                                  (ULONG_MAX - slot.last_reconnect_attempt + now_millis + 1);
    if (probe_elapsed >= CIRCUIT_BREAKER_PROBE_INTERVAL_MS) {
      slot.last_reconnect_attempt = now_millis;
      reconnect_attempted = true;
      _last_slot_reconnect_ms = now_millis;
      MQTT_DEBUG_PRINTLN("MQTT%d circuit breaker probe (attempting single reconnect after %lu ms)", index + 1, probe_elapsed);
      if (slot_uses_jwt) {
        unsigned long current_time = time(nullptr);
        bool token_still_valid = slot.token_expires_at > 0 &&
                                current_time < slot.token_expires_at &&
                                (slot.token_expires_at - current_time) > 120; // >2 min remaining

        if (token_still_valid) {
          // Lightweight reconnect — reuse existing client but refresh JWT for fresh iat
          if (createSlotAuthToken(index)) {
            slot.client->setCredentials(_jwt_username, slot.auth_token);
            MQTT_DEBUG_PRINTLN("MQTT%d circuit breaker probe (fresh token)", index + 1);
          }
          slot.client->connect();
        } else {
          // Token expired — regenerate token but avoid teardown+setup
          // which would allocate new TLS context on potentially fragmented heap
          if (slot.client) {
            if (createSlotAuthToken(index)) {
              slot.client->setCredentials(_jwt_username, slot.auth_token);
              MQTT_DEBUG_PRINTLN("MQTT%d circuit breaker probe (regenerated expired token)", index + 1);
            }
            slot.client->connect();
          } else {
            // Client was destroyed — must do full setup
            bool saved_tripped = slot.circuit_breaker_tripped;
            MQTT_DEBUG_PRINTLN("MQTT%d circuit breaker probe (full setup, no client)", index + 1);
            teardownSlot(index);
            setupSlot(index);
            _slots[index].circuit_breaker_tripped = saved_tripped;
            _slots[index].last_reconnect_attempt = now_millis;
          }
        }
      } else {
        slot.client->connect();
      }
      // If the connect callback fires and sets slot.connected = true,
      // it will clear circuit_breaker_tripped via the onConnect handler
    }
  }

  // Reconnect with exponential backoff (for disconnected slots that already have valid config)
  // Only one reconnect per maintenance cycle to prevent TLS handshakes from blocking other slots
  if (!slot.connected && slot.initial_connect_done && !slot.circuit_breaker_tripped && !reconnect_attempted) {
    static const unsigned long SLOT_BACKOFF_MS[] = { 10000, 30000, 60000, 120000, 300000 };
    static const uint8_t MAX_FAILURES_AT_MAX_BACKOFF = 3; // ~15 min at max backoff before giving up
    unsigned long reconnect_elapsed = (now_millis >= slot.last_reconnect_attempt) ?
                                    (now_millis - slot.last_reconnect_attempt) :
                                    (ULONG_MAX - slot.last_reconnect_attempt + now_millis + 1);
    unsigned int idx = (slot.reconnect_backoff < 5) ? slot.reconnect_backoff : 4;
    unsigned long delay_ms = SLOT_BACKOFF_MS[idx] + (index * 3000UL); // stagger by slot index
    if (reconnect_elapsed >= delay_ms) {
      slot.last_reconnect_attempt = now_millis;
      if (slot.reconnect_backoff < 5) {
        slot.reconnect_backoff++;
      } else {
        slot.max_backoff_failures++;
        if (slot.max_backoff_failures >= MAX_FAILURES_AT_MAX_BACKOFF) {
          slot.circuit_breaker_tripped = true;
          MQTT_DEBUG_PRINTLN("MQTT%d circuit breaker tripped after %d failures at max backoff - stopping reconnect attempts. Reconfigure slot to retry.", index + 1, slot.max_backoff_failures);
          return;
        }
      }
      MQTT_DEBUG_PRINTLN("MQTT%d reconnecting (backoff level %d, failures at max: %d)", index + 1, slot.reconnect_backoff, slot.max_backoff_failures);
      reconnect_attempted = true;
      _last_slot_reconnect_ms = now_millis;
      if (slot_uses_jwt) {
        unsigned long current_time = time(nullptr);
        bool token_still_valid = slot.token_expires_at > 0 &&
                                current_time < slot.token_expires_at &&
                                (slot.token_expires_at - current_time) > 120; // >2 min remaining

        if (token_still_valid) {
          // Token valid — always lightweight reconnect regardless of backoff level.
          // Avoids creating a new TLS session (teardown+setup) which can race with
          // WiFi association and cause drops when multiple slots do it simultaneously.
          if (createSlotAuthToken(index)) {
            slot.client->setCredentials(_jwt_username, slot.auth_token);
            MQTT_DEBUG_PRINTLN("MQTT%d reconnect (fresh token, backoff %d)", index + 1, slot.reconnect_backoff);
          } else {
            MQTT_DEBUG_PRINTLN("MQTT%d reconnect (token refresh failed, backoff %d)", index + 1, slot.reconnect_backoff);
          }
          slot.client->reconnect();
        } else {
          // Token expired — full teardown to get fresh TLS context + credentials
          if (teardown_attempted) {
            // Defer to next cycle to limit heap fragmentation from simultaneous teardowns
            slot.last_reconnect_attempt = now_millis;
            return;
          }
          teardown_attempted = true;
          uint8_t saved_backoff = slot.reconnect_backoff;
          uint8_t saved_failures = slot.max_backoff_failures;
          MQTT_DEBUG_PRINTLN("MQTT%d full teardown+setup (token expired, backoff %d)", index + 1, saved_backoff);
          teardownSlot(index);
          setupSlot(index);
          _slots[index].reconnect_backoff = saved_backoff;
          _slots[index].max_backoff_failures = saved_failures;
          _slots[index].last_reconnect_attempt = now_millis;
        }
      } else {
        // Non-JWT slots — always lightweight reconnect on existing client.
        // recreateMqttClientsForFragmentationRecovery() handles teardown when
        // memory pressure warrants it, without the timing hazard here.
        MQTT_DEBUG_PRINTLN("MQTT%d reconnect (non-JWT, backoff %d)", index + 1, slot.reconnect_backoff);
        slot.client->reconnect();
      }
    }
  }
}

bool MQTTBridge::createSlotAuthToken(int index) {
  if (index < 0 || index >= RUNTIME_MQTT_SLOTS) return false;
  MQTTSlot& slot = _slots[index];
  if (!_identity || !slot.auth_token) return false;

  // Determine JWT audience: preset takes priority, then custom slot audience field
  const char* audience = nullptr;
  unsigned long base_lifetime = 86400; // default 24h
  if (slot.preset && slot.preset->auth_type == MQTT_AUTH_JWT) {
    audience = slot.preset->jwt_audience;
    if (slot.preset->token_lifetime > 0) base_lifetime = slot.preset->token_lifetime;
  } else if (slot.audience[0] != '\0') {
    audience = slot.audience;
  }
  if (!audience || audience[0] == '\0') return false;

  // Ensure JWT username is set
  if (_jwt_username[0] == '\0') {
    char public_key_hex[65];
    mesh::Utils::toHex(public_key_hex, _identity->pub_key, PUB_KEY_SIZE);
    snprintf(_jwt_username, sizeof(_jwt_username), "v1_%s", public_key_hex);
  }

  // Prepare owner key
  const char* owner_key = nullptr;
  char owner_key_uppercase[65];
  if (_prefs->mqtt_owner_public_key[0] != '\0') {
    strncpy(owner_key_uppercase, _prefs->mqtt_owner_public_key, sizeof(owner_key_uppercase) - 1);
    owner_key_uppercase[sizeof(owner_key_uppercase) - 1] = '\0';
    for (int i = 0; owner_key_uppercase[i]; i++) {
      owner_key_uppercase[i] = toupper(owner_key_uppercase[i]);
    }
    owner_key = owner_key_uppercase;
  }

  char client_version[64];
  getClientVersion(client_version, sizeof(client_version));
  const char* email = (_prefs->mqtt_email[0] != '\0') ? _prefs->mqtt_email : nullptr;

  unsigned long current_time = time(nullptr);
  // Stagger token expiry per slot to avoid simultaneous renewal/reconnect
  // Use 5% of lifetime per slot, capped at 300s, so short-lived tokens aren't over-reduced
  unsigned long stagger = index * min((unsigned long)300, base_lifetime / 20);
  unsigned long expires_in = base_lifetime - stagger;
  bool time_synced = (current_time >= 1000000000);

  if (JWTHelper::createAuthToken(
      *_identity, audience,
      0, expires_in, slot.auth_token, AUTH_TOKEN_SIZE,
      owner_key, client_version, email)) {
    slot.token_expires_at = time_synced ? (current_time + expires_in) : 0;
    return true;
  }

  slot.token_expires_at = 0;
  return false;
}

bool MQTTBridge::publishToSlot(int index, const char* topic, const char* payload, bool retained) {
  if (index < 0 || index >= RUNTIME_MQTT_SLOTS) return false;
  MQTTSlot& slot = _slots[index];
  if (!slot.client || !slot.connected) {
    unsigned long now = millis();
    if (now - slot.last_log_time > SLOT_LOG_INTERVAL) {
      slot.last_log_time = now;
      MQTT_DEBUG_PRINTLN("MQTT%d not connected - skipping publish", index + 1);
    }
    return false;
  }

  // Use async publish (enqueue) to avoid blocking the MQTT task loop.
  // Synchronous publish with QoS 1 blocks waiting for PUBACK, which can stall
  // all slots when one slot's network connection is degraded.
  int result = slot.client->publish(topic, 1, retained, payload, strlen(payload), true);
  if (result <= 0) {
    static unsigned long last_fail_log = 0;
    unsigned long now = millis();
    if (now - last_fail_log > 60000) {
      MQTT_DEBUG_PRINTLN("MQTT%d publish failed (result=%d)", index + 1, result);
      last_fail_log = now;
    }
    return false;
  }
  return true;
}

bool MQTTBridge::publishToAllSlots(const char* topic, const char* payload, bool retained) {
  bool published = false;
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    if (_slots[i].enabled && _slots[i].client && _slots[i].connected) {
      if (publishToSlot(i, topic, payload, retained)) {
        published = true;
      }
    }
  }
  return published;
}

// ---------------------------------------------------------------------------
// Topic building - resolves the correct topic for a given slot and message type.
// Presets use hardcoded topic logic; custom slots support user-defined templates.
// ---------------------------------------------------------------------------
bool MQTTBridge::substituteTopicTemplate(const char* tmpl, MQTTMessageType type, int slot_index, char* buf, size_t buf_size) {
  const char* type_str = (type == MSG_STATUS) ? "status" : (type == MSG_PACKETS) ? "packets" : "raw";
  const char* token = _prefs->mqtt_slot_token[slot_index];

  size_t out = 0;
  const char* p = tmpl;
  while (*p && out < buf_size - 1) {
    if (*p == '{') {
      if (strncmp(p, "{iata}", 6) == 0) {
        size_t len = strlen(_iata);
        if (out + len >= buf_size) return false;
        memcpy(buf + out, _iata, len);
        out += len;
        p += 6;
      } else if (strncmp(p, "{device}", 8) == 0) {
        size_t len = strlen(_device_id);
        if (out + len >= buf_size) return false;
        memcpy(buf + out, _device_id, len);
        out += len;
        p += 8;
      } else if (strncmp(p, "{token}", 7) == 0) {
        size_t len = strlen(token);
        if (out + len >= buf_size) return false;
        memcpy(buf + out, token, len);
        out += len;
        p += 7;
      } else if (strncmp(p, "{type}", 6) == 0) {
        size_t len = strlen(type_str);
        if (out + len >= buf_size) return false;
        memcpy(buf + out, type_str, len);
        out += len;
        p += 6;
      } else {
        buf[out++] = *p++;
      }
    } else {
      buf[out++] = *p++;
    }
  }
  buf[out] = '\0';
  return out > 0;
}

bool MQTTBridge::buildTopicForSlot(int index, MQTTMessageType type, char* topic_buf, size_t buf_size) {
  if (index < 0 || index >= RUNTIME_MQTT_SLOTS) return false;
  const MQTTSlot& slot = _slots[index];

  // Preset slots: use hardcoded topic logic
  if (slot.preset) {
    if (slot.preset->topic_style == MQTT_TOPIC_MESHRANK) {
      // MeshRank: packets only, uses per-slot token in topic path
      if (type != MSG_PACKETS) return false;
      const char* token = _prefs->mqtt_slot_token[index];
      if (!token || token[0] == '\0') return false;
      snprintf(topic_buf, buf_size, "meshrank/uplink/%s/%s/packets", token, _device_id);
      return true;
    }
    // MQTT_TOPIC_MESHCORE (default for all other presets)
    if (!isIATAValid()) return false;
    const char* type_str = (type == MSG_STATUS) ? "status" : (type == MSG_PACKETS) ? "packets" : "raw";
    snprintf(topic_buf, buf_size, "meshcore/%s/%s/%s", _iata, _device_id, type_str);
    return true;
  }

  // Custom slots: use topic template if set, otherwise default meshcore format
  if (_prefs->mqtt_slot_topic[index][0] != '\0') {
    return substituteTopicTemplate(_prefs->mqtt_slot_topic[index], type, index, topic_buf, buf_size);
  }
  // Default: meshcore format
  if (!isIATAValid()) return false;
  const char* type_str = (type == MSG_STATUS) ? "status" : (type == MSG_PACKETS) ? "packets" : "raw";
  snprintf(topic_buf, buf_size, "meshcore/%s/%s/%s", _iata, _device_id, type_str);
  return true;
}

void MQTTBridge::publishStatusToSlot(int index) {
  if (index < 0 || index >= RUNTIME_MQTT_SLOTS) return;
  MQTTSlot& slot = _slots[index];
  if (!slot.client || !slot.connected) return;

  // Build per-slot topic (handles IATA check for meshcore, token check for meshrank)
  char status_topic[128];
  if (!buildTopicForSlot(index, MSG_STATUS, status_topic, sizeof(status_topic))) {
    return;  // Slot doesn't support status (e.g., meshrank) or missing required config
  }

  // Reuse pre-allocated buffer to avoid heap alloc/free churn under memory pressure.
  char fallback_status_buffer[STATUS_JSON_BUFFER_SIZE];
  char* json_buffer = fallback_status_buffer;
  bool status_buffer_locked = false;
  #ifdef ESP_PLATFORM
  if (_status_json_buffer != nullptr && _raw_data_mutex != nullptr &&
      xSemaphoreTake(_raw_data_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    json_buffer = _status_json_buffer;
    status_buffer_locked = true;
  }
  #endif

  char origin_id[65];
  char timestamp[32];
  char radio_info[64];

  // Get current timestamp in ISO 8601 format
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S.000000", &timeinfo);
  } else {
    strcpy(timestamp, "2024-01-01T12:00:00.000000");
  }

  snprintf(radio_info, sizeof(radio_info), "%.6f,%.1f,%d,%d",
           _prefs->freq, _prefs->bw, _prefs->sf, _prefs->cr);

  strncpy(origin_id, _device_id, sizeof(origin_id) - 1);
  origin_id[sizeof(origin_id) - 1] = '\0';

  char client_version[64];
  getClientVersion(client_version, sizeof(client_version));

  // Collect stats on-demand if sources are available
  int battery_mv = -1;
  int uptime_secs = -1;
  int errors = -1;
  int noise_floor = -999;
  int tx_air_secs = -1;
  int rx_air_secs = -1;
  int recv_errors = -1;

  if (_board) battery_mv = _board->getBattMilliVolts();
  if (_ms) uptime_secs = _ms->getMillis() / 1000;
  if (_dispatcher) {
    errors = _dispatcher->getErrFlags();
    tx_air_secs = _dispatcher->getTotalAirTime() / 1000;
    rx_air_secs = _dispatcher->getReceiveAirTime() / 1000;
  }
  if (_radio) {
    noise_floor = (int16_t)_radio->getNoiseFloor();
    recv_errors = (int)_radio->getPacketsRecvErrors();
  }

  int len = MQTTMessageBuilder::buildStatusMessage(
    _origin, origin_id, _board_model, _firmware_version, radio_info,
    client_version, "online", timestamp, json_buffer, STATUS_JSON_BUFFER_SIZE,
    battery_mv, uptime_secs, errors, _queue_count, noise_floor,
    tx_air_secs, rx_air_secs, recv_errors
  );

  if (len > 0) {
    int result = slot.client->publish(status_topic, 1, true, json_buffer, strlen(json_buffer));
    if (result <= 0) {
      MQTT_DEBUG_PRINTLN("MQTT%d status publish failed", index + 1);
    }
  }
  #ifdef ESP_PLATFORM
  if (status_buffer_locked) {
    xSemaphoreGive(_raw_data_mutex);
  }
  #endif
}

void MQTTBridge::updateCachedConnectionStatus() {
  bool any_connected = false;
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    if (_slots[i].enabled && _slots[i].connected) {
      any_connected = true;
      break;
    }
  }
  _cached_has_connected_slots = any_connected;
}

bool MQTTBridge::isAnySlotConnected() {
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    if (_slots[i].enabled && _slots[i].connected) {
      return true;
    }
  }
  return false;
}

void MQTTBridge::setSlotPreset(int slot_index, const char* preset_name) {
  if (slot_index < 0 || slot_index >= RUNTIME_MQTT_SLOTS) return;

  // On ESP32, teardown/setup involves TLS and must run on the MQTT task (Core 0).
  // Set a flag so the MQTT task picks it up on its next loop iteration.
  #ifdef ESP_PLATFORM
  if (_mqtt_task_handle != nullptr) {
    _slot_reconfigure_pending[slot_index] = true;
    MQTT_DEBUG_PRINTLN("MQTT%d reconfigure queued (preset: %s)", slot_index + 1, preset_name);
    return;
  }
  #endif

  // Non-ESP32 or bridge not yet started: apply directly
  applySlotPreset(slot_index, preset_name);
}

void MQTTBridge::applySlotPreset(int slot_index, const char* preset_name) {
  if (slot_index < 0 || slot_index >= RUNTIME_MQTT_SLOTS) return;
  MQTTSlot& slot = _slots[slot_index];

  teardownSlot(slot_index);

  if (strcmp(preset_name, MQTT_PRESET_NONE) == 0 || preset_name[0] == '\0') {
    slot.enabled = false;
    slot.preset = nullptr;
    return;
  }

  if (strcmp(preset_name, MQTT_PRESET_CUSTOM) == 0) {
    slot.enabled = true;
    slot.preset = nullptr;
    // Custom broker settings should already be set via setSlotCustomBroker
    if (_initialized && strlen(slot.host) > 0 && slot.port > 0) {
      setupSlot(slot_index);
    }
    return;
  }

  const MQTTPresetDef* preset = findMQTTPreset(preset_name);
  if (preset) {
    slot.enabled = true;
    slot.preset = preset;
    if (_initialized) {
      char reason[80];
      if (!isSlotReady(slot_index, reason, sizeof(reason))) {
        MQTT_DEBUG_PRINTLN("MQTT%d (%s) not ready — run '%s' to connect", slot_index + 1, preset_name, reason);
        return;
      }
      setupSlot(slot_index);
    }
  }
}

void MQTTBridge::setSlotCustomBroker(int slot_index, const char* host, uint16_t port,
                                      const char* username, const char* password) {
  if (slot_index < 0 || slot_index >= RUNTIME_MQTT_SLOTS) return;
  MQTTSlot& slot = _slots[slot_index];

  strncpy(slot.host, host ? host : "", sizeof(slot.host) - 1);
  slot.host[sizeof(slot.host) - 1] = '\0';
  slot.port = port;
  strncpy(slot.username, username ? username : "", sizeof(slot.username) - 1);
  slot.username[sizeof(slot.username) - 1] = '\0';
  strncpy(slot.password, password ? password : "", sizeof(slot.password) - 1);
  slot.password[sizeof(slot.password) - 1] = '\0';
}

// ---------------------------------------------------------------------------
// WiFi connection handling
// ---------------------------------------------------------------------------

void MQTTBridge::checkConfigurationMismatch() {
  // Warn if packets are enabled but both rx and tx are off — nothing will be published
  if (_prefs->mqtt_packets_enabled && !_prefs->mqtt_rx_enabled && _prefs->mqtt_tx_enabled == 0) {
    unsigned long now = millis();
    if (_last_config_warning == 0 || (now - _last_config_warning > CONFIG_WARNING_INTERVAL)) {
      MQTT_DEBUG_PRINTLN("MQTT: Both mqtt.rx and mqtt.tx are off — no packets will be published. Run 'set mqtt.rx on' or 'set mqtt.tx on' to fix.");
      _last_config_warning = now;
    }
  } else {
    _last_config_warning = 0;
  }
}

bool MQTTBridge::handleWiFiConnection(unsigned long now) {
  wl_status_t current_wifi_status = WiFi.status();
  bool transitioned_to_connected = false;

  if (current_wifi_status == WL_CONNECTED && s_wifi_connected_at == 0) {
    s_wifi_connected_at = now;
  }
  if (!_wifi_status_initialized) {
    _last_wifi_status = current_wifi_status;
    _wifi_status_initialized = true;
  }
  if (now - _last_wifi_check <= 10000) {
    return false;
  }
  _last_wifi_check = now;

  if (current_wifi_status == WL_CONNECTED) {
    if (_last_wifi_status != WL_CONNECTED) {
      transitioned_to_connected = true;
      _wifi_disconnected_time = 0;
      s_wifi_connected_at = now;
      _wifi_reconnect_backoff_attempt = 0;
      #ifdef ESP_PLATFORM
      wifi_ps_type_t ps_mode;
      uint8_t ps_pref = _prefs->wifi_power_save;
      if (ps_pref == 1) {
        ps_mode = WIFI_PS_NONE;
      } else if (ps_pref == 2) {
        ps_mode = WIFI_PS_MAX_MODEM;
      } else {
        ps_mode = WIFI_PS_MIN_MODEM;
      }
      esp_wifi_set_ps(ps_mode);
      #ifdef MQTT_WIFI_TX_POWER
      WiFi.setTxPower(MQTT_WIFI_TX_POWER);
      #else
      WiFi.setTxPower(WIFI_POWER_11dBm);
      #endif
      #endif
    }
    if (s_wifi_connected_at == 0) {
      s_wifi_connected_at = now;
    }
    _last_wifi_status = WL_CONNECTED;
  } else {
    if (_last_wifi_status == WL_CONNECTED) {
      _wifi_disconnected_time = now;
      s_wifi_connected_at = 0;
      // Disconnect all slot clients when WiFi drops
      for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
        if (_slots[i].client && _slots[i].connected) {
          _slots[i].client->disconnect();
        }
      }
    } else if (_wifi_disconnected_time > 0) {
      unsigned long disconnected_duration = now - _wifi_disconnected_time;
      static const unsigned long WIFI_BACKOFF_MS[] = { 15000, 30000, 60000, 120000, 300000 };
      unsigned int idx = (_wifi_reconnect_backoff_attempt < 5) ? _wifi_reconnect_backoff_attempt : 4;
      unsigned long delay_ms = WIFI_BACKOFF_MS[idx];
      unsigned long elapsed_since_attempt = (now >= _last_wifi_reconnect_attempt)
          ? (now - _last_wifi_reconnect_attempt)
          : (ULONG_MAX - _last_wifi_reconnect_attempt + now + 1);
      if (disconnected_duration >= delay_ms && elapsed_since_attempt >= delay_ms) {
        _last_wifi_reconnect_attempt = now;
        if (_wifi_reconnect_backoff_attempt < 5) {
          _wifi_reconnect_backoff_attempt++;
        }
        WiFi.disconnect();
        WiFi.begin(_prefs->wifi_ssid, _prefs->wifi_password);
      }
    }
    _last_wifi_status = current_wifi_status;
  }
  return transitioned_to_connected;
}

bool MQTTBridge::isReady() const {
  return _initialized && isWiFiConfigValid(_prefs);
}

bool MQTTBridge::isIATAValid() const {
  if (strlen(_iata) == 0 || strcmp(_iata, "XXX") == 0) {
    return false;
  }
  return true;
}

bool MQTTBridge::isSlotReady(int index, char* reason_buf, size_t reason_size) const {
  if (index < 0 || index >= RUNTIME_MQTT_SLOTS) return false;
  const MQTTSlot& slot = _slots[index];

  if (!slot.enabled) return true;  // disabled slots are "ready" (nothing to do)

  if (slot.preset) {
    if (slot.preset->topic_style == MQTT_TOPIC_MESHRANK) {
      if (_prefs->mqtt_slot_token[index][0] == '\0') {
        if (reason_buf) snprintf(reason_buf, reason_size, "set mqtt%d.token <your_token>", index + 1);
        return false;
      }
    } else if (slot.preset->topic_style == MQTT_TOPIC_MESHCORE) {
      if (!isIATAValid()) {
        if (reason_buf) snprintf(reason_buf, reason_size, "set mqtt.iata <airport_code>");
        return false;
      }
    }
  } else {
    // Custom slot without a topic template uses meshcore format, needs IATA
    if (_prefs->mqtt_slot_topic[index][0] == '\0' && !isIATAValid()) {
      if (reason_buf) snprintf(reason_buf, reason_size, "set mqtt.iata <airport_code> or set mqtt%d.topic <template>", index + 1);
      return false;
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// loop() - non-ESP32 main loop (ESP32 uses mqttTaskLoop via FreeRTOS task)
// ---------------------------------------------------------------------------
void MQTTBridge::loop() {
  if (!_initialized) return;

  #ifdef ESP_PLATFORM
  // On ESP32, loop() is a no-op - all processing happens in the FreeRTOS task
  return;
  #else
  unsigned long now = millis();
  if (handleWiFiConnection(now) && !_ntp_synced) {
    syncTimeWithNTP();
  }
  if (_ntp_sync_pending && WiFi.status() == WL_CONNECTED) {
    _ntp_sync_pending = false;
    syncTimeWithNTP();
  }

  // Deferred slot setup after NTP sync (non-ESP32 path)
  if (_ntp_synced && !_slots_setup_done) {
    _slots_setup_done = true;
    int active_count = 0;
    for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
      if (_slots[i].enabled) {
        if (active_count >= _max_active_slots) {
          _slots[i].enabled = false;
          continue;
        }
        if (!isSlotReady(i)) {
          continue;
        }
        setupSlot(i);
        active_count++;
      }
    }
  }

  // Process pending slot reconfigures
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    if (_slot_reconfigure_pending[i]) {
      _slot_reconfigure_pending[i] = false;
      applySlotPreset(i, _prefs->mqtt_slot_preset[i]);
    }
  }

  // Maintain slot connections (token renewal, reconnect with backoff)
  maintainSlotConnections();

  // Process packet queue
  processPacketQueue();

  // Periodic configuration check (throttled to avoid spam)
  checkConfigurationMismatch();

  // Periodic NTP refresh (every hour) — lightweight, non-blocking.
  if (WiFi.status() == WL_CONNECTED && millis() - _last_ntp_sync > 3600000) {
    refreshNTP();
  }

  // Publish status updates (handle millis() overflow correctly)
  if (_status_enabled) {
    bool has_destinations = _cached_has_connected_slots;

    if (has_destinations) {
      unsigned long now = millis();
      bool should_publish = false;

      if (_last_status_retry != 0) {
        unsigned long retry_elapsed = (now >= _last_status_retry) ?
                                     (now - _last_status_retry) :
                                     (ULONG_MAX - _last_status_retry + now + 1);
        if (retry_elapsed >= STATUS_RETRY_INTERVAL) {
          should_publish = true;
        }
      } else {
        if (_last_status_publish == 0) {
          should_publish = true;
        } else {
          unsigned long elapsed = (now >= _last_status_publish) ?
                               (now - _last_status_publish) :
                               (ULONG_MAX - _last_status_publish + now + 1);
          should_publish = (elapsed >= _status_interval);
        }
      }

      if (should_publish) {
        if (_last_status_publish != 0) {
          unsigned long elapsed = (now >= _last_status_publish) ?
                                 (now - _last_status_publish) :
                                 (ULONG_MAX - _last_status_publish + now + 1);
          MQTT_DEBUG_PRINTLN("Status publish timer expired (elapsed: %lu ms, interval: %lu ms)", elapsed, _status_interval);
        } else {
          MQTT_DEBUG_PRINTLN("Status publish attempt (first publish or retry)");
        }

        _last_status_retry = now;
        if (publishStatus()) {
          _last_status_publish = now;
          _last_status_retry = 0;
          MQTT_DEBUG_PRINTLN("Status published successfully, next publish in %lu ms", _status_interval);
        } else {
          MQTT_DEBUG_PRINTLN("Status publish failed, will retry in %lu ms", STATUS_RETRY_INTERVAL);
        }
      }
    } else {
      if (_last_status_retry != 0) {
        _last_status_retry = 0;
      }
    }

    // Check if status hasn't been published successfully for too long
    if (_status_enabled && _last_status_publish != 0) {
      unsigned long now = millis();
      unsigned long time_since_last_success = (now >= _last_status_publish) ?
                                              (now - _last_status_publish) :
                                              (ULONG_MAX - _last_status_publish + now + 1);
      const unsigned long MAX_FAILURE_TIME_MS = 600000;  // 10 minutes

      if (time_since_last_success > MAX_FAILURE_TIME_MS) {
        static unsigned long last_reinit_log = 0;
        if (now - last_reinit_log > 300000) {
          MQTT_DEBUG_PRINTLN("CRITICAL: Status publish has been failing for %lu ms (>%lu ms), forcing MQTT session reinitialization",
                             time_since_last_success, MAX_FAILURE_TIME_MS);
          last_reinit_log = now;
        }

        recreateMqttClientsForFragmentationRecovery();
        _last_status_publish = 0;
        _last_status_retry = 0;
        MQTT_DEBUG_PRINTLN("MQTT session reinitialized (clients recreated) - reconnection on next loop");
      }
    }
  }

  #ifdef ESP_PLATFORM
  runCriticalMemoryCheckAndRecovery();
  #endif
  #endif
}

// ---------------------------------------------------------------------------
// Packet handling
// ---------------------------------------------------------------------------

void MQTTBridge::onPacketReceived(mesh::Packet *packet) {
  if (!_initialized || !_prefs->mqtt_packets_enabled || !_prefs->mqtt_rx_enabled) return;

  // Check if we have any enabled slots to send to
  bool has_valid_slots = false;
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    if (_slots[i].enabled && _slots[i].client) {
      has_valid_slots = true;
      break;
    }
  }
  if (!has_valid_slots) return;

  // Queue packet for transmission
  queuePacket(packet, false);
}

void MQTTBridge::sendPacket(mesh::Packet *packet) {
  uint8_t tx_mode = _prefs->mqtt_tx_enabled;  // Read live from prefs (no restart needed)
  if (!_initialized || !_prefs->mqtt_packets_enabled || tx_mode == 0) return;

  // Advert mode: only queue self-originated advert packets
  if (tx_mode == 2) {
    if (packet->getPayloadType() != PAYLOAD_TYPE_ADVERT) return;
    if (packet->payload_len < PUB_KEY_SIZE) return;
    // Advert payload starts with advertiser's 32-byte public key — compare to our identity
    if (!_identity || memcmp(_identity->pub_key, packet->payload, PUB_KEY_SIZE) != 0) return;
  }

  // Queue packet for transmission
  queuePacket(packet, true);
}

void MQTTBridge::processPacketQueue() {
  #ifdef ESP_PLATFORM
  // Use FreeRTOS queue
  if (_packet_queue_handle == nullptr) {
    return;
  }

  // Update queue count from actual queue state
  _queue_count = uxQueueMessagesWaiting(_packet_queue_handle);

  if (_queue_count == 0) {
    _queue_disconnected_since = 0;
    return;
  }

  // Use cached connection status to avoid redundant checks
  bool has_connected_slots = _cached_has_connected_slots;

  if (!has_connected_slots) {
    if (_queue_count > 0) {
      unsigned long now = millis();
      if (now - _last_no_broker_log > NO_BROKER_LOG_INTERVAL) {
        MQTT_DEBUG_PRINTLN("Queue has %d packets but no slots connected", _queue_count);
        _last_no_broker_log = now;
      }
      // Flush stale packets after extended disconnect
      if (_queue_disconnected_since == 0) {
        _queue_disconnected_since = now;
      } else if ((now - _queue_disconnected_since) >= QUEUE_STALE_MS) {
        QueuedPacket discard;
        while (xQueueReceive(_packet_queue_handle, &discard, 0) == pdTRUE) {}
        _queue_count = 0;
        MQTT_DEBUG_PRINTLN("Flushed stale packet queue after %lu ms disconnected", now - _queue_disconnected_since);
        _queue_disconnected_since = now;
      }
    }
    return;
  }

  _queue_disconnected_since = 0;
  _last_no_broker_log = 0;

  // Adaptive drain: burst-process when queue has backlog, gentle otherwise
  int processed = 0;
  int max_per_loop = (_queue_count > 5) ? 5 : 1;
  unsigned long loop_start_time = millis();
  const unsigned long MAX_PROCESSING_TIME_MS = (_queue_count > 5) ? 100 : 30;

  while (processed < max_per_loop) {
    unsigned long elapsed = millis() - loop_start_time;
    if (elapsed > MAX_PROCESSING_TIME_MS) {
      break;
    }

    QueuedPacket queued;
    // Try to receive from queue (non-blocking)
    if (xQueueReceive(_packet_queue_handle, &queued, 0) != pdTRUE) {
      break;  // No more packets
    }

    // Publish packet (use stored raw data if available)
#if defined(BOARD_HAS_PSRAM)
    publishPacket(&queued.packet_copy, queued.is_tx,
                  queued.has_raw_data ? queued.raw_data : nullptr,
                  queued.has_raw_data ? queued.raw_len : 0,
                  queued.snr, queued.rssi);
#else
    publishPacket(&queued.packet_copy, queued.is_tx,
                  nullptr, 0, queued.snr, queued.rssi);
#endif

    // Publish raw if enabled
    if (_raw_enabled) {
      publishRaw(&queued.packet_copy);
    }

    _queue_count--;
    processed++;
  }
  #else
  // Non-ESP32: Use circular buffer
  if (_queue_count == 0) {
    return;
  }

  bool has_connected_slots = _cached_has_connected_slots;

  if (!has_connected_slots) {
    if (_queue_count > 0) {
      unsigned long now = millis();
      if (now - _last_no_broker_log > NO_BROKER_LOG_INTERVAL) {
        MQTT_DEBUG_PRINTLN("Queue has %d packets but no slots connected", _queue_count);
        _last_no_broker_log = now;
      }
    }
    return;
  }

  _last_no_broker_log = 0;

  // Adaptive drain: burst-process when queue has backlog, gentle otherwise
  int processed = 0;
  int max_per_loop = (_queue_count > 5) ? 5 : 1;
  unsigned long loop_start_time = millis();
  const unsigned long MAX_PROCESSING_TIME_MS = (_queue_count > 5) ? 100 : 30;

  while (_queue_count > 0 && processed < max_per_loop) {
    unsigned long elapsed = millis() - loop_start_time;
    if (elapsed > MAX_PROCESSING_TIME_MS) {
      break;
    }

    QueuedPacket& queued = _packet_queue[_queue_head];

#if defined(BOARD_HAS_PSRAM)
    publishPacket(&queued.packet_copy, queued.is_tx,
                  queued.has_raw_data ? queued.raw_data : nullptr,
                  queued.has_raw_data ? queued.raw_len : 0,
                  queued.snr, queued.rssi);
#else
    publishPacket(&queued.packet_copy, queued.is_tx,
                  nullptr, 0, queued.snr, queued.rssi);
#endif

    if (_raw_enabled) {
      publishRaw(&queued.packet_copy);
    }

    dequeuePacket();
    processed++;
  }
  #endif
}

// ---------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------

bool MQTTBridge::publishStatus() {
  if (!_cached_has_connected_slots) {
    return false;
  }

  // Reuse pre-allocated buffer to avoid heap alloc/free churn under memory pressure.
  char fallback_status_buffer[STATUS_JSON_BUFFER_SIZE];
  char* json_buffer = fallback_status_buffer;
  bool status_buffer_locked = false;
  #ifdef ESP_PLATFORM
  if (_status_json_buffer != nullptr && _raw_data_mutex != nullptr &&
      xSemaphoreTake(_raw_data_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    json_buffer = _status_json_buffer;
    status_buffer_locked = true;
  }
  #endif
  char origin_id[65];
  char timestamp[32];
  char radio_info[64];

  // Get current timestamp in ISO 8601 format
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S.000000", &timeinfo);
  } else {
    strcpy(timestamp, "2024-01-01T12:00:00.000000");
  }

  snprintf(radio_info, sizeof(radio_info), "%.6f,%.1f,%d,%d",
           _prefs->freq, _prefs->bw, _prefs->sf, _prefs->cr);

  strncpy(origin_id, _device_id, sizeof(origin_id) - 1);
  origin_id[sizeof(origin_id) - 1] = '\0';

  char client_version[64];
  getClientVersion(client_version, sizeof(client_version));

  // Collect stats on-demand if sources are available
  int battery_mv = -1;
  int uptime_secs = -1;
  int errors = -1;
  int noise_floor = -999;
  int tx_air_secs = -1;
  int rx_air_secs = -1;
  int recv_errors = -1;

  if (_board) battery_mv = _board->getBattMilliVolts();
  if (_ms) uptime_secs = _ms->getMillis() / 1000;
  if (_dispatcher) {
    errors = _dispatcher->getErrFlags();
    tx_air_secs = _dispatcher->getTotalAirTime() / 1000;
    rx_air_secs = _dispatcher->getReceiveAirTime() / 1000;
  }
  if (_radio) {
    noise_floor = (int16_t)_radio->getNoiseFloor();
    recv_errors = (int)_radio->getPacketsRecvErrors();
  }

  int len = MQTTMessageBuilder::buildStatusMessage(
    _origin, origin_id, _board_model, _firmware_version, radio_info,
    client_version, "online", timestamp, json_buffer, STATUS_JSON_BUFFER_SIZE,
    battery_mv, uptime_secs, errors, _queue_count, noise_floor,
    tx_air_secs, rx_air_secs, recv_errors
  );

  if (len > 0) {
    bool published = false;
    bool any_slot_wants_status = false;
    char topic[128];
    for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
      if (_slots[i].enabled && _slots[i].client && _slots[i].connected) {
        if (buildTopicForSlot(i, MSG_STATUS, topic, sizeof(topic))) {
          any_slot_wants_status = true;
          bool use_retain = _slots[i].preset ? _slots[i].preset->allow_retain : false;
          if (publishToSlot(i, topic, json_buffer, use_retain)) {
            published = true;
          }
        }
      }
    }
    // If no connected slot accepts status topics (e.g. meshrank is packets-only),
    // treat as success to avoid infinite retry loops
    if (published || !any_slot_wants_status) {
      if (published) MQTT_DEBUG_PRINTLN("Status published");
      #ifdef ESP_PLATFORM
      if (status_buffer_locked) {
        xSemaphoreGive(_raw_data_mutex);
      }
      #endif
      return true;
    }
  }

  #ifdef ESP_PLATFORM
  if (status_buffer_locked) {
    xSemaphoreGive(_raw_data_mutex);
  }
  #endif
  return false;
}

void MQTTBridge::publishPacket(mesh::Packet* packet, bool is_tx,
                                const uint8_t* raw_data, int raw_len,
                                float snr, float rssi) {
  if (!packet) return;

  // Memory pressure check: Skip publishes when heap is severely fragmented
  #ifdef ESP32
  #if defined(BOARD_HAS_PSRAM)
  static const size_t PUBLISH_SKIP_MAX_ALLOC_THRESHOLD = 60000;
  #else
  static const size_t PUBLISH_SKIP_MAX_ALLOC_THRESHOLD = 52000;
  #endif
  unsigned long now = millis();
  if (now - _last_memory_check > 5000) {
    size_t max_alloc = ESP.getMaxAllocHeap();
    if (max_alloc < PUBLISH_SKIP_MAX_ALLOC_THRESHOLD) {
      _skipped_publishes++;
      static unsigned long last_skip_log = 0;
      if (now - last_skip_log > 60000) {
        MQTT_DEBUG_PRINTLN("MQTT: Skipping publish due to memory pressure (Max alloc: %d, threshold: %d, skipped: %d)",
                           max_alloc, (int)PUBLISH_SKIP_MAX_ALLOC_THRESHOLD, _skipped_publishes);
        last_skip_log = now;
      }
      return;
    }
    _last_memory_check = now;
  }
  #endif

  // Use pre-allocated buffer; fallback to single stack buffer if not available
  char json_buffer_stack[PUBLISH_JSON_BUFFER_SIZE];
  char* active_buffer;
  size_t active_buffer_size;
  if (_publish_json_buffer != nullptr) {
    active_buffer = _publish_json_buffer;
    active_buffer_size = PUBLISH_JSON_BUFFER_SIZE;
  } else {
    active_buffer = json_buffer_stack;
    active_buffer_size = PUBLISH_JSON_BUFFER_SIZE;
  }
  char origin_id[65];

  strncpy(origin_id, _device_id, sizeof(origin_id) - 1);
  origin_id[sizeof(origin_id) - 1] = '\0';

  // Build packet message using raw radio data if provided
  int len;
  if (raw_data && raw_len > 0) {
    len = MQTTMessageBuilder::buildPacketJSONFromRaw(
      raw_data, raw_len, packet, is_tx, _origin, origin_id,
      snr, rssi, _timezone, active_buffer, active_buffer_size
    );
  } else if (_last_raw_data && _last_raw_len > 0 && (millis() - _last_raw_timestamp) < 1000) {
    len = MQTTMessageBuilder::buildPacketJSONFromRaw(
      _last_raw_data, _last_raw_len, packet, is_tx, _origin, origin_id,
      _last_snr, _last_rssi, _timezone, active_buffer, active_buffer_size
    );
  } else {
    // Reconstruct wire-format bytes from packet (same as MQTTMessageBuilder::packetToHex).
    // This path is used on non-PSRAM boards where raw_data is not stored in the queue,
    // and ensures the "raw" hex field and SNR/RSSI are accurate in the JSON output.
    uint8_t reconstructed[512];
    uint8_t rlen = packet->writeTo(reconstructed);
    if (rlen > 0) {
      len = MQTTMessageBuilder::buildPacketJSONFromRaw(
        reconstructed, rlen, packet, is_tx, _origin, origin_id,
        snr, rssi, _timezone, active_buffer, active_buffer_size
      );
    } else {
      len = MQTTMessageBuilder::buildPacketJSON(
        packet, is_tx, _origin, origin_id, _timezone, active_buffer, active_buffer_size
      );
    }
  }

  if (len > 0) {
    char topic[128];
    for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
      if (_slots[i].enabled && _slots[i].client && _slots[i].connected) {
        if (buildTopicForSlot(i, MSG_PACKETS, topic, sizeof(topic))) {
          publishToSlot(i, topic, active_buffer, false);
        }
      }
    }
  } else {
    uint8_t packet_type = packet->getPayloadType();
    if (packet_type == 4 || packet_type == 9) {
      MQTT_DEBUG_PRINTLN("Failed to build packet JSON for type=%d (len=%d), packet not published", packet_type, len);
    }
  }
}

void MQTTBridge::publishRaw(mesh::Packet* packet) {
  if (!packet) return;

  // Use pre-allocated buffer; fallback to single stack buffer if not available
  char json_buffer_stack[PUBLISH_JSON_BUFFER_SIZE];
  char* active_buffer;
  size_t active_buffer_size;
  if (_publish_json_buffer != nullptr) {
    active_buffer = _publish_json_buffer;
    active_buffer_size = PUBLISH_JSON_BUFFER_SIZE;
  } else {
    active_buffer = json_buffer_stack;
    active_buffer_size = PUBLISH_JSON_BUFFER_SIZE;
  }
  char origin_id[65];

  strncpy(origin_id, _device_id, sizeof(origin_id) - 1);
  origin_id[sizeof(origin_id) - 1] = '\0';

  int len = MQTTMessageBuilder::buildRawJSON(
    packet, _origin, origin_id, _timezone, active_buffer, active_buffer_size
  );

  if (len > 0) {
    char topic[128];
    for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
      if (_slots[i].enabled && _slots[i].client && _slots[i].connected) {
        if (buildTopicForSlot(i, MSG_RAW, topic, sizeof(topic))) {
          publishToSlot(i, topic, active_buffer, false);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Queue management
// ---------------------------------------------------------------------------

void MQTTBridge::queuePacket(mesh::Packet* packet, bool is_tx) {
  #ifdef ESP_PLATFORM
  // Use FreeRTOS queue for thread-safe operation
  if (_packet_queue_handle == nullptr) {
    return;
  }

  QueuedPacket queued;
  memset(&queued, 0, sizeof(QueuedPacket));

  queued.packet_copy = *packet;  // full value copy — safe from Dispatcher free
  queued.timestamp = millis();
  queued.is_tx = is_tx;
  queued.snr = 0.0f;
  queued.rssi = 0.0f;

  // Capture raw radio data with mutex protection
  if (!is_tx) {
    if (xSemaphoreTake(_raw_data_mutex, 0) == pdTRUE) {
      unsigned long current_time = millis();
      if (_last_raw_len > 0 && (current_time - _last_raw_timestamp) < 1000) {
#if defined(BOARD_HAS_PSRAM)
        if (_last_raw_data && _last_raw_len <= (int)sizeof(queued.raw_data)) {
          memcpy(queued.raw_data, _last_raw_data, _last_raw_len);
          queued.raw_len = _last_raw_len;
          queued.has_raw_data = true;
        }
#endif
        queued.snr = _last_snr;
        queued.rssi = _last_rssi;
      }
      xSemaphoreGive(_raw_data_mutex);
    }
  }

  // Try to send to queue (non-blocking)
  if (xQueueSend(_packet_queue_handle, &queued, 0) != pdTRUE) {
    QueuedPacket oldest;
    if (xQueueReceive(_packet_queue_handle, &oldest, 0) == pdTRUE) {
      MQTT_DEBUG_PRINTLN("Queue full, dropping oldest packet reference");
      if (xQueueSend(_packet_queue_handle, &queued, 0) != pdTRUE) {
        MQTT_DEBUG_PRINTLN("Failed to queue packet after dropping oldest");
        return;
      }
    } else {
      MQTT_DEBUG_PRINTLN("Queue full and cannot remove oldest packet");
      return;
    }
  }

  UBaseType_t queue_messages = uxQueueMessagesWaiting(_packet_queue_handle);
  _queue_count = queue_messages;
  #else
  // Non-ESP32: Use circular buffer
  if (_queue_count >= MAX_QUEUE_SIZE) {
    QueuedPacket& oldest = _packet_queue[_queue_head];
    MQTT_DEBUG_PRINTLN("Queue full, dropping oldest packet (queue size: %d)", _queue_count);
    dequeuePacket();
  }

  QueuedPacket& queued = _packet_queue[_queue_tail];
  memset(&queued, 0, sizeof(QueuedPacket));

  queued.packet_copy = *packet;  // full value copy — safe from Dispatcher free
  queued.timestamp = millis();
  queued.is_tx = is_tx;
  queued.snr = 0.0f;
  queued.rssi = 0.0f;

  if (!is_tx && _last_raw_data && _last_raw_len > 0 && (millis() - _last_raw_timestamp) < 1000) {
#if defined(BOARD_HAS_PSRAM)
    if (_last_raw_len <= (int)sizeof(queued.raw_data)) {
      memcpy(queued.raw_data, _last_raw_data, _last_raw_len);
      queued.raw_len = _last_raw_len;
      queued.has_raw_data = true;
    }
#endif
    queued.snr = _last_snr;
    queued.rssi = _last_rssi;
  }

  _queue_tail = (_queue_tail + 1) % MAX_QUEUE_SIZE;
  _queue_count++;
  #endif
}

void MQTTBridge::dequeuePacket() {
  #ifdef ESP_PLATFORM
  // On ESP32, dequeuePacket() is not used - we use FreeRTOS queue operations directly
  return;
  #else
  if (_queue_count == 0) return;

  QueuedPacket& dequeued = _packet_queue[_queue_head];
  memset(&dequeued, 0, sizeof(QueuedPacket));
#if defined(BOARD_HAS_PSRAM)
  dequeued.has_raw_data = false;
#endif

  _queue_head = (_queue_head + 1) % MAX_QUEUE_SIZE;
  _queue_count--;
  #endif
}

// ---------------------------------------------------------------------------
// Raw radio data storage
// ---------------------------------------------------------------------------

void MQTTBridge::storeRawRadioData(const uint8_t* raw_data, int len, float snr, float rssi) {
  if (len > 0 && len <= LAST_RAW_DATA_SIZE && _last_raw_data) {
    #ifdef ESP_PLATFORM
    if (_raw_data_mutex != nullptr && xSemaphoreTake(_raw_data_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      memcpy(_last_raw_data, raw_data, len);
      _last_raw_len = len;
      _last_snr = snr;
      _last_rssi = rssi;
      _last_raw_timestamp = millis();
      xSemaphoreGive(_raw_data_mutex);
      MQTT_DEBUG_PRINTLN("Stored raw radio data: %d bytes, SNR=%.1f, RSSI=%.1f", len, snr, rssi);
    }
    #else
    memcpy(_last_raw_data, raw_data, len);
    _last_raw_len = len;
    _last_snr = snr;
    _last_rssi = rssi;
    _last_raw_timestamp = millis();
    MQTT_DEBUG_PRINTLN("Stored raw radio data: %d bytes, SNR=%.1f, RSSI=%.1f", len, snr, rssi);
    #endif
  }
}

// ---------------------------------------------------------------------------
// Memory management
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM
void MQTTBridge::runCriticalMemoryCheckAndRecovery() {
  const unsigned long CRITICAL_CHECK_INTERVAL_MS = 60000;
  #if defined(BOARD_HAS_PSRAM)
  const unsigned long PRESSURE_WINDOW_CRITICAL_MS = 180000;
  const unsigned long PRESSURE_WINDOW_MODERATE_MS = 300000;
  const unsigned long RECOVERY_THROTTLE_MS = 300000;
  const size_t PRESSURE_THRESHOLD_CRITICAL = 58000;
  const size_t PRESSURE_THRESHOLD_MODERATE = 70000;
  const size_t HARD_RECOVERY_THRESHOLD = 54000;
  #else
  // Non-PSRAM boards run closer to the edge; use lower thresholds and longer windows.
  const unsigned long PRESSURE_WINDOW_CRITICAL_MS = 300000;
  const unsigned long PRESSURE_WINDOW_MODERATE_MS = 900000;
  const unsigned long RECOVERY_THROTTLE_MS = 600000;
  const size_t PRESSURE_THRESHOLD_CRITICAL = 50000;
  const size_t PRESSURE_THRESHOLD_MODERATE = 56000;
  const size_t HARD_RECOVERY_THRESHOLD = 46000;
  #endif
  const unsigned long CRITICAL_LOG_INTERVAL_MS = 900000;

  unsigned long now = millis();
  if (now - _last_critical_check_run < CRITICAL_CHECK_INTERVAL_MS) {
    return;
  }
  _last_critical_check_run = now;

  size_t free_h = ESP.getFreeHeap();
  size_t max_alloc = ESP.getMaxAllocHeap();
  #ifdef MQTT_MEMORY_DEBUG
  unsigned long internal_f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  unsigned long spiram_f = 0;
  #ifdef BOARD_HAS_PSRAM
  spiram_f = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  #endif
  agentLogHeap("MQTTBridge.cpp:runCriticalMemoryCheckAndRecovery", "critical_memory_check", "H1_H4", free_h, max_alloc, internal_f, spiram_f);
  #endif

  // Pressure timer: track how long max_alloc has been below moderate threshold
  if (max_alloc >= PRESSURE_THRESHOLD_MODERATE) {
    _fragmentation_pressure_since = 0;
  } else {
    if (_fragmentation_pressure_since == 0) {
      _fragmentation_pressure_since = now;
    }
  }

  // Rate-limited diagnostic logging (every 15 min)
  static unsigned long last_critical_log = 0;
  if (now - last_critical_log >= CRITICAL_LOG_INTERVAL_MS) {
    last_critical_log = now;
    if (max_alloc < PRESSURE_THRESHOLD_CRITICAL) {
      MQTT_DEBUG_PRINTLN("CRITICAL: Low memory! Free: %d, Max: %d", (int)free_h, (int)max_alloc);
    } else if (max_alloc < PRESSURE_THRESHOLD_MODERATE) {
      MQTT_DEBUG_PRINTLN("WARNING: Memory pressure. Free: %d, Max: %d", (int)free_h, (int)max_alloc);
    }
    // Log slot client count
    int n_active = 0;
    for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
      if (_slots[i].client != nullptr) n_active++;
    }
    MQTT_DEBUG_PRINTLN("MQTT clients active: %d", n_active);
  }

  // Dedicated recovery
  unsigned long required_window_ms = (max_alloc < PRESSURE_THRESHOLD_CRITICAL)
      ? PRESSURE_WINDOW_CRITICAL_MS
      : PRESSURE_WINDOW_MODERATE_MS;
  bool allow_recovery = !_cached_has_connected_slots || max_alloc < HARD_RECOVERY_THRESHOLD;
  if (_fragmentation_pressure_since != 0 &&
      allow_recovery &&
      (now - _fragmentation_pressure_since) >= required_window_ms &&
      (now - _last_fragmentation_recovery) >= RECOVERY_THROTTLE_MS) {
    _last_fragmentation_recovery = now;
    _fragmentation_pressure_since = 0;
    MQTT_DEBUG_PRINTLN("Fragmentation recovery: recreating MQTT clients (max_alloc=%d, pressure %lu min)", (int)max_alloc, (unsigned long)(required_window_ms / 60000));
    recreateMqttClientsForFragmentationRecovery();
  }

  // Last resort: if ALL enabled slots have circuit breakers tripped for >1 hour,
  // heap is likely too fragmented for TLS to ever succeed — reboot.
  bool all_tripped = true;
  int enabled_count = 0;
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    if (_slots[i].enabled) {
      enabled_count++;
      if (!_slots[i].circuit_breaker_tripped) {
        all_tripped = false;
        break;
      }
    }
  }
  if (enabled_count > 0 && all_tripped) {
    if (_all_tripped_since == 0) {
      _all_tripped_since = now;
    } else if ((now - _all_tripped_since) >= 3600000UL) {
      MQTT_DEBUG_PRINTLN("All MQTT slots circuit-breaker tripped for >1 hour. Restarting ESP.");
      delay(100);
      ESP.restart();
    }
  } else {
    _all_tripped_since = 0;
  }
}
#endif

void MQTTBridge::recreateMqttClientsForFragmentationRecovery() {
  // Disconnect, delete, and recreate all MQTT clients so they allocate fresh buffers.
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    if (_slots[i].enabled && !_slots[i].connected) {
      teardownSlot(i);
      setupSlot(i);
    }
  }
  updateCachedConnectionStatus();
}

// ---------------------------------------------------------------------------
// NTP time sync
// ---------------------------------------------------------------------------

void MQTTBridge::refreshNTP() {
  // Lightweight periodic refresh: just restart SNTP which runs async in the background.
  // No blocking DNS, no UDP sockets, no retry loops on the MQTT task loop.
  // The heavy syncTimeWithNTP() is only used for initial sync and WiFi reconnect recovery.
  configTime(0, 0, "pool.ntp.org");
  _last_ntp_sync = millis();
  MQTT_DEBUG_PRINTLN("NTP refresh triggered (async SNTP)");
}

void MQTTBridge::syncTimeWithNTP() {
  if (!WiFi.isConnected()) {
    MQTT_DEBUG_PRINTLN("Cannot sync time - WiFi not connected");
    return;
  }

  unsigned long now = millis();
  if (_ntp_synced && (now - _last_ntp_sync) < 5000) {
    return;
  }

  static bool sync_in_progress = false;
  if (sync_in_progress) {
    return;
  }
  sync_in_progress = true;

  MQTT_DEBUG_PRINTLN("Syncing time with NTP...");

  #ifdef ESP_PLATFORM
  IPAddress resolved_ip;
  if (!WiFi.hostByName("pool.ntp.org", resolved_ip)) {
    MQTT_DEBUG_PRINTLN("WARNING: DNS resolution failed for pool.ntp.org - NTP sync may fail");
  }
  #endif

  bool ntp_ok = false;
  unsigned long epochTime = 0;
  const unsigned long kMinValidEpoch = 1767225600;  // 2026-01-01 00:00:00 UTC

  _ntp_client.begin();
  const int kMaxNtpRetries = 3;
  for (int attempt = 1; attempt <= kMaxNtpRetries && !ntp_ok; attempt++) {
    if (attempt > 1) {
      MQTT_DEBUG_PRINTLN("NTP retry %d/%d...", attempt, kMaxNtpRetries);
      delay(1000);
    }
    if (_ntp_client.forceUpdate()) {
      epochTime = _ntp_client.getEpochTime();
      if (epochTime >= kMinValidEpoch) {
        ntp_ok = true;
      }
    }
  }
  _ntp_client.end();

  // Fallback: use ESP32 built-in SNTP (configTime) when NTPClient fails
  #ifdef ESP_PLATFORM
  if (!ntp_ok) {
    MQTT_DEBUG_PRINTLN("NTP client failed, trying SNTP fallback...");
    configTime(0, 0, "pool.ntp.org");
    for (int i = 0; i < 20; i++) {
      delay(500);
      epochTime = (unsigned long)time(nullptr);
      if (epochTime >= kMinValidEpoch) {
        ntp_ok = true;
        MQTT_DEBUG_PRINTLN("SNTP fallback succeeded: %lu", epochTime);
        break;
      }
    }
  }
  #endif

  if (ntp_ok) {
    configTime(0, 0, "pool.ntp.org");

    if (_rtc) {
      _rtc->setCurrentTime(epochTime);
    }

    bool was_ntp_synced = _ntp_synced;
    _ntp_synced = true;
    _last_ntp_sync = millis();
    sync_in_progress = false;

    MQTT_DEBUG_PRINTLN("Time synced: %lu", epochTime);

    // If slots are already set up and the time jumped significantly (e.g., SNTP
    // initially returned stale RTC time, then a later sync corrected it), tear down
    // and re-setup all JWT-authenticated slots so they get fresh tokens.
    if (_slots_setup_done && was_ntp_synced) {
      unsigned long current_time = (unsigned long)time(nullptr);
      for (int i = 0; i < _max_active_slots; i++) {
        bool slot_jwt = (_slots[i].preset && _slots[i].preset->auth_type == MQTT_AUTH_JWT) ||
                        (!_slots[i].preset && _slots[i].audience[0] != '\0' && _slots[i].auth_token != nullptr);
        if (_slots[i].enabled && slot_jwt) {
          // Check if the slot's token was created with a stale time
          // (token_expires_at would be far in the past relative to current time)
          if (_slots[i].token_expires_at > 0 && current_time > _slots[i].token_expires_at) {
            MQTT_DEBUG_PRINTLN("MQTT%d token stale after time correction, re-creating", i + 1);
            teardownSlot(i);
            setupSlot(i);
          }
        }
      }
    }

    // Set timezone from string (with DST support) - only if changed
    static char last_timezone[64] = "";
    if (strcmp(_prefs->timezone_string, last_timezone) != 0) {
      if (_timezone) {
        delete _timezone;
        _timezone = nullptr;
      }
      Timezone* tz = createTimezoneFromString(_prefs->timezone_string);
      if (tz) {
        _timezone = tz;
      } else {
        TimeChangeRule utc = {"UTC", Last, Sun, Mar, 0, 0};
        _timezone = new Timezone(utc, utc);
      }
      strncpy(last_timezone, _prefs->timezone_string, sizeof(last_timezone) - 1);
      last_timezone[sizeof(last_timezone) - 1] = '\0';
    }

    (void)gmtime((time_t*)&epochTime);
    (void)localtime((time_t*)&epochTime);
  } else {
    MQTT_DEBUG_PRINTLN("NTP sync failed");
    sync_in_progress = false;
  }
}

// ---------------------------------------------------------------------------
// Timezone helper
// ---------------------------------------------------------------------------

Timezone* MQTTBridge::createTimezoneFromString(const char* tz_string) {
  // Create Timezone objects for common IANA timezone strings

  // North America
  if (strcmp(tz_string, "America/Los_Angeles") == 0 || strcmp(tz_string, "America/Vancouver") == 0) {
    TimeChangeRule pst = {"PST", First, Sun, Nov, 2, -480};  // UTC-8
    TimeChangeRule pdt = {"PDT", Second, Sun, Mar, 2, -420}; // UTC-7
    return new Timezone(pdt, pst);
  } else if (strcmp(tz_string, "America/Denver") == 0) {
    TimeChangeRule mst = {"MST", First, Sun, Nov, 2, -420};  // UTC-7
    TimeChangeRule mdt = {"MDT", Second, Sun, Mar, 2, -360};  // UTC-6
    return new Timezone(mdt, mst);
  } else if (strcmp(tz_string, "America/Chicago") == 0) {
    TimeChangeRule cst = {"CST", First, Sun, Nov, 2, -360};  // UTC-6
    TimeChangeRule cdt = {"CDT", Second, Sun, Mar, 2, -300}; // UTC-5
    return new Timezone(cdt, cst);
  } else if (strcmp(tz_string, "America/New_York") == 0 || strcmp(tz_string, "America/Toronto") == 0) {
    TimeChangeRule est = {"EST", First, Sun, Nov, 2, -300};   // UTC-5
    TimeChangeRule edt = {"EDT", Second, Sun, Mar, 2, -240}; // UTC-4
    return new Timezone(edt, est);
  } else if (strcmp(tz_string, "America/Anchorage") == 0) {
    TimeChangeRule akst = {"AKST", First, Sun, Nov, 2, -540}; // UTC-9
    TimeChangeRule akdt = {"AKDT", Second, Sun, Mar, 2, -480}; // UTC-8
    return new Timezone(akdt, akst);
  } else if (strcmp(tz_string, "Pacific/Honolulu") == 0) {
    TimeChangeRule hst = {"HST", Last, Sun, Oct, 2, -600}; // UTC-10 (no DST)
    return new Timezone(hst, hst);

  // Europe
  } else if (strcmp(tz_string, "Europe/London") == 0) {
    TimeChangeRule gmt = {"GMT", Last, Sun, Oct, 2, 0};     // UTC+0
    TimeChangeRule bst = {"BST", Last, Sun, Mar, 1, 60};    // UTC+1
    return new Timezone(bst, gmt);
  } else if (strcmp(tz_string, "Europe/Paris") == 0 || strcmp(tz_string, "Europe/Berlin") == 0) {
    TimeChangeRule cet = {"CET", Last, Sun, Oct, 3, 60};    // UTC+1
    TimeChangeRule cest = {"CEST", Last, Sun, Mar, 2, 120}; // UTC+2
    return new Timezone(cest, cet);
  } else if (strcmp(tz_string, "Europe/Moscow") == 0) {
    TimeChangeRule msk = {"MSK", Last, Sun, Oct, 3, 180};   // UTC+3 (no DST since 2014)
    return new Timezone(msk, msk);

  // Asia
  } else if (strcmp(tz_string, "Asia/Tokyo") == 0) {
    TimeChangeRule jst = {"JST", Last, Sun, Oct, 2, 540};   // UTC+9 (no DST)
    return new Timezone(jst, jst);
  } else if (strcmp(tz_string, "Asia/Shanghai") == 0 || strcmp(tz_string, "Asia/Hong_Kong") == 0) {
    TimeChangeRule cst = {"CST", Last, Sun, Oct, 2, 480};   // UTC+8 (no DST)
    return new Timezone(cst, cst);
  } else if (strcmp(tz_string, "Asia/Kolkata") == 0) {
    TimeChangeRule ist = {"IST", Last, Sun, Oct, 2, 330};   // UTC+5:30 (no DST)
    return new Timezone(ist, ist);
  } else if (strcmp(tz_string, "Asia/Dubai") == 0) {
    TimeChangeRule gst = {"GST", Last, Sun, Oct, 2, 240};   // UTC+4 (no DST)
    return new Timezone(gst, gst);

  // Australia
  } else if (strcmp(tz_string, "Australia/Sydney") == 0 || strcmp(tz_string, "Australia/Melbourne") == 0) {
    TimeChangeRule aest = {"AEST", First, Sun, Apr, 3, 600};  // UTC+10
    TimeChangeRule aedt = {"AEDT", First, Sun, Oct, 2, 660};   // UTC+11
    return new Timezone(aedt, aest);
  } else if (strcmp(tz_string, "Australia/Perth") == 0) {
    TimeChangeRule awst = {"AWST", Last, Sun, Oct, 2, 480};   // UTC+8 (no DST)
    return new Timezone(awst, awst);

  // Timezone abbreviations (with DST handling)
  } else if (strcmp(tz_string, "PDT") == 0 || strcmp(tz_string, "PST") == 0) {
    TimeChangeRule pst = {"PST", First, Sun, Nov, 2, -480};
    TimeChangeRule pdt = {"PDT", Second, Sun, Mar, 2, -420};
    return new Timezone(pdt, pst);
  } else if (strcmp(tz_string, "MDT") == 0 || strcmp(tz_string, "MST") == 0) {
    TimeChangeRule mst = {"MST", First, Sun, Nov, 2, -420};
    TimeChangeRule mdt = {"MDT", Second, Sun, Mar, 2, -360};
    return new Timezone(mdt, mst);
  } else if (strcmp(tz_string, "CDT") == 0 || strcmp(tz_string, "CST") == 0) {
    TimeChangeRule cst = {"CST", First, Sun, Nov, 2, -360};
    TimeChangeRule cdt = {"CDT", Second, Sun, Mar, 2, -300};
    return new Timezone(cdt, cst);
  } else if (strcmp(tz_string, "EDT") == 0 || strcmp(tz_string, "EST") == 0) {
    TimeChangeRule est = {"EST", First, Sun, Nov, 2, -300};
    TimeChangeRule edt = {"EDT", Second, Sun, Mar, 2, -240};
    return new Timezone(edt, est);
  } else if (strcmp(tz_string, "BST") == 0 || strcmp(tz_string, "GMT") == 0) {
    TimeChangeRule gmt = {"GMT", Last, Sun, Oct, 2, 0};
    TimeChangeRule bst = {"BST", Last, Sun, Mar, 1, 60};
    return new Timezone(bst, gmt);
  } else if (strcmp(tz_string, "CEST") == 0 || strcmp(tz_string, "CET") == 0) {
    TimeChangeRule cet = {"CET", Last, Sun, Oct, 3, 60};
    TimeChangeRule cest = {"CEST", Last, Sun, Mar, 2, 120};
    return new Timezone(cest, cet);

  // UTC and simple offsets
  } else if (strcmp(tz_string, "UTC") == 0) {
    TimeChangeRule utc = {"UTC", Last, Sun, Mar, 0, 0};
    return new Timezone(utc, utc);
  } else if (strncmp(tz_string, "UTC", 3) == 0) {
    int offset = atoi(tz_string + 3);
    TimeChangeRule utc_offset = {"UTC", Last, Sun, Mar, 0, offset * 60};
    return new Timezone(utc_offset, utc_offset);
  } else if (strncmp(tz_string, "GMT", 3) == 0) {
    int offset = atoi(tz_string + 3);
    TimeChangeRule gmt_offset = {"GMT", Last, Sun, Mar, 0, offset * 60};
    return new Timezone(gmt_offset, gmt_offset);
  } else if (strncmp(tz_string, "+", 1) == 0 || strncmp(tz_string, "-", 1) == 0) {
    int offset = atoi(tz_string);
    TimeChangeRule offset_tz = {"TZ", Last, Sun, Mar, 0, offset * 60};
    return new Timezone(offset_tz, offset_tz);
  } else {
    MQTT_DEBUG_PRINTLN("Unknown timezone: %s", tz_string);
    return nullptr;
  }
}

// ---------------------------------------------------------------------------
// Utility methods
// ---------------------------------------------------------------------------

void MQTTBridge::getClientVersion(char* buffer, size_t buffer_size) const {
  if (!buffer || buffer_size == 0) {
    return;
  }
  snprintf(buffer, buffer_size, "meshcore/%s", _firmware_version);
}

void MQTTBridge::optimizeMqttClientConfig(PsychicMqttClient* client, bool needs_large_buffer) {
  if (!client) return;

  // Cloudflare closes WebSocket connections after 100s idle (non-configurable).
#if defined(BOARD_HAS_PSRAM)
  client->setKeepAlive(45);
#else
  // Non-PSRAM: use a longer keepalive to reduce TLS teardown/reconnect churn.
  // 75s is safe behind Cloudflare (100s idle timeout, 25s margin).
  client->setKeepAlive(75);
#endif

  // Buffer sizing: 896 is the minimum safe size for JWT clients (CONNECT + 768-byte JWT).
  // On PSRAM boards, use a uniform size to reduce fragmentation from mixed allocations.
  // On non-PSRAM boards, use smaller buffers for non-JWT slots to reduce heap usage and
  // leave smaller holes during teardown/recreate cycles.
#if defined(BOARD_HAS_PSRAM)
  static const int MQTT_CLIENT_BUFFER_SIZE = 896;
#else
  const int MQTT_CLIENT_BUFFER_SIZE = needs_large_buffer ? 896 : 512;
#endif

  client->setBufferSize(MQTT_CLIENT_BUFFER_SIZE);

  // Access ESP-IDF config to optimize additional settings
  esp_mqtt_client_config_t* config = client->getMqttConfig();
  if (config) {
    #if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
      if (config->buffer.out_size == 0 || config->buffer.out_size > MQTT_CLIENT_BUFFER_SIZE) {
        config->buffer.out_size = MQTT_CLIENT_BUFFER_SIZE;
      }
    #endif
  }
}

void MQTTBridge::logMemoryStatus() {
  MQTT_DEBUG_PRINTLN("Memory: Free=%d, Max=%d, Queue=%d/%d",
                     ESP.getFreeHeap(), ESP.getMaxAllocHeap(), _queue_count, MAX_QUEUE_SIZE);
}

// ---------------------------------------------------------------------------
// Setters and accessors
// ---------------------------------------------------------------------------

void MQTTBridge::setOrigin(const char* origin) {
  strncpy(_origin, origin, sizeof(_origin) - 1);
  _origin[sizeof(_origin) - 1] = '\0';
}

void MQTTBridge::setIATA(const char* iata) {
  strncpy(_iata, iata, sizeof(_iata) - 1);
  _iata[sizeof(_iata) - 1] = '\0';
  for (int i = 0; _iata[i]; i++) {
    _iata[i] = toupper(_iata[i]);
  }
}

void MQTTBridge::setDeviceID(const char* device_id) {
  strncpy(_device_id, device_id, sizeof(_device_id) - 1);
  _device_id[sizeof(_device_id) - 1] = '\0';
  MQTT_DEBUG_PRINTLN("Device ID set to: %s", _device_id);
}

void MQTTBridge::setFirmwareVersion(const char* firmware_version) {
  strncpy(_firmware_version, firmware_version, sizeof(_firmware_version) - 1);
  _firmware_version[sizeof(_firmware_version) - 1] = '\0';
}

void MQTTBridge::setBoardModel(const char* board_model) {
  strncpy(_board_model, board_model, sizeof(_board_model) - 1);
  _board_model[sizeof(_board_model) - 1] = '\0';
}

void MQTTBridge::setBuildDate(const char* build_date) {
  strncpy(_build_date, build_date, sizeof(_build_date) - 1);
  _build_date[sizeof(_build_date) - 1] = '\0';
}

void MQTTBridge::setMessageTypes(bool status, bool packets, bool raw) {
  _status_enabled = status;
  _packets_enabled = packets;
  _raw_enabled = raw;
}

int MQTTBridge::getConnectedBrokers() const {
  int count = 0;
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    if (_slots[i].enabled && _slots[i].connected) {
      count++;
    }
  }
  return count;
}

int MQTTBridge::getQueueSize() const {
  #ifdef ESP_PLATFORM
  if (_packet_queue_handle != nullptr) {
    return uxQueueMessagesWaiting(_packet_queue_handle);
  }
  return 0;
  #else
  return _queue_count;
  #endif
}

void MQTTBridge::setStatsSources(mesh::Dispatcher* dispatcher, mesh::Radio* radio,
                                  mesh::MainBoard* board, mesh::MillisecondClock* ms) {
  _dispatcher = dispatcher;
  _radio = radio;
  _board = board;
  _ms = ms;
}

#endif
