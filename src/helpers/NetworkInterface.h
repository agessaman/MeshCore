#pragma once

#include "NetworkPolicy.h"

#if defined(ESP_PLATFORM)

#include <Arduino.h>
#include <IPAddress.h>
#include "AlertFaultPolicy.h"

#ifndef NETWORK_ETHERNET_BOOT_WAIT_MS
#define NETWORK_ETHERNET_BOOT_WAIT_MS 8000UL
#endif

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
  virtual NetworkMedium medium() const = 0;
  virtual const char* statusName() const = 0;
  virtual int statusCode() const = 0;
  virtual bool configValid(const char* wifi_ssid) const = 0;
  // Configure the DHCP hostname before begin()/bootstrap(). Implementations
  // retain it for any later fallback interface start.
  virtual void setHostname(const char* hostname) = 0;
  virtual bool begin(const char* wifi_ssid, const char* wifi_password) = 0;
  virtual NetworkTransition maintain(uint32_t now_ms, uint8_t wifi_power_save) = 0;

  // Automatic selectors are boot-owned so first-run services can use the
  // chosen link before MQTT starts. Compatibility Wi-Fi builds keep the
  // original MQTT-task-owned begin() path.
  virtual bool isAutomatic() const { return false; }
  virtual bool bootstrap(const char* wifi_ssid, const char* wifi_password,
                         uint32_t wait_ms) {
    (void)wait_ms;
    return begin(wifi_ssid, wifi_password);
  }

  // WebConfig and OTA pin the current route for the lifetime of their session.
  virtual void lockSwitching() {}
  virtual void unlockSwitching() {}

  virtual bool isConnected() const = 0;
  virtual IPAddress localIP() const = 0;
  virtual int rssi() const = 0;  // INT_MIN when the selected medium has no RSSI.
  virtual bool resolveHost(const char* hostname, IPAddress& address) const = 0;
  virtual void formatDiagnostics(char* reply, size_t reply_size) const = 0;

  virtual unsigned long connectedAtMillis() const = 0;
  virtual uint8_t lastDisconnectReason() const = 0;
  virtual unsigned long lastDisconnectTime() const = 0;
  virtual AlertFaultPolicy::OutageSnapshot outageSnapshot() const = 0;

  // Fault reporting normally follows the selected interface. Automatic
  // Ethernet-preferred builds instead keep reporting the primary Ethernet
  // outage while healthy Wi-Fi carries traffic, so prolonged degradation is
  // not hidden by a successful fallback.
  virtual NetworkMedium alertMedium() const { return medium(); }
  virtual AlertFaultPolicy::OutageSnapshot alertOutageSnapshot() const {
    return outageSnapshot();
  }
};

/** Build-selected singleton. Wi-Fi is the compatibility default. */
NetworkInterface& activeNetworkInterface();

#endif
