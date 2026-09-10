#pragma once

#include <stdint.h>

// What a slot's MQTT client is doing, as opposed to whether its network is up,
// and the two decisions that hang off it.
//
// Pure logic: no Arduino, esp-mqtt or bridge dependency, so the shutdown
// contract below is a host test rather than a hardware run.
//
// `client->connected()` answers "is the network up" and was being used for "is
// there anything to stop": a client resolving DNS, negotiating TLS or waiting
// after a failed CONNECT reports not-connected, so teardown skipped it and left
// its task running (F04).

enum class MqttClientState : uint8_t {
  Absent = 0,    // no client object
  Configured,    // client allocated, never started
  Starting,      // start/reconnect requested, awaiting CONNECTED
  Connected,     // CONNECTED received
  Disconnected,  // started, no session (our reconnect ladder governs it)
  Stopped,       // stop completed; the SDK task is joined and gone
  Quarantined,   // stop failed: the SDK task was NOT joined. Never destroy,
                 // never reuse, never free anything it still points at.
};

static inline const char* mqttClientStateName(MqttClientState s) {
  switch (s) {
    case MqttClientState::Absent:       return "absent";
    case MqttClientState::Configured:   return "configured";
    case MqttClientState::Starting:     return "starting";
    case MqttClientState::Connected:    return "connected";
    case MqttClientState::Disconnected: return "disconnected";
    case MqttClientState::Stopped:      return "stopped";
    case MqttClientState::Quarantined:  return "quarantined";
  }
  return "?";
}

// The client has been started and not proven stopped: it may still own a task,
// a socket and a TLS context. Quarantined is deliberately NOT live — it is
// worse: it may own them and we can never find out.
static inline bool mqttClientStateIsLive(MqttClientState s) {
  return s == MqttClientState::Starting || s == MqttClientState::Connected ||
         s == MqttClientState::Disconnected;
}

// A connection attempt is in flight, so an event for it can still arrive.
// Closing the transport is not enough here: PsychicMqttClient::softDisconnect()
// returns immediately when the client is not yet connected, which means an
// in-progress DNS/TLS/CONNECT is left to complete on its own. Cancelling one
// requires a real stop.
static inline bool mqttClientStateHasAttemptInFlight(MqttClientState s) {
  return s == MqttClientState::Starting;
}

// The SDK task is known to be gone (or never existed), so the object may be
// destroyed and anything it pointed at may be freed.
static inline bool mqttClientStateIsProvenStopped(MqttClientState s) {
  return s == MqttClientState::Absent || s == MqttClientState::Configured ||
         s == MqttClientState::Stopped;
}

// The bridge's cooperative stop may only be acknowledged when EVERY client is
// proven stopped.
//
// This is the invariant the whole shutdown contract rests on: the owner treats
// the acknowledgement as proof that the task destroyed its clients, and only
// then frees the queue and buffers and allows a restart. One client whose
// esp_mqtt_client_stop() never completed makes that false — its SDK task may
// still be running — so the acknowledgement must be withheld and the bridge
// left in its unproven state, however many other slots stopped cleanly.
static inline bool mqttStopMayBeAcknowledged(const MqttClientState* states, int count) {
  // An empty set is trivially proven (no slots, nothing to stop); a null array
  // with a nonzero count is a caller bug and must not read as proof.
  if (count <= 0) return true;
  if (states == nullptr) return false;
  for (int i = 0; i < count; i++) {
    if (!mqttClientStateIsProvenStopped(states[i])) return false;
  }
  return true;
}
