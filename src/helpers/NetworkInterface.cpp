#include "NetworkInterface.h"

#if defined(ESP_PLATFORM)

#include "MQTTConnectionPolicy.h"

#include <atomic>
#include <climits>
#include <cstring>

#include <WiFi.h>
#include <esp_wifi.h>

#if defined(NETWORK_PREFER_ETHERNET)
#include "ethernet/ch390/CH390Config.h"
#endif

namespace {

class NetworkInterfaceBase : public NetworkInterface {
 protected:
  std::atomic<uint64_t> _outage_bits{AlertFaultPolicy::packOutageSnapshot({false, 0, 0})};
  std::atomic<unsigned long> _connected_at{0};
  std::atomic<unsigned long> _last_disconnect_time{0};
  std::atomic<uint8_t> _last_disconnect_reason{0};
  bool _status_initialized = false;
  bool _last_connected = false;
  unsigned long _last_status_check = 0;

  AlertFaultPolicy::OutageSnapshot outage() const {
    return AlertFaultPolicy::unpackOutageSnapshot(
        _outage_bits.load(std::memory_order_acquire));
  }

  void setOutage(AlertFaultPolicy::OutageSnapshot snapshot) {
    _outage_bits.store(AlertFaultPolicy::packOutageSnapshot(snapshot),
                       std::memory_order_release);
  }

  void noteConnected(unsigned long now_ms) {
    if (_connected_at.load(std::memory_order_relaxed) == 0) {
      _connected_at.store(now_ms, std::memory_order_relaxed);
    }
    setOutage(AlertFaultPolicy::applyWifiGotIp(outage()));
  }

  void noteDisconnected(unsigned long now_ms, uint8_t reason) {
    _last_disconnect_reason.store(reason, std::memory_order_relaxed);
    _last_disconnect_time.store(now_ms, std::memory_order_relaxed);
    setOutage(AlertFaultPolicy::applyWifiDisconnectEvent(
        (uint32_t)now_ms, reason, outage()));
  }

 public:
  unsigned long connectedAtMillis() const override {
    return _connected_at.load(std::memory_order_relaxed);
  }

  uint8_t lastDisconnectReason() const override {
    return _last_disconnect_reason.load(std::memory_order_relaxed);
  }

  unsigned long lastDisconnectTime() const override {
    return _last_disconnect_time.load(std::memory_order_relaxed);
  }

  AlertFaultPolicy::OutageSnapshot outageSnapshot() const override {
    return outage();
  }
};

class WiFiNetworkInterface final : public NetworkInterfaceBase {
  bool _event_registered = false;
  char _ssid[33] = {};
  char _password[65] = {};
  unsigned long _last_reconnect_attempt = 0;
  uint8_t _reconnect_backoff_attempt = 0;

  void applyPowerPrefs(uint8_t wifi_power_save) {
    wifi_ps_type_t ps_mode = wifi_power_save == 2 ? WIFI_PS_MAX_MODEM : WIFI_PS_NONE;
    esp_wifi_set_ps(ps_mode);
#ifdef MQTT_WIFI_TX_POWER
    WiFi.setTxPower(MQTT_WIFI_TX_POWER);
#else
    WiFi.setTxPower(WIFI_POWER_11dBm);
#endif
  }

 public:
  const char* mediumName() const override { return "wifi"; }
  NetworkMedium medium() const override { return NetworkMedium::WiFi; }
  const char* statusName() const override {
    switch (WiFi.status()) {
      case WL_CONNECTED: return "connected";
      case WL_NO_SSID_AVAIL: return "no_ssid";
      case WL_CONNECT_FAILED: return "connect_failed";
      case WL_CONNECTION_LOST: return "connection_lost";
      case WL_DISCONNECTED: return "disconnected";
      case 255: return "not_started";
      default: return "unknown";
    }
  }
  int statusCode() const override { return (int)WiFi.status(); }

  bool configValid(const char* wifi_ssid) const override {
    return wifi_ssid && wifi_ssid[0] != '\0';
  }

  bool begin(const char* wifi_ssid, const char* wifi_password) override {
    if (!configValid(wifi_ssid)) return false;
    strncpy(_ssid, wifi_ssid, sizeof(_ssid) - 1);
    _ssid[sizeof(_ssid) - 1] = '\0';
    strncpy(_password, wifi_password ? wifi_password : "", sizeof(_password) - 1);
    _password[sizeof(_password) - 1] = '\0';

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setAutoConnect(true);

    if (!_event_registered) {
      WiFi.onEvent([this](WiFiEvent_t event, WiFiEventInfo_t info) {
        switch (event) {
          case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            noteConnected(millis());
            _reconnect_backoff_attempt = 0;
            break;
          case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            noteDisconnected(millis(), info.wifi_sta_disconnected.reason);
            break;
          default:
            break;
        }
      });
      _event_registered = true;
    }

    // Preserve the existing restart behavior: MQTT stop leaves the station up,
    // and begin() must not force a disconnect that races the first DNS lookup.
    if (!isConnected()) {
      WiFi.begin(_ssid, _password);
    } else {
      noteConnected(millis());
    }
    return true;
  }

  NetworkTransition maintain(uint32_t now_ms, uint8_t wifi_power_save) override {
    const bool connected = isConnected();
    if (connected && connectedAtMillis() == 0) noteConnected(now_ms);

    if (!_status_initialized) {
      _last_connected = connected;
      _status_initialized = true;
      setOutage(AlertFaultPolicy::applyWifiStatus(
          now_ms, connected, outage(), false));
    }

    if ((uint32_t)(now_ms - _last_status_check) <= 10000) {
      if (connected && outage().down) noteConnected(now_ms);
      return NetworkTransition::None;
    }
    _last_status_check = now_ms;

    if (connected) {
      const bool transitioned = !_last_connected;
      if (transitioned) {
        setOutage(AlertFaultPolicy::applyWifiStatus(
            now_ms, true, outage(), true));
        _connected_at.store(now_ms, std::memory_order_relaxed);
        _reconnect_backoff_attempt = 0;
        applyPowerPrefs(wifi_power_save);
      }
      _last_connected = true;
      return transitioned ? NetworkTransition::Up : NetworkTransition::None;
    }

    AlertFaultPolicy::OutageSnapshot snapshot = AlertFaultPolicy::applyWifiStatus(
        now_ms, false, outage(), true);
    setOutage(snapshot);
    const bool transitioned = _last_connected;
    if (transitioned) {
      _connected_at.store(0, std::memory_order_relaxed);
    } else if (snapshot.down && MQTTConnectionPolicy::wifiReconnectDue(
                   now_ms, snapshot.started_ms, (uint32_t)_last_reconnect_attempt,
                   _reconnect_backoff_attempt)) {
      _last_reconnect_attempt = now_ms;
      _reconnect_backoff_attempt =
          MQTTConnectionPolicy::nextWifiBackoffAttempt(_reconnect_backoff_attempt);
      WiFi.disconnect();
      WiFi.begin(_ssid, _password);
    }
    _last_connected = false;
    return transitioned ? NetworkTransition::Down : NetworkTransition::None;
  }

  bool isConnected() const override { return WiFi.status() == WL_CONNECTED; }
  IPAddress localIP() const override { return WiFi.localIP(); }
  int rssi() const override { return isConnected() ? WiFi.RSSI() : INT_MIN; }
  bool resolveHost(const char* hostname, IPAddress& address) const override {
    return WiFi.hostByName(hostname, address);
  }
  void formatDiagnostics(char* reply, size_t reply_size) const override {
    snprintf(reply, reply_size,
             "> why:ethernet-not-enabled selected:wifi\n"
             "wifi:state:%s ip:%s",
             statusName(),
             localIP().toString().c_str());
  }
};

#if defined(NETWORK_PREFER_ETHERNET)
class EthernetNetworkInterface final : public NetworkInterfaceBase {
 public:
  enum class EventState : uint8_t {
    None,
    Started,
    LinkDown,
    LinkUp,
    GotIp,
    Stopped,
  };

 private:
  bool _started = false;
  bool _event_registered = false;
  std::atomic<uint8_t> _event_state{static_cast<uint8_t>(EventState::None)};

 public:
  const char* mediumName() const override { return "ethernet"; }
  NetworkMedium medium() const override { return NetworkMedium::Ethernet; }
  const char* statusName() const override {
    return isConnected() ? "connected" : "disconnected";
  }
  int statusCode() const override { return isConnected() ? 1 : 0; }
  bool configValid(const char*) const override { return true; }

  bool begin(const char*, const char*) override {
    if (_started) return true;
    if (!_event_registered) {
      WiFi.onEvent([this](WiFiEvent_t event, WiFiEventInfo_t) {
        switch (event) {
          case ARDUINO_EVENT_ETH_START:
            _event_state.store(static_cast<uint8_t>(EventState::Started),
                               std::memory_order_relaxed);
            break;
          case ARDUINO_EVENT_ETH_CONNECTED:
            _event_state.store(static_cast<uint8_t>(EventState::LinkUp),
                               std::memory_order_relaxed);
            break;
          case ARDUINO_EVENT_ETH_GOT_IP:
            _event_state.store(static_cast<uint8_t>(EventState::GotIp),
                               std::memory_order_relaxed);
            noteConnected(millis());
            break;
          case ARDUINO_EVENT_ETH_DISCONNECTED:
            _event_state.store(static_cast<uint8_t>(EventState::LinkDown),
                               std::memory_order_relaxed);
            // Ethernet has no 802.11 reason code; zero means unavailable.
            noteDisconnected(millis(), 0);
            _connected_at.store(0, std::memory_order_relaxed);
            break;
          case ARDUINO_EVENT_ETH_STOP:
            _event_state.store(static_cast<uint8_t>(EventState::Stopped),
                               std::memory_order_relaxed);
            break;
          default:
            break;
        }
      });
      _event_registered = true;
    }
    _started = beginConfiguredCH390();
    if (_started && isConnected()) noteConnected(millis());
    return _started;
  }

  NetworkTransition maintain(uint32_t now_ms, uint8_t) override {
    const bool connected = isConnected();
    if (!_status_initialized) {
      _last_connected = connected;
      _status_initialized = true;
      setOutage(AlertFaultPolicy::applyWifiStatus(
          now_ms, connected, outage(), false));
      if (connected) noteConnected(now_ms);
      return NetworkTransition::None;
    }

    if (connected == _last_connected) {
      if (connected && outage().down) noteConnected(now_ms);
      return NetworkTransition::None;
    }

    _last_connected = connected;
    if (connected) {
      _connected_at.store(now_ms, std::memory_order_relaxed);
      setOutage(AlertFaultPolicy::applyWifiStatus(
          now_ms, true, outage(), true));
      return NetworkTransition::Up;
    }

    const bool outage_was_down = outage().down;
    _connected_at.store(0, std::memory_order_relaxed);
    AlertFaultPolicy::OutageSnapshot snapshot = AlertFaultPolicy::applyWifiStatus(
        now_ms, false, outage(), true);
    setOutage(snapshot);
    if (!outage_was_down) {
      _last_disconnect_time.store(now_ms, std::memory_order_relaxed);
    }
    return NetworkTransition::Down;
  }

  bool isConnected() const override { return _started && CH390.isConnected(); }
  IPAddress localIP() const override { return CH390.localIP(); }
  int rssi() const override { return INT_MIN; }
  bool resolveHost(const char* hostname, IPAddress& address) const override {
    // Arduino's hostByName is a thin wrapper over the process-wide lwIP resolver;
    // DNS follows the selected esp_netif even though this entry point is named WiFi.
    return WiFi.hostByName(hostname, address);
  }
  void formatDiagnostics(char* reply, size_t reply_size) const override {
    snprintf(reply, reply_size, "> ethernet:%s ip=%s",
             statusName(), localIP().toString().c_str());
  }

  EventState eventState() const {
    return static_cast<EventState>(
        _event_state.load(std::memory_order_relaxed));
  }
  bool sampleLink(bool& known) const {
    known = false;
    if (!_started) return false;

    // IEEE 802.3 BMSR link status is latch-low. Read it twice so the second
    // value is the current carrier state rather than a remembered link flap.
    (void)CH390.readPHY(0x01);
    const uint32_t bmsr = CH390.readPHY(0x01) & 0xffffu;
    if (bmsr != 0 && bmsr != 0xffffu) {
      known = true;
      return (bmsr & (1u << 2)) != 0;
    }

    // A failed/unsupported direct PHY read can still use the driver's events.
    const EventState state = eventState();
    known = state == EventState::LinkDown || state == EventState::LinkUp ||
            state == EventState::GotIp || state == EventState::Stopped;
    return state == EventState::LinkUp || state == EventState::GotIp;
  }
  bool linkUp() const {
    bool known = false;
    return sampleLink(known);
  }
  const char* eventName() const {
    switch (eventState()) {
      case EventState::None: return "none";
      case EventState::Started: return "started";
      case EventState::LinkDown: return "link-down";
      case EventState::LinkUp: return "link-up";
      case EventState::GotIp: return "got-ip";
      case EventState::Stopped: return "stopped";
    }
    return "unknown";
  }
};

class AutomaticNetworkInterface final : public NetworkInterface {
  EthernetNetworkInterface _ethernet;
  WiFiNetworkInterface _wifi;
  NetworkMedium _selected = NetworkMedium::None;
  bool _ethernet_started = false;
  bool _wifi_started = false;
  char _wifi_ssid[33] = {};
  char _wifi_password[65] = {};
  uint32_t _ethernet_stable_since = 0;
  uint32_t _selected_down_since = 0;
  std::atomic<uint8_t> _switch_locks{0};

  NetworkInterface& selectedInterface() {
    return _selected == NetworkMedium::Ethernet
        ? static_cast<NetworkInterface&>(_ethernet)
        : static_cast<NetworkInterface&>(_wifi);
  }
  const NetworkInterface& selectedInterface() const {
    return _selected == NetworkMedium::Ethernet
        ? static_cast<const NetworkInterface&>(_ethernet)
        : static_cast<const NetworkInterface&>(_wifi);
  }

  bool wifiConfigured() const { return _wifi_ssid[0] != '\0'; }

  void rememberWifi(const char* ssid, const char* password) {
    strncpy(_wifi_ssid, ssid ? ssid : "", sizeof(_wifi_ssid) - 1);
    _wifi_ssid[sizeof(_wifi_ssid) - 1] = '\0';
    strncpy(_wifi_password, password ? password : "", sizeof(_wifi_password) - 1);
    _wifi_password[sizeof(_wifi_password) - 1] = '\0';
  }

  void startWifiFallback() {
    if (_wifi_started || !wifiConfigured()) return;
    _wifi_started = _wifi.begin(_wifi_ssid, _wifi_password);
  }

  void select(NetworkMedium medium) {
    if (medium == NetworkMedium::Ethernet) {
      // ESP-IDF gives Wi-Fi a higher default-route priority than Ethernet.
      // Keep only the selected STA associated so sockets cannot silently stay
      // on Wi-Fi after the manager has declared Ethernet active.
      WiFi.setAutoReconnect(false);
      WiFi.disconnect(false, false);
      _wifi_started = false;
    }
    _selected = medium;
    _selected_down_since = 0;
  }

 public:
  const char* mediumName() const override {
    if (_selected == NetworkMedium::Ethernet) return "ethernet";
    if (_selected == NetworkMedium::WiFi) return "wifi";
    return "none";
  }
  NetworkMedium medium() const override { return _selected; }
  const char* statusName() const override {
    return _selected == NetworkMedium::None ? "not_selected"
                                            : selectedInterface().statusName();
  }
  int statusCode() const override {
    return _selected == NetworkMedium::None ? 0 : selectedInterface().statusCode();
  }
  bool configValid(const char* wifi_ssid) const override {
    // Hardware availability and stored credentials are configuration. Current
    // link/DHCP state is runtime state and must not permanently suppress the
    // MQTT task that monitors for a late cable or lease.
    return _ethernet_started || (wifi_ssid && wifi_ssid[0] != '\0');
  }
  bool isAutomatic() const override { return true; }

  bool begin(const char* wifi_ssid, const char* wifi_password) override {
    rememberWifi(wifi_ssid, wifi_password);
    if (!_ethernet_started) {
      _ethernet_started = _ethernet.begin(nullptr, nullptr);
    }
    // bootstrap() owns the initial choice. MQTT begin() is intentionally
    // idempotent and cannot demote a boot-selected Ethernet link because of a
    // momentary status sample between tasks.
    if (_selected == NetworkMedium::None) {
      if (_ethernet.isConnected()) {
        select(NetworkMedium::Ethernet);
      } else if (wifiConfigured()) {
        startWifiFallback();
        _selected = NetworkMedium::WiFi;
      }
    } else if (_selected == NetworkMedium::WiFi) {
      startWifiFallback();
    }
    return _ethernet_started || _wifi_started;
  }

  bool bootstrap(const char* wifi_ssid, const char* wifi_password,
                 uint32_t wait_ms) override {
    rememberWifi(wifi_ssid, wifi_password);
    if (!_ethernet_started) {
      _ethernet_started = _ethernet.begin(nullptr, nullptr);
    }

    const uint32_t started_at = millis();
    while (NetworkPolicy::ethernetBootProbePending(
        _ethernet_started, _ethernet.isConnected(),
        (uint32_t)(millis() - started_at), wait_ms)) {
      delay(25);
    }

    const NetworkMedium initial = NetworkPolicy::bootSelection(
        _ethernet.isConnected(), wifiConfigured());
    if (initial == NetworkMedium::Ethernet) {
      select(initial);
    } else {
      startWifiFallback();
      _selected = initial;
    }
    return initial != NetworkMedium::None;
  }

  NetworkTransition maintain(uint32_t now_ms, uint8_t wifi_power_save) override {
    const NetworkTransition ethernet_transition =
        _ethernet.maintain(now_ms, wifi_power_save);
    const NetworkTransition wifi_transition = _wifi_started
        ? _wifi.maintain(now_ms, wifi_power_save)
        : NetworkTransition::None;

    // Ethernet may recover while a Wi-Fi fallback is still associating. In
    // that sequence the selected enum never changes, but Wi-Fi would win
    // ESP-IDF's default-route priority once it came up. Tear the unused STA
    // down even without a selection edge, and force MQTT to reconnect if it
    // had already become reachable.
    if (_selected == NetworkMedium::Ethernet && _ethernet.isConnected() &&
        _wifi_started) {
      const bool wifi_had_route = _wifi.isConnected();
      select(NetworkMedium::Ethernet);
      if (wifi_had_route) return NetworkTransition::Switched;
    }

    if (_ethernet.isConnected()) {
      if (_ethernet_stable_since == 0) _ethernet_stable_since = now_ms;
    } else {
      _ethernet_stable_since = 0;
    }

    const bool selected_connected = isConnected();
    if (!selected_connected) {
      if (_selected_down_since == 0) _selected_down_since = now_ms;
    } else {
      _selected_down_since = 0;
    }

    const uint32_t selected_down_ms = _selected_down_since == 0
        ? 0 : (uint32_t)(now_ms - _selected_down_since);
    if (_selected == NetworkMedium::Ethernet && !_ethernet.isConnected() &&
        selected_down_ms >= NetworkPolicy::kEthernetDownGraceMs) {
      startWifiFallback();
    }

    const uint32_t ethernet_stable_ms = _ethernet_stable_since == 0
        ? 0 : (uint32_t)(now_ms - _ethernet_stable_since);
    const NetworkPolicy::AutomaticSelectionInput input = {
        _selected, _ethernet.isConnected(),
        _wifi_started && _wifi.isConnected(), wifiConfigured(),
        _switch_locks.load(std::memory_order_relaxed) != 0,
        ethernet_stable_ms, selected_down_ms};
    const NetworkMedium next = NetworkPolicy::automaticSelection(input);

    if (next != _selected) {
      const NetworkMedium previous = _selected;
      select(next);
      return previous == NetworkMedium::None ? NetworkTransition::Up
                                             : NetworkTransition::Switched;
    }

    if (_selected == NetworkMedium::Ethernet) return ethernet_transition;
    if (_selected == NetworkMedium::WiFi) return wifi_transition;
    return NetworkTransition::None;
  }

  void lockSwitching() override {
    _switch_locks.fetch_add(1, std::memory_order_relaxed);
  }
  void unlockSwitching() override {
    uint8_t value = _switch_locks.load(std::memory_order_relaxed);
    while (value != 0 && !_switch_locks.compare_exchange_weak(
               value, static_cast<uint8_t>(value - 1),
               std::memory_order_relaxed, std::memory_order_relaxed)) {}
  }

  bool isConnected() const override {
    return _selected != NetworkMedium::None && selectedInterface().isConnected();
  }
  IPAddress localIP() const override {
    return _selected == NetworkMedium::None ? IPAddress() : selectedInterface().localIP();
  }
  int rssi() const override {
    return _selected == NetworkMedium::None ? INT_MIN : selectedInterface().rssi();
  }
  bool resolveHost(const char* hostname, IPAddress& address) const override {
    return _selected != NetworkMedium::None &&
           selectedInterface().resolveHost(hostname, address);
  }
  void formatDiagnostics(char* reply, size_t reply_size) const override {
    const bool ethernet_connected = _ethernet.isConnected();
    bool ethernet_link_known = false;
    bool ethernet_link_up = _ethernet.sampleLink(ethernet_link_known);
    ethernet_link_known = ethernet_link_known || ethernet_connected;
    ethernet_link_up = ethernet_link_up || ethernet_connected;
    const bool switching_locked =
        _switch_locks.load(std::memory_order_relaxed) != 0;
    const uint32_t now_ms = millis();
    const uint32_t ethernet_stable_ms = _ethernet_stable_since == 0
        ? 0 : (uint32_t)(now_ms - _ethernet_stable_since);
    const NetworkDiagnosticReason reason =
        NetworkPolicy::automaticDiagnosticReason(
            _ethernet_started, ethernet_link_known, ethernet_link_up,
            ethernet_connected, _selected, switching_locked,
            ethernet_stable_ms);
    snprintf(reply, reply_size,
             "> why:%s selected:%s lock:%s\n"
             "eth:init:%s evt:%s link:%s ip:%s\n"
             "wifi:cfg:%s started:%s link:%s",
             NetworkPolicy::diagnosticReasonName(reason), mediumName(),
             switching_locked ? "yes" : "no",
             _ethernet_started ? "ok" : "failed", _ethernet.eventName(),
             ethernet_link_known ? (ethernet_link_up ? "up" : "down")
                                 : "unknown",
             _ethernet.localIP().toString().c_str(),
             wifiConfigured() ? "yes" : "no", _wifi_started ? "yes" : "no",
             (_wifi_started && _wifi.isConnected()) ? "up" : "down");
  }
  unsigned long connectedAtMillis() const override {
    return _selected == NetworkMedium::None ? 0 : selectedInterface().connectedAtMillis();
  }
  uint8_t lastDisconnectReason() const override {
    return _selected == NetworkMedium::None ? 0 : selectedInterface().lastDisconnectReason();
  }
  unsigned long lastDisconnectTime() const override {
    return _selected == NetworkMedium::None ? 0 : selectedInterface().lastDisconnectTime();
  }
  AlertFaultPolicy::OutageSnapshot outageSnapshot() const override {
    return _selected == NetworkMedium::None
        ? AlertFaultPolicy::OutageSnapshot{false, 0, 0}
        : selectedInterface().outageSnapshot();
  }
};
#endif

}  // namespace

NetworkInterface& activeNetworkInterface() {
#if defined(NETWORK_PREFER_ETHERNET)
  static AutomaticNetworkInterface network;
#else
  static WiFiNetworkInterface network;
#endif
  return network;
}
#endif
