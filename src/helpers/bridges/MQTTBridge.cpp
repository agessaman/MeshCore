#include "MQTTBridge.h"
#include "../MQTTMessageBuilder.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Timezone.h>

// Using ESP32's built-in certificate bundle

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

MQTTBridge::MQTTBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc, mesh::LocalIdentity *identity)
    : BridgeBase(prefs, mgr, rtc), _mqtt_client(nullptr),
      _active_brokers(0), _queue_head(0), _queue_tail(0), _queue_count(0),
      _last_status_publish(0), _status_interval(300000), // 5 minutes default
              _ntp_client(_ntp_udp, "pool.ntp.org", 0, 60000), _last_ntp_sync(0), _ntp_synced(false),
              _timezone(nullptr), _last_raw_len(0), _last_snr(0), _last_rssi(0), _last_raw_timestamp(0),
              _analyzer_us_enabled(false), _analyzer_eu_enabled(false), _identity(identity),
              _analyzer_us_client(nullptr), _analyzer_eu_client(nullptr), _config_valid(false),
              _last_no_broker_log(0), _dispatcher(nullptr), _radio(nullptr), _board(nullptr), _ms(nullptr) {
  
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
  _tx_enabled = false;  // Disable TX packets by default
  
  // Initialize MQTT server settings with defaults (empty/null values)
  _prefs->mqtt_server[0] = '\0';  // Empty string
  _prefs->mqtt_port = 0;          // Invalid port
  _prefs->mqtt_username[0] = '\0'; // Empty string
  _prefs->mqtt_password[0] = '\0'; // Empty string
  
  // Override with build flags if defined
#ifdef MQTT_SERVER
  strncpy(_prefs->mqtt_server, MQTT_SERVER, sizeof(_prefs->mqtt_server) - 1);
#endif
#ifdef MQTT_PORT
  _prefs->mqtt_port = MQTT_PORT;
#endif
#ifdef MQTT_USERNAME
  strncpy(_prefs->mqtt_username, MQTT_USERNAME, sizeof(_prefs->mqtt_username) - 1);
#endif
#ifdef MQTT_PASSWORD
  strncpy(_prefs->mqtt_password, MQTT_PASSWORD, sizeof(_prefs->mqtt_password) - 1);
#endif
  
  // Initialize packet queue
  memset(_packet_queue, 0, sizeof(_packet_queue));
  // Initialize has_raw_data flags
  for (int i = 0; i < MAX_QUEUE_SIZE; i++) {
    _packet_queue[i].has_raw_data = false;
  }
  
  // Initialize throttle log timers
  _last_no_broker_log = 0;
  _last_analyzer_us_log = 0;
  _last_analyzer_eu_log = 0;
  
  // Set default broker configuration
  setBrokerDefaults();
}

void MQTTBridge::begin() {
  MQTT_DEBUG_PRINTLN("Initializing MQTT Bridge...");
  
  // Check if WiFi credentials are configured first
  if (!isWiFiConfigValid(_prefs)) {
    MQTT_DEBUG_PRINTLN("MQTT Bridge initialization skipped - WiFi credentials not configured");
    return;
  }
  
  // Validate custom MQTT broker configuration (optional)
  _config_valid = isMQTTConfigValid();
  if (!_config_valid) {
    MQTT_DEBUG_PRINTLN("No valid custom MQTT server configured - analyzer servers will still work");
  } else {
    MQTT_DEBUG_PRINTLN("Custom MQTT server configuration is valid");
  }
  
  // Update origin and IATA from preferences
  strncpy(_origin, _prefs->mqtt_origin, sizeof(_origin) - 1);
  _origin[sizeof(_origin) - 1] = '\0';
  strncpy(_iata, _prefs->mqtt_iata, sizeof(_iata) - 1);
  _iata[sizeof(_iata) - 1] = '\0';
  
  // Strip quotes from MQTT server configuration if present
  stripQuotes(_prefs->mqtt_server, sizeof(_prefs->mqtt_server));
  stripQuotes(_prefs->mqtt_username, sizeof(_prefs->mqtt_username));
  stripQuotes(_prefs->mqtt_password, sizeof(_prefs->mqtt_password));
  
  // Strip quotes from origin if present
  MQTT_DEBUG_PRINTLN("Origin before stripping: '%s'", _origin);
  stripQuotes(_origin, sizeof(_origin));
  MQTT_DEBUG_PRINTLN("Origin after stripping: '%s'", _origin);
  
  // Strip quotes from IATA if present
  MQTT_DEBUG_PRINTLN("IATA before stripping: '%s'", _iata);
  stripQuotes(_iata, sizeof(_iata));
  MQTT_DEBUG_PRINTLN("IATA after stripping: '%s'", _iata);
  
  // Update enabled flags from preferences
  _status_enabled = _prefs->mqtt_status_enabled;
  _packets_enabled = _prefs->mqtt_packets_enabled;
  _raw_enabled = _prefs->mqtt_raw_enabled;
  _tx_enabled = _prefs->mqtt_tx_enabled;
  // Set status interval to 5 minutes (300000 ms), or use preference if set and valid
  // Sanity check: interval should be between 1 second (1000ms) and 1 hour (3600000ms)
  // This field may be uninitialized if preferences were saved before this field was added
  if (_prefs->mqtt_status_interval >= 1000 && _prefs->mqtt_status_interval <= 3600000) {
    _status_interval = _prefs->mqtt_status_interval;
    MQTT_DEBUG_PRINTLN("Using preference status interval: %lu ms", _status_interval);
  } else {
    // Invalid or uninitialized value - fix it in preferences and use default
    if (_prefs->mqtt_status_interval > 0 && _prefs->mqtt_status_interval != 300000) {
      MQTT_DEBUG_PRINTLN("Invalid preference status interval: %lu ms (fixing to default 300000 ms)", 
                         _prefs->mqtt_status_interval);
    }
    _prefs->mqtt_status_interval = 300000; // Fix the preference value
    _status_interval = 300000; // 5 minutes default
    // Note: We don't save preferences here as that should be done by the caller if needed
    // This ensures the correct value is used for this session
  }
  
  MQTT_DEBUG_PRINTLN("Status publishing: enabled=%s, interval=%lu ms", 
                     _status_enabled ? "true" : "false", _status_interval);
  
  MQTT_DEBUG_PRINTLN("Origin: %s, IATA: %s", _origin, _iata);
  MQTT_DEBUG_PRINTLN("Device ID: %s", _device_id);
  MQTT_DEBUG_PRINTLN("WiFi SSID: %s", _prefs->wifi_ssid);
  
  // Initialize WiFi
  MQTT_DEBUG_PRINTLN("Starting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(_prefs->wifi_ssid, _prefs->wifi_password);
  
  // Wait for WiFi connection
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    MQTT_DEBUG_PRINT(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    MQTT_DEBUG_PRINTLN("WiFi connected! IP: %s", WiFi.localIP().toString().c_str());
    
    // Sync time with NTP
    syncTimeWithNTP();
  } else {
    MQTT_DEBUG_PRINTLN("WiFi connection failed!");
    return;
  }
  
  // Initialize PsychicMqttClient
  _mqtt_client = new PsychicMqttClient();
  
  // Set up event callbacks for the main MQTT client
  _mqtt_client->onConnect([this](bool sessionPresent) {
    MQTT_DEBUG_PRINTLN("MQTT client connected, session present: %s", sessionPresent ? "true" : "false");
    // Update broker connection status
    for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
      if (_brokers[i].enabled && !_brokers[i].connected) {
        _brokers[i].connected = true;
        _active_brokers++;
        MQTT_DEBUG_PRINTLN("Broker %d marked as connected", i);
        break;
      }
    }
  });
  
  _mqtt_client->onDisconnect([this](bool sessionPresent) {
    MQTT_DEBUG_PRINTLN("MQTT client disconnected, session present: %s", sessionPresent ? "true" : "false");
    // Update broker connection status
    for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
      if (_brokers[i].connected) {
        _brokers[i].connected = false;
        _active_brokers--;
        MQTT_DEBUG_PRINTLN("Broker %d marked as disconnected", i);
        break;
      }
    }
  });
  
  // Set default broker from preferences or build flags
  setBroker(0, _prefs->mqtt_server, _prefs->mqtt_port, _prefs->mqtt_username, _prefs->mqtt_password, true);
  
  // Setup Let's Mesh Analyzer servers
  setupAnalyzerServers();
  
  // Setup PsychicMqttClient WebSocket clients for analyzer servers
  setupAnalyzerClients();
  
  // Connect to brokers
  connectToBrokers();
  
  _initialized = true;
  MQTT_DEBUG_PRINTLN("MQTT Bridge initialized");
}

void MQTTBridge::end() {
  MQTT_DEBUG_PRINTLN("Stopping MQTT Bridge...");
  
  // Disconnect from all brokers
  for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
    if (_brokers[i].enabled && _brokers[i].connected) {
      _mqtt_client->disconnect();
      _brokers[i].connected = false;
    }
  }
  
  // Disconnect analyzer clients
  if (_analyzer_us_client) {
    _analyzer_us_client->disconnect();
    delete _analyzer_us_client;
    _analyzer_us_client = nullptr;
  }
  if (_analyzer_eu_client) {
    _analyzer_eu_client->disconnect();
    delete _analyzer_eu_client;
    _analyzer_eu_client = nullptr;
  }
  
  // Clean up queued packets to prevent memory leaks
  for (int i = 0; i < _queue_count; i++) {
    int index = (_queue_head + i) % MAX_QUEUE_SIZE;
    if (_packet_queue[index].packet) {
      _mgr->free(_packet_queue[index].packet);
      _packet_queue[index].packet = nullptr;
    }
    // Clear the entire structure to free raw_data buffers
    memset(&_packet_queue[index], 0, sizeof(QueuedPacket));
  }
  
  // Clear packet queue
  _queue_count = 0;
  _queue_head = 0;
  _queue_tail = 0;
  
  // Clear all queue slots for safety
  memset(_packet_queue, 0, sizeof(_packet_queue));
  
  // Clean up timezone object to prevent memory leak
  if (_timezone) {
    delete _timezone;
    _timezone = nullptr;
  }
  
  // Clean up resources
  if (_mqtt_client) {
    delete _mqtt_client;
    _mqtt_client = nullptr;
  }
  
  _initialized = false;
  MQTT_DEBUG_PRINTLN("MQTT Bridge stopped");
}

bool MQTTBridge::isConfigValid() const {
  return _config_valid;
}

bool MQTTBridge::isConfigValid(const NodePrefs* prefs) {
  // Check if MQTT server is configured (not default placeholder)
  if (strlen(prefs->mqtt_server) == 0 || 
      strcmp(prefs->mqtt_server, "your-mqtt-broker.com") == 0) {
    return false;
  }
  
  // Check if MQTT port is valid
  if (prefs->mqtt_port == 0 || prefs->mqtt_port > 65535) {
    return false;
  }
  
  // Check if MQTT username is configured (not default placeholder)
  if (strlen(prefs->mqtt_username) == 0 || 
      strcmp(prefs->mqtt_username, "your-username") == 0) {
    return false;
  }
  
  // Check if MQTT password is configured (not default placeholder)
  if (strlen(prefs->mqtt_password) == 0 || 
      strcmp(prefs->mqtt_password, "your-password") == 0) {
    return false;
  }
  
  return true;
}

bool MQTTBridge::isReady() const {
  return _initialized && isWiFiConfigValid(_prefs);
}

void MQTTBridge::loop() {
  if (!_initialized) return;
  
  // Maintain broker connections
  connectToBrokers();
  
  // Maintain analyzer server connections
  maintainAnalyzerConnections();
  
  // Process packet queue
  processPacketQueue();
  
  // Periodic NTP sync (every hour)
  if (WiFi.status() == WL_CONNECTED && millis() - _last_ntp_sync > 3600000) {
    syncTimeWithNTP();
  }
  
  // Publish status updates (handle millis() overflow correctly)
  if (_status_enabled) {
    unsigned long now = millis();
    unsigned long elapsed = (now >= _last_status_publish) ? 
                           (now - _last_status_publish) : 
                           (ULONG_MAX - _last_status_publish + now + 1);
    
    if (elapsed >= _status_interval) {
      MQTT_DEBUG_PRINTLN("Status publish timer expired (elapsed: %lu ms, interval: %lu ms)", elapsed, _status_interval);
      if (publishStatus()) {
        _last_status_publish = now;  // Only update timer on successful publication
        MQTT_DEBUG_PRINTLN("Status published successfully, next publish in %lu ms", _status_interval);
      } else {
        MQTT_DEBUG_PRINTLN("Status publish failed, will retry next loop");
        // If publication failed (no brokers connected), don't update timer so we retry next loop
      }
    }
  }
  
  // Memory monitoring (every 5 minutes)
  static unsigned long last_memory_log = 0;
  if (millis() - last_memory_log > 300000) { // 5 minutes
    logMemoryStatus();
    // Debug: Log status timer state when memory check happens
    if (_status_enabled) {
      unsigned long elapsed = (millis() >= _last_status_publish) ? 
                             (millis() - _last_status_publish) : 
                             (ULONG_MAX - _last_status_publish + millis() + 1);
      MQTT_DEBUG_PRINTLN("Memory check: Status timer - elapsed: %lu ms, interval: %lu ms, next: %lu ms", 
                         elapsed, _status_interval, _status_interval - elapsed);
    }
    last_memory_log = millis();
  }
  
  // Critical memory check (every 15 minutes)
  static unsigned long last_critical_check = 0;
  if (millis() - last_critical_check > 900000) { // 15 minutes
    if (ESP.getMaxAllocHeap() < 60000) { // Less than 60KB max alloc
      MQTT_DEBUG_PRINTLN("WARNING: Max alloc heap below 60KB - potential memory leak detected!");
      MQTT_DEBUG_PRINTLN("Free: %d, Min: %d, Max: %d", ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
      
      // Attempt memory defragmentation by forcing garbage collection
      MQTT_DEBUG_PRINTLN("Attempting memory defragmentation...");
      // Force a small allocation and immediate free to trigger defrag
      void* temp = malloc(1024);
      if (temp) {
        free(temp);
        MQTT_DEBUG_PRINTLN("Defragmentation complete. New Max Alloc: %d", ESP.getMaxAllocHeap());
      }
    }
    
    // Critical threshold check
    if (ESP.getMaxAllocHeap() < 40000) { // Less than 40KB max alloc
      MQTT_DEBUG_PRINTLN("CRITICAL: Max alloc heap below 40KB - severe memory leak!");
      MQTT_DEBUG_PRINTLN("Free: %d, Min: %d, Max: %d", ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    }
    
    last_critical_check = millis();
  }
}

void MQTTBridge::onPacketReceived(mesh::Packet *packet) {
  if (!_initialized || !_packets_enabled) {
    MQTT_DEBUG_PRINTLN("Packet received but not processing - initialized: %s, packets_enabled: %s", 
                      _initialized ? "true" : "false", _packets_enabled ? "true" : "false");
    return;
  }
  
  // Check if we have any valid brokers to send to
  bool has_valid_brokers = _config_valid || 
                          (_analyzer_us_enabled && _analyzer_us_client) ||
                          (_analyzer_eu_enabled && _analyzer_eu_client);
  
  if (!has_valid_brokers) {
    MQTT_DEBUG_PRINTLN("Packet received but no valid brokers available - discarding");
    return;
  }
  
  // Debug logging for packet types that might be getting filtered
  uint8_t packet_type = packet->getPayloadType();
  if (packet_type == 4 || packet_type == 9) {  // ADVERT or TRACE
    MQTT_DEBUG_PRINTLN("Packet received: type=%d (ADVERT=%d, TRACE=%d), queuing for transmission", 
                      packet_type, (packet_type == 4), (packet_type == 9));
  }
  
  // Queue packet for transmission
  queuePacket(packet, false);
}

void MQTTBridge::sendPacket(mesh::Packet *packet) {
  if (!_initialized || !_packets_enabled || !_tx_enabled) return;
  
  // Queue packet for transmission (only if TX enabled)
  queuePacket(packet, true);
}

bool MQTTBridge::isMQTTConfigValid() {
  // Check if MQTT server is configured (not default placeholder)
  if (strlen(_prefs->mqtt_server) == 0 || 
      strcmp(_prefs->mqtt_server, "your-mqtt-broker.com") == 0) {
    return false;
  }
  
  // Check if MQTT port is valid
  if (_prefs->mqtt_port == 0 || _prefs->mqtt_port > 65535) {
    return false;
  }
  
  // Check if MQTT username is configured (not default placeholder)
  if (strlen(_prefs->mqtt_username) == 0 || 
      strcmp(_prefs->mqtt_username, "your-username") == 0) {
    return false;
  }
  
  // Check if MQTT password is configured (not default placeholder)
  if (strlen(_prefs->mqtt_password) == 0 || 
      strcmp(_prefs->mqtt_password, "your-password") == 0) {
    return false;
  }
  
  return true;
}

void MQTTBridge::connectToBrokers() {
  // Check if MQTT configuration is valid before attempting connection
  if (!_config_valid) {
    return;
  }
  
  // For now, connect to the first enabled broker
  // TODO: Implement multi-broker support with PsychicMqttClient
  for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
    if (!_brokers[i].enabled) continue;
    
    // Check if we need to attempt connection
    if (!_brokers[i].connected && 
        millis() - _brokers[i].last_attempt > _brokers[i].reconnect_interval) {
      
      MQTT_DEBUG_PRINTLN("Connecting to broker %d: %s:%d", i, _brokers[i].host, _brokers[i].port);
      
      // Generate unique client ID
      char client_id[32];
      snprintf(client_id, sizeof(client_id), "%s_%d_%lu", _origin, i, millis());
      
      // Set broker URI and connect using PsychicMqttClient API
      char broker_uri[128];
      snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%d", _brokers[i].host, _brokers[i].port);
      _mqtt_client->setServer(broker_uri);
      
      // Set credentials if provided
      if (strlen(_brokers[i].username) > 0) {
        _mqtt_client->setCredentials(_brokers[i].username, _brokers[i].password);
      }
      
      // Connect to the broker (PsychicMqttClient uses async connection)
      _mqtt_client->connect();
      
      // Update attempt timestamp
      _brokers[i].last_attempt = millis();
      MQTT_DEBUG_PRINTLN("Initiating connection to broker %d", i);
    }
    
    // Maintain connection
    if (_brokers[i].connected) {
      // PsychicMqttClient handles connection maintenance internally
      // TODO: Implement proper connection state checking with callbacks
      if (!_mqtt_client->connected()) {
        _brokers[i].connected = false;
        _active_brokers--;
        MQTT_DEBUG_PRINTLN("Lost connection to broker %d", i);
      }
    }
  }
}

void MQTTBridge::processPacketQueue() {
  if (_queue_count == 0) {
    return;
  }
  
  // Check if we have any connected brokers (custom or analyzer)
  bool has_connected_brokers = isAnyBrokerConnected() || 
                               (_analyzer_us_enabled && _analyzer_us_client && _analyzer_us_client->connected()) ||
                               (_analyzer_eu_enabled && _analyzer_eu_client && _analyzer_eu_client->connected());
  
  if (!has_connected_brokers) {
    if (_queue_count > 0) {
      // Only log this message periodically to avoid spam (every 30 seconds max)
      unsigned long now = millis();
      if (now - _last_no_broker_log > NO_BROKER_LOG_INTERVAL) {
        MQTT_DEBUG_PRINTLN("Queue has %d packets but no brokers connected", _queue_count);
        _last_no_broker_log = now;
      }
    }
    return;
  }
  
  // Reset the log timer when brokers are connected
  _last_no_broker_log = 0;
  
  // MQTT_DEBUG_PRINTLN("Processing packet queue - count: %d", _queue_count);
  
  // Process up to MAX_QUEUE_SIZE packets per loop to keep up with packet arrival rate
  int processed = 0;
  int max_per_loop = MAX_QUEUE_SIZE; // Process all queued packets per loop
  while (_queue_count > 0 && processed < max_per_loop) {
    QueuedPacket& queued = _packet_queue[_queue_head];
    
    MQTT_DEBUG_PRINTLN("Processing queued packet (is_tx: %s)", queued.is_tx ? "true" : "false");
    
    // Publish packet (use stored raw data if available)
    publishPacket(queued.packet, queued.is_tx, 
                  queued.has_raw_data ? queued.raw_data : nullptr,
                  queued.has_raw_data ? queued.raw_len : 0,
                  queued.has_raw_data ? queued.snr : 0.0f,
                  queued.has_raw_data ? queued.rssi : 0.0f);
    
    // Publish raw if enabled
    if (_raw_enabled) {
      publishRaw(queued.packet);
    }
    
    // Free packet memory before removing from queue
    if (queued.packet) {
      _mgr->free(queued.packet);
      queued.packet = nullptr;
    }
    
    // Remove from queue
    dequeuePacket();
    processed++;
  }
}

bool MQTTBridge::publishStatus() {
  // Check if we have any valid destinations (custom brokers or analyzer servers)
  bool has_custom_brokers = isAnyBrokerConnected() && _config_valid;
  bool has_analyzer_servers = (_analyzer_us_enabled && _analyzer_us_client && _analyzer_us_client->connected()) ||
                               (_analyzer_eu_enabled && _analyzer_eu_client && _analyzer_eu_client->connected());
  
  MQTT_DEBUG_PRINTLN("publishStatus() called - custom_brokers: %s, analyzer_servers: %s", 
                     has_custom_brokers ? "yes" : "no", has_analyzer_servers ? "yes" : "no");
  
  if (!has_custom_brokers && !has_analyzer_servers) {
    MQTT_DEBUG_PRINTLN("No destinations available for status publish");
    return false;  // No destinations available
  }
  
  // Status messages with stats can be larger (~400-500 bytes), so increase buffer size
  char json_buffer[768];  // Increased from 512 to accommodate stats object
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
  
  // Build radio info string (freq,bw,sf,cr)
  snprintf(radio_info, sizeof(radio_info), "%.6f,%.1f,%d,%d", 
           _prefs->freq, _prefs->bw, _prefs->sf, _prefs->cr);
  
  // Use actual device ID
  strncpy(origin_id, _device_id, sizeof(origin_id) - 1);
  origin_id[sizeof(origin_id) - 1] = '\0';
  
  // Build client version string
  char client_version[64];
  snprintf(client_version, sizeof(client_version), "meshcore-custom-repeater/%s", _build_date);
  
  // Collect stats on-demand if sources are available
  int battery_mv = -1;
  int uptime_secs = -1;
  int errors = -1;
  int noise_floor = -999;
  int tx_air_secs = -1;
  int rx_air_secs = -1;
  
  if (_board) {
    battery_mv = _board->getBattMilliVolts();
  }
  if (_ms) {
    uptime_secs = _ms->getMillis() / 1000;
  }
  if (_dispatcher) {
    errors = _dispatcher->getErrFlags();
    tx_air_secs = _dispatcher->getTotalAirTime() / 1000;
    rx_air_secs = _dispatcher->getReceiveAirTime() / 1000;
  }
  if (_radio) {
    noise_floor = (int16_t)_radio->getNoiseFloor();
  }
  
  // Build status message with stats
  int len = MQTTMessageBuilder::buildStatusMessage(
    _origin,
    origin_id,
    _board_model,  // model - now dynamic!
    _firmware_version,  // firmware version
    radio_info,
    client_version,  // client version
    "online",
    timestamp,
    json_buffer,
    sizeof(json_buffer),
    battery_mv,
    uptime_secs,
    errors,
    _queue_count,  // Use current queue length
    noise_floor,
    tx_air_secs,
    rx_air_secs
  );
  
          if (len > 0) {
            bool published = false;
            
            // Publish to all connected custom brokers
            if (_config_valid) {
              for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
                if (_brokers[i].enabled && _brokers[i].connected) {
                  char topic[128];
                  snprintf(topic, sizeof(topic), "meshcore/%s/%s/status", _iata, _device_id);
                  MQTT_DEBUG_PRINTLN("Publishing status to topic: %s", topic);
                  
                  // Set broker for this connection (PsychicMqttClient uses URI format)
                  char broker_uri[128];
                  snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%d", _brokers[i].host, _brokers[i].port);
                  _mqtt_client->setServer(broker_uri);
                  if (_mqtt_client->publish(topic, 1, true, json_buffer, strlen(json_buffer)) > 0) {
                    published = true;
                  }
                }
              }
            }
            
            // Always publish to Let's Mesh Analyzer servers if enabled and connected
            if (has_analyzer_servers) {
              char analyzer_topic[128];
              snprintf(analyzer_topic, sizeof(analyzer_topic), "meshcore/%s/%s/status", _iata, _device_id);
              
              // Try to publish to analyzer servers
              bool analyzer_published = false;
              if (_analyzer_us_enabled && _analyzer_us_client && _analyzer_us_client->connected()) {
                _analyzer_us_client->publish(analyzer_topic, 1, true, json_buffer, strlen(json_buffer));
                analyzer_published = true;
                MQTT_DEBUG_PRINTLN("Published status to US analyzer server");
              }
              if (_analyzer_eu_enabled && _analyzer_eu_client && _analyzer_eu_client->connected()) {
                _analyzer_eu_client->publish(analyzer_topic, 1, true, json_buffer, strlen(json_buffer));
                analyzer_published = true;
                MQTT_DEBUG_PRINTLN("Published status to EU analyzer server");
              }
              
              if (analyzer_published) {
                published = true;
              }
            }
            
            // Return true if we successfully published to at least one destination
            if (published) {
              MQTT_DEBUG_PRINTLN("Status published successfully");
              return true;
            }
          }
          
          MQTT_DEBUG_PRINTLN("Status publish failed - no destinations or build failed");
          return false;  // Failed to build or publish message
}

void MQTTBridge::publishPacket(mesh::Packet* packet, bool is_tx, 
                                const uint8_t* raw_data, int raw_len, 
                                float snr, float rssi) {
  if (!packet) return;
  
<<<<<<< HEAD
  // Size-adaptive buffer: estimate needed size based on packet size
  // Most packets are <100 bytes (need ~400 byte JSON), large packets need ~1500 bytes
  int packet_size = packet->getRawLength();
  size_t json_buffer_size = (packet_size > 150) ? 2048 : 1024;
  char json_buffer[2048]; // Always allocate max, but pass actual needed size to builders
  char origin_id[65];
  
  // Use actual device ID
  strncpy(origin_id, _device_id, sizeof(origin_id) - 1);
  origin_id[sizeof(origin_id) - 1] = '\0';
  
  // Build packet message using raw radio data if provided
  // Use size-adaptive buffer size based on actual packet size
  size_t buffer_size = (packet->getRawLength() > 150) ? 2048 : 1024;
  int len;
  if (raw_data && raw_len > 0) {
    // Use provided raw radio data
    len = MQTTMessageBuilder::buildPacketJSONFromRaw(
      raw_data, raw_len, packet, is_tx, _origin, origin_id, 
      snr, rssi, _timezone, json_buffer, buffer_size
    );
  } else if (_last_raw_len > 0 && (millis() - _last_raw_timestamp) < 1000) {
    // Fallback to global raw radio data (within 1 second of packet)
    len = MQTTMessageBuilder::buildPacketJSONFromRaw(
      _last_raw_data, _last_raw_len, packet, is_tx, _origin, origin_id, 
      _last_snr, _last_rssi, _timezone, json_buffer, buffer_size
    );
  } else {
    // Fallback to reconstructed packet data
    len = MQTTMessageBuilder::buildPacketJSON(
      packet, is_tx, _origin, origin_id, _timezone, json_buffer, buffer_size
    );
  }
  
  if (len > 0) {
    // Publish to custom brokers (only if config is valid)
    if (_config_valid) {
      for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
        if (_brokers[i].enabled && _brokers[i].connected) {
          // Double-check that the client is actually connected before publishing
          // This prevents race conditions where onConnect fires but connection isn't ready yet
          if (!_mqtt_client || !_mqtt_client->connected()) {
            // Connection state is out of sync - mark broker as disconnected
            _brokers[i].connected = false;
            _active_brokers--;
            continue;
          }
          
          char topic[128];
          snprintf(topic, sizeof(topic), "meshcore/%s/%s/packets", _iata, _device_id);
          // MQTT_DEBUG_PRINTLN("Publishing packet to topic: %s", topic);
          
          // Set broker for this connection (PsychicMqttClient uses URI format)
          char broker_uri[128];
          snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%d", _brokers[i].host, _brokers[i].port);
          _mqtt_client->setServer(broker_uri);
          _mqtt_client->publish(topic, 1, false, json_buffer, strlen(json_buffer)); // qos=1, retained=false
        }
      }
    }
    
    // Always publish to Let's Mesh Analyzer servers (independent of custom broker config)
    char analyzer_topic[128];
    snprintf(analyzer_topic, sizeof(analyzer_topic), "meshcore/%s/%s/packets", _iata, _device_id);
    publishToAnalyzerServers(analyzer_topic, json_buffer, false);
  } else {
    // Debug: log when packet message building fails
    uint8_t packet_type = packet->getPayloadType();
    if (packet_type == 4 || packet_type == 9) {  // ADVERT or TRACE
      MQTT_DEBUG_PRINTLN("Failed to build packet JSON for type=%d (len=%d), packet not published", packet_type, len);
    }
  }
}

void MQTTBridge::publishRaw(mesh::Packet* packet) {
  if (!packet) return;
  
  // Large packets need larger buffer for raw JSON too
  char json_buffer[2048];
  char origin_id[65];
  
  // Use actual device ID
  strncpy(origin_id, _device_id, sizeof(origin_id) - 1);
  origin_id[sizeof(origin_id) - 1] = '\0';
  
  // Build raw message
  int len = MQTTMessageBuilder::buildRawJSON(
    packet, _origin, origin_id, _timezone, json_buffer, sizeof(json_buffer)
  );
  
  if (len > 0) {
    // Publish to custom brokers (only if config is valid)
    if (_config_valid) {
      for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
        if (_brokers[i].enabled && _brokers[i].connected) {
          char topic[128];
          snprintf(topic, sizeof(topic), "meshcore/%s/%s/raw", _iata, _device_id);
          
          // Set broker for this connection (PsychicMqttClient uses URI format)
          char broker_uri[128];
          snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%d", _brokers[i].host, _brokers[i].port);
          _mqtt_client->setServer(broker_uri);
          _mqtt_client->publish(topic, 1, false, json_buffer, strlen(json_buffer)); // qos=1, retained=false
        }
      }
    }
    
    // Always publish to Let's Mesh Analyzer servers (independent of custom broker config)
    char analyzer_topic[128];
    snprintf(analyzer_topic, sizeof(analyzer_topic), "meshcore/%s/%s/raw", _iata, _device_id);
    publishToAnalyzerServers(analyzer_topic, json_buffer, false);
  }
}

void MQTTBridge::queuePacket(mesh::Packet* packet, bool is_tx) {
  if (_queue_count >= MAX_QUEUE_SIZE) {
    // Queue full, remove oldest and free its memory
    QueuedPacket& oldest = _packet_queue[_queue_head];
    if (oldest.packet) {
      MQTT_DEBUG_PRINTLN("Queue full, dropping oldest packet (queue size: %d)", _queue_count);
      _mgr->free(oldest.packet);
      oldest.packet = nullptr;
    }
    // dequeuePacket() will clear the structure
    dequeuePacket();
  }
  
  QueuedPacket& queued = _packet_queue[_queue_tail];
  // Clear structure first to ensure clean state (removes any stale data)
  memset(&queued, 0, sizeof(QueuedPacket));
  
  queued.packet = packet;
  queued.timestamp = millis();
  queued.is_tx = is_tx;
  queued.has_raw_data = false; // Default to false, set true if we have valid data
  
  // Capture current raw radio data if available (within 1 second window)
  if (_last_raw_len > 0 && (millis() - _last_raw_timestamp) < 1000) {
    if (_last_raw_len <= sizeof(queued.raw_data)) {
      memcpy(queued.raw_data, _last_raw_data, _last_raw_len);
      queued.raw_len = _last_raw_len;
      queued.snr = _last_snr;
      queued.rssi = _last_rssi;
      queued.has_raw_data = true;
    }
  }
  
  _queue_tail = (_queue_tail + 1) % MAX_QUEUE_SIZE;
  _queue_count++;
}

void MQTTBridge::dequeuePacket() {
  if (_queue_count == 0) return;
  
  // Clear the dequeued packet structure to free memory and prevent stale data
  QueuedPacket& dequeued = _packet_queue[_queue_head];
  memset(&dequeued, 0, sizeof(QueuedPacket));
  dequeued.has_raw_data = false; // Explicitly set after memset
  
  _queue_head = (_queue_head + 1) % MAX_QUEUE_SIZE;
  _queue_count--;
}

bool MQTTBridge::isAnyBrokerConnected() {
  for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
    if (_brokers[i].enabled && _brokers[i].connected) {
      return true;
    }
  }
  return false;
}

void MQTTBridge::setBrokerDefaults() {
  for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
    memset(&_brokers[i], 0, sizeof(MQTTBroker));
    _brokers[i].port = 1883;
    _brokers[i].qos = 0;
    _brokers[i].enabled = false;
    _brokers[i].connected = false;
    _brokers[i].reconnect_interval = 5000; // 5 seconds
  }
}

void MQTTBridge::setBroker(int broker_index, const char* host, uint16_t port, 
                          const char* username, const char* password, bool enabled) {
  if (broker_index < 0 || broker_index >= MAX_MQTT_BROKERS_COUNT) return;
  
  MQTTBroker& broker = _brokers[broker_index];
  strncpy(broker.host, host, sizeof(broker.host) - 1);
  broker.port = port;
  strncpy(broker.username, username, sizeof(broker.username) - 1);
  strncpy(broker.password, password, sizeof(broker.password) - 1);
  broker.enabled = enabled;
  broker.connected = false;
  broker.reconnect_interval = 5000;
}

void MQTTBridge::setOrigin(const char* origin) {
  strncpy(_origin, origin, sizeof(_origin) - 1);
  _origin[sizeof(_origin) - 1] = '\0';
}

void MQTTBridge::setIATA(const char* iata) {
  strncpy(_iata, iata, sizeof(_iata) - 1);
  _iata[sizeof(_iata) - 1] = '\0';
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

void MQTTBridge::storeRawRadioData(const uint8_t* raw_data, int len, float snr, float rssi) {
  if (len > 0 && len <= sizeof(_last_raw_data)) {
    memcpy(_last_raw_data, raw_data, len);
    _last_raw_len = len;
    _last_snr = snr;
    _last_rssi = rssi;
    _last_raw_timestamp = millis();
    MQTT_DEBUG_PRINTLN("Stored raw radio data: %d bytes, SNR=%.1f, RSSI=%.1f", len, snr, rssi);
  }
}

void MQTTBridge::setupAnalyzerServers() {
  // Update analyzer server settings from preferences
  _analyzer_us_enabled = _prefs->mqtt_analyzer_us_enabled;
  _analyzer_eu_enabled = _prefs->mqtt_analyzer_eu_enabled;
  
  MQTT_DEBUG_PRINTLN("Analyzer servers - US: %s, EU: %s", 
                     _analyzer_us_enabled ? "enabled" : "disabled",
                     _analyzer_eu_enabled ? "enabled" : "disabled");
  
  // Create authentication token if any analyzer servers are enabled
  if (_analyzer_us_enabled || _analyzer_eu_enabled) {
    if (createAuthToken()) {
      MQTT_DEBUG_PRINTLN("Created authentication token for analyzer servers");
    } else {
      MQTT_DEBUG_PRINTLN("Failed to create authentication token");
    }
  }
}

bool MQTTBridge::createAuthToken() {
  if (!_identity) {
    MQTT_DEBUG_PRINTLN("No identity available for creating auth token");
    return false;
  }
  
  // Create username in the format: v1_{UPPERCASE_PUBLIC_KEY}
  char public_key_hex[65];
  mesh::Utils::toHex(public_key_hex, _identity->pub_key, PUB_KEY_SIZE);
  
  snprintf(_analyzer_username, sizeof(_analyzer_username), "v1_%s", public_key_hex);
  
  MQTT_DEBUG_PRINTLN("Creating auth token for username: %s", _analyzer_username);
  
  bool us_token_created = false;
  bool eu_token_created = false;
  
  // Get current time for expiration tracking
  unsigned long current_time = time(nullptr);
  unsigned long expires_in = 86400; // 24 hours
  
  // Prepare owner public key (if set) - convert to uppercase hex
  const char* owner_key = nullptr;
  char owner_key_uppercase[65];
  if (_prefs->mqtt_owner_public_key[0] != '\0') {
    // Copy and convert to uppercase
    strncpy(owner_key_uppercase, _prefs->mqtt_owner_public_key, sizeof(owner_key_uppercase) - 1);
    owner_key_uppercase[sizeof(owner_key_uppercase) - 1] = '\0';
    for (int i = 0; owner_key_uppercase[i]; i++) {
      owner_key_uppercase[i] = toupper(owner_key_uppercase[i]);
    }
    owner_key = owner_key_uppercase;
    MQTT_DEBUG_PRINTLN("Using owner public key: %s", owner_key);
  }
  
  // Build client version string (same format as used in status messages)
  char client_version[64];
  snprintf(client_version, sizeof(client_version), "meshcoretomqtt/%s", _build_date);
  
  // Create JWT token for US server
  if (_analyzer_us_enabled) {
    MQTT_DEBUG_PRINTLN("Creating JWT token for US server...");
    if (JWTHelper::createAuthToken(
        *_identity, "mqtt-us-v1.letsmesh.net", 
        0, expires_in, _auth_token_us, sizeof(_auth_token_us),
        owner_key, client_version)) {
      MQTT_DEBUG_PRINTLN("Created auth token for US server");
      us_token_created = true;
    } else {
      MQTT_DEBUG_PRINTLN("Failed to create auth token for US server");
    }
  }
  
  // Create JWT token for EU server
  if (_analyzer_eu_enabled) {
    MQTT_DEBUG_PRINTLN("Creating JWT token for EU server...");
    if (JWTHelper::createAuthToken(
        *_identity, "mqtt-eu-v1.letsmesh.net", 
        0, expires_in, _auth_token_eu, sizeof(_auth_token_eu),
        owner_key, client_version)) {
      MQTT_DEBUG_PRINTLN("Created auth token for EU server");
      eu_token_created = true;
    } else {
      MQTT_DEBUG_PRINTLN("Failed to create auth token for EU server");
    }
  }
  
  return us_token_created || eu_token_created;
}

void MQTTBridge::publishToAnalyzerServers(const char* topic, const char* payload, bool retained) {
  if (!_analyzer_us_enabled && !_analyzer_eu_enabled) {
    MQTT_DEBUG_PRINTLN("No analyzer servers enabled, skipping publish to topic: %s", topic);
    return;
  }
  
  MQTT_DEBUG_PRINTLN("Publishing to analyzer servers via WebSocket MQTT");
  MQTT_DEBUG_PRINTLN("Topic: %s", topic);
  MQTT_DEBUG_PRINTLN("Payload length: %d", strlen(payload));
  MQTT_DEBUG_PRINTLN("US enabled: %s, EU enabled: %s", _analyzer_us_enabled ? "true" : "false", _analyzer_eu_enabled ? "true" : "false");
  
  // Publish to US server if enabled
  if (_analyzer_us_enabled && _analyzer_us_client) {
    MQTT_DEBUG_PRINTLN("Publishing to US analyzer server");
    publishToAnalyzerClient(_analyzer_us_client, topic, payload, retained);
  } else {
    MQTT_DEBUG_PRINTLN("US analyzer server not available (enabled: %s, client: %s)", 
                      _analyzer_us_enabled ? "true" : "false", _analyzer_us_client ? "exists" : "null");
  }
  
  // Publish to EU server if enabled
  if (_analyzer_eu_enabled && _analyzer_eu_client) {
    MQTT_DEBUG_PRINTLN("Publishing to EU analyzer server");
    publishToAnalyzerClient(_analyzer_eu_client, topic, payload, retained);
  } else {
    MQTT_DEBUG_PRINTLN("EU analyzer server not available (enabled: %s, client: %s)", 
                      _analyzer_eu_enabled ? "true" : "false", _analyzer_eu_client ? "exists" : "null");
  }
}

// Google Trust Services - GTS Root R4
const char* GTS_ROOT_R4 = 
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDejCCAmKgAwIBAgIQf+UwvzMTQ77dghYQST2KGzANBgkqhkiG9w0BAQsFADBX\n"
    "MQswCQYDVQQGEwJCRTEZMBcGA1UEChMQR2xvYmFsU2lnbiBudi1zYTEQMA4GA1UE\n"
    "CxMHUm9vdCBDQTEbMBkGA1UEAxMSR2xvYmFsU2lnbiBSb290IENBMB4XDTIzMTEx\n"
    "NTAzNDMyMVoXDTI4MDEyODAwMDA0MlowRzELMAkGA1UEBhMCVVMxIjAgBgNVBAoT\n"
    "GUdvb2dsZSBUcnVzdCBTZXJ2aWNlcyBMTEMxFDASBgNVBAMTC0dUUyBSb290IFI0\n"
    "MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAE83Rzp2iLYK5DuDXFgTB7S0md+8Fhzube\n"
    "Rr1r1WEYNa5A3XP3iZEwWus87oV8okB2O6nGuEfYKueSkWpz6bFyOZ8pn6KY019e\n"
    "WIZlD6GEZQbR3IvJx3PIjGov5cSr0R2Ko4H/MIH8MA4GA1UdDwEB/wQEAwIBhjAd\n"
    "BgNVHSUEFjAUBggrBgEFBQcDAQYIKwYBBQUHAwIwDwYDVR0TAQH/BAUwAwEB/zAd\n"
    "BgNVHQ4EFgQUgEzW63T/STaj1dj8tT7FavCUHYwwHwYDVR0jBBgwFoAUYHtmGkUN\n"
    "l8qJUC99BM00qP/8/UswNgYIKwYBBQUHAQEEKjAoMCYGCCsGAQUFBzAChhpodHRw\n"
    "Oi8vaS5wa2kuZ29vZy9nc3IxLmNydDAtBgNVHR8EJjAkMCKgIKAehhxodHRwOi8v\n"
    "Yy5wa2kuZ29vZy9yL2dzcjEuY3JsMBMGA1UdIAQMMAowCAYGZ4EMAQIBMA0GCSqG\n"
    "SIb3DQEBCwUAA4IBAQAYQrsPBtYDh5bjP2OBDwmkoWhIDDkic574y04tfzHpn+cJ\n"
    "odI2D4SseesQ6bDrarZ7C30ddLibZatoKiws3UL9xnELz4ct92vID24FfVbiI1hY\n"
    "+SW6FoVHkNeWIP0GCbaM4C6uVdF5dTUsMVs/ZbzNnIdCp5Gxmx5ejvEau8otR/Cs\n"
    "kGN+hr/W5GvT1tMBjgWKZ1i4//emhA1JG1BbPzoLJQvyEotc03lXjTaCzv8mEbep\n"
    "8RqZ7a2CPsgRbuvTPBwcOMBBmuFeU88+FSBX6+7iP0il8b4Z0QFqIwwMHfs/L6K1\n"
    "vepuoxtGzi4CZ68zJpiq1UvSqTbFJjtbD4seiMHl\n"
    "-----END CERTIFICATE-----\n";

void MQTTBridge::setupAnalyzerClients() {
  if (!_analyzer_us_enabled && !_analyzer_eu_enabled) {
    MQTT_DEBUG_PRINTLN("No analyzer servers enabled, skipping PsychicMqttClient setup");
    return;
  }

  MQTT_DEBUG_PRINTLN("Setting up PsychicMqttClient WebSocket clients...");

  // Setup US server client
  if (_analyzer_us_enabled) {
    _analyzer_us_client = new PsychicMqttClient();

    // Set up event callbacks for US server
    _analyzer_us_client->onConnect([this](bool sessionPresent) {
      MQTT_DEBUG_PRINTLN("Connected to Let's Mesh US server, session present: %s", sessionPresent ? "true" : "false");
      // Publish status message when connected
      publishStatusToAnalyzerClient(_analyzer_us_client, "mqtt-us-v1.letsmesh.net");
    });

    _analyzer_us_client->onDisconnect([this](bool sessionPresent) {
      MQTT_DEBUG_PRINTLN("Disconnected from Let's Mesh US server, session present: %s", sessionPresent ? "true" : "false");
    });

            _analyzer_us_client->onError([this](esp_mqtt_error_codes error) {
              MQTT_DEBUG_PRINTLN("Let's Mesh US server error - error_type: %d, connect_return_code: %d", 
                                error.error_type, error.connect_return_code);
            });

    // Set up WebSocket MQTT over TLS connection to US server
    _analyzer_us_client->setServer("wss://mqtt-us-v1.letsmesh.net:443/mqtt");
    MQTT_DEBUG_PRINTLN("US Server - Username: %s", _analyzer_username);
    MQTT_DEBUG_PRINTLN("US Server - Auth token length: %d", strlen(_auth_token_us));
    MQTT_DEBUG_PRINTLN("US Server - Auth token (first 50 chars): %.50s...", _auth_token_us);
    _analyzer_us_client->setCredentials(_analyzer_username, _auth_token_us);

    // Configure TLS - use specific GTS Root R4 certificate
    MQTT_DEBUG_PRINTLN("Using GTS Root R4 certificate for US server");
    _analyzer_us_client->setCACert(GTS_ROOT_R4);

    // Connect to US server (async connection)
    _analyzer_us_client->connect();
    MQTT_DEBUG_PRINTLN("Initiating connection to Let's Mesh US server");
  }

  // Setup EU server client
  if (_analyzer_eu_enabled) {
    _analyzer_eu_client = new PsychicMqttClient();

    // Set up event callbacks for EU server
    _analyzer_eu_client->onConnect([this](bool sessionPresent) {
      MQTT_DEBUG_PRINTLN("Connected to Let's Mesh EU server, session present: %s", sessionPresent ? "true" : "false");
      // Publish status message when connected
      publishStatusToAnalyzerClient(_analyzer_eu_client, "mqtt-eu-v1.letsmesh.net");
    });

    _analyzer_eu_client->onDisconnect([this](bool sessionPresent) {
      MQTT_DEBUG_PRINTLN("Disconnected from Let's Mesh EU server, session present: %s", sessionPresent ? "true" : "false");
    });

            _analyzer_eu_client->onError([this](esp_mqtt_error_codes error) {
              MQTT_DEBUG_PRINTLN("Let's Mesh EU server error - error_type: %d, connect_return_code: %d", 
                                error.error_type, error.connect_return_code);
            });

    // Set up WebSocket MQTT over TLS connection to EU server
    _analyzer_eu_client->setServer("wss://mqtt-eu-v1.letsmesh.net:443/mqtt");
    MQTT_DEBUG_PRINTLN("EU Server - Username: %s", _analyzer_username);
    MQTT_DEBUG_PRINTLN("EU Server - Auth token length: %d", strlen(_auth_token_eu));
    MQTT_DEBUG_PRINTLN("EU Server - Auth token (first 50 chars): %.50s...", _auth_token_eu);
    _analyzer_eu_client->setCredentials(_analyzer_username, _auth_token_eu);

    // Configure TLS - use specific GTS Root R4 certificate
    MQTT_DEBUG_PRINTLN("Using GTS Root R4 certificate for EU server");
    _analyzer_eu_client->setCACert(GTS_ROOT_R4);

    // Connect to EU server (async connection)
    _analyzer_eu_client->connect();
    MQTT_DEBUG_PRINTLN("Initiating connection to Let's Mesh EU server");
  }
}

void MQTTBridge::publishToAnalyzerClient(PsychicMqttClient* client, const char* topic, const char* payload, bool retained) {
  if (!client) {
    return; // Don't log null client - this is expected if analyzer is disabled
  }
  
  if (!client->connected()) {
    // Throttle log spam - only log periodically for each analyzer server
    unsigned long now = millis();
    bool should_log = false;
    
    if (client == _analyzer_us_client && (now - _last_analyzer_us_log > ANALYZER_LOG_INTERVAL)) {
      should_log = true;
      _last_analyzer_us_log = now;
    } else if (client == _analyzer_eu_client && (now - _last_analyzer_eu_log > ANALYZER_LOG_INTERVAL)) {
      should_log = true;
      _last_analyzer_eu_log = now;
    }
    
    if (should_log) {
      MQTT_DEBUG_PRINTLN("PsychicMqttClient not connected - skipping publish to topic: %s", topic);
    }
    return;
  }
  
  // Reset log timer when connected
  if (client == _analyzer_us_client) {
    _last_analyzer_us_log = 0;
  } else if (client == _analyzer_eu_client) {
    _last_analyzer_eu_log = 0;
  }
  
  MQTT_DEBUG_PRINTLN("Publishing to analyzer client - topic: %s, payload length: %d, retained: %s", 
                    topic, strlen(payload), retained ? "true" : "false");
  
  // Publish message using PsychicMqttClient API
  int result = client->publish(topic, 1, retained, payload, strlen(payload));
  if (result > 0) {
    MQTT_DEBUG_PRINTLN("PsychicMqttClient message published successfully, result=%d", result);
  } else {
    MQTT_DEBUG_PRINTLN("PsychicMqttClient publish failed, result=%d", result);
  }
}

void MQTTBridge::publishStatusToAnalyzerClient(PsychicMqttClient* client, const char* server_name) {
  if (!client || !client->connected()) {
    return;
  }
  
  // Create status message
  char status_topic[128];
  snprintf(status_topic, sizeof(status_topic), "meshcore/%s/%s/status", _iata, _device_id);
  
  // Build proper status message using MQTTMessageBuilder
  // Status messages with stats can be larger (~400-500 bytes)
  char json_buffer[768];  // Increased from 512 to accommodate stats object
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
  
  // Build radio info string (freq,bw,sf,cr)
  snprintf(radio_info, sizeof(radio_info), "%.6f,%.1f,%d,%d", 
           _prefs->freq, _prefs->bw, _prefs->sf, _prefs->cr);
  
  // Use actual device ID
  strncpy(origin_id, _device_id, sizeof(origin_id) - 1);
  origin_id[sizeof(origin_id) - 1] = '\0';
  
  // Build client version string
  char client_version[64];
  snprintf(client_version, sizeof(client_version), "meshcore-custom-repeater/%s", _build_date);
  
  // Collect stats on-demand if sources are available
  int battery_mv = -1;
  int uptime_secs = -1;
  int errors = -1;
  int noise_floor = -999;
  int tx_air_secs = -1;
  int rx_air_secs = -1;
  
  if (_board) {
    battery_mv = _board->getBattMilliVolts();
  }
  if (_ms) {
    uptime_secs = _ms->getMillis() / 1000;
  }
  if (_dispatcher) {
    errors = _dispatcher->getErrFlags();
    tx_air_secs = _dispatcher->getTotalAirTime() / 1000;
    rx_air_secs = _dispatcher->getReceiveAirTime() / 1000;
  }
  if (_radio) {
    noise_floor = (int16_t)_radio->getNoiseFloor();
  }
  
  // Build status message using MQTTMessageBuilder with stats
  int len = MQTTMessageBuilder::buildStatusMessage(
    _origin,
    origin_id,
    _board_model,  // model
    _firmware_version,  // firmware version
    radio_info,
    client_version,  // client version
    "online",
    timestamp,
    json_buffer,
    sizeof(json_buffer),
    battery_mv,
    uptime_secs,
    errors,
    _queue_count,  // Use current queue length
    noise_floor,
    tx_air_secs,
    rx_air_secs
  );
  
  if (len > 0) {
    MQTT_DEBUG_PRINTLN("Publishing status to %s server", server_name);
    MQTT_DEBUG_PRINTLN("Status topic: %s", status_topic);
    MQTT_DEBUG_PRINTLN("Status payload: %s", json_buffer);
    
    // Publish status message (retained)
    int result = client->publish(status_topic, 1, true, json_buffer, strlen(json_buffer));
    if (result > 0) {
      MQTT_DEBUG_PRINTLN("Status published to %s server successfully, result=%d", server_name, result);
    } else {
      MQTT_DEBUG_PRINTLN("Status publish to %s server failed, result=%d", server_name, result);
    }
  }
}

void MQTTBridge::maintainAnalyzerConnections() {
<<<<<<< HEAD
  // PsychicMqttClient handles connection maintenance and reconnection automatically
  // No manual maintenance needed - the library manages this internally
  // Connection state changes are handled via the onConnect/onDisconnect callbacks
=======
  if (!_identity) {
    return;
  }
  
  unsigned long current_time = time(nullptr);
  // If time is not synced (time() returns 0 or very small value), skip expiration checks
  // Tokens will still work but we can't track expiration properly
  // If expiration time was set before time sync, it will be a small value, so we'll renew
  bool time_synced = (current_time >= 1000000000); // After year 2001
  
  const unsigned long RENEWAL_BUFFER = 3600; // Renew tokens 1 hour before expiration
  const unsigned long RENEWAL_THROTTLE_MS = 60000; // Don't attempt renewal more than once per minute
  
  unsigned long now_millis = millis();
  
  // Check and renew US server token if needed
  if (_analyzer_us_enabled && _analyzer_us_client) {
    // Check if token is expired or will expire soon
    // If time wasn't synced when token was created, expiration time will be invalid, so renew
    bool token_needs_renewal = (_token_us_expires_at == 0) || 
                               !time_synced ||
                               !(_token_us_expires_at >= 1000000000) || // Expiration time invalid
                               (time_synced && current_time >= _token_us_expires_at) ||
                               (time_synced && current_time >= (_token_us_expires_at - RENEWAL_BUFFER));
    
    // Throttle renewal attempts - don't try more than once per minute to avoid blocking
    bool can_attempt_renewal = (now_millis - _last_token_renewal_attempt_us) >= RENEWAL_THROTTLE_MS;
    
    // Check if client is disconnected and needs reconnection with new token
    bool needs_reconnect = !_analyzer_us_client->connected();
    
    if (token_needs_renewal && can_attempt_renewal) {
      _last_token_renewal_attempt_us = now_millis;
      MQTT_DEBUG_PRINTLN("US token expired or expiring soon (expires_at: %lu, current: %lu), renewing...", 
                         _token_us_expires_at, current_time);
      
      // Prepare owner public key (if set) - convert to uppercase hex
      const char* owner_key = nullptr;
      char owner_key_uppercase[65];
      if (_prefs->mqtt_owner_public_key[0] != '\0') {
        // Copy and convert to uppercase
        strncpy(owner_key_uppercase, _prefs->mqtt_owner_public_key, sizeof(owner_key_uppercase) - 1);
        owner_key_uppercase[sizeof(owner_key_uppercase) - 1] = '\0';
        for (int i = 0; owner_key_uppercase[i]; i++) {
          owner_key_uppercase[i] = toupper(owner_key_uppercase[i]);
        }
        owner_key = owner_key_uppercase;
      }
      
      // Build client version string (same format as used in status messages)
      char client_version[64];
      snprintf(client_version, sizeof(client_version), "meshcoretomqtt/%s", _build_date);
      
      // Renew the token
      if (JWTHelper::createAuthToken(
          *_identity, "mqtt-us-v1.letsmesh.net", 
          0, 86400, _auth_token_us, sizeof(_auth_token_us),
          owner_key, client_version)) {
        unsigned long expires_in = 86400; // 24 hours
        _token_us_expires_at = current_time + expires_in;
        MQTT_DEBUG_PRINTLN("US token renewed, new expiration: %lu", _token_us_expires_at);
        
        // Update client credentials with new token
        _analyzer_us_client->setCredentials(_analyzer_username, _auth_token_us);
        
        // Reconnect to apply new token (whether currently connected or not)
        // If connected, disconnect first to ensure new token is used
        if (_analyzer_us_client->connected()) {
          MQTT_DEBUG_PRINTLN("Disconnecting US server to apply new token...");
          _analyzer_us_client->disconnect();
        }
        MQTT_DEBUG_PRINTLN("Reconnecting to US server with renewed token...");
        _last_reconnect_attempt_us = now_millis; // Update reconnect timestamp to throttle subsequent attempts
        _analyzer_us_client->connect();
      } else {
        MQTT_DEBUG_PRINTLN("Failed to renew US token");
        _token_us_expires_at = 0;
      }
    } else if (needs_reconnect) {
      // Token is still valid but connection is lost - reconnect with existing token
      // Throttle reconnection attempts to avoid spamming
      unsigned long reconnect_elapsed = (now_millis >= _last_reconnect_attempt_us) ?
                                      (now_millis - _last_reconnect_attempt_us) :
                                      (ULONG_MAX - _last_reconnect_attempt_us + now_millis + 1);
      if (reconnect_elapsed >= RECONNECT_THROTTLE_MS) {
        _last_reconnect_attempt_us = now_millis;
        MQTT_DEBUG_PRINTLN("US server disconnected but token still valid, reconnecting...");
        _analyzer_us_client->connect();
      } else {
        // Throttled - only log periodically to avoid spam (every 5 minutes max)
        static unsigned long last_throttle_log_us = 0;
        if (now_millis - last_throttle_log_us > 300000) {
          MQTT_DEBUG_PRINTLN("US server reconnection throttled (last attempt %lu ms ago, need %lu ms)", 
                            reconnect_elapsed, RECONNECT_THROTTLE_MS);
          last_throttle_log_us = now_millis;
        }
      }
    }
  }
  
  // Check and renew EU server token if needed
  if (_analyzer_eu_enabled && _analyzer_eu_client) {
    // Check if token is expired or will expire soon
    // If time wasn't synced when token was created, expiration time will be invalid, so renew
    bool token_needs_renewal = (_token_eu_expires_at == 0) || 
                               !time_synced ||
                               !(_token_eu_expires_at >= 1000000000) || // Expiration time invalid
                               (time_synced && current_time >= _token_eu_expires_at) ||
                               (time_synced && current_time >= (_token_eu_expires_at - RENEWAL_BUFFER));
    
    // Throttle renewal attempts - don't try more than once per minute to avoid blocking
    bool can_attempt_renewal = (now_millis - _last_token_renewal_attempt_eu) >= RENEWAL_THROTTLE_MS;
    
    // Check if client is disconnected and needs reconnection with new token
    bool needs_reconnect = !_analyzer_eu_client->connected();
    
    if (token_needs_renewal && can_attempt_renewal) {
      _last_token_renewal_attempt_eu = now_millis;
      MQTT_DEBUG_PRINTLN("EU token expired or expiring soon (expires_at: %lu, current: %lu), renewing...", 
                         _token_eu_expires_at, current_time);
      
      // Prepare owner public key (if set) - convert to uppercase hex
      const char* owner_key = nullptr;
      char owner_key_uppercase[65];
      if (_prefs->mqtt_owner_public_key[0] != '\0') {
        // Copy and convert to uppercase
        strncpy(owner_key_uppercase, _prefs->mqtt_owner_public_key, sizeof(owner_key_uppercase) - 1);
        owner_key_uppercase[sizeof(owner_key_uppercase) - 1] = '\0';
        for (int i = 0; owner_key_uppercase[i]; i++) {
          owner_key_uppercase[i] = toupper(owner_key_uppercase[i]);
        }
        owner_key = owner_key_uppercase;
      }
      
      // Build client version string (same format as used in status messages)
      char client_version[64];
      snprintf(client_version, sizeof(client_version), "meshcoretomqtt/%s", _build_date);
      
      // Renew the token
      if (JWTHelper::createAuthToken(
          *_identity, "mqtt-eu-v1.letsmesh.net", 
          0, 86400, _auth_token_eu, sizeof(_auth_token_eu),
          owner_key, client_version)) {
        unsigned long expires_in = 86400; // 24 hours
        _token_eu_expires_at = current_time + expires_in;
        MQTT_DEBUG_PRINTLN("EU token renewed, new expiration: %lu", _token_eu_expires_at);
        
        // Update client credentials with new token
        _analyzer_eu_client->setCredentials(_analyzer_username, _auth_token_eu);
        
        // Reconnect to apply new token (whether currently connected or not)
        // If connected, disconnect first to ensure new token is used
        if (_analyzer_eu_client->connected()) {
          MQTT_DEBUG_PRINTLN("Disconnecting EU server to apply new token...");
          _analyzer_eu_client->disconnect();
        }
        MQTT_DEBUG_PRINTLN("Reconnecting to EU server with renewed token...");
        _last_reconnect_attempt_eu = now_millis; // Update reconnect timestamp to throttle subsequent attempts
        _analyzer_eu_client->connect();
      } else {
        MQTT_DEBUG_PRINTLN("Failed to renew EU token");
        _token_eu_expires_at = 0;
      }
    } else if (needs_reconnect) {
      // Token is still valid but connection is lost - reconnect with existing token
      // Throttle reconnection attempts to avoid spamming
      unsigned long reconnect_elapsed = (now_millis >= _last_reconnect_attempt_eu) ?
                                      (now_millis - _last_reconnect_attempt_eu) :
                                      (ULONG_MAX - _last_reconnect_attempt_eu + now_millis + 1);
      if (reconnect_elapsed >= RECONNECT_THROTTLE_MS) {
        _last_reconnect_attempt_eu = now_millis;
        MQTT_DEBUG_PRINTLN("EU server disconnected but token still valid, reconnecting...");
        _analyzer_eu_client->connect();
      } else {
        // Throttled - only log periodically to avoid spam (every 5 minutes max)
        static unsigned long last_throttle_log_eu = 0;
        if (now_millis - last_throttle_log_eu > 300000) {
          MQTT_DEBUG_PRINTLN("EU server reconnection throttled (last attempt %lu ms ago, need %lu ms)", 
                            reconnect_elapsed, RECONNECT_THROTTLE_MS);
          last_throttle_log_eu = now_millis;
        }
      }
    }
  }
  
  // Note: PsychicMqttClient handles automatic reconnection internally,
  // but we need to ensure tokens are renewed before reconnection attempts
>>>>>>> 1ebe417c (Enhance CommonCLI and JWTHelper for MQTT owner public key support)
}

void MQTTBridge::setMessageTypes(bool status, bool packets, bool raw) {
  _status_enabled = status;
  _packets_enabled = packets;
  _raw_enabled = raw;
}

int MQTTBridge::getConnectedBrokers() const {
  int count = 0;
  for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
    if (_brokers[i].enabled && _brokers[i].connected) {
      count++;
    }
  }
  return count;
}

int MQTTBridge::getQueueSize() const {
  return _queue_count;
}

void MQTTBridge::setStatsSources(mesh::Dispatcher* dispatcher, mesh::Radio* radio, 
                                  mesh::MainBoard* board, mesh::MillisecondClock* ms) {
  _dispatcher = dispatcher;
  _radio = radio;
  _board = board;
  _ms = ms;
}

void MQTTBridge::syncTimeWithNTP() {
  if (!WiFi.isConnected()) {
    MQTT_DEBUG_PRINTLN("Cannot sync time - WiFi not connected");
    return;
  }
  
  MQTT_DEBUG_PRINTLN("Syncing time with NTP...");
  
  // Begin NTP client
  _ntp_client.begin();
  
  // Force update (blocking call with timeout)
  if (_ntp_client.forceUpdate()) {
    unsigned long epochTime = _ntp_client.getEpochTime();
    
    // Set system timezone to UTC first
    // This ensures time() returns UTC time
    configTime(0, 0, "pool.ntp.org");
    
    // Update the device's RTC clock with UTC time
    if (_rtc) {
      _rtc->setCurrentTime(epochTime);
      _ntp_synced = true;
      _last_ntp_sync = millis();
      
      MQTT_DEBUG_PRINTLN("Time synced: %lu", epochTime);
      
      // Set timezone from string (with DST support) - only if changed
      static char last_timezone[64] = "";
      if (strcmp(_prefs->timezone_string, last_timezone) != 0) {
        MQTT_DEBUG_PRINTLN("Setting timezone: %s", _prefs->timezone_string);
        
        // Clean up old timezone object to prevent memory leak
        if (_timezone) {
          delete _timezone;
          _timezone = nullptr;
        }
        
        // Create timezone object based on timezone string
        Timezone* tz = createTimezoneFromString(_prefs->timezone_string);
        if (tz) {
          MQTT_DEBUG_PRINTLN("Timezone created successfully");
          // Store timezone for later use in message building
          _timezone = tz;
        } else {
          MQTT_DEBUG_PRINTLN("Failed to create timezone, using UTC");
          // Create UTC timezone as fallback
          TimeChangeRule utc = {"UTC", Last, Sun, Mar, 0, 0};
          _timezone = new Timezone(utc, utc);
        }
        
        // Remember this timezone string
        strncpy(last_timezone, _prefs->timezone_string, sizeof(last_timezone) - 1);
        last_timezone[sizeof(last_timezone) - 1] = '\0';
        
        // Force memory defragmentation after timezone recreation
        MQTT_DEBUG_PRINTLN("Forcing memory defragmentation after timezone change");
        void* temp = malloc(1024);
        if (temp) {
          free(temp);
          MQTT_DEBUG_PRINTLN("Defragmentation complete. Max Alloc: %d", ESP.getMaxAllocHeap());
        }
      }
      
      // Show current time in both UTC and local
      struct tm* utc_timeinfo = gmtime((time_t*)&epochTime);
      struct tm* local_timeinfo = localtime((time_t*)&epochTime);
      
      if (utc_timeinfo) {
        MQTT_DEBUG_PRINTLN("UTC time: %04d-%02d-%02d %02d:%02d:%02d", 
                          utc_timeinfo->tm_year + 1900, utc_timeinfo->tm_mon + 1, utc_timeinfo->tm_mday,
                          utc_timeinfo->tm_hour, utc_timeinfo->tm_min, utc_timeinfo->tm_sec);
      }
      
      if (local_timeinfo) {
        MQTT_DEBUG_PRINTLN("Local time: %04d-%02d-%02d %02d:%02d:%02d", 
                          local_timeinfo->tm_year + 1900, local_timeinfo->tm_mon + 1, local_timeinfo->tm_mday,
                          local_timeinfo->tm_hour, local_timeinfo->tm_min, local_timeinfo->tm_sec);
      }
    } else {
      MQTT_DEBUG_PRINTLN("No RTC clock available for time sync");
    }
  } else {
    MQTT_DEBUG_PRINTLN("NTP sync failed");
  }
  
  _ntp_client.end();
}

Timezone* MQTTBridge::createTimezoneFromString(const char* tz_string) {
  // Create Timezone objects for common IANA timezone strings
  // Using TimeChangeRule definitions for proper DST handling
  
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
    // Pacific Time (PST/PDT)
    TimeChangeRule pst = {"PST", First, Sun, Nov, 2, -480};  // UTC-8
    TimeChangeRule pdt = {"PDT", Second, Sun, Mar, 2, -420}; // UTC-7
    return new Timezone(pdt, pst);
  } else if (strcmp(tz_string, "MDT") == 0 || strcmp(tz_string, "MST") == 0) {
    // Mountain Time (MST/MDT)
    TimeChangeRule mst = {"MST", First, Sun, Nov, 2, -420};  // UTC-7
    TimeChangeRule mdt = {"MDT", Second, Sun, Mar, 2, -360};  // UTC-6
    return new Timezone(mdt, mst);
  } else if (strcmp(tz_string, "CDT") == 0 || strcmp(tz_string, "CST") == 0) {
    // Central Time (CST/CDT)
    TimeChangeRule cst = {"CST", First, Sun, Nov, 2, -360};  // UTC-6
    TimeChangeRule cdt = {"CDT", Second, Sun, Mar, 2, -300}; // UTC-5
    return new Timezone(cdt, cst);
  } else if (strcmp(tz_string, "EDT") == 0 || strcmp(tz_string, "EST") == 0) {
    // Eastern Time (EST/EDT)
    TimeChangeRule est = {"EST", First, Sun, Nov, 2, -300};   // UTC-5
    TimeChangeRule edt = {"EDT", Second, Sun, Mar, 2, -240}; // UTC-4
    return new Timezone(edt, est);
  } else if (strcmp(tz_string, "BST") == 0 || strcmp(tz_string, "GMT") == 0) {
    // British Time (GMT/BST)
    TimeChangeRule gmt = {"GMT", Last, Sun, Oct, 2, 0};     // UTC+0
    TimeChangeRule bst = {"BST", Last, Sun, Mar, 1, 60};    // UTC+1
    return new Timezone(bst, gmt);
  } else if (strcmp(tz_string, "CEST") == 0 || strcmp(tz_string, "CET") == 0) {
    // Central European Time (CET/CEST)
    TimeChangeRule cet = {"CET", Last, Sun, Oct, 3, 60};    // UTC+1
    TimeChangeRule cest = {"CEST", Last, Sun, Mar, 2, 120}; // UTC+2
    return new Timezone(cest, cet);
  
  // UTC and simple offsets
  } else if (strcmp(tz_string, "UTC") == 0) {
    TimeChangeRule utc = {"UTC", Last, Sun, Mar, 0, 0};
    return new Timezone(utc, utc);
  } else if (strncmp(tz_string, "UTC", 3) == 0) {
    // Handle UTC+/-X format (UTC-8, UTC+5, etc.)
    int offset = atoi(tz_string + 3);
    TimeChangeRule utc_offset = {"UTC", Last, Sun, Mar, 0, offset * 60};
    return new Timezone(utc_offset, utc_offset);
  } else if (strncmp(tz_string, "GMT", 3) == 0) {
    // Handle GMT+/-X format (GMT-8, GMT+5, etc.)
    int offset = atoi(tz_string + 3);
    TimeChangeRule gmt_offset = {"GMT", Last, Sun, Mar, 0, offset * 60};
    return new Timezone(gmt_offset, gmt_offset);
  } else if (strncmp(tz_string, "+", 1) == 0 || strncmp(tz_string, "-", 1) == 0) {
    // Handle simple +/-X format (+5, -8, etc.)
    int offset = atoi(tz_string);
    TimeChangeRule offset_tz = {"TZ", Last, Sun, Mar, 0, offset * 60};
    return new Timezone(offset_tz, offset_tz);
  } else {
    // Unknown timezone, return null
    MQTT_DEBUG_PRINTLN("Unknown timezone: %s", tz_string);
    return nullptr;
  }
}

void MQTTBridge::logMemoryStatus() {
  MQTT_DEBUG_PRINTLN("=== Memory Status ===");
  MQTT_DEBUG_PRINTLN("Free heap: %d bytes", ESP.getFreeHeap());
  MQTT_DEBUG_PRINTLN("Min free heap: %d bytes", ESP.getMinFreeHeap());
  MQTT_DEBUG_PRINTLN("Max alloc heap: %d bytes", ESP.getMaxAllocHeap());
  MQTT_DEBUG_PRINTLN("Heap size: %d bytes", ESP.getHeapSize());
  MQTT_DEBUG_PRINTLN("Free PSRAM: %d bytes", ESP.getFreePsram());
  MQTT_DEBUG_PRINTLN("Queue size: %d/%d packets", _queue_count, MAX_QUEUE_SIZE);
  MQTT_DEBUG_PRINTLN("===================");
}

#endif
