#if defined(PICO_MQTT_OBSERVER)
#include "helpers/NetworkLink.h"
#include "helpers/MQTTConnectionPolicy.h"
#include <WiFi.h>
#include <FreeRTOS.h>
#include <semphr.h>
#include <lwip_wrap.h>
#include <lwip/init.h>
#include <cyw43.h>

namespace {
void runOnLwip(void (*callback)(void*), void* context) {
  // Arduino-Pico creates the dispatch queue lazily in its lwip_init wrapper.
  lwip_init();
  lwip_callback(callback, context);
}

class PicoWifiNetworkLink : public NetworkLink {
  struct Snapshot {
    bool up = false;
    int code = WL_DISCONNECTED, signal = 0;
    int link = CYW43_LINK_DOWN;
    bool joined = false;
    IPAddress ip{0, 0, 0, 0}, dns{0, 0, 0, 0};
    const char* phase = "not started";
    uint32_t connected_at = 0, disconnected_at = 0;
  };
  mutable StaticSemaphore_t _mutex_storage;
  SemaphoreHandle_t _mutex = xSemaphoreCreateMutexStatic(&_mutex_storage);
  Snapshot _snapshot;
  char _ssid[32] = {}, _password[64] = {}, _hostname[64] = {};
  uint32_t _last_attempt = 0;
  uint32_t _dhcp_started = 0, _last_signal = 0;
  uint8_t _attempt = 0;
  bool _started = false;
  bool _waiting_dhcp = false;
  Snapshot read() const {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    Snapshot value = _snapshot;
    xSemaphoreGive(_mutex);
    return value;
  }
  void startAttempt() {
    // The environment-local CYW43 patch makes this issue one join and return.
    runOnLwip([](void* context) {
      auto& self = *static_cast<PicoWifiNetworkLink*>(context);
      WiFi.mode(WIFI_STA);
      if (self._hostname[0]) WiFi.setHostname(self._hostname);
      WiFi.beginNoBlock(self._ssid, self._password[0] ? self._password : nullptr);
    }, this);
    _last_attempt = millis();
    _waiting_dhcp = false;
  }
public:
  const char* mediumName() const override { return "wifi"; }
  NetworkMedium medium() const override { return NetworkMedium::WiFi; }
  const char* statusName() const override { return read().phase; }
  int statusCode() const override { return read().code; }
  bool configValid(const char* ssid) const override { return ssid && *ssid; }
  void setHostname(const char* value) override {
    snprintf(_hostname, sizeof(_hostname), "%s", value ? value : "pico-observer");
    runOnLwip([](void* context) {
      WiFi.setHostname(static_cast<PicoWifiNetworkLink*>(context)->_hostname);
    }, this);
  }
  bool begin(const char* ssid, const char* password) override {
    const bool changed = strcmp(_ssid, ssid ? ssid : "") || strcmp(_password, password ? password : "");
    snprintf(_ssid, sizeof(_ssid), "%s", ssid ? ssid : "");
    snprintf(_password, sizeof(_password), "%s", password ? password : "");
    if (changed || !_started) {
      _attempt = 0;
      if (_ssid[0]) startAttempt();
      else if (_started) runOnLwip([](void*) { WiFi.disconnect(); }, nullptr);
      _started = true;
    }
    maintain(millis(), 0);
    return _ssid[0];
  }
  NetworkTransition maintain(uint32_t now, uint8_t) override {
    Snapshot next = read();
    const bool before = next.up;
    struct Observation { Snapshot* value; bool signal; } observation{
      &next, !before || now - _last_signal >= 5000
    };
    runOnLwip([](void* context) {
      auto& observation = *static_cast<Observation*>(context);
      auto& value = *observation.value;
      value.code = WiFi.status();
      value.link = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
      // LINK_JOIN also covers an unfinished WPA handshake in this pinned core.
      value.joined = value.link == CYW43_LINK_JOIN && cyw43_state.wifi_join_state == 1;
      value.ip = WiFi.localIP();
      value.up = value.joined && value.code == WL_CONNECTED && value.ip != IPAddress(0, 0, 0, 0);
      value.dns = value.up ? WiFi.dnsIP() : IPAddress(0, 0, 0, 0);
      if (value.up && observation.signal) value.signal = WiFi.RSSI();
      if (!value.up) { value.ip = IPAddress(0, 0, 0, 0); value.signal = 0; }
    }, &observation);
    if (next.up) {
      if (observation.signal) _last_signal = now;
      if (!before) next.connected_at = now;
      _attempt = 0;
      _waiting_dhcp = false;
      next.phase = "connected";
    } else {
      if (before) { next.disconnected_at = now; next.connected_at = 0; }
      if (next.joined && !_waiting_dhcp) { _dhcp_started = now; _waiting_dhcp = true; }
      if (!next.joined) _waiting_dhcp = false;
      const bool expired = _waiting_dhcp ? now - _dhcp_started >= 60000 : now - _last_attempt >= 30000;
      next.phase = !_started ? "not started" : !_ssid[0] ? "no SSID" :
          next.link == CYW43_LINK_BADAUTH ? "auth failed" :
          next.link == CYW43_LINK_NONET ? "SSID not found" :
          next.link == CYW43_LINK_FAIL ? "join failed" :
          next.joined ? (expired ? "DHCP timeout" : "waiting DHCP") :
          expired ? "join timeout" : "joining";
      if (_started && _ssid[0] && (next.link < 0 || expired) &&
          now - _last_attempt >= MQTTConnectionPolicy::wifiReconnectBackoffMs(_attempt)) {
        _attempt = MQTTConnectionPolicy::nextWifiBackoffAttempt(_attempt);
        startAttempt();
      }
    }
    xSemaphoreTake(_mutex, portMAX_DELAY); _snapshot = next; xSemaphoreGive(_mutex);
    return before == next.up ? NetworkTransition::None : next.up ? NetworkTransition::Up : NetworkTransition::Down;
  }
  bool isConnected() const override { return read().up; }
  IPAddress localIP() const override { return read().ip; }
  int rssi() const override { return read().signal; }
  bool resolveHost(const char* name, IPAddress& ip) const override { return WiFi.hostByName(name, ip, 5000) == 1; }
  void formatDiagnostics(char* out, size_t size) const override {
    const auto value = read();
    snprintf(out, size, "> wifi %s state:%s wl:%d link:%d ip:%s rssi:%d", value.up ? "up" : "down",
             value.phase, value.code, value.link, value.ip.toString().c_str(), value.signal);
  }
  void formatDns(char* out, size_t size) const override {
    snprintf(out, size, "> %s", read().dns.toString().c_str());
  }
  unsigned long connectedAtMillis() const override { return read().connected_at; }
  uint8_t lastDisconnectReason() const override { return 0; }
  unsigned long lastDisconnectTime() const override { return read().disconnected_at; }
  AlertFaultPolicy::OutageSnapshot outageSnapshot() const override {
    const auto value = read();
    return { !value.up, value.disconnected_at, 0 };
  }
};
}
NetworkLink& activeNetworkLink() { static PicoWifiNetworkLink link; return link; }
#endif
