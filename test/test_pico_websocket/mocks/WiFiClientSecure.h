#pragma once
#include "WiFi.h"
#define BR_TLS12 0x0303
namespace BearSSL {
class WiFiClientSecure : public WiFiClient {
public:
  inline static uint32_t minimum = 0, maximum = 0;
  inline static X509List* trust = nullptr;
  void setTrustAnchors(X509List* value) { trust = value; }
  void setSSLVersion(uint32_t min, uint32_t max) { minimum = min; maximum = max; }
};
}
