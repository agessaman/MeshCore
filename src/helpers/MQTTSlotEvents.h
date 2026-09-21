#pragma once
#include "MQTTClientState.h"
#include "MQTTEventChannel.h"

// The production transition, independent of SDK handles. Called only by the
// worker; callbacks supply records through MqttEventChannel.
template<class Slot>
bool applyMqttSlotEvent(Slot& slot, const MqttSlotEvent& event,
                        bool& force_jwt_mint, bool& publish_status) {
  if (event.incarnation != slot.incarnation || !mqttClientStateIsLive(slot.client_state)) return false;
  switch (event.kind) {
    case MqttSlotEvent::Kind::Connected:
      if (!slot.enabled || (slot.client_state != MqttClientState::Starting &&
                            slot.client_state != MqttClientState::Disconnected)) return false;
      slot.client_state = MqttClientState::Connected;
      slot.connected = true;
      slot.connected_at_ms = event.at_ms;
      // A CONNACK does not reset the backoff: maintenance waits for stability.
      slot.circuit_breaker_tripped = false;
      slot.last_tls_err = 0;
      slot.last_tls_stack_err = 0;
      slot.last_sock_errno = 0;
      slot.last_connack_code = 0;
      slot.last_error_time = 0;
      slot.current_outage_started_ms = 0;
      force_jwt_mint = false;
      publish_status = true;
      break;
    case MqttSlotEvent::Kind::Disconnected:
      slot.client_state = MqttClientState::Disconnected;
      ++slot.disconnect_count;
      if (!slot.first_disconnect_time) slot.first_disconnect_time = event.at_ms;
      if (!slot.current_outage_started_ms) slot.current_outage_started_ms = event.at_ms;
      slot.connected = false;
      slot.connected_at_ms = 0;
      publish_status = false;
      break;
    case MqttSlotEvent::Kind::Error:
      slot.last_tls_err = event.tls;
      slot.last_tls_stack_err = event.tls_stack;
      slot.last_sock_errno = event.socket;
      slot.last_connack_code = event.connack;
      slot.last_error_time = event.at_ms;
      if (event.connack) force_jwt_mint = true;
      break;
  }
  return true;
}

template<class Slot, class Start>
int startMqttSlotAttempt(Slot& slot, Start start) {
  const MqttClientState previous = slot.client_state;
  slot.client_state = MqttClientState::Starting;
  const int result = start();
  if (result == 0) {
    ++slot.generation;
    slot.applied_token_expires_at = slot.token_expires_at;
  } else {
    slot.client_state = previous;
  }
  return result;
}
