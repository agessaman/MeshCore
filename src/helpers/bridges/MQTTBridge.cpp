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

bool MQTTBridge::isConfigValid(const NodePrefs* prefs) {
  if (!prefs || !isWiFiConfigValid(prefs)) return false;
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    const char* preset_name = prefs->mqtt_slot_preset[i];
    if (preset_name[0] == '\0' || strcmp(preset_name, MQTT_PRESET_NONE) == 0) continue;
    if (strcmp(preset_name, MQTT_PRESET_CUSTOM) == 0) {
      if (prefs->mqtt_slot_host[i][0] != '\0' && prefs->mqtt_slot_port[i] != 0) return true;
    } else if (findMQTTPreset(preset_name) != nullptr) {
      return true;
    }
  }
  return false;
}

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
    case 18:  return "group cipher mismatch";
    case 40:  return "cipher suite rejected";
    case 49:  return "invalid PMKID";
    case 61:  return "AP BSS management";
    case 88:  return "AP BSS management";
    case 168: return "AP band-steering kick";
    case 34:  return "AP state mismatch (class 3 frame)";
    case 39:  return "SSID not found";
    case 63:  return "SA query timeout (PMF)";
    case 200: return "signal lost";
    case 201: return "security mismatch";
    case 202: return "auth mode rejected";
    case 204: return "handshake timeout";
    default:  return nullptr;
  }
}

const char* MQTTBridge::tlsErrorStr(int32_t err) {
  switch (err) {
    case 0x8001: return "DNS failed";
    case 0x8002: return "socket error";
    case 0x8004: return "connect refused";
    case 0x8006: return "TLS timeout";
    case 0x8008: return "connection timeout";
    case 0x800B: return "cert verify failed";
    case 0x8010: return "mbedTLS error";
    case 0x801A: return "TLS handshake failed";
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
      // Default to UTC; setRules() will be called from syncTimeWithNTP when a
      // non-UTC timezone string is configured. Timezone has no default ctor,
      // so we must pass rules here.
      _timezone_storage(TimeChangeRule{"UTC", Last, Sun, Mar, 0, 0}, TimeChangeRule{"UTC", Last, Sun, Mar, 0, 0}),
      _timezone(&_timezone_storage),
      _last_raw_len(0), _last_snr(0), _last_rssi(0), _last_raw_timestamp(0),
      _identity(identity),
      _cached_has_connected_slots(false),
      _last_memory_check(0), _skipped_publishes(0),
      _last_no_broker_log(0), _queue_disconnected_since(0),
      _last_config_warning(0),
      _dispatcher(nullptr), _radio(nullptr), _board(nullptr), _ms(nullptr),
#ifdef WITH_SNMP
      _snmp_agent(nullptr),
#endif
      _last_wifi_check(0), _last_wifi_status(WL_DISCONNECTED), _wifi_status_initialized(false),
      _wifi_disconnected_time(0), _last_wifi_reconnect_attempt(0), _wifi_reconnect_backoff_attempt(0),
      _last_slot_reconnect_ms(0)
#ifdef ESP_PLATFORM
      , _packet_queue_handle(nullptr), _mqtt_task_handle(nullptr),
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
    // auth_token[0] == '\0' after memset above — no valid token
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

  // On PSRAM boards, allocate raw radio buffer and JSON char buffers in PSRAM to preserve
  // internal heap. On non-PSRAM boards these are inline arrays in the class object —
  // no separate allocation needed.
  #if defined(BOARD_HAS_PSRAM)
  _last_raw_data       = (uint8_t*)psram_malloc(LAST_RAW_DATA_SIZE);
  _publish_json_buffer = (char*)psram_malloc(PUBLISH_JSON_BUFFER_SIZE);
  _status_json_buffer  = (char*)psram_malloc(STATUS_JSON_BUFFER_SIZE);
  #else
  memset(_last_raw_data, 0, sizeof(_last_raw_data));
  #endif
  // JSON document scratch space is now a StaticJsonDocument inline class member —
  // no heap allocation needed; reused via doc.clear() on every publish.
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
  // Non-PSRAM: use inline class-member storage with static queue creation.
  // Eliminates a separate heap allocation, reducing startup fragmentation.
  _packet_queue_storage = _packet_queue_inline;
  _packet_queue_handle = xQueueCreateStatic(MAX_QUEUE_SIZE, sizeof(QueuedPacket),
                                             _packet_queue_storage, &_packet_queue_struct);
  #endif
  if (_packet_queue_handle == nullptr) {
    _packet_queue_handle = xQueueCreate(MAX_QUEUE_SIZE, sizeof(QueuedPacket));
  }
  if (_packet_queue_handle == nullptr) {
    MQTT_DEBUG_PRINTLN("Failed to create packet queue!");
    #if defined(BOARD_HAS_PSRAM)
    psram_free(_packet_queue_storage);
    #endif
    _packet_queue_storage = nullptr;
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
    #if defined(BOARD_HAS_PSRAM)
    psram_free(_packet_queue_storage);
    #endif
    _packet_queue_storage = nullptr;
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

  // Allocate persistent MQTT client objects once. They live for the bridge's
  // lifetime so reconfigure/reconnect paths reuse the same mbedTLS context
  // instead of churning ~40 KB of internal heap per cycle.
  initSlotClients();

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
  #if defined(BOARD_HAS_PSRAM)
  psram_free(_packet_queue_storage);
  #endif
  _packet_queue_storage = nullptr;

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

  // Disconnect and delete persistent MQTT clients. teardownSlot() intentionally
  // only disconnects; destruction happens here so the mbedTLS contexts survive
  // the reconfigure/reconnect hot path.
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    teardownSlot(i);
  }
  destroySlotClients();

  // Timezone is inline class storage (_timezone_storage) since Phase 3 of
  // the MQTT memory-defrag work — nothing to delete. _timezone always
  // points at &_timezone_storage and stays valid for the bridge lifetime.

  // Free PSRAM-backed buffers (non-PSRAM builds use inline class arrays — no free needed)
  #if defined(BOARD_HAS_PSRAM)
  psram_free(_last_raw_data);       _last_raw_data = nullptr;
  psram_free(_publish_json_buffer); _publish_json_buffer = nullptr;
  psram_free(_status_json_buffer);  _status_json_buffer = nullptr;
  #endif
  // JSON documents are now StaticJsonDocument inline members — no heap allocation to free.

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
          } else {
            MQTT_DEBUG_PRINTLN("Status publish failed, will retry in %lu ms", STATUS_RETRY_INTERVAL);
          }
        }
      }
    }

    // Update cached connection status periodically (every 5 seconds)
    // This ensures cache stays accurate even if callbacks miss updates
    static unsigned long last_slot_status_update = 0;
    if (now - last_slot_status_update > 5000) {
      updateCachedConnectionStatus();
      last_slot_status_update = now;
    }

    // Adaptive delay: 5 ms when packets are queued, 50 ms when idle.
    // The previous "status approaching" check (widening to 5 ms for 10 s before each status
    // publish) caused 2 000 unnecessary wakeups per interval; the 50 ms idle tick catches
    // the status deadline with at most 50 ms of extra latency, which is irrelevant at a
    // 5-minute interval.
    vTaskDelay(pdMS_TO_TICKS(_queue_count > 0 ? 5 : 50));
  }
}
#endif

// ---------------------------------------------------------------------------
// Slot management
// ---------------------------------------------------------------------------

// Allocate one PsychicMqttClient per slot and register its persistent callbacks.
// Called exactly once per bridge lifetime from begin(); the objects live until
// destroySlotClients(). Reconfiguring a slot (preset change, JWT renewal,
// reconnect) reuses the same client — no delete/new cycles, so the mbedTLS
// context and its ~40 KB of internal-heap buffers are allocated once instead
// of every reconfigure.
void MQTTBridge::initSlotClients() {
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    MQTTSlot& slot = _slots[i];
    if (slot.client != nullptr) continue;

    slot.client = new PsychicMqttClient();
    slot.client->setAutoReconnect(false);  // we handle reconnect with our own backoff

    const int index = i;  // capture a fresh copy so lambdas refer to the right slot
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
  }
}

void MQTTBridge::destroySlotClients() {
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    MQTTSlot& slot = _slots[i];
    if (slot.client == nullptr) continue;

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
}

void MQTTBridge::setupSlot(int index) {
  if (index < 0 || index >= RUNTIME_MQTT_SLOTS) return;
  MQTTSlot& slot = _slots[index];

  if (!slot.enabled) {
    teardownSlot(index);
    return;
  }

  // Persistent client is expected to have been allocated by initSlotClients().
  // If it hasn't, we can't proceed — bail loudly rather than silently leaking.
  if (slot.client == nullptr) {
    MQTT_DEBUG_PRINTLN("MQTT%d: setupSlot before initSlotClients() — skipping", index + 1);
    return;
  }

  // Reconfigure path: if we're re-applying (e.g. after a preset change), stop
  // the existing connection cleanly first. The client object (and its mbedTLS
  // context) is reused; setCredentials / setServer below overwrite the config
  // fields in place before connect() restarts the ESP-IDF client.
  if (slot.initial_connect_done) {
    if (slot.client->connected()) {
      slot.client->disconnect();
    }
    // Clear TLS verification fields so a stale CA-bundle attach or cert
    // pointer from a prior preset doesn't override the new one.
    esp_mqtt_client_config_t* cfg = slot.client->getMqttConfig();
    #if ESP_IDF_VERSION_MAJOR == 5
    cfg->broker.verification.certificate = nullptr;
    cfg->broker.verification.certificate_len = 0;
    cfg->broker.verification.crt_bundle_attach = nullptr;
    cfg->credentials.username = nullptr;
    cfg->credentials.authentication.password = nullptr;
    #else
    cfg->cert_pem = nullptr;
    cfg->cert_len = 0;
    cfg->crt_bundle_attach = nullptr;
    cfg->username = nullptr;
    cfg->password = nullptr;
    #endif
    slot.auth_token[0] = '\0';
    slot.connected = false;
    slot.token_expires_at = 0;
    slot.last_token_renewal = 0;
    slot.reconnect_backoff = 0;
    slot.max_backoff_failures = 0;
    slot.circuit_breaker_tripped = false;
    slot.last_reconnect_attempt = 0;
  }

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

  if (slot.preset) {
    // Preset-based slot
    slot.client->setServer(slot.preset->server_url);
    if (slot.preset->ca_cert) {
      slot.client->setCACert(slot.preset->ca_cert);
    }

    // Try to create token and connect (will succeed only if NTP synced)
    if (slot.preset->auth_type == MQTT_AUTH_JWT) {
      createSlotAuthToken(index);
      if (slot.auth_token[0] != '\0') {
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
      // JWT auth for custom slot — create initial token (buffer is always inline)
      createSlotAuthToken(index);
      if (slot.auth_token[0] != '\0') {
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

// Disconnect the slot's MQTT client and clear per-connection state, but leave
// the client object alive so a subsequent setupSlot() can reuse its mbedTLS
// context. This is called both on reconfigure (preset change) and at shutdown;
// destruction of the underlying client happens once in destroySlotClients().
void MQTTBridge::teardownSlot(int index) {
  if (index < 0 || index >= RUNTIME_MQTT_SLOTS) return;
  MQTTSlot& slot = _slots[index];

  if (slot.client && slot.client->connected()) {
    slot.client->disconnect();
    #ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(50));
    #else
    delay(50);
    #endif
  }

  slot.auth_token[0] = '\0';
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
  slot.last_deferred_log_ms = 0;
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
                    (!_slots[i].preset && _slots[i].audience[0] != '\0');
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
                       (!slot.preset && slot.audience[0] != '\0');
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
          MQTT_DEBUG_PRINTLN("MQTT%d int_heap=%d at token renewal reconnect", index + 1,
              (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
          MQTT_DEBUG_PRINTLN("  radio_state=%d, last_rx=%lums ago",
              _radio ? _radio->getRadioState() : -1,
              (_radio && _radio->getLastRecvMillis() > 0) ? (_ms->getMillis() - _radio->getLastRecvMillis()) : 0);
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

  // Phase 4 (MQTT memory-defrag): the MIN_TLS_HEAP preflight was a workaround
  // for the fragmentation caused by per-reconnect mbedTLS allocations. With
  // persistent clients (Phase 1), the mbedTLS context is allocated once at
  // startup and the preflight is no longer necessary.

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
      MQTT_DEBUG_PRINTLN("MQTT%d circuit breaker probe (attempting single reconnect after %lu ms, int_heap=%d)", index + 1, probe_elapsed,
          (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      MQTT_DEBUG_PRINTLN("  radio_state=%d, last_rx=%lums ago",
          _radio ? _radio->getRadioState() : -1,
          (_radio && _radio->getLastRecvMillis() > 0) ? (_ms->getMillis() - _radio->getLastRecvMillis()) : 0);
      if (slot_uses_jwt) {
        // Regenerate or refresh token, then reconnect the persistent client.
        // The client object and its mbedTLS context are always live post
        // initSlotClients(), so no full setup is ever needed here.
        if (createSlotAuthToken(index)) {
          slot.client->setCredentials(_jwt_username, slot.auth_token);
          MQTT_DEBUG_PRINTLN("MQTT%d circuit breaker probe (fresh token)", index + 1);
        }
        slot.client->reconnect();
      } else {
        slot.client->reconnect();
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
      MQTT_DEBUG_PRINTLN("MQTT%d reconnecting (backoff level %d, failures at max: %d, int_heap=%d)", index + 1, slot.reconnect_backoff, slot.max_backoff_failures,
          (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      MQTT_DEBUG_PRINTLN("  radio_state=%d, last_rx=%lums ago",
          _radio ? _radio->getRadioState() : -1,
          (_radio && _radio->getLastRecvMillis() > 0) ? (_ms->getMillis() - _radio->getLastRecvMillis()) : 0);
      reconnect_attempted = true;
      _last_slot_reconnect_ms = now_millis;
      if (slot_uses_jwt) {
        // Always lightweight reconnect on the persistent client. A stale/expired
        // token is handled by regenerating it in place and updating credentials
        // — no teardown is needed because the client and its mbedTLS context
        // persist for the bridge lifetime.
        if (createSlotAuthToken(index)) {
          slot.client->setCredentials(_jwt_username, slot.auth_token);
          MQTT_DEBUG_PRINTLN("MQTT%d reconnect (fresh token, backoff %d)", index + 1, slot.reconnect_backoff);
        } else {
          MQTT_DEBUG_PRINTLN("MQTT%d reconnect (token refresh failed, backoff %d)", index + 1, slot.reconnect_backoff);
        }
        slot.client->reconnect();
      } else {
        // Non-JWT slots — lightweight reconnect on existing client.
        MQTT_DEBUG_PRINTLN("MQTT%d reconnect (non-JWT, backoff %d)", index + 1, slot.reconnect_backoff);
        slot.client->reconnect();
      }
    }
  }
}

bool MQTTBridge::createSlotAuthToken(int index) {
  if (index < 0 || index >= RUNTIME_MQTT_SLOTS) return false;
  MQTTSlot& slot = _slots[index];
  if (!_identity) return false;

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

bool MQTTBridge::publishToSlot(int index, const char* topic, const char* payload, bool retained, uint8_t qos) {
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

  // QoS 0 for the high-rate packet/raw publish paths: no PUBACK, no outbox store,
  // no per-message heap alloc — critical for non-PSRAM fragmentation. QoS 1 is used
  // only for low-rate retained status messages where delivery matters.
  //
  // esp_mqtt_client_enqueue return convention: QoS 0 returns msg_id == 0 on success
  // (no tracking since there's no PUBACK); QoS 1/2 return a positive msg_id. Negative
  // values (-1 generic failure, -2 outbox full) are the only actual failures.
  int result = slot.client->publish(topic, qos, retained, payload, strlen(payload), true);
  if (result < 0) {
    // QoS0 packet/raw publishes are best-effort and may be retried from the
    // bridge queue; avoid logging transient first-attempt failures here.
    if (qos > 0) {
      static unsigned long last_fail_log = 0;
      unsigned long now = millis();
      if (now - last_fail_log > 60000) {
        MQTT_DEBUG_PRINTLN("MQTT%d publish failed (result=%d qos=%u)", index + 1, result, (unsigned)qos);
        last_fail_log = now;
      }
    }
    return false;
  }
  return true;
}

bool MQTTBridge::publishToAllSlots(const char* topic, const char* payload, bool retained, uint8_t qos) {
  bool published = false;
  for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
    if (_slots[i].enabled && _slots[i].client && _slots[i].connected) {
      if (publishToSlot(i, topic, payload, retained, qos)) {
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
  // _status_json_buffer and _last_raw_data are both Core 0-owned; no mutex needed.
  char fallback_status_buffer[STATUS_JSON_BUFFER_SIZE];
  char* json_buffer = (_status_json_buffer != nullptr) ? _status_json_buffer : fallback_status_buffer;

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

  // Internal heap free (for diagnosing repeater hangs from internal heap exhaustion)
  int internal_heap_free = (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

  int len = MQTTMessageBuilder::buildStatusMessage(
    _status_json_doc,
    _origin, origin_id, _board_model, _firmware_version, radio_info,
    client_version, "online", timestamp, json_buffer, STATUS_JSON_BUFFER_SIZE,
    battery_mv, uptime_secs, errors, _queue_count, noise_floor,
    tx_air_secs, rx_air_secs, recv_errors, internal_heap_free
  );

  if (len > 0) {
    int result = slot.client->publish(status_topic, 1, true, json_buffer, strlen(json_buffer));
    if (result <= 0) {
      MQTT_DEBUG_PRINTLN("MQTT%d status publish failed", index + 1);
    }
  }
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
    if (current_wifi_status != WL_CONNECTED) {
      _wifi_disconnected_time = now;
    }
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
        ps_mode = WIFI_PS_NONE;  // default: no power save; eliminates DTIM wake latency on mains-powered bridges
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

    // Phase 4 (MQTT memory-defrag): the "recreate on prolonged status failure"
    // path and the periodic runCriticalMemoryCheckAndRecovery() call have been
    // removed. They were both symptoms of the heap churn introduced by
    // delete/new cycles of the MQTT client; with persistent clients the
    // allocator stays healthy and these recovery hooks aren't required.
  }
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
  static const uint8_t MAX_QOS0_RETRY_ATTEMPTS = 3;
  static const unsigned long RETRY_DELAY_BASE_MS = 300UL;
  static const unsigned long RETRY_DELAY_JITTER_MS = 200UL;
  #ifdef MQTT_DIAG_VERBOSE
  static unsigned long last_retry_schedule_log = 0;
  #endif

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

    unsigned long now_ms = millis();
    if (queued.next_retry_ms != 0 && now_ms < queued.next_retry_ms) {
      // Not ready yet; put it back and stop draining this cycle.
      xQueueSend(_packet_queue_handle, &queued, 0);
      break;
    }

    // Update Core 0-owned last-raw-data for publishStatus() — no mutex needed since
    // _last_raw_data is now written only here (Core 0) and read only by publishStatus() (Core 0).
    if (!queued.is_tx && queued.has_raw_data && _last_raw_data) {
      memcpy(_last_raw_data, queued.raw_data, queued.raw_len);
      _last_raw_len       = queued.raw_len;
      _last_snr           = queued.snr;
      _last_rssi          = queued.rssi;
      _last_raw_timestamp = millis();
    }

    bool packet_published = publishPacket(&queued.packet_copy, queued.is_tx,
                                          queued.has_raw_data ? queued.raw_data : nullptr,
                                          queued.has_raw_data ? queued.raw_len  : 0,
                                          queued.snr, queued.rssi);
    taskYIELD();  // allow higher-priority tasks to run between packet publishes

    // Publish raw if enabled
    bool raw_published = false;
    if (_raw_enabled) {
      raw_published = publishRaw(&queued.packet_copy);
    }

    bool any_published = packet_published || raw_published;
    if (!any_published && queued.retry_attempts < MAX_QOS0_RETRY_ATTEMPTS) {
      queued.retry_attempts++;
      unsigned long retry_delay_ms = RETRY_DELAY_BASE_MS + (now_ms % RETRY_DELAY_JITTER_MS);
      queued.next_retry_ms = now_ms + retry_delay_ms;
      #ifdef MQTT_DIAG_VERBOSE
      if (now_ms - last_retry_schedule_log > 5000UL) {
        unsigned long age_ms = (queued.timestamp > 0 && now_ms >= queued.timestamp) ? (now_ms - queued.timestamp) : 0;
        MQTT_DEBUG_PRINTLN("Retry scheduled: attempt=%u/%u delay=%lu age=%lu q=%u pkt_type=%u packet_ok=%d raw_ok=%d",
          (unsigned)queued.retry_attempts, (unsigned)MAX_QOS0_RETRY_ATTEMPTS,
          retry_delay_ms, age_ms, (unsigned)uxQueueMessagesWaiting(_packet_queue_handle),
          (unsigned)queued.packet_copy.getPayloadType(), packet_published ? 1 : 0, raw_published ? 1 : 0);
        last_retry_schedule_log = now_ms;
      }
      #endif
      if (xQueueSend(_packet_queue_handle, &queued, 0) != pdTRUE) {
        MQTT_DEBUG_PRINTLN("Retry requeue failed, dropping packet (attempt=%u)", queued.retry_attempts);
      }
    } else if (!any_published) {
      // Intentional: QoS0 best-effort packets are dropped silently in normal
      // builds; detailed exhaustion logs are only emitted in verbose mode.
      #ifdef MQTT_DIAG_VERBOSE
      static unsigned long last_retry_drop_log = 0;
      if (now_ms - last_retry_drop_log > 60000UL) {
        unsigned long age_ms = (queued.timestamp > 0 && now_ms >= queued.timestamp) ? (now_ms - queued.timestamp) : 0;
        MQTT_DEBUG_PRINTLN("Packet dropped after retry exhaustion (attempts=%u age=%lu pkt_type=%u packet_ok=%d raw_ok=%d)",
          queued.retry_attempts, age_ms, (unsigned)queued.packet_copy.getPayloadType(),
          packet_published ? 1 : 0, raw_published ? 1 : 0);
        last_retry_drop_log = now_ms;
      }
      #endif
    }

    _queue_count = uxQueueMessagesWaiting(_packet_queue_handle);
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
  static const uint8_t MAX_QOS0_RETRY_ATTEMPTS = 3;
  static const unsigned long RETRY_DELAY_BASE_MS = 300UL;
  static const unsigned long RETRY_DELAY_JITTER_MS = 200UL;
  #ifdef MQTT_DIAG_VERBOSE
  static unsigned long last_retry_schedule_log = 0;
  #endif

  while (_queue_count > 0 && processed < max_per_loop) {
    unsigned long elapsed = millis() - loop_start_time;
    if (elapsed > MAX_PROCESSING_TIME_MS) {
      break;
    }

    QueuedPacket& queued = _packet_queue[_queue_head];
    unsigned long now_ms = millis();
    if (queued.next_retry_ms != 0 && now_ms < queued.next_retry_ms) {
      break;
    }

    if (!queued.is_tx && queued.has_raw_data && _last_raw_data) {
      memcpy(_last_raw_data, queued.raw_data, queued.raw_len);
      _last_raw_len       = queued.raw_len;
      _last_snr           = queued.snr;
      _last_rssi          = queued.rssi;
      _last_raw_timestamp = millis();
    }

    bool packet_published = publishPacket(&queued.packet_copy, queued.is_tx,
                                          queued.has_raw_data ? queued.raw_data : nullptr,
                                          queued.has_raw_data ? queued.raw_len  : 0,
                                          queued.snr, queued.rssi);
    // No taskYIELD() on non-ESP32 platforms (non-FreeRTOS, cooperative scheduling not needed)

    bool raw_published = false;
    if (_raw_enabled) {
      raw_published = publishRaw(&queued.packet_copy);
    }

    bool any_published = packet_published || raw_published;
    if (!any_published && queued.retry_attempts < MAX_QOS0_RETRY_ATTEMPTS) {
      queued.retry_attempts++;
      unsigned long retry_delay_ms = RETRY_DELAY_BASE_MS + (now_ms % RETRY_DELAY_JITTER_MS);
      queued.next_retry_ms = now_ms + retry_delay_ms;
      #ifdef MQTT_DIAG_VERBOSE
      if (now_ms - last_retry_schedule_log > 5000UL) {
        unsigned long age_ms = (queued.timestamp > 0 && now_ms >= queued.timestamp) ? (now_ms - queued.timestamp) : 0;
        MQTT_DEBUG_PRINTLN("Retry scheduled: attempt=%u/%u delay=%lu age=%lu q=%d pkt_type=%u packet_ok=%d raw_ok=%d",
          (unsigned)queued.retry_attempts, (unsigned)MAX_QOS0_RETRY_ATTEMPTS,
          retry_delay_ms, age_ms, _queue_count,
          (unsigned)queued.packet_copy.getPayloadType(), packet_published ? 1 : 0, raw_published ? 1 : 0);
        last_retry_schedule_log = now_ms;
      }
      #endif
      break;  // keep packet at head for delayed retry
    } else if (!any_published) {
      // Intentional: QoS0 best-effort packets are dropped silently in normal
      // builds; detailed exhaustion logs are only emitted in verbose mode.
      #ifdef MQTT_DIAG_VERBOSE
      static unsigned long last_retry_drop_log = 0;
      if (now_ms - last_retry_drop_log > 60000UL) {
        unsigned long age_ms = (queued.timestamp > 0 && now_ms >= queued.timestamp) ? (now_ms - queued.timestamp) : 0;
        MQTT_DEBUG_PRINTLN("Packet dropped after retry exhaustion (attempts=%u age=%lu pkt_type=%u packet_ok=%d raw_ok=%d)",
          queued.retry_attempts, age_ms, (unsigned)queued.packet_copy.getPayloadType(),
          packet_published ? 1 : 0, raw_published ? 1 : 0);
        last_retry_drop_log = now_ms;
      }
      #endif
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
  // _status_json_buffer and _last_raw_data are both Core 0-owned; no mutex needed.
  char fallback_status_buffer[STATUS_JSON_BUFFER_SIZE];
  char* json_buffer = (_status_json_buffer != nullptr) ? _status_json_buffer : fallback_status_buffer;
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

  // Internal heap free (for diagnosing repeater hangs from internal heap exhaustion)
  int internal_heap_free = (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

  int len = MQTTMessageBuilder::buildStatusMessage(
    _status_json_doc,
    _origin, origin_id, _board_model, _firmware_version, radio_info,
    client_version, "online", timestamp, json_buffer, STATUS_JSON_BUFFER_SIZE,
    battery_mv, uptime_secs, errors, _queue_count, noise_floor,
    tx_air_secs, rx_air_secs, recv_errors, internal_heap_free
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
          if (publishToSlot(i, topic, json_buffer, use_retain, 1)) {
            published = true;
          }
        }
      }
    }
    // If no connected slot accepts status topics (e.g. meshrank is packets-only),
    // treat as success to avoid infinite retry loops
    if (published || !any_slot_wants_status) {
      if (published) MQTT_DEBUG_PRINTLN("Status published");
      return true;
    }
  }

  return false;
}

bool MQTTBridge::publishPacket(mesh::Packet* packet, bool is_tx,
                                const uint8_t* raw_data, int raw_len,
                                float snr, float rssi) {
  if (!packet) return false;

  // Memory pressure check: Skip publishes when there's not enough contiguous
  // heap for the publish itself (JSON buffer + esp-mqtt outbox frame + WiFi TX
  // path). Headroom only — NOT an mbedTLS preflight: persistent clients keep
  // their TLS contexts allocated for the bridge lifetime, so the old ~52 KB
  // "reserve space for reconnect" guard is obsolete post Phase 1. Publish
  // payload is capped at PUBLISH_JSON_BUFFER_SIZE (2 KB); 8 KB is a safe
  // ceiling including esp-mqtt frame overhead and transient TCP buffers.
  #ifdef ESP32
  #if defined(BOARD_HAS_PSRAM)
  static const size_t PUBLISH_SKIP_MAX_ALLOC_THRESHOLD = 16000;
  #else
  static const size_t PUBLISH_SKIP_MAX_ALLOC_THRESHOLD = 8000;
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
      return false;
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
      _packet_json_doc,
      raw_data, raw_len, packet, is_tx, _origin, origin_id,
      snr, rssi, _timezone, active_buffer, active_buffer_size
    );
  } else if (_last_raw_data && _last_raw_len > 0 && (millis() - _last_raw_timestamp) < 1000) {
    len = MQTTMessageBuilder::buildPacketJSONFromRaw(
      _packet_json_doc,
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
        _packet_json_doc,
        reconstructed, rlen, packet, is_tx, _origin, origin_id,
        snr, rssi, _timezone, active_buffer, active_buffer_size
      );
    } else {
      len = MQTTMessageBuilder::buildPacketJSON(
        _packet_json_doc,
        packet, is_tx, _origin, origin_id, _timezone, active_buffer, active_buffer_size
      );
    }
  }

  if (len > 0) {
    bool published = false;
    char topic[128];
    for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
      if (_slots[i].enabled && _slots[i].client && _slots[i].connected) {
        if (buildTopicForSlot(i, MSG_PACKETS, topic, sizeof(topic))) {
          if (publishToSlot(i, topic, active_buffer, false)) {
            published = true;
          }
        }
      }
    }
    return published;
  } else {
    uint8_t packet_type = packet->getPayloadType();
    if (packet_type == 4 || packet_type == 9) {
      MQTT_DEBUG_PRINTLN("Failed to build packet JSON for type=%d (len=%d), packet not published", packet_type, len);
    }
  }
  return false;
}

bool MQTTBridge::publishRaw(mesh::Packet* packet) {
  if (!packet) return false;

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
    bool published = false;
    char topic[128];
    for (int i = 0; i < RUNTIME_MQTT_SLOTS; i++) {
      if (_slots[i].enabled && _slots[i].client && _slots[i].connected) {
        if (buildTopicForSlot(i, MSG_RAW, topic, sizeof(topic))) {
          if (publishToSlot(i, topic, active_buffer, false)) {
            published = true;
          }
        }
      }
    }
    return published;
  }
  return false;
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

  // Consume staged raw data (written by storeRawRadioData() on Core 1, same call sequence).
  // No mutex needed — both sites run on Core 1 before xQueueSend() crosses the core boundary.
  if (!is_tx && _staged_raw_valid) {
    if (_staged_raw_len <= (int)sizeof(queued.raw_data)) {
      memcpy(queued.raw_data, _staged_raw, _staged_raw_len);
      queued.raw_len    = (uint8_t)_staged_raw_len;
      queued.has_raw_data = true;
    }
    queued.snr          = _staged_snr;
    queued.rssi         = _staged_rssi;
    _staged_raw_valid   = false;  // consumed; cleared before xQueueSend
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

  if (!is_tx && _staged_raw_valid) {
    if (_staged_raw_len <= (int)sizeof(queued.raw_data)) {
      memcpy(queued.raw_data, _staged_raw, _staged_raw_len);
      queued.raw_len    = (uint8_t)_staged_raw_len;
      queued.has_raw_data = true;
    }
    queued.snr          = _staged_snr;
    queued.rssi         = _staged_rssi;
    _staged_raw_valid   = false;
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
  dequeued.has_raw_data = false;

  _queue_head = (_queue_head + 1) % MAX_QUEUE_SIZE;
  _queue_count--;
  #endif
}

// ---------------------------------------------------------------------------
// Raw radio data storage
// ---------------------------------------------------------------------------

void MQTTBridge::storeRawRadioData(const uint8_t* raw_data, int len, float snr, float rssi) {
  // Writes into the Core 1-only staging area. No mutex needed: this function and
  // queuePacket() are both called from Core 1 in guaranteed sequence for each packet.
  if (len > 0 && len <= (int)LAST_RAW_DATA_SIZE) {
    memcpy(_staged_raw, raw_data, len);
    _staged_raw_len   = len;
    _staged_snr       = snr;
    _staged_rssi      = rssi;
    _staged_raw_valid = true;
    MQTT_DEBUG_PRINTLN("Stored raw radio data: %d bytes, SNR=%.1f, RSSI=%.1f", len, snr, rssi);
  }
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
                        (!_slots[i].preset && _slots[i].audience[0] != '\0');
        if (_slots[i].enabled && slot_jwt && _slots[i].client) {
          // Token created before NTP corrected the clock — refresh credentials
          // in place and reconnect the persistent client. No teardown needed.
          if (_slots[i].token_expires_at > 0 && current_time > _slots[i].token_expires_at) {
            MQTT_DEBUG_PRINTLN("MQTT%d token stale after time correction, re-creating", i + 1);
            if (createSlotAuthToken(i)) {
              _slots[i].client->setCredentials(_jwt_username, _slots[i].auth_token);
            }
            _slots[i].client->reconnect();
          }
        }
      }
    }

    // Set timezone from string (with DST support) — only if changed.
    // Reuses the inline _timezone_storage via setRules() instead of
    // deleting/newing a Timezone, which was a per-change heap alloc pair.
    static char last_timezone[64] = "";
    if (strcmp(_prefs->timezone_string, last_timezone) != 0) {
      TimeChangeRule dst_rule, std_rule;
      if (!timezoneRulesFromString(_prefs->timezone_string, dst_rule, std_rule)) {
        TimeChangeRule utc = {"UTC", Last, Sun, Mar, 0, 0};
        dst_rule = utc;
        std_rule = utc;
      }
      _timezone_storage.setRules(dst_rule, std_rule);
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

// Populates dst_out and std_out with the DST/standard TimeChangeRules for the
// given timezone string. Returns true on match, false on unknown strings. Zero
// heap allocation — the caller then passes these into Timezone::setRules() on
// an existing Timezone object.
bool MQTTBridge::timezoneRulesFromString(const char* tz_string, TimeChangeRule& dst_out, TimeChangeRule& std_out) {
  // GCC refuses to implicitly build a TimeChangeRule temporary from a bare
  // braced-init-list on the right-hand side of operator= (the aggregate has a
  // char[6] member). Name the type explicitly so a proper temporary is formed.
  // North America
  if (strcmp(tz_string, "America/Los_Angeles") == 0 || strcmp(tz_string, "America/Vancouver") == 0) {
    std_out = TimeChangeRule{"PST", First, Sun, Nov, 2, -480};
    dst_out = TimeChangeRule{"PDT", Second, Sun, Mar, 2, -420};
    return true;
  } else if (strcmp(tz_string, "America/Denver") == 0) {
    std_out = TimeChangeRule{"MST", First, Sun, Nov, 2, -420};
    dst_out = TimeChangeRule{"MDT", Second, Sun, Mar, 2, -360};
    return true;
  } else if (strcmp(tz_string, "America/Chicago") == 0) {
    std_out = TimeChangeRule{"CST", First, Sun, Nov, 2, -360};
    dst_out = TimeChangeRule{"CDT", Second, Sun, Mar, 2, -300};
    return true;
  } else if (strcmp(tz_string, "America/New_York") == 0 || strcmp(tz_string, "America/Toronto") == 0) {
    std_out = TimeChangeRule{"EST", First, Sun, Nov, 2, -300};
    dst_out = TimeChangeRule{"EDT", Second, Sun, Mar, 2, -240};
    return true;
  } else if (strcmp(tz_string, "America/Anchorage") == 0) {
    std_out = TimeChangeRule{"AKST", First, Sun, Nov, 2, -540};
    dst_out = TimeChangeRule{"AKDT", Second, Sun, Mar, 2, -480};
    return true;
  } else if (strcmp(tz_string, "Pacific/Honolulu") == 0) {
    TimeChangeRule hst = {"HST", Last, Sun, Oct, 2, -600};
    dst_out = hst; std_out = hst;
    return true;

  // Europe
  } else if (strcmp(tz_string, "Europe/London") == 0) {
    std_out = TimeChangeRule{"GMT", Last, Sun, Oct, 2, 0};
    dst_out = TimeChangeRule{"BST", Last, Sun, Mar, 1, 60};
    return true;
  } else if (strcmp(tz_string, "Europe/Paris") == 0 || strcmp(tz_string, "Europe/Berlin") == 0) {
    std_out = TimeChangeRule{"CET", Last, Sun, Oct, 3, 60};
    dst_out = TimeChangeRule{"CEST", Last, Sun, Mar, 2, 120};
    return true;
  } else if (strcmp(tz_string, "Europe/Moscow") == 0) {
    TimeChangeRule msk = {"MSK", Last, Sun, Oct, 3, 180};
    dst_out = msk; std_out = msk;
    return true;

  // Asia
  } else if (strcmp(tz_string, "Asia/Tokyo") == 0) {
    TimeChangeRule jst = {"JST", Last, Sun, Oct, 2, 540};
    dst_out = jst; std_out = jst;
    return true;
  } else if (strcmp(tz_string, "Asia/Shanghai") == 0 || strcmp(tz_string, "Asia/Hong_Kong") == 0) {
    TimeChangeRule cst = {"CST", Last, Sun, Oct, 2, 480};
    dst_out = cst; std_out = cst;
    return true;
  } else if (strcmp(tz_string, "Asia/Kolkata") == 0) {
    TimeChangeRule ist = {"IST", Last, Sun, Oct, 2, 330};
    dst_out = ist; std_out = ist;
    return true;
  } else if (strcmp(tz_string, "Asia/Dubai") == 0) {
    TimeChangeRule gst = {"GST", Last, Sun, Oct, 2, 240};
    dst_out = gst; std_out = gst;
    return true;

  // Australia
  } else if (strcmp(tz_string, "Australia/Sydney") == 0 || strcmp(tz_string, "Australia/Melbourne") == 0) {
    std_out = TimeChangeRule{"AEST", First, Sun, Apr, 3, 600};
    dst_out = TimeChangeRule{"AEDT", First, Sun, Oct, 2, 660};
    return true;
  } else if (strcmp(tz_string, "Australia/Perth") == 0) {
    TimeChangeRule awst = {"AWST", Last, Sun, Oct, 2, 480};
    dst_out = awst; std_out = awst;
    return true;

  // Timezone abbreviations (with DST handling)
  } else if (strcmp(tz_string, "PDT") == 0 || strcmp(tz_string, "PST") == 0) {
    std_out = TimeChangeRule{"PST", First, Sun, Nov, 2, -480};
    dst_out = TimeChangeRule{"PDT", Second, Sun, Mar, 2, -420};
    return true;
  } else if (strcmp(tz_string, "MDT") == 0 || strcmp(tz_string, "MST") == 0) {
    std_out = TimeChangeRule{"MST", First, Sun, Nov, 2, -420};
    dst_out = TimeChangeRule{"MDT", Second, Sun, Mar, 2, -360};
    return true;
  } else if (strcmp(tz_string, "CDT") == 0 || strcmp(tz_string, "CST") == 0) {
    std_out = TimeChangeRule{"CST", First, Sun, Nov, 2, -360};
    dst_out = TimeChangeRule{"CDT", Second, Sun, Mar, 2, -300};
    return true;
  } else if (strcmp(tz_string, "EDT") == 0 || strcmp(tz_string, "EST") == 0) {
    std_out = TimeChangeRule{"EST", First, Sun, Nov, 2, -300};
    dst_out = TimeChangeRule{"EDT", Second, Sun, Mar, 2, -240};
    return true;
  } else if (strcmp(tz_string, "BST") == 0 || strcmp(tz_string, "GMT") == 0) {
    std_out = TimeChangeRule{"GMT", Last, Sun, Oct, 2, 0};
    dst_out = TimeChangeRule{"BST", Last, Sun, Mar, 1, 60};
    return true;
  } else if (strcmp(tz_string, "CEST") == 0 || strcmp(tz_string, "CET") == 0) {
    std_out = TimeChangeRule{"CET", Last, Sun, Oct, 3, 60};
    dst_out = TimeChangeRule{"CEST", Last, Sun, Mar, 2, 120};
    return true;

  // UTC and simple offsets
  } else if (strcmp(tz_string, "UTC") == 0) {
    TimeChangeRule utc = {"UTC", Last, Sun, Mar, 0, 0};
    dst_out = utc; std_out = utc;
    return true;
  } else if (strncmp(tz_string, "UTC", 3) == 0) {
    int offset = atoi(tz_string + 3);
    TimeChangeRule utc_offset = {"UTC", Last, Sun, Mar, 0, offset * 60};
    dst_out = utc_offset; std_out = utc_offset;
    return true;
  } else if (strncmp(tz_string, "GMT", 3) == 0) {
    int offset = atoi(tz_string + 3);
    TimeChangeRule gmt_offset = {"GMT", Last, Sun, Mar, 0, offset * 60};
    dst_out = gmt_offset; std_out = gmt_offset;
    return true;
  } else if (tz_string[0] == '+' || tz_string[0] == '-') {
    int offset = atoi(tz_string);
    TimeChangeRule offset_tz = {"TZ", Last, Sun, Mar, 0, offset * 60};
    dst_out = offset_tz; std_out = offset_tz;
    return true;
  } else {
    MQTT_DEBUG_PRINTLN("Unknown timezone: %s", tz_string);
    return false;
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
