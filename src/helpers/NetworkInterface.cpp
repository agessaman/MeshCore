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

// Arduino-ESP32's built-in ETH implementation calls this before bringing up
// Ethernet. It creates the shared Arduino network event group/task used by
// WiFiGenericClass::hostByName(), even when no Wi-Fi interface is started.
// ESP32-CH390 initializes esp_netif directly and omits this Arduino layer.
extern void tcpipInit();
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
  char _hostname[32] = {};
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

  void setHostname(const char* hostname) override {
    strncpy(_hostname, hostname ? hostname : "", sizeof(_hostname) - 1);
    _hostname[sizeof(_hostname) - 1] = '\0';
  }

  bool begin(const char* wifi_ssid, const char* wifi_password) override {
    if (!configValid(wifi_ssid)) return false;
    strncpy(_ssid, wifi_ssid, sizeof(_ssid) - 1);
    _ssid[sizeof(_ssid) - 1] = '\0';
    strncpy(_password, wifi_password ? wifi_password : "", sizeof(_password) - 1);
    _password[sizeof(_password) - 1] = '\0';

    // Arduino-ESP32 applies this stored value when it creates the STA netif.
    // It must be set before WiFi.mode()/begin() for the first DHCP exchange.
    if (_hostname[0] != '\0') WiFi.setHostname(_hostname);
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
  std::atomic<bool> _started{false};
  bool _event_registered = false;
  char _hostname[32] = {};
  std::atomic<uint8_t> _event_state{static_cast<uint8_t>(EventState::None)};

 public:
  const char* mediumName() const override { return "ethernet"; }
  NetworkMedium medium() const override { return NetworkMedium::Ethernet; }
  const char* statusName() const override {
    return isConnected() ? "connected" : "disconnected";
  }
  int statusCode() const override { return isConnected() ? 1 : 0; }
  bool configValid(const char*) const override { return true; }

  void setHostname(const char* hostname) override {
    strncpy(_hostname, hostname ? hostname : "", sizeof(_hostname) - 1);
    _hostname[sizeof(_hostname) - 1] = '\0';
  }

  bool begin(const char*, const char*) override {
    if (_started.load(std::memory_order_acquire)) return true;
    // DNS and WiFiClientSecure are transport-neutral sockets in this Arduino
    // core, but their hostname path still uses WiFiGeneric's event group.
    // Initialize that shared runtime without enabling or associating Wi-Fi.
    tcpipInit();
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
    _event_state.store(static_cast<uint8_t>(EventState::None),
                       std::memory_order_relaxed);
    const bool started = beginConfiguredCH390(_hostname);
    _started.store(started, std::memory_order_release);
    if (started && isConnected()) noteConnected(millis());
    return started;
  }

  bool restart() {
    CH390.end();
    _started.store(false, std::memory_order_release);
    // Preserve status/outage history across attempts. maintain() observes the
    // resulting edge in this same task iteration, so a retry cannot reset a
    // prolonged-down alert timer or hide a previously connected transition.
    return begin(nullptr, nullptr);
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

  bool isConnected() const override {
    return _started.load(std::memory_order_acquire) && CH390.isConnected();
  }
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
  bool started() const { return _started.load(std::memory_order_acquire); }
  bool sampleLink(bool& known) const {
    known = false;
    if (!_started.load(std::memory_order_acquire)) return false;

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
  std::atomic<NetworkMedium> _selected{NetworkMedium::None};
  std::atomic<bool> _ethernet_started{false};
  std::atomic<bool> _wifi_started{false};
  char _wifi_ssid[33] = {};
  char _wifi_password[65] = {};
  std::atomic<uint32_t> _ethernet_stable_since{0};
  std::atomic<uint32_t> _selected_down_since{0};
  std::atomic<uint32_t> _last_ethernet_init_attempt{0};
  std::atomic<uint8_t> _ethernet_retry_attempt{0};
  std::atomic<uint32_t> _ethernet_no_ip_since{0};
  std::atomic<uint8_t> _switch_locks{0};
  std::atomic<bool> _switch_in_progress{false};

  NetworkInterface& selectedInterface(NetworkMedium selected) {
    return selected == NetworkMedium::Ethernet
        ? static_cast<NetworkInterface&>(_ethernet)
        : static_cast<NetworkInterface&>(_wifi);
  }
  const NetworkInterface& selectedInterface(NetworkMedium selected) const {
    return selected == NetworkMedium::Ethernet
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
    if (_wifi_started.load(std::memory_order_acquire) || !wifiConfigured()) return;
    _wifi_started.store(_wifi.begin(_wifi_ssid, _wifi_password),
                        std::memory_order_release);
  }

  bool beginUnlockedMutation() {
    bool expected = false;
    if (!_switch_in_progress.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
      return false;
    }
    if (_switch_locks.load(std::memory_order_acquire) != 0) {
      _switch_in_progress.store(false, std::memory_order_release);
      return false;
    }
    return true;
  }

  void endUnlockedMutation() {
    _switch_in_progress.store(false, std::memory_order_release);
  }

  bool selectIfUnlocked(NetworkMedium medium) {
    if (!beginUnlockedMutation()) return false;
    select(medium);
    endUnlockedMutation();
    return true;
  }

  void startWifiFallbackIfUnlocked() {
    if (!beginUnlockedMutation()) return;
    startWifiFallback();
    endUnlockedMutation();
  }

  void select(NetworkMedium medium) {
    if (medium == NetworkMedium::Ethernet) {
      // ESP-IDF gives Wi-Fi a higher default-route priority than Ethernet.
      // Keep only the selected STA associated so sockets cannot silently stay
      // on Wi-Fi after the manager has declared Ethernet active.
      WiFi.setAutoReconnect(false);
      WiFi.disconnect(false, false);
      _wifi_started.store(false, std::memory_order_release);
    }
    _selected_down_since.store(0, std::memory_order_relaxed);
    _selected.store(medium, std::memory_order_release);
  }

  bool startOrRetryEthernet(uint32_t now_ms, bool restart) {
    const bool started = restart ? _ethernet.restart()
                                 : _ethernet.begin(nullptr, nullptr);
    _last_ethernet_init_attempt.store(now_ms, std::memory_order_relaxed);
    _ethernet_started.store(started, std::memory_order_release);
    if (started) {
      _ethernet_retry_attempt.store(0, std::memory_order_relaxed);
    } else {
      uint8_t attempt = _ethernet_retry_attempt.load(std::memory_order_relaxed);
      if (attempt != UINT8_MAX) ++attempt;
      _ethernet_retry_attempt.store(attempt, std::memory_order_relaxed);
    }
    return started;
  }

 public:
  const char* mediumName() const override {
    const NetworkMedium selected = _selected.load(std::memory_order_acquire);
    if (selected == NetworkMedium::Ethernet) return "ethernet";
    if (selected == NetworkMedium::WiFi) return "wifi";
    return "none";
  }
  NetworkMedium medium() const override {
    return _selected.load(std::memory_order_acquire);
  }
  const char* statusName() const override {
    const NetworkMedium selected = _selected.load(std::memory_order_acquire);
    return selected == NetworkMedium::None ? "not_selected"
                                            : selectedInterface(selected).statusName();
  }
  int statusCode() const override {
    const NetworkMedium selected = _selected.load(std::memory_order_acquire);
    return selected == NetworkMedium::None ? 0
                                           : selectedInterface(selected).statusCode();
  }
  bool configValid(const char* wifi_ssid) const override {
    // Hardware availability and stored credentials are configuration. Current
    // link/DHCP state is runtime state and must not permanently suppress the
    // MQTT task that monitors for a late cable or lease.
    (void)wifi_ssid;
    return true;
  }
  bool isAutomatic() const override { return true; }

  void setHostname(const char* hostname) override {
    // Whichever medium wins now or during a later failover presents the same
    // stable DHCP identity to the LAN.
    _ethernet.setHostname(hostname);
    _wifi.setHostname(hostname);
  }

  bool begin(const char* wifi_ssid, const char* wifi_password) override {
    rememberWifi(wifi_ssid, wifi_password);
    if (!_ethernet_started.load(std::memory_order_acquire)) {
      const uint32_t now_ms = millis();
      const uint8_t attempt =
          _ethernet_retry_attempt.load(std::memory_order_relaxed);
      if (NetworkPolicy::ethernetInitRetryDue(
              attempt, now_ms,
              _last_ethernet_init_attempt.load(std::memory_order_relaxed))) {
        startOrRetryEthernet(now_ms, attempt != 0);
      }
    }
    // bootstrap() owns the initial choice. MQTT begin() is intentionally
    // idempotent and cannot demote a boot-selected Ethernet link because of a
    // momentary status sample between tasks.
    const NetworkMedium selected = _selected.load(std::memory_order_acquire);
    if (selected == NetworkMedium::None) {
      if (_ethernet.isConnected()) {
        select(NetworkMedium::Ethernet);
      } else if (wifiConfigured()) {
        startWifiFallback();
        _selected.store(NetworkMedium::WiFi, std::memory_order_release);
      }
    } else if (selected == NetworkMedium::WiFi) {
      startWifiFallback();
    }
    return _ethernet_started.load(std::memory_order_acquire) ||
           _wifi_started.load(std::memory_order_acquire);
  }

  bool bootstrap(const char* wifi_ssid, const char* wifi_password,
                 uint32_t wait_ms) override {
    rememberWifi(wifi_ssid, wifi_password);
    if (!_ethernet_started.load(std::memory_order_acquire)) {
      startOrRetryEthernet(millis(), false);
    }

    const uint32_t started_at = millis();
    for (;;) {
      bool link_known = false;
      const bool link_up = _ethernet.sampleLink(link_known);
      if (!NetworkPolicy::ethernetBootProbePending(
              _ethernet_started.load(std::memory_order_acquire),
              _ethernet.isConnected(), link_known, link_up,
              (uint32_t)(millis() - started_at), wait_ms)) {
        break;
      }
      delay(25);
    }

    const NetworkMedium initial = NetworkPolicy::bootSelection(
        _ethernet.isConnected(), wifiConfigured());
    if (initial == NetworkMedium::Ethernet) {
      select(initial);
    } else {
      startWifiFallback();
      _selected.store(initial, std::memory_order_release);
    }
    return initial != NetworkMedium::None;
  }

  NetworkTransition maintain(uint32_t now_ms, uint8_t wifi_power_save) override {
    bool ethernet_started = _ethernet_started.load(std::memory_order_acquire);
    const bool switching_locked =
        _switch_locks.load(std::memory_order_acquire) != 0;
    const bool ethernet_stopped =
        ethernet_started &&
        _ethernet.eventState() == EthernetNetworkInterface::EventState::Stopped;
    if (ethernet_stopped) {
      _ethernet_started.store(false, std::memory_order_release);
      ethernet_started = false;
      if (_ethernet_retry_attempt.load(std::memory_order_relaxed) == 0) {
        _ethernet_retry_attempt.store(1, std::memory_order_relaxed);
        _last_ethernet_init_attempt.store(now_ms, std::memory_order_relaxed);
      }
    }
    const EthernetNetworkInterface::EventState ethernet_event =
        _ethernet.eventState();
    const bool ethernet_link_up =
        ethernet_event == EthernetNetworkInterface::EventState::LinkUp ||
        ethernet_event == EthernetNetworkInterface::EventState::GotIp;
    if (ethernet_started && ethernet_link_up && !_ethernet.isConnected()) {
      if (_ethernet_no_ip_since.load(std::memory_order_relaxed) == 0) {
        uint32_t started_at = now_ms;
        if (started_at == 0) started_at = 1;
        _ethernet_no_ip_since.store(started_at, std::memory_order_relaxed);
      }
    } else {
      _ethernet_no_ip_since.store(0, std::memory_order_relaxed);
    }
    if (!switching_locked && NetworkPolicy::ethernetNoIpRecoveryDue(
            ethernet_started, ethernet_link_up, _ethernet.isConnected(),
            now_ms, _ethernet_no_ip_since.load(std::memory_order_relaxed))) {
      ethernet_started = startOrRetryEthernet(now_ms, true);
      _ethernet_no_ip_since.store(0, std::memory_order_relaxed);
    }
    if (!ethernet_started && !switching_locked) {
      const uint8_t attempt =
          _ethernet_retry_attempt.load(std::memory_order_relaxed);
      if (NetworkPolicy::ethernetInitRetryDue(
              attempt, now_ms,
              _last_ethernet_init_attempt.load(std::memory_order_relaxed))) {
        ethernet_started = startOrRetryEthernet(now_ms, true);
      }
    }

    const NetworkTransition ethernet_transition =
        _ethernet.maintain(now_ms, wifi_power_save);
    const bool wifi_started = _wifi_started.load(std::memory_order_acquire);
    const NetworkTransition wifi_transition = wifi_started
        ? _wifi.maintain(now_ms, wifi_power_save)
        : NetworkTransition::None;

    // Ethernet may recover while a Wi-Fi fallback is still associating. In
    // that sequence the selected enum never changes, but Wi-Fi would win
    // ESP-IDF's default-route priority once it came up. Tear the unused STA
    // down even without a selection edge, and force MQTT to reconnect if it
    // had already become reachable.
    NetworkMedium selected = _selected.load(std::memory_order_acquire);
    if (selected == NetworkMedium::Ethernet && _ethernet.isConnected() &&
        wifi_started) {
      const bool wifi_had_route = _wifi.isConnected();
      if (selectIfUnlocked(NetworkMedium::Ethernet) && wifi_had_route) {
        return NetworkTransition::Switched;
      }
    }

    if (_ethernet.isConnected()) {
      if (_ethernet_stable_since.load(std::memory_order_relaxed) == 0) {
        _ethernet_stable_since.store(now_ms, std::memory_order_relaxed);
      }
    } else {
      _ethernet_stable_since.store(0, std::memory_order_relaxed);
    }

    const bool selected_connected = isConnected();
    if (!selected_connected) {
      if (_selected_down_since.load(std::memory_order_relaxed) == 0) {
        _selected_down_since.store(now_ms, std::memory_order_relaxed);
      }
    } else {
      _selected_down_since.store(0, std::memory_order_relaxed);
    }

    const uint32_t selected_down_since =
        _selected_down_since.load(std::memory_order_relaxed);
    const uint32_t selected_down_ms = selected_down_since == 0
        ? 0 : (uint32_t)(now_ms - selected_down_since);
    selected = _selected.load(std::memory_order_acquire);
    if (selected == NetworkMedium::Ethernet && !_ethernet.isConnected() &&
        selected_down_ms >= NetworkPolicy::kEthernetDownGraceMs) {
      startWifiFallbackIfUnlocked();
    }

    const uint32_t ethernet_stable_since =
        _ethernet_stable_since.load(std::memory_order_relaxed);
    const uint32_t ethernet_stable_ms = ethernet_stable_since == 0
        ? 0 : (uint32_t)(now_ms - ethernet_stable_since);
    const bool selection_locked =
        _switch_locks.load(std::memory_order_acquire) != 0;
    const NetworkPolicy::AutomaticSelectionInput input = {
        selected, _ethernet.isConnected(),
        _wifi_started.load(std::memory_order_acquire) && _wifi.isConnected(),
        wifiConfigured(), selection_locked,
        ethernet_stable_ms, selected_down_ms};
    const NetworkMedium next = NetworkPolicy::automaticSelection(input);

    if (next != selected) {
      if (selectIfUnlocked(next)) {
        return selected == NetworkMedium::None ? NetworkTransition::Up
                                               : NetworkTransition::Switched;
      }
    }

    if (selected == NetworkMedium::Ethernet) return ethernet_transition;
    if (selected == NetworkMedium::WiFi) return wifi_transition;
    return NetworkTransition::None;
  }

  void lockSwitching() override {
    uint8_t value = _switch_locks.load(std::memory_order_relaxed);
    while (value != UINT8_MAX && !_switch_locks.compare_exchange_weak(
               value, static_cast<uint8_t>(value + 1),
               std::memory_order_acq_rel, std::memory_order_relaxed)) {}
    while (_switch_in_progress.load(std::memory_order_acquire)) delay(1);
  }
  void unlockSwitching() override {
    uint8_t value = _switch_locks.load(std::memory_order_relaxed);
    while (value != 0 && !_switch_locks.compare_exchange_weak(
               value, static_cast<uint8_t>(value - 1),
               std::memory_order_relaxed, std::memory_order_relaxed)) {}
  }

  bool isConnected() const override {
    const NetworkMedium selected = _selected.load(std::memory_order_acquire);
    return selected != NetworkMedium::None &&
           selectedInterface(selected).isConnected();
  }
  IPAddress localIP() const override {
    const NetworkMedium selected = _selected.load(std::memory_order_acquire);
    return selected == NetworkMedium::None ? IPAddress()
                                           : selectedInterface(selected).localIP();
  }
  int rssi() const override {
    const NetworkMedium selected = _selected.load(std::memory_order_acquire);
    return selected == NetworkMedium::None ? INT_MIN
                                           : selectedInterface(selected).rssi();
  }
  bool resolveHost(const char* hostname, IPAddress& address) const override {
    const NetworkMedium selected = _selected.load(std::memory_order_acquire);
    return selected != NetworkMedium::None &&
           selectedInterface(selected).resolveHost(hostname, address);
  }
  void formatDiagnostics(char* reply, size_t reply_size) const override {
    const NetworkMedium selected = _selected.load(std::memory_order_acquire);
    const bool ethernet_started =
        _ethernet_started.load(std::memory_order_acquire);
    const bool wifi_started = _wifi_started.load(std::memory_order_acquire);
    const bool ethernet_connected = _ethernet.isConnected();
    bool ethernet_link_known = false;
    bool ethernet_link_up = _ethernet.sampleLink(ethernet_link_known);
    ethernet_link_known = ethernet_link_known || ethernet_connected;
    ethernet_link_up = ethernet_link_up || ethernet_connected;
    const bool switching_locked =
        _switch_locks.load(std::memory_order_acquire) != 0;
    const uint32_t now_ms = millis();
    const uint32_t ethernet_stable_since =
        _ethernet_stable_since.load(std::memory_order_relaxed);
    const uint32_t ethernet_stable_ms = ethernet_stable_since == 0
        ? 0 : (uint32_t)(now_ms - ethernet_stable_since);
    const uint8_t retry_attempt =
        _ethernet_retry_attempt.load(std::memory_order_relaxed);
    const uint32_t retry_delay =
        NetworkPolicy::ethernetInitRetryDelayMs(retry_attempt);
    const uint32_t since_attempt = (uint32_t)(
        now_ms - _last_ethernet_init_attempt.load(std::memory_order_relaxed));
    const uint32_t retry_in = !ethernet_started && retry_delay > since_attempt
        ? retry_delay - since_attempt : 0;
    const NetworkDiagnosticReason reason =
        NetworkPolicy::automaticDiagnosticReason(
            ethernet_started, ethernet_link_known, ethernet_link_up,
            ethernet_connected, selected, switching_locked,
            ethernet_stable_ms);
    snprintf(reply, reply_size,
             "> why:%s selected:%s lock:%s\n"
             "eth:init:%s retry:%u/%lums evt:%s link:%s ip:%s\n"
             "wifi:cfg:%s started:%s link:%s",
             NetworkPolicy::diagnosticReasonName(reason),
             NetworkPolicy::mediumName(selected),
             switching_locked ? "yes" : "no",
             ethernet_started ? "ok" : "failed", retry_attempt,
             (unsigned long)retry_in, _ethernet.eventName(),
             ethernet_link_known ? (ethernet_link_up ? "up" : "down")
                                 : "unknown",
             _ethernet.localIP().toString().c_str(),
             wifiConfigured() ? "yes" : "no", wifi_started ? "yes" : "no",
             (wifi_started && _wifi.isConnected()) ? "up" : "down");
  }
  unsigned long connectedAtMillis() const override {
    const NetworkMedium selected = _selected.load(std::memory_order_acquire);
    return selected == NetworkMedium::None
        ? 0 : selectedInterface(selected).connectedAtMillis();
  }
  uint8_t lastDisconnectReason() const override {
    const NetworkMedium selected = _selected.load(std::memory_order_acquire);
    return selected == NetworkMedium::None
        ? 0 : selectedInterface(selected).lastDisconnectReason();
  }
  unsigned long lastDisconnectTime() const override {
    const NetworkMedium selected = _selected.load(std::memory_order_acquire);
    return selected == NetworkMedium::None
        ? 0 : selectedInterface(selected).lastDisconnectTime();
  }
  AlertFaultPolicy::OutageSnapshot outageSnapshot() const override {
    const NetworkMedium selected = _selected.load(std::memory_order_acquire);
    return selected == NetworkMedium::None
        ? AlertFaultPolicy::OutageSnapshot{false, 0, 0}
        : selectedInterface(selected).outageSnapshot();
  }
  NetworkMedium alertMedium() const override {
    return NetworkMedium::Ethernet;
  }
  AlertFaultPolicy::OutageSnapshot alertOutageSnapshot() const override {
    return _ethernet.outageSnapshot();
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
