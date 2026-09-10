#pragma once

#include <stdint.h>
#include <string.h>

// The complete, owned description of what a slot's MQTT client should be
// configured with, plus the decision of whether the existing SDK client can be
// reconfigured in place or has to be recreated.
//
// Pure logic: no Arduino, esp-mqtt or PsychicMqttClient dependency, so every
// transition in the table below is exercised on the host
// (test/test_mqtt_effective_config).
//
// Why this exists (F02/F03). The bridge used to apply configuration field by
// field, skipping the cleanup of stale fields whenever
// `slot.initial_connect_done` was false — which `teardownSlot()` had just
// cleared, so normal live reconfiguration always skipped it. On hardware that
// sent the previous broker's JWT username in the CONNECT to a newly configured
// anonymous endpoint. Two rules fix it:
//
//   1. Every field is written on every apply. "Absent" is a value that gets
//      written (the empty string), never an omission — IDF's
//      `esp_mqtt_set_if_config()` treats NULL as "leave unchanged", so a
//      cleared wrapper pointer cannot erase an SDK-held credential, while an
//      empty string both overwrites it and leaves the CONNECT's username flag
//      clear (verified on an ESP32-S3 against a local broker, IDF 4.4).
//   2. Where a field cannot be overwritten safely, the client is recreated.
//      That is the exception, not the rule: esp-mqtt's stop/start cycle is the
//      fork's documented internal-heap fragmentation driver, so recreation must
//      never happen on the reconnect or token-renewal path.

// Matches MQTTSlot::broker_uri so an effective config can hold its own copy.
#define MQTT_EFFECTIVE_URI_MAX 128

enum class MqttTransport : uint8_t {
  Unknown = 0,
  Tcp,   // mqtt://
  Tls,   // mqtts://
  Ws,    // ws://
  Wss,   // wss://
};

enum class MqttTrust : uint8_t {
  Plaintext = 0,  // unencrypted transport: no server verification exists
  Bundle,         // shared CA bundle attach callback
  PemCert,        // one specific CA certificate
};

enum class MqttAuth : uint8_t {
  None = 0,
  UserPass,
  Jwt,
};

struct MqttEffectiveConfig {
  char          uri[MQTT_EFFECTIVE_URI_MAX] = {0};
  MqttTransport transport = MqttTransport::Unknown;
  MqttTrust     trust = MqttTrust::Plaintext;
  MqttAuth      auth = MqttAuth::None;
  // Borrowed pointers with a lifetime at least as long as the client's: preset
  // certificates live in flash, credentials in the slot/bridge storage.
  const char*   pem = nullptr;
  const char*   username = nullptr;   // nullptr == absent, written as ""
  const char*   password = nullptr;
  uint16_t      buffer_size = 0;
  uint16_t      keepalive = 0;
  bool          valid = false;        // false until successfully built
};

// Client certificates are deliberately absent: `setClientCertificate()` has no
// caller in the firmware (no broker preset uses mutual TLS), so modelling it
// here would be untested configuration surface. Adding mutual TLS means adding
// it to this struct and to the recreate decision below.

static inline MqttTransport mqttTransportFromUri(const char* uri) {
  if (uri == nullptr) return MqttTransport::Unknown;
  if (strncmp(uri, "mqtts://", 8) == 0) return MqttTransport::Tls;
  if (strncmp(uri, "mqtt://", 7) == 0)  return MqttTransport::Tcp;
  if (strncmp(uri, "wss://", 6) == 0)   return MqttTransport::Wss;
  if (strncmp(uri, "ws://", 5) == 0)    return MqttTransport::Ws;
  return MqttTransport::Unknown;
}

static inline bool mqttTransportIsEncrypted(MqttTransport t) {
  return t == MqttTransport::Tls || t == MqttTransport::Wss;
}

static inline const char* mqttTransportName(MqttTransport t) {
  switch (t) {
    case MqttTransport::Tcp: return "mqtt";
    case MqttTransport::Tls: return "mqtts";
    case MqttTransport::Ws:  return "ws";
    case MqttTransport::Wss: return "wss";
    default:                 return "?";
  }
}

// A credential the SDK must be told to forget is written as "" rather than
// left NULL. See rule 1 above.
static inline const char* mqttFieldOrEmpty(const char* v) { return v ? v : ""; }

// Builds an effective config, deriving the transport from the URI and
// normalising the trust policy against it: an unencrypted transport verifies
// nothing, whatever certificate material was requested, so recording anything
// else would make a plaintext endpoint look like a trust change away from a
// verified one (and vice versa). Returns false for an empty or unrecognised
// URI, leaving *out invalid.
static inline bool mqttBuildEffectiveConfig(const char* uri,
                                            MqttAuth auth,
                                            const char* username,
                                            const char* password,
                                            MqttTrust requested_trust,
                                            const char* pem,
                                            uint16_t buffer_size,
                                            uint16_t keepalive,
                                            MqttEffectiveConfig* out) {
  if (out == nullptr) return false;
  *out = MqttEffectiveConfig();
  if (uri == nullptr || uri[0] == '\0') return false;
  if (strlen(uri) >= MQTT_EFFECTIVE_URI_MAX) return false;

  const MqttTransport transport = mqttTransportFromUri(uri);
  if (transport == MqttTransport::Unknown) return false;

  strncpy(out->uri, uri, MQTT_EFFECTIVE_URI_MAX - 1);
  out->uri[MQTT_EFFECTIVE_URI_MAX - 1] = '\0';
  out->transport = transport;
  out->auth = auth;
  out->username = username;
  out->password = password;
  out->buffer_size = buffer_size;
  out->keepalive = keepalive;

  if (!mqttTransportIsEncrypted(transport)) {
    out->trust = MqttTrust::Plaintext;
    out->pem = nullptr;
  } else if (requested_trust == MqttTrust::PemCert && pem != nullptr) {
    out->trust = MqttTrust::PemCert;
    out->pem = pem;
  } else if (requested_trust == MqttTrust::Bundle) {
    out->trust = MqttTrust::Bundle;
  } else {
    // Encrypted transport with no usable trust material. Recorded as Plaintext
    // trust so it is never confused with a verified policy; whether to connect
    // at all is the caller's decision.
    out->trust = MqttTrust::Plaintext;
  }

  out->valid = true;
  return true;
}

struct MqttRecreateDecision {
  bool        recreate = false;
  const char* reason = "reuse";   // static literal, for logs and tests
};

// Whether the SDK client that `applied` was written to can be reconfigured in
// place to reach `desired`.
//
//   transport (scheme) change   -> recreate. One `wss`->`mqtt` transition was
//       observed working by reuse on hardware, but that proves one direction
//       once, not that no websocket transport state survives the switch; the
//       retained-`frame_state` bug class is real and unfixed upstream even in
//       IDF 5.3, and this transition only happens when an operator changes the
//       endpoint.
//   trust change                -> recreate. `cert_pem` cannot be cleared by
//       writing "" (an empty PEM is a parse failure, not "no certificate"), and
//       the bundle attach is a function pointer whose set_config semantics are
//       not established on this build. Verification policy is the one field
//       where a stale value silently weakens security.
//   capacity growth             -> recreate. IDF 4.4 allocates the MQTT buffers
//       in `esp_mqtt_client_init()` and `esp_mqtt_set_config()` does not resize
//       them; the wrapper's own reassembly buffer is likewise allocated once.
//   everything else             -> reuse, including every credential and
//       auth-mode change, because those are rewritten unconditionally.
static inline MqttRecreateDecision mqttConfigRecreateDecision(
    const MqttEffectiveConfig& applied,
    const MqttEffectiveConfig& desired,
    uint16_t allocated_buffer_size) {
  MqttRecreateDecision d;
  if (!desired.valid) {
    d.recreate = false;
    d.reason = "invalid-desired";
    return d;
  }
  if (!applied.valid) {
    // Nothing applied yet: a fresh client needs configuring, not recreating.
    d.reason = "first-apply";
    return d;
  }
  if (applied.transport != desired.transport) {
    d.recreate = true;
    d.reason = "transport-change";
    return d;
  }
  if (applied.trust != desired.trust) {
    d.recreate = true;
    d.reason = "trust-change";
    return d;
  }
  if (desired.trust == MqttTrust::PemCert && applied.pem != desired.pem) {
    d.recreate = true;
    d.reason = "ca-cert-change";
    return d;
  }
  if (desired.buffer_size > allocated_buffer_size) {
    d.recreate = true;
    d.reason = "buffer-growth";
    return d;
  }
  return d;
}
