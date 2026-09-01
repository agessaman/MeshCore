#pragma once

#include <stdint.h>

enum class NetworkTransition : uint8_t {
  None,
  Up,
  Down,
};

namespace NetworkPolicy {

struct MQTTTransitionActions {
  bool disconnect_slots;
  bool retry_disconnected_slots_now;
};

static constexpr MQTTTransitionActions mqttActions(NetworkTransition transition) {
  return {
      transition == NetworkTransition::Down,
      transition == NetworkTransition::Up,
  };
}

// `start ota` uses the selected LAN only when reachable and not explicitly
// forced to SoftAP. Manifest OTA has no fallback and checks connectivity itself.
static constexpr bool startOtaUsesSelectedNetwork(bool force_ap,
                                                  bool network_connected) {
  return !force_ap && network_connected;
}

}  // namespace NetworkPolicy
