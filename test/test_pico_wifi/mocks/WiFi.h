#pragma once
#include "Arduino.h"
#include "IPAddress.h"
#include "cyw43.h"
constexpr int WL_DISCONNECTED = 6, WL_CONNECTED = 3, WIFI_STA = 1;
struct MockWiFi {
  int code = WL_DISCONNECTED, begins = 0, disconnects = 0, signals = 0;
  IPAddress ip{0,0,0,0}, dns{0,0,0,0};
  std::string ssid, password, hostname;
  bool open = false;
  void mode(int) { assert(mock_lwip_context); }
  void setHostname(const char* value) { assert(mock_lwip_context); hostname = value; }
  int beginNoBlock(const char* name, const char* key) {
    assert(mock_lwip_context);
    ++begins; ssid = name; open = key == nullptr; password = key ? key : "";
    code = WL_DISCONNECTED; ip = IPAddress(0,0,0,0);
    mock_link = CYW43_LINK_JOIN; cyw43_state.wifi_join_state = 0x201;
    return code;
  }
  void disconnect() {
    assert(mock_lwip_context); ++disconnects;
    code = WL_DISCONNECTED; ip = IPAddress(0,0,0,0);
    mock_link = CYW43_LINK_DOWN; cyw43_state.wifi_join_state = 0;
  }
  int status() { assert(mock_lwip_context); return code; }
  IPAddress localIP() { assert(mock_lwip_context); return ip; }
  IPAddress dnsIP() { assert(mock_lwip_context); return dns; }
  int RSSI() { assert(mock_lwip_context); ++signals; return -62; }
  int hostByName(const char*, IPAddress&, int) { return 1; }
};
inline MockWiFi WiFi;
