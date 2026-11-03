#pragma once
#include "Client.h"
#include <string>
class MQTTClient {
  Client* _client = nullptr;
public:
  inline static std::string host, id, user, password;
  inline static uint16_t port = 0;
  inline static int connects = 0, closes = 0, qos = -1;
  inline static bool retained = false;
  MQTTClient(int, int) {}
  void begin(const char* h, uint16_t p, Client& client) { host = h; port = p; _client = &client; }
  void setOptions(uint16_t, bool, int) {}
  bool connect(const char* device, const char* username, const char* secret) {
    ++connects; id = device; user = username; password = secret;
    return _client->connect(host.c_str(), port);
  }
  int returnCode() { return 0; }
  int lastError() { return -10; }
  bool loop() { return connected(); }
  bool connected() { return _client && _client->connected(); }
  bool publish(const char*, const char*, int, bool retain, uint8_t level) {
    retained = retain; qos = level; return connected();
  }
  void disconnect() { ++closes; if (_client) _client->stop(); }
};
