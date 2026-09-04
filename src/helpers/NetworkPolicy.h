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
static constexpr uint32_t kEthernetNoLinkBootGraceMs = 750;
static constexpr uint32_t kEthernetNoIpRecoveryMs = 120000;
static constexpr uint32_t kEthernetInitRetryMinMs = 5000;
static constexpr uint32_t kEthernetInitRetryMaxMs = 300000;
static constexpr uint32_t kNtpRetryMs = 30000;
static constexpr uint32_t kManualOtaSessionTimeoutMs =
    15UL * 60UL * 1000UL;

static inline uint32_t ethernetInitRetryDelayMs(uint8_t attempt) {
  if (attempt == 0) return 0;
  uint32_t delay_ms = kEthernetInitRetryMinMs;
  for (uint8_t i = 1; i < attempt && delay_ms < kEthernetInitRetryMaxMs; ++i) {
    delay_ms = delay_ms > kEthernetInitRetryMaxMs / 2
        ? kEthernetInitRetryMaxMs : delay_ms * 2;
  }
  return delay_ms;
}

static inline bool ethernetInitRetryDue(uint8_t attempt,
                                        uint32_t now_ms,
                                        uint32_t last_attempt_ms) {
  return attempt == 0 ||
         (uint32_t)(now_ms - last_attempt_ms) >=
             ethernetInitRetryDelayMs(attempt);
}

static constexpr bool ethernetNoIpRecoveryDue(bool initialized,
                                              bool link_up,
                                              bool connected,
                                              uint32_t now_ms,
                                              uint32_t no_ip_since_ms) {
  return initialized && link_up && !connected && no_ip_since_ms != 0 &&
         (uint32_t)(now_ms - no_ip_since_ms) >= kEthernetNoIpRecoveryMs;
}

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

// Give the PHY a short window to report carrier. Once a definitive link-down
// sample arrives, do not stall mesh startup for the full DHCP deadline. A
// present link (or an unknown PHY state) still receives the entire probe.
static constexpr bool ethernetBootProbePending(bool ethernet_initialized,
                                                bool ethernet_connected,
                                                bool link_known,
                                                bool link_up,
                                                uint32_t elapsed_ms,
                                                uint32_t wait_ms) {
  return ethernet_initialized && !ethernet_connected && elapsed_ms < wait_ms &&
         (!link_known || link_up || elapsed_ms < kEthernetNoLinkBootGraceMs);
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

static constexpr bool manualOtaTimeoutDue(uint32_t now_ms,
                                          uint32_t started_ms,
                                          bool upload_in_progress) {
  return !upload_in_progress &&
         (uint32_t)(now_ms - started_ms) >= kManualOtaSessionTimeoutMs;
}

}  // namespace NetworkPolicy
