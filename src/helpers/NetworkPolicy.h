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

enum class NetworkDiagnosticReason : uint8_t {
  EthernetActive,
  EthernetInitFailed,
  EthernetLinkUnknown,
  EthernetLinkDown,
  EthernetAwaitingIp,
  EthernetStabilizing,
  SwitchingLocked,
  EthernetReady,
};

namespace NetworkPolicy {

struct MQTTTransitionActions {
  bool stop_started_slots;
  bool retry_disconnected_slots_now;
  bool reset_reconnect_backoff;
};

static constexpr MQTTTransitionActions mqttActions(NetworkTransition transition) {
  return {
      transition == NetworkTransition::Down || transition == NetworkTransition::Switched,
      transition == NetworkTransition::Up || transition == NetworkTransition::Switched,
      transition == NetworkTransition::Switched,
  };
}

static constexpr const char* mediumName(NetworkMedium medium) {
  return medium == NetworkMedium::Ethernet ? "ethernet"
       : medium == NetworkMedium::WiFi ? "wifi"
                                       : "none";
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

static inline NetworkDiagnosticReason automaticDiagnosticReason(
    bool ethernet_initialized, bool ethernet_link_known,
    bool ethernet_link_up, bool ethernet_connected, NetworkMedium selected,
    bool switching_locked, uint32_t ethernet_stable_ms) {
  if (!ethernet_initialized) {
    return NetworkDiagnosticReason::EthernetInitFailed;
  }
  if (!ethernet_link_known) {
    return NetworkDiagnosticReason::EthernetLinkUnknown;
  }
  if (!ethernet_link_up) {
    return NetworkDiagnosticReason::EthernetLinkDown;
  }
  if (!ethernet_connected) {
    return NetworkDiagnosticReason::EthernetAwaitingIp;
  }
  if (selected == NetworkMedium::Ethernet) {
    return NetworkDiagnosticReason::EthernetActive;
  }
  if (switching_locked) {
    return NetworkDiagnosticReason::SwitchingLocked;
  }
  if (selected == NetworkMedium::WiFi &&
      ethernet_stable_ms < kEthernetFailbackStableMs) {
    return NetworkDiagnosticReason::EthernetStabilizing;
  }
  return NetworkDiagnosticReason::EthernetReady;
}

static inline const char* diagnosticReasonName(
    NetworkDiagnosticReason reason) {
  switch (reason) {
    case NetworkDiagnosticReason::EthernetActive:
      return "ethernet-active";
    case NetworkDiagnosticReason::EthernetInitFailed:
      return "ethernet-init-failed";
    case NetworkDiagnosticReason::EthernetLinkUnknown:
      return "ethernet-link-unknown";
    case NetworkDiagnosticReason::EthernetLinkDown:
      return "ethernet-link-down";
    case NetworkDiagnosticReason::EthernetAwaitingIp:
      return "ethernet-awaiting-ip";
    case NetworkDiagnosticReason::EthernetStabilizing:
      return "ethernet-stabilizing";
    case NetworkDiagnosticReason::SwitchingLocked:
      return "switching-locked";
    case NetworkDiagnosticReason::EthernetReady:
      return "ethernet-ready";
  }
  return "unknown";
}

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

// Once the Ethernet controller has initialized, allow the entire boot probe
// window for link negotiation and DHCP. PHY carrier is useful diagnostic data,
// but it must not shorten the advertised deadline when carrier reporting lags.
static constexpr bool ethernetBootProbePending(bool ethernet_initialized,
                                                bool ethernet_connected,
                                                uint32_t elapsed_ms,
                                                uint32_t wait_ms) {
  return ethernet_initialized && !ethernet_connected && elapsed_ms < wait_ms;
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
        (!input.wifi_connected ||
         input.ethernet_stable_ms >= kEthernetFailbackStableMs)) {
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
