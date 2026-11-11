#pragma once

#include <stdint.h>

// Pure timing and state-transition policy used by MQTTBridge's connection
// maintenance loop. Keeping these decisions independent of Arduino, WiFi, and
// the MQTT client lets host tests exercise the exact production policy with a
// deterministic clock.
namespace MQTTConnectionPolicy {

static const uint32_t kReconnectGuardMs = 15000UL;
// Shortest gap between two first-time slot setups, even when one finishes instantly.
static const uint32_t kFirstSetupMinGapMs = 2000UL;
static const uint32_t kStableResetMs = 120000UL;
// Breaker probe interval in minutes (mqtt.breaker.interval). The floor stays
// above the 5-minute top backoff rung so a tripped slot still retries less often.
static const uint8_t kBreakerProbeMinMinutes = 10;
static const uint8_t kBreakerProbeMaxMinutes = 30;
static const uint8_t kBreakerProbeDefaultMinutes = 30;
static const uint32_t kCircuitBreakerProbeMs = kBreakerProbeDefaultMinutes * 60000UL;
static const uint32_t kRenewalThrottleMs = 60000UL;
static const uint32_t kSlotStaggerMs = 3000UL;
static const uint8_t kMaxFailuresAtMaxBackoff = 3;
static const uint32_t kDefaultJwtLifetimeSecs = 86400UL;
static const uint32_t kMaxJwtStaggerSecs = 300UL;
static const uint32_t kMinimumValidEpoch = 1000000000UL;
static const uint32_t kJwtReconnectSafetyMarginSecs = 60UL;
static const uint32_t kJwtClockThreshold = 1735689600UL; // 2025-01-01 UTC
// A wall clock at or past this instant was set from a real source (NTP or an
// admin); anything earlier is the firmware's unset-clock default (1715770351,
// 15 May 2024), so a delta spanning the sync is meaningless.
static const uint32_t kSyncedClockEpoch = kJwtClockThreshold;

// Unsigned subtraction is intentionally used: it is the standard millis()
// idiom and remains correct across a single 32-bit counter rollover.
static inline uint32_t elapsedMs(uint32_t now, uint32_t then) {
  return now - then;
}

static inline bool reconnectGuardActive(uint32_t now, uint32_t last_reconnect) {
  return elapsedMs(now, last_reconnect) < kReconnectGuardMs;
}

// First setup (last_attempt == 0) is due immediately. A stamped failure waits
// retry_interval_ms. last_cross_slot_ms == 0 means no handshake was armed, so
// it does not borrow reconnectGuardActive's epoch rule and hold the first slot
// for 15 s of uptime. already_attempted_this_pass is the one-setup budget.
// guard_released lets a first setup past the guard early (firstSetupGuardReleased).
static inline bool deferredSlotSetupDue(uint32_t now, uint32_t last_attempt,
                                        uint32_t retry_interval_ms,
                                        uint32_t last_cross_slot_ms,
                                        bool already_attempted_this_pass,
                                        bool guard_released = false) {
  if (already_attempted_this_pass) return false;
  if (last_cross_slot_ms != 0 && !guard_released &&
      reconnectGuardActive(now, last_cross_slot_ms)) {
    return false;
  }
  if (last_attempt == 0) return true;
  return elapsedMs(now, last_attempt) >= retry_interval_ms;
}

// The armer's handshake is over: it connected or failed and its on-connect work
// ran, or the slot was disabled or lost its client.
static inline bool firstSetupHandshakeDone(bool armer_enabled, bool armer_has_client,
                                           bool armer_attempt_pending,
                                           bool armer_completion_pending) {
  if (!armer_enabled || !armer_has_client) return true;
  return !armer_attempt_pending && !armer_completion_pending;
}

// A first setup may start before the 15 s guard expires when the guard was armed
// by another first setup whose handshake is done, but not within kFirstSetupMinGapMs.
// Anything else (retries, reconnects, reconfigures) keeps the full guard.
static inline bool firstSetupGuardReleased(uint32_t now, uint32_t last_cross_slot_ms,
                                           bool first_attempt, bool armed_by_first_setup,
                                           bool armer_handshake_done) {
  if (!first_attempt || !armed_by_first_setup || !armer_handshake_done) return false;
  return elapsedMs(now, last_cross_slot_ms) >= kFirstSetupMinGapMs;
}

// One reconfigure per pass. A request that does not open a connection is not
// held for the handshake guard; a connecting one is, so the caller can arm
// that guard and the next request waits its turn.
static inline bool slotReconfigureDue(bool pending, bool opens_connection,
                                     uint32_t now_ms, uint32_t last_connect_attempt_ms,
                                     bool already_applied_this_pass) {
  if (!pending || already_applied_this_pass) return false;
  if (!opens_connection) return true;
  return !reconnectGuardActive(now_ms, last_connect_attempt_ms);
}

static inline bool stableConnection(uint32_t now, uint32_t connected_at) {
  return connected_at != 0 && elapsedMs(now, connected_at) >= kStableResetMs;
}

static inline uint32_t reconnectBackoffMs(uint8_t reconnect_backoff) {
  static const uint32_t kBackoffMs[] = {
    10000UL, 30000UL, 60000UL, 120000UL, 300000UL
  };
  const uint8_t index = reconnect_backoff < 5 ? reconnect_backoff : 4;
  return kBackoffMs[index];
}

static inline uint32_t reconnectDelayMs(uint8_t reconnect_backoff, uint8_t slot_index) {
  return reconnectBackoffMs(reconnect_backoff) +
         static_cast<uint32_t>(slot_index) * kSlotStaggerMs;
}

static inline bool reconnectDue(uint32_t now, uint32_t last_attempt,
                                uint8_t reconnect_backoff, uint8_t slot_index) {
  return elapsedMs(now, last_attempt) >= reconnectDelayMs(reconnect_backoff, slot_index);
}

struct BackoffAdvance {
  uint8_t reconnect_backoff;
  uint8_t max_backoff_failures;
  bool circuit_breaker_tripped;
  bool should_reconnect;
};

// Advance the ladder immediately before a due reconnect. The first visit to
// the 300-second rung changes level 4 to the saturated marker 5. Three later
// failures at that rung trip the breaker; the third does not launch another
// connection attempt.
static inline BackoffAdvance advanceBackoff(uint8_t reconnect_backoff,
                                            uint8_t max_backoff_failures) {
  BackoffAdvance result = {
    reconnect_backoff, max_backoff_failures, false, true
  };
  if (result.reconnect_backoff < 5) {
    result.reconnect_backoff++;
    return result;
  }

  if (result.max_backoff_failures < UINT8_MAX) {
    result.max_backoff_failures++;
  }
  if (result.max_backoff_failures >= kMaxFailuresAtMaxBackoff) {
    result.circuit_breaker_tripped = true;
    result.should_reconnect = false;
  }
  return result;
}

static inline bool breakerProbeMinutesValid(int32_t minutes) {
  return minutes >= kBreakerProbeMinMinutes && minutes <= kBreakerProbeMaxMinutes;
}

// Parses CLI minutes; rejects non-digits and anything over three digits so a
// huge value cannot wrap into range.
static inline bool parseBreakerProbeMinutes(const char* text, uint8_t* out) {
  if (text == nullptr || out == nullptr || *text == '\0') return false;
  uint32_t value = 0;
  int digits = 0;
  for (const char* p = text; *p; ++p) {
    if (*p < '0' || *p > '9' || ++digits > 3) return false;
    value = value * 10U + static_cast<uint32_t>(*p - '0');
  }
  if (!breakerProbeMinutesValid(static_cast<int32_t>(value))) return false;
  *out = static_cast<uint8_t>(value);
  return true;
}

// Out-of-range stored values fall back to the default interval.
static inline uint32_t breakerProbeMs(uint8_t minutes) {
  return breakerProbeMinutesValid(minutes) ? minutes * 60000UL : kCircuitBreakerProbeMs;
}

static inline bool circuitBreakerProbeDue(uint32_t now, uint32_t last_attempt,
                                          uint32_t probe_ms) {
  return elapsedMs(now, last_attempt) >= probe_ms;
}

// The first reconnect after the link returns always makes a real attempt. The
// failures it would count may have been the link's, so it advances the ladder
// but cannot trip the breaker; the next ordinary due attempt can.
static inline BackoffAdvance advanceBackoffForLinkRecovery(uint8_t reconnect_backoff,
                                                           uint8_t max_backoff_failures) {
  BackoffAdvance result = advanceBackoff(reconnect_backoff, max_backoff_failures);
  result.circuit_breaker_tripped = false;
  result.should_reconnect = true;
  return result;
}

// Move a disconnected slot's deadline to "due now" after the physical link
// returns. Preserve its earned backoff and breaker state: a normal reconnect
// gets one immediate attempt at its current rung, while a tripped breaker gets
// one immediate probe. Unsigned subtraction keeps this wrap-safe.
static inline uint32_t immediateRetryLastAttempt(uint32_t now,
                                                bool circuit_breaker_tripped,
                                                uint8_t reconnect_backoff,
                                                uint8_t slot_index,
                                                uint32_t probe_ms) {
  const uint32_t delay = circuit_breaker_tripped
      ? probe_ms
      : reconnectDelayMs(reconnect_backoff, slot_index);
  return now - delay;
}

// WiFi station reconnect backoff. The bridge drives its own STA reconnect loop
// separate from the per-slot MQTT reconnects, with a slightly longer first rung
// (15 s vs the slot ladder's 10 s). Extracted from handleWiFiConnection() so the
// ladder and its wrap-safe timing are exercised by host tests instead of a
// second inline copy of the backoff math.
static inline uint32_t wifiReconnectBackoffMs(uint8_t attempt) {
  static const uint32_t kBackoffMs[] = {
    15000UL, 30000UL, 60000UL, 120000UL, 300000UL
  };
  const uint8_t index = attempt < 5 ? attempt : 4;
  return kBackoffMs[index];
}

// A reconnect is due only once the link has been down for the current rung AND
// no attempt has been made within that rung (both measured wrap-safely). This
// mirrors the two-part guard the bridge applied inline.
static inline bool wifiReconnectDue(uint32_t now, uint32_t disconnected_since,
                                    uint32_t last_attempt, uint8_t attempt) {
  const uint32_t delay = wifiReconnectBackoffMs(attempt);
  return elapsedMs(now, disconnected_since) >= delay &&
         elapsedMs(now, last_attempt) >= delay;
}

// The attempt counter climbs to 5 and then saturates; the index clamp in
// wifiReconnectBackoffMs() holds it at the 300 s rung.
static inline uint8_t nextWifiBackoffAttempt(uint8_t attempt) {
  return attempt < 5 ? static_cast<uint8_t>(attempt + 1) : attempt;
}

// Current WiFi outage start (millis), or 0 while associated. handleWiFiConnection()
// applies this on each STA status observation. Reconnect attempts that fire
// while already down must pass connected=false and last_connected=false so the
// start is preserved — WiFi.disconnect() in the backoff ladder is not a new
// outage, and must not become the clock AlertReporter quotes as downtime.
static inline uint32_t wifiCurrentOutageStartMs(uint32_t now, bool connected,
                                                bool last_connected,
                                                uint32_t current_start,
                                                bool initialized) {
  if (!initialized) {
    return connected ? 0U : now;
  }
  if (connected) {
    return last_connected ? current_start : 0U;
  }
  return last_connected ? now : current_start;
}

// Each later slot expires up to five percent of the base lifetime earlier,
// capped at five minutes per slot. Runtime slot indexes are bounded by the
// persisted MQTT slot count; the final clamp also prevents underflow if this
// helper is used with unexpected input.
static inline uint32_t jwtLifetimeSecs(uint32_t base_lifetime, uint8_t slot_index) {
  uint32_t per_slot_stagger = base_lifetime / 20UL;
  if (per_slot_stagger > kMaxJwtStaggerSecs) {
    per_slot_stagger = kMaxJwtStaggerSecs;
  }
  uint64_t stagger = static_cast<uint64_t>(slot_index) * per_slot_stagger;
  if (stagger > base_lifetime) {
    stagger = base_lifetime;
  }
  return base_lifetime - static_cast<uint32_t>(stagger);
}

static inline uint32_t renewalBufferSecs(uint32_t lifetime_secs) {
  uint32_t buffer = lifetime_secs / 10UL;
  if (buffer < 60UL) buffer = 60UL;
  if (buffer > 300UL) buffer = 300UL;
  return buffer;
}

static inline bool tokenNeedsRenewal(bool time_synced, uint32_t current_time,
                                     uint32_t token_expires_at,
                                     uint32_t renewal_buffer_secs) {
  if (!time_synced) {
    return token_expires_at == 0;
  }
  if (token_expires_at < kMinimumValidEpoch) {
    return true;
  }
  if (current_time >= token_expires_at) {
    return true;
  }
  return current_time >= token_expires_at - renewal_buffer_secs;
}

// A reconnect may keep its credentials only when their validity is known to
// outlast the next handshake; uncertainty refreshes them before reconnecting.
// Clearing the renewal buffer is not enough: tokenNeedsRenewal() fires the moment
// remaining reaches it, so reuse must clear it by the handshake margin too, or the
// reused token is renewed — and bounced, on a broker enforcing exp — seconds later.
// Pass 0 for renewal_buffer_secs to gate on the flat margin alone.
static inline bool canReuseJwtForReconnect(bool time_synced, bool has_token,
                                           bool force_mint,
                                           uint32_t current_time,
                                           uint32_t token_expires_at,
                                           uint32_t renewal_buffer_secs) {
  // Saturate rather than wrap: a wrapped floor would silently weaken this gate.
  const uint32_t floor_secs =
      renewal_buffer_secs > UINT32_MAX - kJwtReconnectSafetyMarginSecs
          ? UINT32_MAX
          : renewal_buffer_secs + kJwtReconnectSafetyMarginSecs;
  return time_synced &&
         has_token &&
         !force_mint &&
         token_expires_at >= kMinimumValidEpoch &&
         current_time < token_expires_at &&
         (token_expires_at - current_time) > floor_secs;
}

static inline bool renewalAttemptAllowed(uint32_t now, uint32_t last_attempt) {
  return elapsedMs(now, last_attempt) >= kRenewalThrottleMs;
}

static inline bool jwtClockAvailable(bool ntp_synced, uint32_t current_time) {
  return ntp_synced || current_time >= kJwtClockThreshold;
}

// How a given MQTT slot fares at bridge setup on this hardware. MQTTBridge's
// setup loop iterates runtime slots in index order and connects the first
// `max_active` *enabled* slots, skipping the rest (each WSS/TLS link needs
// ~40 KB internal heap, so non-PSRAM caps at 2 concurrent, PSRAM at 5). Slots at
// or beyond the runtime array size (`slot_count`, e.g. 3 on non-PSRAM) are never
// iterated at all. Extracted so the CLI can tell the operator, at
// `set mqttN.preset` time, whether a slot will actually come up — and so the
// exact rule is host-tested rather than hand-reasoned (it is easy to conflate
// slot_count with max_active).
enum class SlotActivation : uint8_t {
  Connects,       // enabled and within the concurrent-connection budget
  Disabled,       // slot has no preset ("none") — not attempted
  BeyondArray,    // index >= slot_count: outside the runtime slot array here
  OverActiveCap,  // enabled, but lower-numbered slots already fill the budget
};

// `enabled` must have at least `slot_count` entries; `slot` is 0-based. Mirrors
// the bridge's first-come-by-index activation order exactly.
static inline SlotActivation classifySlotActivation(int slot, const bool* enabled,
                                                    int slot_count, int max_active) {
  if (slot < 0) return SlotActivation::Disabled;
  if (slot >= slot_count) return SlotActivation::BeyondArray;
  if (enabled == nullptr || !enabled[slot]) return SlotActivation::Disabled;
  int rank = 0;  // this slot's position among enabled slots, counting by index
  for (int i = 0; i <= slot; i++) {
    if (enabled[i]) rank++;
  }
  return (rank <= max_active) ? SlotActivation::Connects
                              : SlotActivation::OverActiveCap;
}

// What to do with a slot whose token a clock correction just proved stale. Four
// outcomes, because esp-mqtt accepts a reconnect request only from
// MQTT_STATE_WAIT_RECONNECT: asking a live client to reconnect is refused and
// leaves it running on the stale token, so a live session has to have its
// transport closed first — and that costs a handshake, which is only worth
// spending where the broker actually enforces exp.
enum class StaleTokenAction : uint8_t {
  Defer,      // no fresh credentials — leave the slot to the backoff ladder
  Reconnect,  // client is down: start it, or wake one that is waiting
  Bounce,     // live session the broker will reject: close the transport, then reconnect
  KeepAlive,  // live session the broker tolerates: stage credentials, keep the handshake
};

// A failed mint yields Defer even when the slot is down: reconnecting then would
// re-present the credentials the correction just invalidated. Minting fails for
// recoverable reasons (allocation pressure), and the ladder retries.
static inline StaleTokenAction classifyStaleToken(bool minted, bool connected,
                                                  bool broker_enforces_exp) {
  if (!minted) return StaleTokenAction::Defer;
  if (!connected) return StaleTokenAction::Reconnect;
  return broker_enforces_exp ? StaleTokenAction::Bounce : StaleTokenAction::KeepAlive;
}

// Which clock to fall back on when no NTP server answered.
enum class ClockSource : uint8_t {
  None,    // nothing plausible to work from — stay unsynced
  System,  // libc already holds a usable time
  Rtc,     // libc does not, but the RTC does
};

// System first: a clock SNTP set recently outranks an RTC that may have drifted.
// The RTC matters on a cold boot, where ESP32RTCClock::begin() seeds libc with a 2024
// placeholder on power-on while a detected chip already holds real time and
// AutoDiscoverRTCClock::begin() never copies one into the other. Never while
// validating a server: that asks whether a specific host answers, and no clock can
// answer it. Pass rtc_time 0 when the board has no clock to consult.
static inline ClockSource chooseFallbackClock(bool validating_server, uint32_t system_time,
                                              uint32_t rtc_time, uint32_t min_valid_epoch) {
  if (validating_server) return ClockSource::None;
  if (system_time >= min_valid_epoch) return ClockSource::System;
  if (rtc_time >= min_valid_epoch) return ClockSource::Rtc;
  return ClockSource::None;
}

} // namespace MQTTConnectionPolicy
