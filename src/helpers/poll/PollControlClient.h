#pragma once

// One CoreScope control session (owners, policy, registration, shared key)
// over one HTTP/1.1 connection. Blocking; the MQTT bridge runs it on a
// short-lived task of its own, never on the mesh loop or a CLI handler.

#include "PollBuild.h"

#if defined(WITH_REPEATER_POLL)

#include <atomic>

#include "PollControlTypes.h"

namespace poll {

static const uint32_t kControlConnectMs = 5000;
// mbedTLS checks its limit only between handshake steps, and one step that
// verifies a P-384 chain in software takes about 5 s on an ESP32-S3.
static const uint32_t kControlHandshakeMs = 12000;
static const uint32_t kControlResponseMs = 5000;
static const uint32_t kControlBudgetMs = 25000;
static const uint16_t kControlBodyMax = 6144;

// Negative request statuses (see ControlResult).
enum ControlFailure : int16_t {
  CTL_ERR_CONNECT = -1,    // DNS, TCP or TLS setup failed
  CTL_ERR_TIMEOUT = -2,    // no complete reply in time, or the session budget ran out
  CTL_ERR_OVERSIZE = -3,   // the body is over kControlBodyMax
  CTL_ERR_PROTOCOL = -4,   // not HTTP, or a 200 whose body is not a JSON object
  CTL_ERR_LINK = -5,       // the network link dropped, or the session was told to stop
  CTL_ERR_NO_MEMORY = -6,
};

struct ControlEnv {
  const std::atomic<bool>* abort;   // raised by the bridge on stop; may be null
  bool tls_bundle_ready;            // the global CA bundle is loaded
};

// Fills res (started = true, finished_at left to the caller). Never logs a key.
void runControlSession(const ControlRequest& req, ControlResult& res, const ControlEnv& env);

// Wipes private key material from res.
void wipeControlResult(ControlResult& res);

}  // namespace poll

#endif  // WITH_REPEATER_POLL
