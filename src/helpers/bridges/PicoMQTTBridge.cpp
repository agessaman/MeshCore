#if defined(PICO_MQTT_OBSERVER)
#include "PicoMQTTBridge.h"
#include "helpers/mqtt/PicoObserverMqttClient.h"
#include "helpers/mqtt/PicoObserverConfigPolicy.h"
#include "helpers/MQTTMessageBuilder.h"
#include "helpers/MQTTPacketFilter.h"
#include "helpers/MQTTPacketQueuePolicy.h"
#include "helpers/MQTTConnectionPolicy.h"
#include "helpers/MQTTPresetPolicy.h"
#include "helpers/MQTTTopicRouter.h"
#include "helpers/NetworkHostname.h"
#include "helpers/NtpValidation.h"
#include "helpers/JWTHelper.h"
#include "helpers/AdvertSignature.h"
#include "helpers/TxtDataHelpers.h"
#include <WiFiUdp.h>
#include <sys/time.h>
#include <pico/rand.h>
#include <lwip/init.h>
#include <memory>
#include <new>
#include <math.h>

MQTTBridge* MQTTBridge::_instance = nullptr;

namespace {
class BoundedJsonAllocator : public ArduinoJson::Allocator {
  struct alignas(max_align_t) Header { size_t size; };
  size_t _used = 0;
  static constexpr size_t kLimit = 8192;
public:
  void* allocate(size_t size) override {
    if (size > kLimit - _used) return nullptr;
    auto* header = static_cast<Header*>(malloc(sizeof(Header) + size));
    if (!header) return nullptr;
    header->size = size; _used += size;
    return header + 1;
  }
  void deallocate(void* value) override {
    if (!value) return;
    auto* header = static_cast<Header*>(value) - 1;
    _used -= header->size; free(header);
  }
  void* reallocate(void* value, size_t size) override {
    if (!value) return allocate(size);
    if (!size) { deallocate(value); return nullptr; }
    auto* header = static_cast<Header*>(value) - 1;
    const size_t old = header->size;
    if (size > kLimit - (_used - old)) return nullptr;
    auto* next = static_cast<Header*>(realloc(header, sizeof(Header) + size));
    if (!next) return nullptr;
    next->size = size; _used = _used - old + size;
    return next + 1;
  }
};

struct Runtime {
  PicoObserverMqttClient client;
  BoundedJsonAllocator allocator;
  JsonDocument json{&allocator};
  char payload[2048], topic[256], auth[768], username[70], client_version[80];
  uint32_t expires = 0;
  uint32_t last_attempt = 0, ntp_at = 0, last_status = 0;
  uint8_t backoff = 0, max_failures = 0, ntp_server = 0;
  bool ntp_scheduled = false;
  bool attempted = false, tripped = false;
};

bool probeNtp(const char* host, uint32_t& epoch, const std::atomic<bool>& stop) {
  IPAddress ip;
  if (!activeNetworkLink().resolveHost(host, ip) || stop.load()) return false;
  WiFiUDP udp;
  if (!udp.begin(0)) return false;
  const NtpValidation::Nonce nonce{get_rand_32(), get_rand_32()};
  uint8_t bytes[NtpValidation::kPacketSize];
  NtpValidation::buildRequest(bytes, nonce);
  bool sent = udp.beginPacket(ip, 123) && udp.write(bytes, sizeof(bytes)) == sizeof(bytes) && udp.endPacket();
  const uint32_t start = millis();
  while (sent && !stop.load() && millis() - start < 1000) {
    const int size = udp.parsePacket();
    if (size > 0) {
      const bool source = udp.remoteIP() == ip && udp.remotePort() == 123;
      const int count = udp.read(bytes, sizeof(bytes));
      const auto result = NtpValidation::validate(bytes, count > 0 ? count : 0, source,
                                                 nonce, 1767225600UL, 4102444800UL);
      if (result.reject == NtpValidation::kAccepted) { epoch = result.epoch; udp.stop(); return true; }
    }
    delay(5);
  }
  udp.stop();
  return false;
}
}

MQTTBridge::MQTTBridge(NodePrefs* prefs, MQTTPrefs* obs, mesh::PacketManager* mgr,
                     mesh::RTCClock* rtc, mesh::LocalIdentity* identity)
  : BridgeBase(prefs, mgr, rtc), _obs(obs), _identity(identity) {
  _health_mutex = xSemaphoreCreateMutex();
}

MQTTBridge::~MQTTBridge() {
  // App retains the bridge for its lifetime; no owner may delete a live worker.
  configASSERT(!_task);
  if (_health_mutex) vSemaphoreDelete(_health_mutex);
}

MQTTBridge::Health MQTTBridge::readHealth() const {
  if (!_health_mutex) return Health{};
  xSemaphoreTake(_health_mutex, portMAX_DELAY);
  const Health value = _health;
  xSemaphoreGive(_health_mutex);
  return value;
}

void MQTTBridge::setDeviceID(const char* value) { snprintf(_last_config.device, sizeof(_last_config.device), "%s", value); }
void MQTTBridge::setFirmwareVersion(const char* value) { snprintf(_last_config.firmware, sizeof(_last_config.firmware), "%s", value); }
void MQTTBridge::setBoardModel(const char* value) { snprintf(_last_config.model, sizeof(_last_config.model), "%s", value); }
void MQTTBridge::setBuildDate(const char* value) { snprintf(_last_config.date, sizeof(_last_config.date), "%s", value); }
void MQTTBridge::setStatsSources(mesh::Dispatcher* dispatcher, mesh::Radio* radio,
                               mesh::MainBoard* board, mesh::MillisecondClock*) {
  _dispatcher = dispatcher; _radio = radio; _board = board;
}

void MQTTBridge::getEffectiveMqttOrigin(const NodePrefs* prefs, const MQTTPrefs* obs, char* out, size_t size) {
  snprintf(out, size, "%s", obs && obs->mqtt_origin[0] ? obs->mqtt_origin : prefs ? prefs->node_name : "");
  StrHelper::stripSurroundingQuotes(out, size);
}

bool MQTTBridge::isConfigValid(const MQTTPrefs* obs) {
  if (!obs || !obs->wifi_ssid[0]) return false;
  const bool custom = !strcmp(obs->mqtt_slot_preset[0], "custom");
  const auto* preset = custom ? nullptr : findMQTTPreset(obs->mqtt_slot_preset[0]);
  if (!custom && !preset) return false;
  ObserverTransportPolicy::Endpoint endpoint;
  if (custom && !ObserverTransportPolicy::customEndpoint(obs->mqtt_slot_host[0], obs->mqtt_slot_port[0], endpoint))
    return false;
  if (preset && preset->topic_style == MQTT_TOPIC_MESHRANK) return obs->mqtt_slot_token[0][0];
  const char* iata = mqttSlotIata(obs->mqtt_iata, obs->mqtt_slot_iata[0]);
  return (custom && obs->mqtt_slot_topic[0][0]) || (mqttIataValid(iata) && strcmp(iata, "XXX"));
}

void MQTTBridge::snapshotConfiguration() {
  if (!_config_queue) return;
  Configuration next = _last_config;
  next.observer = *_obs;
  next.identity = *_identity;
  mesh::Utils::toHex(next.device, _identity->pub_key, PUB_KEY_SIZE);
  getEffectiveMqttOrigin(_prefs, _obs, next.origin, sizeof(next.origin));
  next.frequency = _prefs->freq; next.bandwidth = _prefs->bw;
  next.sf = _prefs->sf; next.cr = _prefs->cr; next.repeat = !_prefs->disable_fwd;
  if (_last_config.generation && !memcmp(&next, &_last_config, sizeof(next))) return;
  ++next.generation;
  _last_config = next;
  xQueueOverwrite(_config_queue, &next);
}

void MQTTBridge::begin() {
  if (_initialized || isStopUnproven()) return;
  if (!_health_mutex) return;
  _captures = xQueueCreate(6, sizeof(Capture));
  _config_queue = xQueueCreate(1, sizeof(Configuration));
  _rtc_updates = xQueueCreate(1, sizeof(uint32_t));
  if (!_captures || !_config_queue || !_rtc_updates) { release(); return; }
  _stop.store(false); _acked.store(false); _time_valid.store(false);
  xSemaphoreTake(_health_mutex, portMAX_DELAY); _health = Health{}; xSemaphoreGive(_health_mutex);
  _last_config.generation = 0;
  snapshotConfiguration();
  _state = MQTTLifecycle::apply(_state, MQTTLifecycle::Event::StartRequested).next;
  // Create the core's lwIP dispatcher on core 0 before the network worker runs.
  lwip_init();
  if (xTaskCreateAffinitySet(taskEntry, "pico-mqtt", 16384 / sizeof(StackType_t),
                            this, 1, 2, &_task) != pdPASS) {
    _task = nullptr; release();
    _state = MQTTLifecycle::apply(_state, MQTTLifecycle::Event::StartFailed).next;
    return;
  }
  _initialized = true;
  _state = MQTTLifecycle::apply(_state, MQTTLifecycle::Event::StartCompleted).next;
  _instance = this;
}

void MQTTBridge::end() {
  if (!_task) return;
  if (_stop.load()) return;
  _initialized = false;
  _stop_started = millis();
  _stop.store(true, std::memory_order_release);
  _state = MQTTLifecycle::apply(_state, MQTTLifecycle::Event::StopRequested).next;
}

void MQTTBridge::release() {
  if (_captures) vQueueDelete(_captures);
  if (_config_queue) vQueueDelete(_config_queue);
  if (_rtc_updates) vQueueDelete(_rtc_updates);
  _captures = _config_queue = _rtc_updates = nullptr;
}

void MQTTBridge::pollLateStopAck() {
  if (!stopAcknowledgedLate()) return;
  _task = nullptr; release();
  _state = MQTTLifecycle::apply(_state, MQTTLifecycle::Event::StopAcknowledged).next;
}

void MQTTBridge::loop() {
  if (_initialized && _acked.load(std::memory_order_acquire)) end();
  if (!_initialized) {
    if (_state == MQTTLifecycle::State::StopRequested && millis() - _stop_started >= 60000)
      _state = MQTTLifecycle::apply(_state, MQTTLifecycle::Event::StopTimedOut).next;
    return;
  }
  uint32_t epoch;
  if (xQueueReceive(_rtc_updates, &epoch, 0) == pdTRUE && _rtc) _rtc->setCurrentTime(epoch);
  if (millis() - _last_config_sample >= 250) {
    _last_config_sample = millis(); snapshotConfiguration();
  }
  if (millis() - _last_sample >= 5000) {
    _last_sample = millis();
    RadioStats next;
    if (_board) next.battery = _board->getBattMilliVolts();
    if (_radio) { next.noise = _radio->getNoiseFloor(); next.rx_errors = _radio->getPacketsRecvErrors(); }
    if (_dispatcher) {
      next.errors = _dispatcher->getErrFlags();
      next.tx_air = _dispatcher->getTotalAirTime() / 1000;
      next.rx_air = _dispatcher->getReceiveAirTime() / 1000;
      next.sent = _dispatcher->getNumSentDirect() + _dispatcher->getNumSentFlood();
      next.received = _dispatcher->getNumRecvDirect() + _dispatcher->getNumRecvFlood();
    }
    xSemaphoreTake(_health_mutex, portMAX_DELAY); _radio_stats = next; xSemaphoreGive(_health_mutex);
  }
}

void MQTTBridge::storeRawRadioData(const uint8_t* data, int size, float snr, float rssi) {
  _staged.length = 0;
  if (!data || size <= 0 || size > 256) return;
  memcpy(_staged.raw, data, size); _staged.length = size;
  _staged.snr = snr; _staged.rssi = rssi;
}

void MQTTBridge::capture(mesh::Packet* packet, bool tx) {
  if (!_initialized || !packet) return;
  const bool allowed = tx ? (_obs->mqtt_tx_enabled == 1 ||
    (_obs->mqtt_tx_enabled == 2 && packet->getPayloadType() == PAYLOAD_TYPE_ADVERT &&
      packet->payload_len >= PUB_KEY_SIZE && !memcmp(packet->payload, _identity->pub_key, PUB_KEY_SIZE)))
    : _obs->mqtt_rx_enabled;
  if (!allowed || !MQTTPacketFilter::allows(_obs->mqtt_slot_packet_filter[0], packet->getPayloadType())) {
    ++_filtered; _staged.length = 0; return;
  }
  if (!_time_valid.load(std::memory_order_acquire)) { ++_unsynced; _staged.length = 0; return; }
  if (!tx && _obs->mqtt_advert_verify && packet->getPayloadType() == PAYLOAD_TYPE_ADVERT) {
    uint8_t message[AdvertSignature::kMaxMessageLen]; const uint8_t* signature = nullptr;
    const int size = AdvertSignature::buildMessage(packet->payload, packet->payload_len, message, &signature);
    if (size < 0 || !mesh::Identity(packet->payload).verify(signature, message, size)) {
      ++_filtered; _staged.length = 0; return;
    }
  }
  Capture item = _staged; _staged.length = 0;
  item.packet = *packet; item.tx = tx; item.captured_ms = millis();
  if (tx) item.length = 0;
  item.score = !tx && _radio ? _radio->packetScore(item.snr, item.length) : NAN;
  if (xQueueSend(_captures, &item, 0) != pdTRUE) {
    Capture discarded;
    xQueueReceive(_captures, &discarded, 0); ++_dropped;
    if (xQueueSend(_captures, &item, 0) != pdTRUE) ++_dropped;
  }
}

void MQTTBridge::taskEntry(void* value) {
  auto* self = static_cast<MQTTBridge*>(value);
  self->run();
  self->_acked.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

void MQTTBridge::run() {
  std::unique_ptr<Runtime> runtime(new(std::nothrow) Runtime);
  std::unique_ptr<Configuration> incoming(new(std::nothrow) Configuration);
  if (!runtime || !incoming) {
    xSemaphoreTake(_health_mutex, portMAX_DELAY);
    snprintf(_health.state, sizeof(_health.state), "out of memory");
    xSemaphoreGive(_health_mutex);
    return;
  }
  Runtime& r = *runtime;
  Health health;
  uint32_t last_report = 0;
  bool reported = false;
  // Stack scan, heap walk and mutex at most once a second unless the state changes.
  auto report = [&](bool force = false) {
    const uint32_t at = millis();
    if (!force && reported && at - last_report < 1000) return;
    reported = true; last_report = at;
    health.stack_free = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
    health.min_heap = std::min(health.min_heap, static_cast<uint32_t>(rp2040.getFreeHeap()));
    xSemaphoreTake(_health_mutex, portMAX_DELAY); _health = health; xSemaphoreGive(_health_mutex);
  };
  auto state = [&](const char* value) { snprintf(health.state, sizeof(health.state), "%s", value); report(true); };
  auto& network = activeNetworkLink();
  bool configured = false;
  Capture item;
  bool pending = false;
  uint8_t retries = 0;
  uint32_t retry_at = 0;
  while (!_stop.load(std::memory_order_acquire)) {
    if (xQueueReceive(_config_queue, incoming.get(), 0) == pdTRUE) {
      const Configuration& next = *incoming;
      const auto& before = _config.observer;
      const auto& after = next.observer;
      // Only Wi-Fi, endpoint, credential, token and topic changes need a new session.
      const bool reconnect = !configured ||
        memcmp(next.identity.pub_key, _config.identity.pub_key, PUB_KEY_SIZE) ||
        strcmp(next.firmware, _config.firmware) || PicoObserverConfigPolicy::connectionChanged(after, before);
      const bool rename = !configured || strcmp(next.origin, _config.origin);
      const bool restatus = rename || next.frequency != _config.frequency || next.bandwidth != _config.bandwidth ||
        next.sf != _config.sf || next.cr != _config.cr || next.repeat != _config.repeat ||
        strcmp(next.model, _config.model) || after.mqtt_status_enabled != before.mqtt_status_enabled ||
        after.mqtt_status_interval != before.mqtt_status_interval;
      const bool ntp_changed = configured && strcmp(after.mqtt_ntp_server, before.mqtt_ntp_server);
      _config = next;
      if (rename) {
        char hostname[NetworkHostname::kBufferSize];
        NetworkHostname::build(hostname, sizeof(hostname), _config.origin, _config.identity.pub_key, PUB_KEY_SIZE);
        network.setHostname(hostname);
      }
      if (reconnect) {
        r.client.disconnect(); health.connected = false;
        _dropped.fetch_add(getQueueSize() + (pending ? 1 : 0));
        xQueueReset(_captures); pending = false; retries = 0;
        network.begin(_config.observer.wifi_ssid, _config.observer.wifi_password);
        r.attempted = r.tripped = false; r.backoff = r.max_failures = 0; r.expires = 0;
        snprintf(r.client_version, sizeof(r.client_version), "meshcore/%s", _config.firmware);
      }
      if (restatus) r.last_status = 0;
      if (ntp_changed) _force_ntp.store(true);
      if (reconnect) { configured = true; state("configured"); }
    }
    if (!configured) { delay(10); continue; }
    const uint32_t now = millis();
    const auto transition = network.maintain(now, 0);
    if (!network.isConnected()) {
      if (health.connected) r.client.disconnect();
      health.connected = false; state("wifi down"); delay(50); continue;
    }
    if (transition == NetworkTransition::Up) r.attempted = false;
    const bool forced = _force_ntp.exchange(false);
    if (forced) r.ntp_server = 0;
    if (forced || !r.ntp_scheduled || static_cast<int32_t>(now - r.ntp_at) >= 0) {
      if (!health.connected) state("NTP sync");
      const char* servers[] = {effectiveNtpPrimary(&_config.observer), "time.google.com", "time.cloudflare.com", "time.aws.com", "time.nist.gov"};
      constexpr uint8_t count = sizeof(servers) / sizeof(servers[0]);
      // While connected, one probe per pass bounds the MQTT service gap to one DNS + probe.
      const uint8_t probes = health.connected ? 1 : count;
      bool synced = false;
      for (uint8_t i = 0; i < probes && !_stop.load(); ++i) {
        uint32_t epoch;
        if (probeNtp(servers[r.ntp_server], epoch, _stop)) {
          timeval value{static_cast<time_t>(epoch), 0}; settimeofday(&value, nullptr);
          xQueueOverwrite(_rtc_updates, &epoch); health.time_valid = true;
          _time_valid.store(true, std::memory_order_release);
          synced = true; break;
        }
        r.ntp_server = (r.ntp_server + 1) % count;
      }
      if (synced) r.ntp_server = 0;
      r.ntp_scheduled = true;
      r.ntp_at = millis() + (synced ? 3600000UL : 30000UL);
    }
    if (!health.time_valid) { state("waiting for time"); delay(50); continue; }
    if (_stop.load()) break;
    const auto& obs = _config.observer;
    const char* preset_name = obs.mqtt_slot_preset[0];
    const bool custom = !strcmp(preset_name, "custom");
    const MQTTPresetDef* preset = custom ? nullptr : findMQTTPreset(preset_name);
    if (!custom && !preset) { state("slot1 disabled"); delay(50); continue; }
    if (health.connected && !r.client.service()) {
      health.connected = false; health.outage_at = now;
      r.last_attempt = now; state("disconnected");
    }
    const uint32_t epoch = time(nullptr);
    if (health.connected && r.expires && MQTTConnectionPolicy::tokenNeedsRenewal(true, epoch, r.expires,
        MQTTConnectionPolicy::renewalBufferSecs(preset && preset->token_lifetime ? preset->token_lifetime : 86400))) {
      r.client.disconnect(); health.connected = false; r.attempted = false;
    }
    if (!health.connected) {
      const bool due = !r.attempted || (r.tripped ? MQTTConnectionPolicy::circuitBreakerProbeDue(now, r.last_attempt) :
        MQTTConnectionPolicy::reconnectDue(now, r.last_attempt, r.backoff, 0));
      if (!due) { report(); delay(50); continue; }
      if (r.attempted && !r.tripped) {
        auto next = MQTTConnectionPolicy::advanceBackoff(r.backoff, r.max_failures);
        r.backoff = next.reconnect_backoff; r.max_failures = next.max_backoff_failures; r.tripped = next.circuit_breaker_tripped;
        if (!next.should_reconnect) { r.last_attempt = now; state("circuit open"); continue; }
      }
      r.attempted = health.attempted = true; r.last_attempt = now;
      const char* iata = mqttSlotIata(obs.mqtt_iata, obs.mqtt_slot_iata[0]);
      if (preset && preset->topic_style == MQTT_TOPIC_MESHRANK && !obs.mqtt_slot_token[0][0]) {
        state("token required"); continue;
      }
      if (!(preset && preset->topic_style == MQTT_TOPIC_MESHRANK) &&
          !(custom && obs.mqtt_slot_topic[0][0]) && (!mqttIataValid(iata) || !strcmp(iata, "XXX"))) {
        state("IATA required"); continue;
      }
      ObserverTransportPolicy::Endpoint endpoint;
      const bool valid_endpoint = custom ?
        ObserverTransportPolicy::customEndpoint(obs.mqtt_slot_host[0], obs.mqtt_slot_port[0], endpoint) :
        ObserverTransportPolicy::parseEndpoint(preset->server_url, endpoint);
      if (!valid_endpoint) { state("invalid endpoint"); continue; }
      const char* pem = preset ? preset->ca_cert : nullptr;
      const bool default_trust = !pem && mqttTransportIsEncrypted(endpoint.transport);
      if (default_trust) pem = ISRG_ROOT_X1;
      const char* username = custom ? obs.mqtt_slot_username[0] : "";
      const char* password = custom ? obs.mqtt_slot_password[0] : "";
      const char* audience = preset ? preset->jwt_audience : obs.mqtt_slot_audience[0];
      const bool jwt = preset ? preset->auth_type == MQTT_AUTH_JWT : audience && *audience;
      r.expires = 0;
      if (jwt) {
        const uint32_t lifetime = preset && preset->token_lifetime ? preset->token_lifetime : 86400;
        char owner[sizeof(obs.mqtt_owner_public_key)];
        if (!JWTHelper::createAuthToken(_config.identity, audience, epoch, lifetime, r.auth, sizeof(r.auth),
              PicoObserverConfigPolicy::ownerKeyClaim(obs.mqtt_owner_public_key, owner, sizeof(owner)),
              r.client_version, obs.mqtt_email[0] ? obs.mqtt_email : nullptr)) {
          state("JWT failed"); continue;
        }
        snprintf(r.username, sizeof(r.username), "v1_%s", _config.device);
        for (char* p = r.username + 3; *p; ++p) *p = toupper(*p);
        username = r.username; password = r.auth; r.expires = epoch + lifetime;
      } else if (preset && preset->auth_type == MQTT_AUTH_USERPASS) {
        username = mqttPresetUsesDevicePubkeyUsername(preset) ? _config.device :
                   preset->userpass_username ? preset->userpass_username : obs.mqtt_slot_username[0];
        password = preset->userpass_password ? preset->userpass_password : obs.mqtt_slot_password[0];
        if (!*username || !*password) { state("credentials required"); continue; }
      }
      state("connecting");
      IPAddress address;
      if (!network.resolveHost(endpoint.host, address)) { state("DNS failed"); continue; }
      bool ok = r.client.connect(endpoint, pem, _config.device, username, password, 75,
                                 default_trust ? GTS_ROOT_R4 : nullptr);
      r.last_attempt = millis();
      // A command queued during connect must be applied before any publish.
      if (_stop.load() || uxQueueMessagesWaiting(_config_queue)) { r.client.disconnect(); continue; }
      health.connected = ok; health.error_code = r.client.error();
      if (ok) {
        health.connected_at = millis(); health.outage_at = 0; r.last_status = 0;
        r.tripped = false; r.max_failures = 0;
        state("connected");
      }
      else { ++health.errors; if (!health.outage_at) health.outage_at = millis(); state("connect failed"); }
      report(); continue;
    }
    if (MQTTConnectionPolicy::stableConnection(now, health.connected_at)) { r.backoff = r.max_failures = 0; r.tripped = false; }
    auto publish = [&](int type, size_t length) {
      const auto style = preset && preset->topic_style == MQTT_TOPIC_MESHRANK ? MQTT_ROUTE_MESHRANK :
                         custom && obs.mqtt_slot_topic[0][0] ? MQTT_ROUTE_CUSTOM : MQTT_ROUTE_MESHCORE;
      if (!length || !mqttBuildPublicationTopic(style, type, obs.mqtt_slot_topic[0],
          mqttSlotIata(obs.mqtt_iata, obs.mqtt_slot_iata[0]), _config.device, obs.mqtt_slot_token[0], r.topic, sizeof(r.topic))) return false;
      const bool retained = type == MQTT_PUBLICATION_STATUS && preset && preset->allow_retain;
      const bool ok = r.client.publish(r.topic, r.payload, length, retained, type == MQTT_PUBLICATION_STATUS ? 1 : 0);
      if (ok) ++health.published; else ++health.errors;
      return ok;
    };
    if (obs.mqtt_status_enabled && (!r.last_status ||
        now - r.last_status >= PicoObserverConfigPolicy::statusIntervalMs(obs.mqtt_status_interval))) {
      r.last_status = now;
      RadioStats stats;
      xSemaphoreTake(_health_mutex, portMAX_DELAY); stats = _radio_stats; xSemaphoreGive(_health_mutex);
      timeval tv; gettimeofday(&tv, nullptr); char stamp[40], radio[80];
      MQTTMessageBuilder::formatIsoTimestampForMqtt(tv.tv_sec, tv.tv_usec, nullptr, stamp, sizeof(stamp));
      snprintf(radio, sizeof(radio), "%.6f,%.1f,%u,%u", _config.frequency, _config.bandwidth, _config.sf, _config.cr);
      const int size = MQTTMessageBuilder::buildStatusMessage(r.json, _config.origin, _config.device,
        _config.model, _config.firmware, radio, r.client_version, "online", stamp, r.payload, sizeof(r.payload),
        stats.battery, now / 1000, stats.errors, getQueueSize(), stats.noise, stats.tx_air, stats.rx_air,
        stats.rx_errors, rp2040.getFreeHeap(), stats.sent, stats.received, _config.repeat ? "on" : "off");
      publish(MQTT_PUBLICATION_STATUS, size);
    }
    if (!pending && xQueueReceive(_captures, &item, 0) == pdTRUE) { pending = true; retries = 0; }
    if (pending && MQTTPacketQueuePolicy::retryReady(millis(), retry_at, retries)) {
      if (millis() - item.captured_ms >= MQTTPacketQueuePolicy::kDisconnectedStaleMs) {
        ++_dropped; pending = false;
      }
      else {
        bool success = false, eligible = false;
        if (obs.mqtt_packets_enabled) {
          eligible = true;
          const int size = item.length ? MQTTMessageBuilder::buildPacketJSONFromRaw(r.json,
            item.raw, item.length, &item.packet, item.tx, _config.origin, _config.device,
            item.snr, item.rssi, item.score, nullptr, r.payload, sizeof(r.payload)) :
            MQTTMessageBuilder::buildPacketJSON(r.json, &item.packet, item.tx, _config.origin, _config.device, nullptr, r.payload, sizeof(r.payload));
          success = publish(MQTT_PUBLICATION_PACKETS, size);
        }
        if (obs.mqtt_raw_enabled && !(preset && preset->topic_style == MQTT_TOPIC_MESHRANK)) {
          eligible = true;
          const int size = MQTTMessageBuilder::buildRawJSON(r.json, &item.packet, _config.origin, _config.device, nullptr, r.payload, sizeof(r.payload));
          success = publish(MQTT_PUBLICATION_RAW, size) || success;
        }
        const auto decision = MQTTPacketQueuePolicy::retryDecision(!eligible || success, retries, millis());
        if (decision.action == MQTTPacketQueuePolicy::RetryAction::Schedule) {
          retries = decision.retry_attempts; retry_at = decision.next_retry_ms;
        } else {
          if (decision.action == MQTTPacketQueuePolicy::RetryAction::Drop) ++_dropped;
          pending = false;
        }
      }
    }
    report(); delay(10);
  }
  r.client.disconnect();
  health.connected = false; state("stopped");
}

void MQTTBridge::formatMqttStatusReply(char* out, size_t size, const MQTTPrefs*) {
  if (!_instance) { snprintf(out, size, "> stopped"); return; }
  if (!_instance->_initialized) {
    snprintf(out, size, "> %s", _instance->isStopUnproven() ? "stopping; waiting for worker" : "stopped");
    return;
  }
  const auto h = _instance->readHealth();
  snprintf(out, size, "> slot1:%s time:%s q:%d (slots2-6 inactive)", h.state, h.time_valid ? "ok" : "wait", _instance->getQueueSize());
}
void MQTTBridge::formatMqttStatsReply(char* out, size_t size) {
  if (!_instance) { snprintf(out, size, "> stopped"); return; }
  const auto h = _instance->readHealth();
  snprintf(out, size, "> ok:%lu err:%lu drop:%lu filt:%lu time_drop:%lu heap_min:%lu stack_free:%lu q:%d",
    (unsigned long)h.published, (unsigned long)h.errors, (unsigned long)_instance->_dropped.load(),
    (unsigned long)_instance->_filtered.load(), (unsigned long)_instance->_unsynced.load(),
    (unsigned long)h.min_heap, (unsigned long)h.stack_free, _instance->getQueueSize());
}
void MQTTBridge::formatSlotDiagReply(char* out, size_t size, int slot) {
  if (slot != 0) { snprintf(out, size, "> inactive (Pico supports slot1 only)"); return; }
  if (!_instance) { snprintf(out, size, "> stopped"); return; }
  const auto h = _instance->readHealth();
  snprintf(out, size, "> %s code:%d connected:%u", h.state, h.error_code, h.connected);
}
bool MQTTBridge::ntpDiag(char* out, size_t size, bool) {
  const auto h = readHealth();
  snprintf(out, size, "> time:%s primary:%s (cached; probe unavailable)", h.time_valid ? "valid" : "waiting", effectiveNtpPrimary(_obs));
  return true;
}
#endif
