#pragma once

// MQTT observer hosted by the companion radio firmware. Wraps ObserverConfig
// (/mqtt.json + observer CLI) and MQTTBridge behind a header that pulls in
// neither, so companion translation units never see the repeater's NodePrefs.

#ifdef WITH_MQTT_BRIDGE

#include <Mesh.h>
#include <helpers/ObserverConfig.h>
#include <helpers/bridges/MQTTBridgeHost.h>

class MQTTBridge;

class CompanionObserver : public ObserverCallbacks {
public:
  CompanionObserver(mesh::MainBoard& board, MQTTBridgeHost& host,
                    const char* firmware_version, const char* build_date);

  // Load /mqtt.json. When it has no WiFi SSID, adopt the companion's own WiFi
  // credentials once so an existing WiFi companion keeps its network.
  void loadPrefs(FILESYSTEM* fs, const char* import_ssid, const char* import_pwd);
  // Start the network link and the bridge. Call after loadPrefs().
  void begin(mesh::Dispatcher* dispatcher, mesh::Radio* radio, mesh::MillisecondClock* ms,
             mesh::PacketManager* mgr, mesh::RTCClock* rtc, mesh::LocalIdentity* identity);
  void loop();

  void onPacketRx(mesh::Packet* pkt);
  void onPacketTx(mesh::Packet* pkt);
  void onRawRx(const uint8_t* raw, int len, float snr, float rssi);

  // Observer CLI (get/set mqtt.*, wifi.*, timezone, ...). Returns true when the
  // command was recognized; `reply` must hold at least 161 bytes. sender_timestamp
  // is 0 for local consoles (USB, app) and the sender's clock for remote admin.
  bool handleCommand(uint32_t sender_timestamp, const char* command, char* reply);

  const MQTTPrefs* prefs() const { return _config.prefs(); }
  bool isBridgeRunning() const;
  int queueSize() const;
  // Slots currently connected / configured, for the UI. -1 when not running.
  int connectedSlots() const;
  int configuredSlots() const;

  // ObserverCallbacks
  const char* getObserverNodeName() override { return _host.getObserverNodeName(); }
  void saveNodePrefs() override { }   // ota_channel is not persisted on companions
  bool saveObserverPrefs() override;
  const char* getFirmwareVer() override { return _firmware_version; }
  void restartBridge() override;
  void restartBridgeSlot(int slot) override;
  bool syncMqttNtp() override;
  bool isMqttBridgeRunning() override { return isBridgeRunning(); }
  bool runMqttNtpDiag(char* reply, size_t reply_size, bool verbose) override;

private:
  void startBridge();

  mesh::MainBoard& _board;
  MQTTBridgeHost& _host;
  const char* _firmware_version;
  const char* _build_date;
  uint8_t _ota_channel = 0;
  ObserverConfig _config;
  FILESYSTEM* _fs = nullptr;
  MQTTBridge* _bridge = nullptr;
  mesh::Dispatcher* _dispatcher = nullptr;
  mesh::Radio* _radio = nullptr;
  mesh::MillisecondClock* _ms = nullptr;
  mesh::LocalIdentity* _identity = nullptr;
  // A stop timed out; restart once the MQTT task acknowledges it.
  bool _resume_pending = false;
};

#endif  // WITH_MQTT_BRIDGE
