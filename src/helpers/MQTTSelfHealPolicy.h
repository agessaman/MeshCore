#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Pure policy for a bounded self-heal reboot when MQTT is in a state that only
// a reboot recovers. The caller samples bridge state, calls tick() on a fixed
// cadence, and performs the returned action; nothing here touches hardware, so
// host tests drive it with a fake clock.
namespace MQTTSelfHealPolicy {

enum class Cause : uint8_t { None, StopUnproven, ClientQuarantined, AllSlotsDown };
static const uint8_t kCauseCount = 3;

enum class Action : uint8_t {
  None,
  Alert,             // condition held long enough; announce before rebooting
  Reboot,            // grace after the alert has elapsed
  ClearBootCounter,  // MQTT has been healthy; forget earlier self-heal reboots
};

// Build-time overrides so a bench build can shorten the windows without patching code.
#ifndef MQTT_SELFHEAL_STOP_UNPROVEN_MS
#define MQTT_SELFHEAL_STOP_UNPROVEN_MS (10UL * 60UL * 1000UL)
#endif
#ifndef MQTT_SELFHEAL_QUARANTINED_MS
#define MQTT_SELFHEAL_QUARANTINED_MS (30UL * 60UL * 1000UL)
#endif
#ifndef MQTT_SELFHEAL_ALL_DOWN_MS
#define MQTT_SELFHEAL_ALL_DOWN_MS (2UL * 60UL * 60UL * 1000UL)
#endif
#ifndef MQTT_SELFHEAL_MIN_UPTIME_MS
#define MQTT_SELFHEAL_MIN_UPTIME_MS (30UL * 60UL * 1000UL)
#endif
#ifndef MQTT_SELFHEAL_GRACE_MS
#define MQTT_SELFHEAL_GRACE_MS (60UL * 1000UL)
#endif
#ifndef MQTT_SELFHEAL_HEALTHY_MS
#define MQTT_SELFHEAL_HEALTHY_MS (10UL * 60UL * 1000UL)
#endif

struct Config {
  uint32_t stop_unproven_ms;   // stop never acknowledged by the bridge task
  uint32_t quarantined_ms;     // a client is retained and cannot be reused
  uint32_t all_slots_down_ms;  // internet reachable, bridge running, no slot connected
  uint32_t min_uptime_ms;      // never self-heal a freshly booted node
  uint32_t alert_grace_ms;     // delay between the alert and the reboot
  uint32_t healthy_reset_ms;   // connected this long clears the boot counter
  uint32_t evidence_max_age_ms;  // oldest NTP reply that still proves the internet is reachable
  uint8_t max_backoff_shift;   // hold time doubles per consecutive reboot, up to this
  uint8_t max_consecutive;     // stop rebooting after this many without recovery
  uint8_t all_down_max_reboots;  // all-slots-down reboot allowance since last healthy
};

// A late stop ack and the breaker probe both resolve well inside these windows,
// so only states that are truly stuck reach them.
static inline Config defaultConfig() {
  Config c{};
  c.stop_unproven_ms = MQTT_SELFHEAL_STOP_UNPROVEN_MS;
  c.quarantined_ms = MQTT_SELFHEAL_QUARANTINED_MS;
  c.all_slots_down_ms = MQTT_SELFHEAL_ALL_DOWN_MS;
  c.min_uptime_ms = MQTT_SELFHEAL_MIN_UPTIME_MS;
  c.alert_grace_ms = MQTT_SELFHEAL_GRACE_MS;
  c.healthy_reset_ms = MQTT_SELFHEAL_HEALTHY_MS;
  c.evidence_max_age_ms = 60UL * 60UL * 1000UL;
  c.max_backoff_shift = 3;
  c.max_consecutive = 5;
  c.all_down_max_reboots = 1;
  return c;
}

// Evidence age meaning "no successful NTP exchange yet".
static const uint32_t kNoEvidence = UINT32_MAX;

struct Inputs {
  bool enabled;             // operator pref
  uint32_t now_ms;          // millis()
  bool bridge_running;      // false when the operator stopped the bridge
  bool bridge_wanted;       // operator's bridge-enabled pref
  bool stop_unproven;
  bool client_quarantined;
  bool network_up;
  uint32_t evidence_age_ms;  // since the last successful NTP exchange, or kNoEvidence
  uint8_t candidate_slots;   // enabled slots whose last failure was not a broker refusal
  uint8_t connected_slots;
  bool ota_in_progress;      // a firmware update is running; never alert or reboot
};

struct State {
  bool active[kCauseCount];
  bool alerted[kCauseCount];
  uint32_t since_ms[kCauseCount];
  uint32_t alerted_ms[kCauseCount];
  bool uptime_ok;
  bool healthy;
  bool healthy_reported;
  uint32_t healthy_since_ms;
};

struct Result {
  Action action;
  Cause cause;
  uint32_t held_ms;
  bool reboot_suppressed;  // alert only: the consecutive-reboot limit is reached
};

static inline uint8_t causeIndex(Cause c) { return (uint8_t)((uint8_t)c - 1); }

static inline const char* causeLabel(Cause c) {
  switch (c) {
    case Cause::StopUnproven: return "stop unproven";
    case Cause::ClientQuarantined: return "client quarantined";
    case Cause::AllSlotsDown: return "all slots down";
    default: return "none";
  }
}

// Compact token for the status payload, in the style of reset_reason.
static inline const char* causeToken(Cause c) {
  switch (c) {
    case Cause::StopUnproven: return "stop_unproven";
    case Cause::ClientQuarantined: return "client_quarantined";
    case Cause::AllSlotsDown: return "all_slots_down";
    default: return "none";
  }
}

static inline uint32_t baseHoldMs(const Config& cfg, Cause c) {
  switch (c) {
    case Cause::StopUnproven: return cfg.stop_unproven_ms;
    case Cause::ClientQuarantined: return cfg.quarantined_ms;
    case Cause::AllSlotsDown: return cfg.all_slots_down_ms;
    default: return UINT32_MAX;
  }
}

// Required hold doubles for each consecutive self-heal reboot, saturating.
static inline uint32_t requiredHoldMs(const Config& cfg, Cause c, uint8_t consecutive_reboots) {
  uint8_t shift = consecutive_reboots < cfg.max_backoff_shift ? consecutive_reboots
                                                              : cfg.max_backoff_shift;
  uint64_t v = (uint64_t)baseHoldMs(cfg, c) << shift;
  return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

static inline bool causePresent(const Inputs& in, const Config& cfg, Cause c) {
  switch (c) {
    case Cause::StopUnproven:
      return in.stop_unproven;
    case Cause::ClientQuarantined:
      return in.client_quarantined;
    case Cause::AllSlotsDown:
      // Only when a reboot could plausibly help: the internet answers and some
      // slot failed for a reason other than the broker refusing its credentials.
      return in.bridge_running && in.network_up && in.evidence_age_ms != kNoEvidence &&
             in.evidence_age_ms <= cfg.evidence_max_age_ms && in.candidate_slots > 0 &&
             in.connected_slots == 0;
    default:
      return false;
  }
}

static inline bool rebootSuppressed(const Config& cfg, Cause c, uint8_t consecutive_reboots) {
  return consecutive_reboots >= cfg.max_consecutive ||
         (c == Cause::AllSlotsDown && consecutive_reboots >= cfg.all_down_max_reboots);
}

static inline Result tick(State& st, const Inputs& in, const Config& cfg,
                          uint8_t consecutive_reboots) {
  Result r{};
  r.action = Action::None;
  r.cause = Cause::None;

  if (!st.uptime_ok && in.now_ms >= cfg.min_uptime_ms) st.uptime_ok = true;

  // Track each cause's hold time even while disabled or young, so enabling the
  // pref or reaching minimum uptime acts on how long the fault has really held.
  for (uint8_t i = 0; i < kCauseCount; i++) {
    bool present = causePresent(in, cfg, (Cause)(i + 1));
    if (present && !st.active[i]) {
      st.active[i] = true;
      st.alerted[i] = false;
      st.since_ms[i] = in.now_ms;
    } else if (!present && st.active[i]) {
      st.active[i] = false;
      st.alerted[i] = false;
    }
  }

  // A bridge deliberately off and not stuck is healthy too, so the counter can clear.
  bool healthy = !in.stop_unproven && !in.client_quarantined &&
                 (in.bridge_wanted ? in.bridge_running && in.connected_slots > 0 : true);
  if (healthy && !st.healthy) {
    st.healthy = true;
    st.healthy_reported = false;
    st.healthy_since_ms = in.now_ms;
  } else if (!healthy) {
    st.healthy = false;
  }
  if (st.healthy && !st.healthy_reported &&
      (uint32_t)(in.now_ms - st.healthy_since_ms) >= cfg.healthy_reset_ms) {
    st.healthy_reported = true;
    if (consecutive_reboots > 0) {
      r.action = Action::ClearBootCounter;
      return r;
    }
  }

  // Disabled or mid-update: forget pending alerts so acting later needs a fresh alert and grace.
  if (!in.enabled || in.ota_in_progress) {
    for (uint8_t i = 0; i < kCauseCount; i++) st.alerted[i] = false;
    return r;
  }
  if (!st.uptime_ok) return r;

  // A pending reboot outranks a new alert so the grace timer cannot be starved.
  for (uint8_t i = 0; i < kCauseCount; i++) {
    Cause c = (Cause)(i + 1);
    if (rebootSuppressed(cfg, c, consecutive_reboots)) continue;
    if (!st.active[i] || !st.alerted[i]) continue;
    if ((uint32_t)(in.now_ms - st.alerted_ms[i]) >= cfg.alert_grace_ms) {
      r.action = Action::Reboot;
      r.cause = (Cause)(i + 1);
      r.held_ms = (uint32_t)(in.now_ms - st.since_ms[i]);
      return r;
    }
  }

  for (uint8_t i = 0; i < kCauseCount; i++) {
    if (!st.active[i] || st.alerted[i]) continue;
    Cause c = (Cause)(i + 1);
    uint32_t held = (uint32_t)(in.now_ms - st.since_ms[i]);
    if (held >= requiredHoldMs(cfg, c, consecutive_reboots)) {
      st.alerted[i] = true;
      st.alerted_ms[i] = in.now_ms;
      r.action = Action::Alert;
      r.cause = c;
      r.held_ms = held;
      r.reboot_suppressed = rebootSuppressed(cfg, c, consecutive_reboots);
      return r;
    }
  }
  return r;
}

// The bridge was started, stopped or restarted: the all-slots-down hold and the
// healthy timer describe the previous run. Stop-unproven and quarantine persist.
static inline void onBridgeRestart(State& st) {
  const uint8_t i = causeIndex(Cause::AllSlotsDown);
  st.active[i] = false;
  st.alerted[i] = false;
  st.healthy = false;
  st.healthy_reported = false;
}

// Boot-loop limiter record kept in RTC no-init memory. A power-on leaves
// garbage there, which fails validation and reads as zero reboots.
static const uint32_t kRtcMagic = 0x4D51534Cu;

struct RtcRecord {
  uint32_t magic;
  uint8_t consecutive;
  uint8_t last_cause;
  uint16_t check;
};

static inline uint16_t rtcCheck(uint8_t consecutive, uint8_t last_cause) {
  return (uint16_t)~(uint16_t)(((uint16_t)last_cause << 8) | consecutive);
}

static inline bool rtcValid(const RtcRecord& rec) {
  return rec.magic == kRtcMagic && rec.check == rtcCheck(rec.consecutive, rec.last_cause) &&
         rec.last_cause <= kCauseCount;
}

static inline uint8_t rtcConsecutive(const RtcRecord& rec) {
  return rtcValid(rec) ? rec.consecutive : 0;
}

static inline RtcRecord rtcMake(uint8_t consecutive, Cause cause) {
  RtcRecord rec{};
  rec.magic = kRtcMagic;
  rec.consecutive = consecutive;
  rec.last_cause = (uint8_t)cause;
  rec.check = rtcCheck(rec.consecutive, rec.last_cause);
  return rec;
}

// Record to store immediately before a self-heal reboot.
static inline RtcRecord rtcBeforeReboot(const RtcRecord& cur, Cause cause) {
  uint8_t n = rtcConsecutive(cur);
  if (n < UINT8_MAX) n++;
  return rtcMake(n, cause);
}

static inline RtcRecord rtcCleared() { return rtcMake(0, Cause::None); }

// Whole minutes, or seconds below one minute so a bench build's short grace reads sensibly.
static inline void formatDuration(uint32_t ms, char* buf, size_t len) {
  if (ms < 60000UL) snprintf(buf, len, "%us", (unsigned)(ms / 1000UL));
  else snprintf(buf, len, "%um", (unsigned)(ms / 60000UL));
}

// "MQTT self-heal: <cause> for <N>m, rebooting in <M>m" or "... reboot limit reached".
static inline int formatAlert(const Result& r, const Config& cfg, char* buf, size_t len) {
  if (!buf || len == 0) return 0;
  char held[12];
  formatDuration(r.held_ms, held, sizeof(held));
  if (r.reboot_suppressed) {
    return snprintf(buf, len, "MQTT self-heal: %s for %s, reboot limit reached",
                    causeLabel(r.cause), held);
  }
  char grace[12];
  formatDuration(cfg.alert_grace_ms, grace, sizeof(grace));
  return snprintf(buf, len, "MQTT self-heal: %s for %s, rebooting in %s",
                  causeLabel(r.cause), held, grace);
}

}  // namespace MQTTSelfHealPolicy
