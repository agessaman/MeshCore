#pragma once

#if defined(PICO_MQTT_OBSERVER)
#include <MQTT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <memory>
#include "WebSocketStreamClient.h"

class PicoObserverMqttClient {
  WiFiClient _tcp;
  BearSSL::WiFiClientSecure _tls;
  std::unique_ptr<BearSSL::X509List> _ca;
  const char* _pem = nullptr;
  const char* _extra_pem = nullptr;
  WebSocketStreamClient _ws;
  MQTTClient _mqtt{1024, 1024};
  bool _websocket = false;
  int _error = 0;
public:
  ~PicoObserverMqttClient() { disconnect(); }
  bool connect(const ObserverTransportPolicy::Endpoint& endpoint, const char* pem,
               const char* client_id, const char* username, const char* password,
               uint16_t keepalive, const char* extra_pem = nullptr) {
    disconnect();
    _error = 0;
    const bool encrypted = mqttTransportIsEncrypted(endpoint.transport);
    if (encrypted && !pem) { _error = -1001; return false; }
    if (encrypted && (_pem != pem || _extra_pem != extra_pem || !_ca)) {
      _ca.reset(new(std::nothrow) BearSSL::X509List(pem));
      if (!_ca || !_ca->getCount() || (extra_pem && !_ca->append(extra_pem))) {
        _ca.reset(); _error = -1002; return false;
      }
      _pem = pem;
      _extra_pem = extra_pem;
    }
    _websocket = endpoint.transport == MqttTransport::Ws || endpoint.transport == MqttTransport::Wss;
    Client* transport = &_tcp;
    if (_websocket) {
      _ws.configure(endpoint, encrypted ? _ca.get() : nullptr);
      transport = &_ws;
    } else if (encrypted) {
      _tls.setTrustAnchors(_ca.get());
      _tls.setSSLVersion(BR_TLS12, BR_TLS12);
      transport = &_tls;
    }
    _mqtt.begin(endpoint.host, endpoint.port, *transport);
    _mqtt.setOptions(keepalive, true, 2000);
    bool ok = _mqtt.connect(client_id, username, password);
    if (!ok) _error = _mqtt.returnCode() ? _mqtt.returnCode() : static_cast<int>(_mqtt.lastError());
    return ok;
  }
  bool service() { return _mqtt.loop(); }
  bool connected() { return _mqtt.connected(); }
  bool publish(const char* topic, const char* payload, size_t size, bool retain, uint8_t qos) {
    const bool ok = _mqtt.publish(topic, payload, static_cast<int>(size), retain, qos);
    if (!ok) _error = static_cast<int>(_mqtt.lastError());
    return ok;
  }
  int error() const { return _error; }
  void disconnect() {
    if (_mqtt.connected()) _mqtt.disconnect();
    _ws.stop(); _tcp.stop(); _tls.stop();
  }
};
#endif
