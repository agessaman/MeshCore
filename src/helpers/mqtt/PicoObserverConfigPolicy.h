#pragma once

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../MQTTPrefsStorage.h"

// Pico observer configuration rules, kept free of SDK types for host tests.
namespace PicoObserverConfigPolicy {
static const uint32_t kDefaultStatusIntervalMs = 300000UL;

// Same accepted range and fallback as the ESP32 bridge.
inline uint32_t statusIntervalMs(uint32_t configured) {
  return configured >= 1000UL && configured <= 3600000UL ? configured : kDefaultStatusIntervalMs;
}

// The ESP32 bridge uppercases the owner claim; the CLI stores it as typed.
inline const char* ownerKeyClaim(const char* configured, char* out, size_t size) {
  if (!configured || !*configured || !out || !size) return nullptr;
  size_t i = 0;
  for (; configured[i] && i + 1 < size; ++i) out[i] = static_cast<char>(toupper(static_cast<unsigned char>(configured[i])));
  out[i] = 0;
  return out;
}

// True when slot 1's Wi-Fi, endpoint, credentials, token claims or topic inputs differ.
inline bool connectionChanged(const MQTTPrefs& a, const MQTTPrefs& b) {
  return strcmp(a.wifi_ssid, b.wifi_ssid) || strcmp(a.wifi_password, b.wifi_password) ||
         strcmp(a.mqtt_iata, b.mqtt_iata) || strcmp(a.mqtt_slot_iata[0], b.mqtt_slot_iata[0]) ||
         strcmp(a.mqtt_slot_preset[0], b.mqtt_slot_preset[0]) ||
         strcmp(a.mqtt_slot_host[0], b.mqtt_slot_host[0]) || a.mqtt_slot_port[0] != b.mqtt_slot_port[0] ||
         strcmp(a.mqtt_slot_username[0], b.mqtt_slot_username[0]) ||
         strcmp(a.mqtt_slot_password[0], b.mqtt_slot_password[0]) ||
         strcmp(a.mqtt_slot_token[0], b.mqtt_slot_token[0]) ||
         strcmp(a.mqtt_slot_topic[0], b.mqtt_slot_topic[0]) ||
         strcmp(a.mqtt_slot_audience[0], b.mqtt_slot_audience[0]) ||
         strcmp(a.mqtt_owner_public_key, b.mqtt_owner_public_key) || strcmp(a.mqtt_email, b.mqtt_email);
}
}
