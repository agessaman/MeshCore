#pragma once

#include <stdint.h>

enum class NetworkTransition : uint8_t {
  None,
  Up,
  Down,
  Switched,
};

enum class NetworkMedium : uint8_t {
  None,
  Ethernet,
  WiFi,
};

namespace NetworkPolicy {

struct MQTTTransitionActions {
  bool disconnect_slots;
  bool retry_disconnected_slots_now;
};

static constexpr MQTTTransitionActions mqttActions(NetworkTransition transition) {
  return {
      transition == NetworkTransition::Down || transition == NetworkTransition::Switched,
      transition == NetworkTransition::Up || transition == NetworkTransition::Switched,
  };
}

struct AutomaticSelectionInput {
  NetworkMedium selected;
  bool ethernet_connected;
  bool wifi_connected;
  bool wifi_configured;
  bool switching_locked;
  uint32_t ethernet_stable_ms;
  uint32_t selected_down_ms;
};

// Keep short link flaps from bouncing the default route. Ethernet is allowed
// to recover before Wi-Fi is started, and must then remain usable before it can
// preempt a working Wi-Fi fallback.
static constexpr uint32_t kEthernetDownGraceMs = 3000;
static constexpr uint32_t kEthernetFailbackStableMs = 10000;
static constexpr uint32_t kNtpRetryMs = 30000;

static constexpr bool ntpPendingAfterConnectivitySample(
    bool synced, bool pending, bool was_connected, bool connected) {
  return pending || (!synced && connected && !was_connected);
}

static constexpr bool ntpRetryDue(bool synced, bool connected,
                                  uint32_t now_ms, uint32_t last_attempt_ms) {
  return !synced && connected &&
         (uint32_t)(now_ms - last_attempt_ms) >= kNtpRetryMs;
}

static constexpr NetworkMedium bootSelection(bool ethernet_connected,
                                             bool wifi_configured) {
  return ethernet_connected ? NetworkMedium::Ethernet
       : wifi_configured ? NetworkMedium::WiFi
                         : NetworkMedium::None;
}

static inline NetworkMedium automaticSelection(
    const AutomaticSelectionInput& input) {
  if (input.switching_locked) return input.selected;

  if (input.selected == NetworkMedium::Ethernet) {
    if (input.ethernet_connected) return NetworkMedium::Ethernet;
    if (input.wifi_configured && input.wifi_connected &&
        input.selected_down_ms >= kEthernetDownGraceMs) {
      return NetworkMedium::WiFi;
    }
    return NetworkMedium::Ethernet;
  }

  if (input.selected == NetworkMedium::WiFi) {
    if (input.ethernet_connected &&
        input.ethernet_stable_ms >= kEthernetFailbackStableMs) {
      return NetworkMedium::Ethernet;
    }
    return NetworkMedium::WiFi;
  }

  if (input.ethernet_connected) return NetworkMedium::Ethernet;
  if (input.wifi_configured && input.wifi_connected) return NetworkMedium::WiFi;
  return NetworkMedium::None;
}

// `start ota` uses the selected LAN only when reachable and not explicitly
// forced to SoftAP. Manifest OTA has no fallback and checks connectivity itself.
static constexpr bool startOtaUsesSelectedNetwork(bool force_ap,
                                                  bool network_connected) {
  return !force_ap && network_connected;
}

}  // namespace NetworkPolicy
