#pragma once
#include "Client.h"
#include <functional>
#include <vector>
#include <string>
enum WStype_t { WStype_CONNECTED, WStype_DISCONNECTED, WStype_BIN,
  WStype_FRAGMENT_BIN_START, WStype_FRAGMENT, WStype_FRAGMENT_FIN,
  WStype_TEXT, WStype_FRAGMENT_TEXT_START, WStype_ERROR, WStype_PING };
class WebSocketsClient {
protected:
  uint32_t _lastConnectionFail = 0;
  struct { std::string cProtocol = "mqtt"; } _client;
public:
  struct Event { WStype_t type; std::vector<uint8_t> data; };
  inline static WebSocketsClient* instance = nullptr;
  std::function<void(WStype_t,uint8_t*,size_t)> callback;
  std::vector<Event> events;
  std::string host, path, protocol;
  uint16_t port = 0;
  BearSSL::X509List* trust = nullptr;
  int begin_count = 0, loop_count = 0, closes = 0, sends = 0;
  bool fail_connect = false, fail_write = false;
  void responseProtocol(const char* value) { _client.cProtocol = value; }
  WebSocketsClient() { instance = this; }
  void onEvent(std::function<void(WStype_t,uint8_t*,size_t)> value) { callback = value; }
  void begin(const char* h, uint16_t p, const char* url, const char* proto) {
    host = h; port = p; path = url; protocol = proto; _lastConnectionFail = 0; ++begin_count;
  }
  void beginSslWithCA(const char* h, uint16_t p, const char* url, BearSSL::X509List* ca, const char* proto) {
    trust = ca; begin(h,p,url,proto);
  }
  void setReconnectInterval(unsigned long) {}
  void loop() {
    ++loop_count;
    if (fail_connect) { _lastConnectionFail = millis(); return; }
    if (!events.empty()) {
      auto event = events.front(); events.erase(events.begin());
      callback(event.type, event.data.data(), event.data.size());
    }
  }
  bool sendBIN(const uint8_t*, size_t) { ++sends; return !fail_write; }
  void disconnect() { ++closes; }
};
