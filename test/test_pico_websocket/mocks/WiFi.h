#pragma once
#include "Client.h"
class WiFiClient : public Client {
  bool _connected = false;
public:
  int connect(const char*, uint16_t) override { _connected = true; return 1; }
  int connect(IPAddress, uint16_t) override { _connected = true; return 1; }
  size_t write(uint8_t) override { return _connected ? 1 : 0; }
  size_t write(const uint8_t*, size_t size) override { return _connected ? size : 0; }
  int available() override { return 0; }
  int read() override { return -1; }
  int read(uint8_t*, size_t) override { return 0; }
  int peek() override { return -1; }
  void flush() override {}
  void stop() override { _connected = false; }
  uint8_t connected() override { return _connected; }
  operator bool() override { return connected(); }
};
