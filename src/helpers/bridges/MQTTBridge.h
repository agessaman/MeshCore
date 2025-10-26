#pragma once

#include "MeshCore.h"
#include "helpers/bridges/BridgeBase.h"
#include <PubSubClient.h>
#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Timezone.h>

#if defined(MQTT_DEBUG) && defined(ARDUINO)
  #include <Arduino.h>
  #define MQTT_DEBUG_PRINT(F, ...) Serial.printf("MQTT: " F, ##__VA_ARGS__)
  #define MQTT_DEBUG_PRINTLN(F, ...) Serial.printf("MQTT: " F "\n", ##__VA_ARGS__)
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
 * - Multiple MQTT broker support
 * - Automatic reconnection with exponential backoff
 * - JSON message formatting for status, packets, and raw data
 * - Configurable topics and QoS levels
 * - Packet queuing during connection issues
 *
 * Message Types:
 * - Status: Device connection status and metadata
 * - Packets: Full packet data with RF characteristics
 * - Raw: Minimal raw packet data for map integration
 *
 * Configuration:
 * - Define WITH_MQTT_BRIDGE to enable this bridge
 * - Configure brokers via CLI commands
 * - Set origin name and IATA code for topic structure
 */
class MQTTBridge : public BridgeBase {
private:
  PubSubClient* _mqtt_client;
  WiFiClient* _wifi_client;
  
  // MQTT broker configuration
  struct MQTTBroker {
    char host[64];
    uint16_t port;
    char username[32];
    char password[64];
    char client_id[32];
    uint8_t qos;
    bool enabled;
    bool connected;
    unsigned long last_attempt;
    unsigned long reconnect_interval;
  };
  
  static const int MAX_MQTT_BROKERS_COUNT = 3;
  MQTTBroker _brokers[MAX_MQTT_BROKERS_COUNT];
  int _active_brokers;
  
  // Message configuration
  char _origin[32];
  char _iata[8];
  char _device_id[65];  // Device public key (hex string)
  char _firmware_version[64];  // Firmware version string
  char _board_model[64];  // Board model string
  bool _status_enabled;
  bool _packets_enabled;
  bool _raw_enabled;
  unsigned long _last_status_publish;
  unsigned long _status_interval;
  
  // Packet queue for offline scenarios
  struct QueuedPacket {
    mesh::Packet* packet;
    unsigned long timestamp;
    bool is_tx;
  };
  
  static const int MAX_QUEUE_SIZE = 50;
  QueuedPacket _packet_queue[MAX_QUEUE_SIZE];
  int _queue_head;
  int _queue_tail;
  int _queue_count;
  
  // NTP time sync
  WiFiUDP _ntp_udp;
  NTPClient _ntp_client;
  unsigned long _last_ntp_sync;
  bool _ntp_synced;
  
  // Timezone handling
  Timezone* _timezone;
  
  // Internal methods
  void connectToBrokers();
  void processPacketQueue();
  void publishStatus();
  void publishPacket(mesh::Packet* packet, bool is_tx);
  void publishRaw(mesh::Packet* packet);
  void queuePacket(mesh::Packet* packet, bool is_tx);
  void dequeuePacket();
  bool isAnyBrokerConnected();
  void setBrokerDefaults();
  void syncTimeWithNTP();
  Timezone* createTimezoneFromString(const char* tz_string);
  
public:
  /**
   * Constructs an MQTTBridge instance
   *
   * @param prefs Node preferences for configuration settings
   * @param mgr PacketManager for allocating and queuing packets
   * @param rtc RTCClock for timestamping debug messages
   */
  MQTTBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc);

  /**
   * Initializes the MQTT bridge
   *
   * - Sets up default broker configuration
   * - Initializes WiFi client
   * - Prepares MQTT clients for each broker
   */
  void begin() override;

  /**
   * Stops the MQTT bridge
   *
   * - Disconnects from all brokers
   * - Clears packet queue
   * - Releases resources
   */
  void end() override;

  /**
   * Main loop handler
   * - Maintains broker connections
   * - Processes packet queue
   * - Publishes status updates
   */
  void loop() override;

  /**
   * Called when a packet is received via mesh
   * Queues the packet for MQTT publishing if enabled
   *
   * @param packet The received mesh packet
   */
  void onPacketReceived(mesh::Packet *packet) override;

  /**
   * Called when a packet needs to be transmitted via MQTT
   * Publishes the packet to all connected brokers
   *
   * @param packet The mesh packet to transmit
   */
  void sendPacket(mesh::Packet *packet) override;

  /**
   * Configure MQTT broker settings
   *
   * @param broker_index Broker index (0-2)
   * @param host Broker hostname
   * @param port Broker port
   * @param username MQTT username
   * @param password MQTT password
   * @param enabled Whether broker is enabled
   */
  void setBroker(int broker_index, const char* host, uint16_t port, 
                 const char* username, const char* password, bool enabled);

  /**
   * Set device origin name for MQTT topics
   *
   * @param origin Device name
   */
  void setOrigin(const char* origin);

  /**
   * Set IATA code for MQTT topics
   *
   * @param iata Airport code
   */
  void setIATA(const char* iata);

  /**
   * Set device public key for MQTT topics
   *
   * @param device_id Device public key (hex string)
   */
  void setDeviceID(const char* device_id);

  /**
   * Set firmware version for status messages
   *
   * @param firmware_version Firmware version string
   */
  void setFirmwareVersion(const char* firmware_version);

  /**
   * Set board model for status messages
   *
   * @param board_model Board model string
   */
  void setBoardModel(const char* board_model);

  /**
   * Enable/disable message types
   *
   * @param status Enable status messages
   * @param packets Enable packet messages
   * @param raw Enable raw messages
   */
  void setMessageTypes(bool status, bool packets, bool raw);

  /**
   * Get connection status for all brokers
   *
   * @return Number of connected brokers
   */
  int getConnectedBrokers() const;

  /**
   * Get queue status
   *
   * @return Number of queued packets
   */
  int getQueueSize() const;
};

#endif
