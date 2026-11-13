#include "CompanionObserver.h"

#ifdef WITH_MQTT_BRIDGE

#include <Arduino.h>
#include "bridges/MQTTBridge.h"
#include "MQTTPrefsAtomicStore.h"
#include "NetworkHostname.h"
#include "NetworkLink.h"
#include "TxtDataHelpers.h"

CompanionObserver::CompanionObserver(mesh::MainBoard& board, MQTTBridgeHost& host,
                                     const char* firmware_version, const char* build_date)
    : _board(board), _host(host), _firmware_version(firmware_version), _build_date(build_date),
      _config(&board, this, &_ota_channel) { }

void CompanionObserver::loadPrefs(FILESYSTEM* fs, const char* import_ssid, const char* import_pwd) {
  _fs = fs;
  // No legacy /com_prefs on a companion, so nothing waits on the gate.
  MQTTPrefsAtomicStore::LegacyUpgradeGate legacy_upgrade(false);
  _config.loadMQTTPrefs(fs, &legacy_upgrade);

  MQTTPrefs* obs = _config.prefs();
  if (!_config.isHeld() && obs->wifi_ssid[0] == 0 && import_ssid && import_ssid[0]) {
    if (strlen(import_ssid) >= sizeof(obs->wifi_ssid)) {
      // A truncated SSID would silently join nothing; leave it for the user.
      Serial.printf("Observer: companion WiFi SSID longer than %u chars, not adopted; "
                    "set wifi.ssid/wifi.pwd\n", (unsigned)(sizeof(obs->wifi_ssid) - 1));
      return;
    }
    StrHelper::strncpy(obs->wifi_ssid, import_ssid, sizeof(obs->wifi_ssid));
    StrHelper::strncpy(obs->wifi_password, import_pwd ? import_pwd : "", sizeof(obs->wifi_password));
    if (_config.saveMQTTPrefs(fs)) {
      Serial.printf("Observer: adopted companion WiFi '%s' into /mqtt.json\n", obs->wifi_ssid);
    }
  }
}

void CompanionObserver::begin(mesh::Dispatcher* dispatcher, mesh::Radio* radio, mesh::MillisecondClock* ms,
                              mesh::PacketManager* mgr, mesh::RTCClock* rtc, mesh::LocalIdentity* identity) {
  _dispatcher = dispatcher;
  _radio = radio;
  _ms = ms;
  _mgr = mgr;
  _rtc = rtc;
  _identity = identity;

#ifdef ESP_PLATFORM
  MQTTPrefs* obs = _config.prefs();
  // Same bring-up order as the repeater observer: cap TX power before the first
  // association, and name the DHCP lease on boot-owned links.
  NetworkLink& link = activeNetworkLink();
  link.setWifiTxPower(obs->wifi_tx_power);
  if (link.isAutomatic()) {
    char hostname[NetworkHostname::kBufferSize];
    NetworkHostname::build(hostname, sizeof(hostname), _host.getObserverNodeName(),
                           identity->pub_key, PUB_KEY_SIZE);
    link.setHostname(hostname);
    link.bootstrap(obs->wifi_ssid, obs->wifi_password, NETWORK_ETHERNET_BOOT_WAIT_MS);
  }
#endif

  // Deferred construction, as in the repeater (static init crashes on ESP32 classic).
  _bridge_ctl.setState(*this, _bridge, true);
}

MQTTBridge* CompanionObserver::newBridge() {
  return new MQTTBridge(&_host, _config.prefs(), _mgr, _rtc, _identity);
}

void CompanionObserver::prepareBridgeStart(MQTTBridge* b, bool restart) {
  (void)restart;
  char device_id[65];
  mesh::Utils::toHex(device_id, _identity->pub_key, PUB_KEY_SIZE);
  b->setDeviceID(device_id);
  b->setFirmwareVersion(_firmware_version);
  b->setBoardModel(_board.getManufacturerName());
  b->setBuildDate(_build_date);
  b->setStatsSources(_dispatcher, _radio, &_board, _ms);
}

void CompanionObserver::loop() {
  _bridge_ctl.poll(*this, _bridge);
}

void CompanionObserver::onPacketRx(mesh::Packet* pkt) {
  if (_bridge) _bridge->onPacketReceived(pkt);
}

void CompanionObserver::onPacketTx(mesh::Packet* pkt) {
  if (_bridge) _bridge->sendPacket(pkt);
}

void CompanionObserver::onRawRx(const uint8_t* raw, int len, float snr, float rssi) {
  if (_bridge) _bridge->storeRawRadioData(raw, len, snr, rssi);
}

bool CompanionObserver::handleCommand(uint32_t sender_timestamp, const char* command, char* reply) {
  // sender_timestamp 0 means a local console; remote admin gets secrets masked.
  // Self-heal reboots are not driven on companions; the saved setting is left for the repeater role.
  const bool selfheal_cmd = strlen(command) >= 17 && strncmp(&command[3], " mqtt.selfheal", 14) == 0 &&
      (command[17] == 0 || command[17] == ' ' || command[17] == '\r' || command[17] == '\n');
  if (selfheal_cmd && memcmp(command, "set", 3) == 0) {
    strcpy(reply, "Err - self-heal not supported on companion observer");
    return true;
  }
  if (selfheal_cmd && memcmp(command, "get", 3) == 0) {
    snprintf(reply, 161, "> not supported on companion (saved: %s)",
             _config.prefs()->mqtt_selfheal_enabled ? "on" : "off");
    return true;
  }
  if (memcmp(command, "set ", 4) == 0) return _config.handleSetCmd(sender_timestamp, &command[4], reply);
  if (memcmp(command, "get ", 4) == 0) return _config.handleGetCmd(sender_timestamp, &command[4], reply);
  if (memcmp(command, "ota", 3) == 0 && (command[3] == 0 || command[3] == ' ')) {
    strcpy(reply, "Err - OTA not supported on companion observer");
    return true;
  }
  char buf[161];
  StrHelper::strncpy(buf, command, sizeof(buf));
  return _config.handleCommand(sender_timestamp, buf, reply);
}

int CompanionObserver::queueSize() const {
  return _bridge ? _bridge->getQueueSize() : 0;
}

bool CompanionObserver::isBridgeRunning() const {
  return _bridge && _bridge->isRunning();
}

int CompanionObserver::connectedSlots() const {
  int up = 0;
  return MQTTBridge::getSlotCounts(&up, nullptr) ? up : -1;
}

int CompanionObserver::configuredSlots() const {
  int enabled = 0;
  return MQTTBridge::getSlotCounts(nullptr, &enabled) ? enabled : -1;
}

bool CompanionObserver::saveObserverPrefs() {
  return _fs && _config.saveMQTTPrefs(_fs);
}

void CompanionObserver::restartBridge() {
  _bridge_ctl.restart(*this, _bridge);
}

void CompanionObserver::restartBridgeSlot(int slot) {
  if (slot < 0 || slot >= MAX_MQTT_SLOTS) return;
  _bridge_ctl.restartSlot(*this, _bridge, slot);
}

bool CompanionObserver::syncMqttNtp() {
  if (!_bridge || !_bridge->isRunning()) return false;
  return _bridge->requestForcedNtpSync(0);
}

bool CompanionObserver::runMqttNtpDiag(char* reply, size_t reply_size, bool verbose) {
  if (!_bridge || !_bridge->isRunning()) return false;
  return _bridge->ntpDiag(reply, reply_size, verbose);
}

#endif  // WITH_MQTT_BRIDGE
