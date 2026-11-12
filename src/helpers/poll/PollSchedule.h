#pragma once

// Poll scheduling: which target is due, how slots are spread across an
// interval, the silence backoffs, and the per-window request cap. Ported from
// the observer plugin's schedule.py and poll_state.py. Pure and header-only:
// no clock reads, no heap, no floats. Times are epoch seconds or second
// durations.

#include <stdint.h>

#include "PollTypes.h"

namespace poll {

// A link that missed twice in a row is treated as gone for now.
static const uint8_t kNoReplyFastTries = 2;
static const uint8_t kFloodRepliesBeforeDaily = 2;

// Seconds from last to now. An unset stamp counts from the epoch; a stamp in
// the future counts as no progress, so a stepped clock can't make a target
// look overdue.
static inline uint32_t elapsedSince(uint32_t last, uint32_t now) {
  if (last == 0) return now;
  if (last > now) return 0;
  return now - last;
}

// ceil(every / 2), matching the float half-period in schedule.py.
static inline uint32_t halfPeriod(uint32_t every) { return every / 2 + every % 2; }

// Start of the latest slot at or before now. Negative before the first phase
// in the epoch, which keeps a fresh clock from looking due.
static inline int64_t slotStart(uint32_t now, uint32_t every, uint32_t phase) {
  int64_t span = (int64_t)now - (int64_t)phase;
  int64_t period = (int64_t)every;
  int64_t index = span / period;
  if (span % period != 0 && span < 0) index--;  // floor, not truncation
  return index * period + (int64_t)phase;
}

// The same relative slot in a longer period; false when there is no phase.
static inline bool scaledPhase(uint32_t phase, uint32_t interval_s, uint32_t every, uint32_t* out) {
  if (interval_s == 0) return false;
  *out = (uint32_t)(((uint64_t)phase * every) / interval_s);
  return true;
}

// Even offset per target so targets sharing an interval are spread across it:
// interval * index / count. index is the position among targets with the same
// interval, sorted by public key (memcmp order).
static inline uint32_t spreadPhase(uint32_t interval_s, uint8_t index, uint8_t count) {
  if (count == 0 || index >= count) return 0;
  return (uint32_t)(((uint64_t)interval_s * index) / count);
}

// Due once per every: at phase into each period, and only if at least half a
// period passed since last. Without a phase, due every seconds after last.
static inline bool slotDue(uint32_t last, uint32_t now, uint32_t every, uint32_t phase,
                           bool has_phase) {
  if (every == 0) return true;  // nothing to divide by; treat as always due
  uint32_t elapsed = elapsedSince(last, now);
  if (!has_phase) return elapsed >= every;
  int64_t slot = slotStart(now, every, phase);
  // last < slot also rules out a poll in the current period, and a future last.
  return (int64_t)last < slot && elapsed >= halfPeriod(every);
}

// Port of schedule.is_due for the guest-key path, which has no password
// branches. A target that answered as admin, or that is held by next_due_at,
// is never due.
static inline bool isDue(const TargetState& t, uint32_t interval_s, uint32_t now, uint32_t phase,
                         uint32_t retry_s = 20 * 60) {
  if (t.admin_seen) return false;
  if (t.next_due_at > now) return false;

  uint32_t last = t.last_attempt;
  if (t.unheard_since != 0 && elapsedSince(t.unheard_since, now) >= kDay) {
    uint32_t scaled = 0;
    bool has_phase = scaledPhase(phase, interval_s, kDay, &scaled);
    return slotDue(last, now, kDay, scaled, has_phase);
  }
  if (t.flood_replies >= kFloodRepliesBeforeDaily) {
    uint32_t every = interval_s > kDay ? interval_s : kDay;
    uint32_t scaled = 0;
    bool has_phase = scaledPhase(phase, interval_s, every, &scaled);
    return slotDue(last, now, every, scaled, has_phase);
  }
  if (t.code == CODE_NO_BATTERY) {
    uint32_t every = interval_s > kDay ? interval_s : kDay;
    uint32_t scaled = 0;
    bool has_phase = scaledPhase(phase, interval_s, every, &scaled);
    return slotDue(last, now, every, scaled, has_phase);
  }
  if (t.code == CODE_NO_REPLY) {
    // Quick retries only for a target heard within the last day, so a dead
    // link doesn't cost a login every retry period.
    bool recent = elapsedSince(t.last_success, now) < kDay;
    if (recent && t.no_reply_count < kNoReplyFastTries) {
      uint32_t retry = interval_s < retry_s ? interval_s : retry_s;
      return elapsedSince(last, now) >= retry;
    }
  }
  return slotDue(last, now, interval_s, phase, true);
}

// Roughly when the normal schedule next polls, for status display.
static inline uint32_t nextSlot(const TargetState& t, uint32_t interval_s, uint32_t now,
                                uint32_t phase) {
  if (interval_s == 0) return now;
  uint32_t last = t.last_attempt;
  int64_t slot = slotStart(now, interval_s, phase);
  if ((int64_t)last < slot && elapsedSince(last, now) >= halfPeriod(interval_s)) return now;
  int64_t next = slot + (int64_t)interval_s;
  if (next <= 0) return 0;
  if (next > (int64_t)0xFFFFFFFFUL) return 0xFFFFFFFFUL;
  return (uint32_t)next;
}

// Per-window request cap: true once max_requests polls started inside window_s.
// *until is then the moment the oldest of them leaves the window.
static inline bool capReached(const TargetState& t, uint32_t window_s, uint8_t max_requests,
                              uint32_t now, uint32_t* until) {
  uint8_t count = 0;
  uint32_t oldest = 0;
  for (uint8_t i = 0; i < kSendHistory; i++) {
    uint32_t at = t.sends[i];
    if (at == 0) continue;  // empty slot
    if ((int64_t)now - (int64_t)at >= (int64_t)window_s) continue;  // left the window
    if (count == 0 || at < oldest) oldest = at;
    count++;
  }
  // A zero limit blocks rather than floods; it is a policy that hasn't loaded.
  if (count < max_requests) return false;
  if (until != nullptr) *until = count ? oldest + window_s : now;
  return true;
}

// Record that a poll started now.
static inline void recordSend(TargetState& t, uint32_t now) {
  t.last_attempt = now;
  for (uint8_t i = 0; i + 1 < kSendHistory; i++) t.sends[i] = t.sends[i + 1];
  t.sends[kSendHistory - 1] = now;
}

// Fold a finished poll into the saved state, as poll_state.record does for the
// fields kept here. last_attempt is set by recordSend at the start, not here.
static inline void recordResult(TargetState& t, uint8_t code, uint32_t now, bool was_quick_retry) {
  t.code = code;
  t.retried = was_quick_retry;
  if (code == CODE_NO_REPLY) {
    // Past 255 the count returns to 253 so the modulo-3 login cadence stays exact.
    t.no_reply_count = (t.no_reply_count == 255) ? 253 : (uint8_t)(t.no_reply_count + 1);
    if (t.unheard_since == 0) t.unheard_since = now;
    // A target merely out of reach hasn't changed its guest password, so the
    // learned one is kept through an outage of this length, whatever it is.
    if (elapsedSince(t.unheard_since, now) >= 7 * kDay) t.learned_pw = LEARNED_NONE;
    return;
  }
  // Any answer at all proves the target alive: stop the silence clock.
  if (code != CODE_NOT_DIRECT && code != CODE_ERROR) t.unheard_since = 0;
  if (code == CODE_OK || code == CODE_NO_BATTERY) {
    t.last_success = now;
    t.no_reply_count = 0;
  }
}

// Pick the due target with the oldest last attempt, or -1. Scheduled polls keep
// gap_s between starts so a backlog drains gently; 0 means none started yet.
static inline int pickDue(const TargetState* targets, const bool* due, uint8_t count, uint32_t now,
                          uint32_t last_scheduled_at, uint32_t gap_s = 60) {
  if (targets == nullptr || due == nullptr || count == 0) return -1;
  if (last_scheduled_at != 0 && elapsedSince(last_scheduled_at, now) < gap_s) return -1;
  int best = -1;
  uint32_t best_last = 0;
  for (uint8_t i = 0; i < count; i++) {
    if (!due[i]) continue;
    uint32_t last = targets[i].last_attempt;
    if (best < 0 || last < best_last) {  // ties keep the earlier index
      best = i;
      best_last = last;
    }
  }
  return best;
}

}  // namespace poll
