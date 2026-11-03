#pragma once

#if defined(PICO_MQTT_OBSERVER)
#include <Client.h>
#include <WebSocketsClient.h>
#include "ObserverTransportPolicy.h"

class WebSocketStreamClient : public Client {
  class ObservedClient : public WebSocketsClient {
  public:
    ObservedClient() { _lastConnectionFail = 0; }
    bool attemptFailed() const { return _lastConnectionFail != 0; }
    bool mqttProtocol() const { return _client.cProtocol == "mqtt"; }
  } _ws;
  ObserverTransportPolicy::ByteQueue<2048> _rx;
  ObserverTransportPolicy::Endpoint _endpoint;
  BearSSL::X509List* _ca = nullptr;
  bool _connected = false, _failed = false, _servicing = false, _fragmented = false;
public:
  WebSocketStreamClient() {
    _ws.onEvent([this](WStype_t type, uint8_t* data, size_t size) {
      switch (type) {
        case WStype_CONNECTED:
          _connected = _ws.mqttProtocol();
          _failed = !_connected;
          break;
        case WStype_DISCONNECTED: _connected = false; _failed = true; break;
        case WStype_BIN:
        case WStype_FRAGMENT_BIN_START:
        case WStype_FRAGMENT:
        case WStype_FRAGMENT_FIN:
          if ((type == WStype_FRAGMENT || type == WStype_FRAGMENT_FIN) && !_fragmented) {
            _failed = true; break;
          }
          if (type == WStype_FRAGMENT_BIN_START) _fragmented = true;
          if (type == WStype_FRAGMENT_FIN) _fragmented = false;
          if (!_rx.append(data, size)) _failed = true;
          break;
        case WStype_TEXT:
        case WStype_FRAGMENT_TEXT_START:
        case WStype_ERROR: _failed = true; break;
        default: break;
      }
    });
  }
  ~WebSocketStreamClient() { _ws.onEvent(nullptr); stop(); }
  WebSocketStreamClient(const WebSocketStreamClient&) = delete;
  WebSocketStreamClient& operator=(const WebSocketStreamClient&) = delete;
  void configure(const ObserverTransportPolicy::Endpoint& endpoint, BearSSL::X509List* ca) {
    stop(); _endpoint = endpoint; _ca = ca;
  }
  void service() {
    if (_servicing || _failed) return;
    _servicing = true;
    _ws.loop();
    _servicing = false;
    if (_ws.attemptFailed()) _failed = true;
    if (_failed) { _ws.disconnect(); _connected = false; }
  }
  int connect(const char*, uint16_t) override {
    _rx.clear(); _failed = _connected = _fragmented = false;
    if (_endpoint.transport == MqttTransport::Wss) {
      if (!_ca) { _failed = true; return 0; }
      _ws.beginSslWithCA(_endpoint.host, _endpoint.port, _endpoint.path, _ca, "mqtt");
    } else _ws.begin(_endpoint.host, _endpoint.port, _endpoint.path, "mqtt");
    // Only this call may attempt a connection. loop() cannot retry after a failure.
    _ws.setReconnectInterval(0);
    const uint32_t start = millis();
    while (!_connected && !_failed && millis() - start < 25000) { service(); delay(1); }
    if (!_connected) stop();
    return _connected;
  }
  int connect(IPAddress, uint16_t) override { return connect(_endpoint.host, _endpoint.port); }
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    if (!_connected || _failed || !size) return 0;
    if (!_ws.sendBIN(data, size)) { _failed = true; return 0; }
    return size;
  }
  int available() override { service(); return _failed ? 0 : _rx.size(); }
  int read() override { return available() ? _rx.read() : -1; }
  int read(uint8_t* data, size_t length) override {
    size_t count = 0;
    while (count < length && available()) data[count++] = _rx.read();
    return count;
  }
  int peek() override { return available() ? _rx.peek() : -1; }
  void flush() override {}
  void stop() override { _failed = true; _connected = _fragmented = false; _ws.disconnect(); _rx.clear(); }
  uint8_t connected() override { service(); return _connected && !_failed; }
  operator bool() override { return connected(); }
};
#endif
