#include "MQTTBridge.h"
#include "../MQTTMessageBuilder.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Timezone.h>

#ifdef WITH_MQTT_BRIDGE

MQTTBridge::MQTTBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc)
    : BridgeBase(prefs, mgr, rtc), _mqtt_client(nullptr), _wifi_client(nullptr),
      _active_brokers(0), _queue_head(0), _queue_tail(0), _queue_count(0),
      _last_status_publish(0), _status_interval(300000), // 5 minutes default
      _ntp_client(_ntp_udp, "pool.ntp.org", 0, 60000), _last_ntp_sync(0), _ntp_synced(false),
      _timezone(nullptr), _last_raw_len(0), _last_snr(0), _last_rssi(0), _last_raw_timestamp(0) {
  
  // Initialize default values
  strncpy(_origin, "MeshCore-Repeater", sizeof(_origin) - 1);
  strncpy(_iata, "XXX", sizeof(_iata) - 1);
  strncpy(_device_id, "DEVICE_ID_PLACEHOLDER", sizeof(_device_id) - 1);
  strncpy(_firmware_version, "unknown", sizeof(_firmware_version) - 1);
  strncpy(_board_model, "unknown", sizeof(_board_model) - 1);
  _status_enabled = true;
  _packets_enabled = true;
  _raw_enabled = false;
  _tx_enabled = false;  // Disable TX packets by default
  
  // Initialize packet queue
  memset(_packet_queue, 0, sizeof(_packet_queue));
  
  // Set default broker configuration
  setBrokerDefaults();
}

void MQTTBridge::begin() {
  MQTT_DEBUG_PRINTLN("Initializing MQTT Bridge...");
  
  // Update origin and IATA from preferences
  strncpy(_origin, _prefs->mqtt_origin, sizeof(_origin) - 1);
  _origin[sizeof(_origin) - 1] = '\0';
  strncpy(_iata, _prefs->mqtt_iata, sizeof(_iata) - 1);
  _iata[sizeof(_iata) - 1] = '\0';
  
  // Strip quotes from origin if present
  MQTT_DEBUG_PRINTLN("Origin before stripping: '%s'", _origin);
  size_t origin_len = strlen(_origin);
  if (origin_len > 0) {
    if (_origin[0] == '"') {
      memmove(_origin, _origin + 1, origin_len);
      origin_len--;
    }
    if (origin_len > 0 && _origin[origin_len-1] == '"') {
      _origin[origin_len-1] = '\0';
    }
  }
  MQTT_DEBUG_PRINTLN("Origin after stripping: '%s'", _origin);
  
  // Strip quotes from IATA if present
  MQTT_DEBUG_PRINTLN("IATA before stripping: '%s'", _iata);
  size_t iata_len = strlen(_iata);
  if (iata_len > 0) {
    if (_iata[0] == '"') {
      memmove(_iata, _iata + 1, iata_len);
      iata_len--;
    }
    if (iata_len > 0 && _iata[iata_len-1] == '"') {
      _iata[iata_len-1] = '\0';
    }
  }
  MQTT_DEBUG_PRINTLN("IATA after stripping: '%s'", _iata);
  
  // Update enabled flags from preferences
  _status_enabled = _prefs->mqtt_status_enabled;
  _packets_enabled = _prefs->mqtt_packets_enabled;
  _raw_enabled = _prefs->mqtt_raw_enabled;
  _tx_enabled = _prefs->mqtt_tx_enabled;
  _status_interval = _prefs->mqtt_status_interval;
  
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
  
  // Initialize WiFi client
  _wifi_client = new WiFiClient();
  _mqtt_client = new PubSubClient(*_wifi_client);
  
  // Set default broker (meshtastic.pugetmesh.org)
  setBroker(0, "meshtastic.pugetmesh.org", 1883, "meshdev", "large4cats", true);
  
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
  
  // Clear packet queue
  _queue_count = 0;
  _queue_head = 0;
  _queue_tail = 0;
  
  // Clean up resources
  if (_mqtt_client) {
    delete _mqtt_client;
    _mqtt_client = nullptr;
  }
  if (_wifi_client) {
    delete _wifi_client;
    _wifi_client = nullptr;
  }
  
  _initialized = false;
  MQTT_DEBUG_PRINTLN("MQTT Bridge stopped");
}

void MQTTBridge::loop() {
  if (!_initialized) return;
  
  // Maintain broker connections
  connectToBrokers();
  
  // Process packet queue
  processPacketQueue();
  
  // Periodic NTP sync (every hour)
  if (WiFi.status() == WL_CONNECTED && millis() - _last_ntp_sync > 3600000) {
    syncTimeWithNTP();
  }
  
  // Publish status updates
  if (_status_enabled && millis() - _last_status_publish > _status_interval) {
    publishStatus();
    _last_status_publish = millis();
  }
}

void MQTTBridge::onPacketReceived(mesh::Packet *packet) {
  if (!_initialized || !_packets_enabled) return;
  
  // Queue packet for transmission
  queuePacket(packet, false);
}

void MQTTBridge::sendPacket(mesh::Packet *packet) {
  if (!_initialized || !_packets_enabled || !_tx_enabled) return;
  
  // Queue packet for transmission (only if TX enabled)
  queuePacket(packet, true);
}

void MQTTBridge::connectToBrokers() {
  for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
    if (!_brokers[i].enabled) continue;
    
    // Check if we need to attempt connection
    if (!_brokers[i].connected && 
        millis() - _brokers[i].last_attempt > _brokers[i].reconnect_interval) {
      
      MQTT_DEBUG_PRINTLN("Connecting to broker %d: %s:%d", i, _brokers[i].host, _brokers[i].port);
      
      // Set broker for this connection
      _mqtt_client->setServer(_brokers[i].host, _brokers[i].port);
      
      // Generate unique client ID
      char client_id[32];
      snprintf(client_id, sizeof(client_id), "%s_%d_%lu", _origin, i, millis());
      
      // Attempt connection
      bool connected = _mqtt_client->connect(
        client_id,
        _brokers[i].username,
        _brokers[i].password
      );
      
      if (connected) {
        _brokers[i].connected = true;
        _brokers[i].reconnect_interval = 5000; // Reset to 5 seconds
        _active_brokers++;
        MQTT_DEBUG_PRINTLN("Connected to broker %d", i);
        
        // Publish initial status
        if (_status_enabled) {
          publishStatus();
        }
      } else {
        _brokers[i].connected = false;
        _brokers[i].last_attempt = millis();
        // Exponential backoff: 5s, 10s, 20s, 30s max
        _brokers[i].reconnect_interval = min(30000UL, _brokers[i].reconnect_interval * 2);
        MQTT_DEBUG_PRINTLN("Failed to connect to broker %d", i);
      }
    }
    
    // Maintain connection
    if (_brokers[i].connected) {
      _mqtt_client->loop();
      if (!_mqtt_client->connected()) {
        _brokers[i].connected = false;
        _active_brokers--;
        MQTT_DEBUG_PRINTLN("Lost connection to broker %d", i);
      }
    }
  }
}

void MQTTBridge::processPacketQueue() {
  if (_queue_count == 0 || !isAnyBrokerConnected()) return;
  
  // Process up to 5 packets per loop to avoid blocking
  int processed = 0;
  while (_queue_count > 0 && processed < 5) {
    QueuedPacket& queued = _packet_queue[_queue_head];
    
    // Publish packet
    publishPacket(queued.packet, queued.is_tx);
    
    // Publish raw if enabled
    if (_raw_enabled) {
      publishRaw(queued.packet);
    }
    
    // Remove from queue
    dequeuePacket();
    processed++;
  }
}

void MQTTBridge::publishStatus() {
  if (!isAnyBrokerConnected()) return;
  
  char json_buffer[512];
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
  
  // Build status message
  int len = MQTTMessageBuilder::buildStatusMessage(
    _origin,
    origin_id,
    _board_model,  // model - now dynamic!
    _firmware_version,  // firmware version
    radio_info,
    "custom-mqtt-bridge",
    "online",
    timestamp,
    json_buffer,
    sizeof(json_buffer)
  );
  
  if (len > 0) {
    // Publish to all connected brokers
    for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
      if (_brokers[i].enabled && _brokers[i].connected) {
        char topic[128];
        snprintf(topic, sizeof(topic), "meshcore/%s/%s/status", _iata, _device_id);
        MQTT_DEBUG_PRINTLN("Publishing status to topic: %s", topic);
        
        _mqtt_client->setServer(_brokers[i].host, _brokers[i].port);
        _mqtt_client->publish(topic, json_buffer, true); // retained
      }
    }
  }
}

void MQTTBridge::publishPacket(mesh::Packet* packet, bool is_tx) {
  if (!packet) return;
  
  char json_buffer[1024];
  char origin_id[65];
  
  // Use actual device ID
  strncpy(origin_id, _device_id, sizeof(origin_id) - 1);
  origin_id[sizeof(origin_id) - 1] = '\0';
  
  // Build packet message using raw radio data if available
  int len;
  if (_last_raw_len > 0 && (millis() - _last_raw_timestamp) < 1000) {
    // Use raw radio data (within 1 second of packet)
    len = MQTTMessageBuilder::buildPacketJSONFromRaw(
      _last_raw_data, _last_raw_len, packet, is_tx, _origin, origin_id, 
      _last_snr, _last_rssi, _timezone, json_buffer, sizeof(json_buffer)
    );
  } else {
    // Fallback to reconstructed packet data
    len = MQTTMessageBuilder::buildPacketJSON(
      packet, is_tx, _origin, origin_id, _timezone, json_buffer, sizeof(json_buffer)
    );
  }
  
  if (len > 0) {
    // Publish to all connected brokers
    for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
      if (_brokers[i].enabled && _brokers[i].connected) {
        char topic[128];
        snprintf(topic, sizeof(topic), "meshcore/%s/%s/packets", _iata, _device_id);
        MQTT_DEBUG_PRINTLN("Publishing packet to topic: %s", topic);
        
        _mqtt_client->setServer(_brokers[i].host, _brokers[i].port);
        _mqtt_client->publish(topic, json_buffer);
      }
    }
  }
}

void MQTTBridge::publishRaw(mesh::Packet* packet) {
  if (!packet) return;
  
  char json_buffer[512];
  char origin_id[65];
  
  // Use actual device ID
  strncpy(origin_id, _device_id, sizeof(origin_id) - 1);
  origin_id[sizeof(origin_id) - 1] = '\0';
  
  // Build raw message
  int len = MQTTMessageBuilder::buildRawJSON(
    packet, _origin, origin_id, _timezone, json_buffer, sizeof(json_buffer)
  );
  
  if (len > 0) {
    // Publish to all connected brokers
    for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
      if (_brokers[i].enabled && _brokers[i].connected) {
        char topic[128];
        snprintf(topic, sizeof(topic), "meshcore/%s/%s/raw", _iata, _device_id);
        
        _mqtt_client->setServer(_brokers[i].host, _brokers[i].port);
        _mqtt_client->publish(topic, json_buffer);
      }
    }
  }
}

void MQTTBridge::queuePacket(mesh::Packet* packet, bool is_tx) {
  if (_queue_count >= MAX_QUEUE_SIZE) {
    // Queue full, remove oldest
    dequeuePacket();
  }
  
  QueuedPacket& queued = _packet_queue[_queue_tail];
  queued.packet = packet;
  queued.timestamp = millis();
  queued.is_tx = is_tx;
  
  _queue_tail = (_queue_tail + 1) % MAX_QUEUE_SIZE;
  _queue_count++;
}

void MQTTBridge::dequeuePacket() {
  if (_queue_count == 0) return;
  
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
      
      // Set timezone from string (with DST support)
      MQTT_DEBUG_PRINTLN("Setting timezone: %s", _prefs->timezone_string);
      
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

#endif
