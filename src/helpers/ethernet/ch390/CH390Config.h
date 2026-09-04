#pragma once

#include <ESP32_CH390.h>

/**
 * Bring up the repository's CH390 lwIP interface from the board build flags.
 * Shared by the companion transport wrapper and the observer network adapter so
 * pin and static-IP behavior cannot drift between the two paths.
 */
static inline bool beginConfiguredCH390(const char* hostname = nullptr) {
  ch390_config_t config = CH390_DEFAULT_CONFIG();
  config.spi_miso_gpio = ETH_MISO_PIN;
  config.spi_mosi_gpio = ETH_MOSI_PIN;
  config.spi_sck_gpio = ETH_SCLK_PIN;
  config.spi_cs_gpio = ETH_CS_PIN;
  config.int_gpio = ETH_INT_PIN;
  if (hostname && hostname[0] != '\0') {
    if (!CH390.setHostname(hostname)) {
      Serial.printf("Network: invalid Ethernet hostname %s\n", hostname);
      return false;
    }
  }
  if (!CH390.begin(config)) return false;

#if defined(ETHERNET_STATIC_IP) && defined(ETHERNET_STATIC_GATEWAY) && defined(ETHERNET_STATIC_SUBNET)
  IPAddress ip(ETHERNET_STATIC_IP);
  IPAddress gateway(ETHERNET_STATIC_GATEWAY);
  IPAddress subnet(ETHERNET_STATIC_SUBNET);
  #if defined(ETHERNET_STATIC_DNS)
  IPAddress dns(ETHERNET_STATIC_DNS);
  CH390.config(ip, gateway, subnet, dns);
  #else
  CH390.config(ip, gateway, subnet);
  #endif
#endif
  return true;
}
