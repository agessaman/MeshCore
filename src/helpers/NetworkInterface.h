#pragma once

#include "NetworkPolicy.h"

#if defined(ESP_PLATFORM)

#include <Arduino.h>
#include <IPAddress.h>
#include "AlertFaultPolicy.h"

/**
 * Physical network selected for IP-based services.
 *
 * The interface owns link bring-up and medium-specific maintenance. MQTT, NTP,
 * OTA, and other socket users only consume connectivity and addressing. Normal
 * MQTT shutdown deliberately does not stop this interface because an OTA
 * download runs after the broker clients have been released.
 */
class NetworkInterface {
 public:
  virtual ~NetworkInterface() = default;

  virtual const char* mediumName() const = 0;
  virtual const char* statusName() const = 0;
  virtual int statusCode() const = 0;
  virtual bool configValid(const char* wifi_ssid) const = 0;
  virtual bool begin(const char* wifi_ssid, const char* wifi_password) = 0;
  virtual NetworkTransition maintain(uint32_t now_ms, uint8_t wifi_power_save) = 0;

  virtual bool isConnected() const = 0;
  virtual IPAddress localIP() const = 0;
  virtual int rssi() const = 0;  // INT_MIN when the selected medium has no RSSI.
  virtual bool resolveHost(const char* hostname, IPAddress& address) const = 0;

  virtual unsigned long connectedAtMillis() const = 0;
  virtual uint8_t lastDisconnectReason() const = 0;
  virtual unsigned long lastDisconnectTime() const = 0;
  virtual AlertFaultPolicy::OutageSnapshot outageSnapshot() const = 0;
};

/** Build-selected singleton. Wi-Fi is the compatibility default. */
NetworkInterface& activeNetworkInterface();

#endif
