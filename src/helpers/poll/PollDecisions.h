#pragma once

// Whether this observer polls a repeater, from CoreScope's owners reply.
// Freshness uses that reply's latest_ts. There is no per-poll battery GET.
// No clock reads. latest_ts 0 means no reading; owners_ok_at 0 means CoreScope
// has never answered. A stamp still in the future has not elapsed.

#include "PollTypes.h"

#include <stdint.h>
#include <string.h>

namespace poll {

static const uint32_t kOwnersStaleS = 1800;
static const uint32_t kStartupHoldS = 2UL * 600UL + 60UL;
static const uint32_t kHoldRecheckS = 600;

enum OwnerRole : uint8_t {
  OWNER_ROLE_ABSENT = 0,
  OWNER_ROLE_POLLING,
  OWNER_ROLE_INDEPENDENT,
  OWNER_ROLE_STANDBY,
  OWNER_ROLE_EXCLUDED,
  OWNER_ROLE_PAUSED,
  OWNER_ROLE_DISABLED,
  OWNER_ROLE_UNKNOWN
};

// One node's entry in the owners reply, already extracted from JSON.
struct OwnerEntry {
  bool     present;        // the owners reply has an entry for this node
  uint8_t  role;           // OwnerRole
  bool     mine;
  bool     excluded;
  uint8_t  standby_rank;   // >= 1; default 1
  uint8_t  owner_hops;
  uint32_t latest_ts;      // newest reading CoreScope has for the node, 0 = none
};

enum SkipReason : uint8_t {
  SKIP_NONE = 0,
  SKIP_GATE,
  SKIP_EXCLUDED,
  SKIP_CAP,
  SKIP_FRESH,
  SKIP_STANDBY,
  SKIP_STANDBY_HOLD,
  SKIP_STARTUP_HOLD,
  SKIP_ROLE
};

struct Decision {
  bool     poll;
  uint8_t  reason;   // SkipReason
  uint32_t until;    // when to look again, 0 = unknown
};

struct FreshnessInput {
  uint8_t  role;             // poll::Role (poller or standby)
  uint8_t  rank;             // standby rank
  uint32_t window_s;         // policyWindow(...)
  uint32_t grace_s;
  bool     corescope_reachable;   // owners answered recently enough to trust latest_ts
  uint32_t latest_ts;        // newest reading known, 0 = none
  uint32_t standby_since;    // TargetState::standby_since
  uint32_t hold_from;        // max(start time, last time the gate was closed)
  uint32_t hold_recheck_s;   // 600
};

static inline uint8_t ownerRoleFromText(const char* text) {
  if (!text || !text[0]) return OWNER_ROLE_ABSENT;
  if (strcmp(text, "polling") == 0) return OWNER_ROLE_POLLING;
  if (strcmp(text, "independent") == 0) return OWNER_ROLE_INDEPENDENT;
  if (strcmp(text, "standby") == 0) return OWNER_ROLE_STANDBY;
  if (strcmp(text, "excluded") == 0) return OWNER_ROLE_EXCLUDED;
  if (strcmp(text, "paused") == 0) return OWNER_ROLE_PAUSED;
  if (strcmp(text, "disabled") == 0) return OWNER_ROLE_DISABLED;
  return OWNER_ROLE_UNKNOWN;
}

// Community off, or no entry: poller. An older server that omits role and
// does not mark the node mine leaves this observer on standby.
static inline uint8_t mapRole(bool community, const OwnerEntry& e) {
  if (!community || !e.present) return ROLE_POLLER;
  if (e.role == OWNER_ROLE_EXCLUDED || e.excluded) return ROLE_EXCLUDED;
  if (e.role == OWNER_ROLE_PAUSED) return ROLE_PAUSED;
  if (e.role == OWNER_ROLE_DISABLED) return ROLE_DISABLED;
  if (e.role == OWNER_ROLE_POLLING || e.role == OWNER_ROLE_INDEPENDENT ||
      (e.role == OWNER_ROLE_ABSENT && e.mine)) {
    return ROLE_POLLER;
  }
  if (!e.mine) return ROLE_STANDBY;
  return ROLE_POLLER;
}

static inline const char* skipReasonName(uint8_t reason) {
  switch (reason) {
    case SKIP_GATE: return "gate";
    case SKIP_EXCLUDED: return "excluded";
    case SKIP_CAP: return "cap";
    case SKIP_FRESH: return "fresh";
    case SKIP_STANDBY: return "standby";
    case SKIP_STANDBY_HOLD: return "standby_hold";
    case SKIP_STARTUP_HOLD: return "starting";
    case SKIP_ROLE: return "role";
    default: return "";
  }
}

static inline uint32_t satAdd(uint32_t a, uint32_t b) {
  if (a > UINT32_MAX - b) return UINT32_MAX;
  return a + b;
}

static inline uint32_t satMul(uint32_t a, uint32_t b) {
  if (a != 0 && b > UINT32_MAX / a) return UINT32_MAX;
  return a * b;
}

// True while now is still strictly before base + span. A span that does not
// fit in uint32 is treated as not yet elapsed, so the subtract cannot wrap.
static inline bool beforeDeadline(uint32_t now, uint32_t base, uint32_t span) {
  if (span > UINT32_MAX - base) return true;
  return now < base + span;
}

static inline Decision makeDecision(bool poll_now, uint8_t reason, uint32_t until) {
  Decision d;
  d.poll = poll_now;
  d.reason = reason;
  d.until = until;
  return d;
}

static inline Decision decideFreshness(const FreshnessInput& in, uint32_t now) {
  const bool standby = in.role == ROLE_STANDBY;
  // Rank 0 would drop the grace and let every standby ask with the poller.
  const uint32_t rank = (standby && in.rank < 1) ? 1U : in.rank;
  uint32_t wait = in.window_s;
  if (standby) wait = satAdd(wait, satMul(in.grace_s, rank));

  if (!in.corescope_reachable) {
    if (!standby) return makeDecision(true, SKIP_NONE, 0);
    return makeDecision(false, SKIP_STANDBY_HOLD, satAdd(now, in.hold_recheck_s));
  }

  if (in.latest_ts == 0 && standby) {
    const uint32_t base = in.standby_since > in.hold_from ? in.standby_since : in.hold_from;
    if (beforeDeadline(now, base, wait)) {
      return makeDecision(false, SKIP_STANDBY, satAdd(base, wait));
    }
    return makeDecision(true, SKIP_NONE, 0);
  }

  uint32_t latest = in.latest_ts;
  // A pause or a restart zeros every timer; start the wait over or the
  // poller and its standbys all ask on the same tick.
  if (latest != 0 && standby && in.hold_from > latest) latest = in.hold_from;

  if (latest != 0 && beforeDeadline(now, latest, wait)) {
    const uint8_t reason = standby ? SKIP_STANDBY : SKIP_FRESH;
    return makeDecision(false, reason, satAdd(latest, wait));
  }
  return makeDecision(true, SKIP_NONE, 0);
}

// Forget the owners map once it has not been answered for stale_s.
// A zero stamp has never been answered, so the observer polls itself.
static inline bool ownersLapsed(uint32_t owners_ok_at, uint32_t now, uint32_t stale_s = kOwnersStaleS) {
  if (owners_ok_at == 0) return true;
  if (owners_ok_at > now) return false;
  return (now - owners_ok_at) > stale_s;
}

// Scheduled polls wait out the restart stampede: a claim must have gone out
// and owners must have answered strictly after it, unless the hold has run out.
static inline bool startupHold(bool community, bool has_url, uint32_t started_at,
                               uint32_t claim_sent_at, uint32_t owners_ok_at, uint32_t now) {
  if (!community || !has_url) return false;
  bool expired = false;
  if (started_at == 0) {
    expired = now >= kStartupHoldS;
  } else if (started_at <= now) {
    expired = (now - started_at) >= kStartupHoldS;
  }
  if (expired) return false;
  if (claim_sent_at == 0 || owners_ok_at == 0) return true;
  const uint32_t mark = started_at > claim_sent_at ? started_at : claim_sent_at;
  return !(owners_ok_at > mark);
}

}  // namespace poll
