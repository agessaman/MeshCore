#pragma once

// When the mesh loop asks CoreScope for a control session, what the session
// asks for, and how its policy answer folds into the copy in use. A TLS
// session is expensive, so one is filed only when something depends on fresh
// data, and never more often than every control period. Pure.

#include "PollControlTypes.h"
#include "PollPolicy.h"
#include "PollRegistration.h"

#include <stdint.h>

namespace poll {

static const uint32_t kControlEveryS = 600;       // the plugin's policy and owners refresh
static const uint32_t kControlTimeoutS = 180;     // a filed request the bridge never answered
static const uint32_t kOwnersReachableS = 900;    // owners this recent stand in for the plugin's battery GET
static const uint32_t kSharedKeyMaxAgeS = kDay;   // the plugin caches the shared key a day

struct ControlNeed {
  bool enabled;
  bool has_url;
  bool clock_ok;
  bool registration_due;   // registrationDue() and an own key exists
  bool claim_unanswered;   // community on and a claim was published after owners last answered
  bool gate_closed;
  bool starting;           // the startup hold is active
  bool policy_answered;    // the policy endpoint answered 200 or 404 this boot
  bool target_due;         // a target's scheduled slot is due now
  bool manual_due;         // a `poll now` waits on a check first, as the plugin's Test refreshes the policy
};

static inline bool controlNeeded(const ControlNeed& n) {
  if (!n.enabled || !n.has_url || !n.clock_ok) return false;
  return n.registration_due || n.claim_unanswered || n.gate_closed || n.starting ||
         !n.policy_answered || n.target_due || n.manual_due;
}

// One session at a time, at most one per every_s, whatever is due.
static inline bool controlAllowed(bool pending, bool ever_requested, uint32_t since_last_s,
                                  uint32_t every_s) {
  if (pending) return false;
  return !ever_requested || since_last_s >= every_s;
}

// The shared key is fetched while the own key is not usable and none fresh is
// held. A first registration answers for itself: on a 404 the client fetches
// the shared key in the same session, and on success it is not needed.
static inline bool sharedKeyWanted(bool have_shared, uint32_t shared_at,
                                   const RegistrationState& reg, bool registering, uint32_t now) {
  if (chooseKey(reg, false, now) == KEY_OWN) return false;
  if (have_shared && elapsedSec(now, shared_at) < kSharedKeyMaxAgeS) return false;
  if (registering && reg.tried_at == 0) return false;
  return true;
}

// A shared key is cached a day; past that it stays in use only until a renewal fails, as the
// plugin replaces an expired cache entry with the failed lookup.
static inline bool sharedKeyUsable(bool have_shared, uint32_t shared_at, bool renewal_failed,
                                   uint32_t now) {
  if (!have_shared) return false;
  return elapsedSec(now, shared_at) < kSharedKeyMaxAgeS || !renewal_failed;
}

static inline uint8_t controlWants(bool community, bool registering, bool want_shared) {
  // The client skips the policy GET when the owners reply carried one.
  uint8_t w = WANT_POLICY;
  if (community) w |= WANT_OWNERS;
  if (registering) w |= WANT_REGISTER;
  if (want_shared) w |= WANT_SHARED_KEY;
  return w;
}

enum PolicyAnswer : uint8_t {
  POLICY_KEPT = 0,   // no usable answer: the copy in use and its switches stay
  POLICY_APPLIED,    // a 200: the server's fields, clamped
  POLICY_NONE,       // a 404: this CoreScope has no policy; the copy in use stays
};

// Only a 200 replaces the copy, as community.apply does; a 404 or a failure
// keeps it (community.note_failure), so a saved switch survives both.
static inline uint8_t applyPolicyAnswer(Policy& p, bool ok, int16_t status, const Policy& server,
                                        uint32_t now, bool fast = false) {
  if (ok) {
    p = server;
    policyClamp(p, fast);
    p.ever_fetched = true;
    p.fetched_at = now;
    p.last_status = 200;
    return POLICY_APPLIED;
  }
  p.last_status = status;
  return status == 404 ? POLICY_NONE : POLICY_KEPT;
}

static inline bool savedGateEqual(const SavedGate& a, const SavedGate& b) {
  return a.blocked == b.blocked && a.version_disabled == b.version_disabled &&
         a.paused == b.paused && a.paused_until == b.paused_until;
}

// The gate in force: the policy fetched this boot, else the copy saved in flash.
static inline uint8_t gateInForce(const Policy& p, const SavedGate& saved, const char* own_version,
                                  uint32_t now) {
  if (p.ever_fetched) return policyGate(p, own_version, now);
  return savedGate(saved, now);
}

}  // namespace poll
