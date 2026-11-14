#pragma once

// Whether a target that is due (or asked for with `poll now`) is polled, in the
// order the plugin's poll_decisions.skip_reason checks: CoreScope's gate, the
// policy's exclusions, the request cap, the owner's role, the startup hold,
// then freshness. Pure: the caller works out the inputs and passes now.

#include "PollDecisions.h"
#include "PollPolicy.h"
#include "PollSchedule.h"
#include "PollTypes.h"

#include <stdint.h>

namespace poll {

struct CycleInput {
  bool     test;                  // `poll now`: no cap, role, startup hold or freshness
  uint8_t  gate;                  // poll::Gate
  bool     excluded;              // on the policy's exclusion list
  uint8_t  role;                  // mapRole(...)
  uint8_t  rank;                  // standby rank
  bool     starting;              // startupHold(...)
  bool     has_url;
  uint32_t window_s;              // policyWindow(...)
  uint8_t  max_requests;
  uint32_t grace_s;
  bool     corescope_reachable;   // owners answered recently enough to trust latest_ts
  uint32_t latest_ts;
  uint32_t hold_from;             // max(start time, last time the gate was closed)
  uint32_t hold_recheck_s;
};

// Sets t.standby_since as the role says, and t.next_due_at when the answer is
// "not before then" (cap, fresh, standby). A gate, exclusion, role or startup
// skip leaves next_due_at alone: those lift when CoreScope says so.
static inline Decision decideCycle(TargetState& t, const CycleInput& in, uint32_t now) {
  if (in.gate != GATE_OPEN) return makeDecision(false, SKIP_GATE, 0);
  if (in.excluded) return makeDecision(false, SKIP_EXCLUDED, 0);
  if (in.test) return makeDecision(true, SKIP_NONE, 0);
  uint32_t until = 0;
  if (capReached(t, in.window_s, in.max_requests, now, &until)) {
    t.next_due_at = until;
    return makeDecision(false, SKIP_CAP, until);
  }
  if (in.role == ROLE_EXCLUDED) return makeDecision(false, SKIP_EXCLUDED, 0);
  if (in.role == ROLE_PAUSED || in.role == ROLE_DISABLED) return makeDecision(false, SKIP_ROLE, 0);
  if (in.role == ROLE_STANDBY) {
    if (t.standby_since == 0) t.standby_since = now;
  } else {
    t.standby_since = 0;
  }
  // No next_due_at: polls start on the first tick after CoreScope answers.
  if (in.starting) return makeDecision(false, SKIP_STARTUP_HOLD, 0);
  if (!in.has_url) return makeDecision(true, SKIP_NONE, 0);

  FreshnessInput f;
  f.role = in.role;
  f.rank = in.rank;
  f.window_s = in.window_s;
  f.grace_s = in.grace_s;
  f.corescope_reachable = in.corescope_reachable;
  f.latest_ts = in.latest_ts;
  f.standby_since = t.standby_since;
  f.hold_from = in.hold_from;
  f.hold_recheck_s = in.hold_recheck_s;
  const Decision d = decideFreshness(f, now);
  if (!d.poll && d.until != 0) t.next_due_at = d.until;
  return d;
}

}  // namespace poll
