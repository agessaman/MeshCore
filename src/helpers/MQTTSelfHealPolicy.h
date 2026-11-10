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

struct Config {
  uint32_t stop_unproven_ms;   // stop never acknowledged by the bridge task
  uint32_t quarantined_ms;     // a client is retained and cannot be reused
  uint32_t all_slots_down_ms;  // network up, bridge running, no slot connected
  uint32_t min_uptime_ms;      // never self-heal a freshly booted node
  uint32_t alert_grace_ms;     // delay between the alert and the reboot
  uint32_t healthy_reset_ms;   // connected this long clears the boot counter
  uint8_t max_backoff_shift;   // hold time doubles per consecutive reboot, up to this
  uint8_t max_consecutive;     // stop rebooting after this many without recovery
};

// Proposed defaults. A late stop ack and the breaker probe both resolve well
// inside these windows, so only states that are truly stuck reach them.
static inline Config defaultConfig() {
  Config c{};
  c.stop_unproven_ms = 10UL * 60UL * 1000UL;
  c.quarantined_ms = 30UL * 60UL * 1000UL;
  c.all_slots_down_ms = 6UL * 60UL * 60UL * 1000UL;
  c.min_uptime_ms = 30UL * 60UL * 1000UL;
  c.alert_grace_ms = 5UL * 60UL * 1000UL;
  c.healthy_reset_ms = 10UL * 60UL * 1000UL;
  c.max_backoff_shift = 3;
  c.max_consecutive = 5;
  return c;
}

struct Inputs {
  bool enabled;             // operator pref
  uint32_t now_ms;          // millis()
  bool bridge_running;      // false when the operator stopped the bridge
  bool stop_unproven;
  bool client_quarantined;
  bool network_up;
  uint8_t enabled_slots;
  uint8_t connected_slots;
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

static inline bool causePresent(const Inputs& in, Cause c) {
  switch (c) {
    case Cause::StopUnproven:
      return in.stop_unproven;
    case Cause::ClientQuarantined:
      return in.client_quarantined;
    case Cause::AllSlotsDown:
      return in.bridge_running && in.network_up && in.enabled_slots > 0 &&
             in.connected_slots == 0;
    default:
      return false;
  }
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
    bool present = causePresent(in, (Cause)(i + 1));
    if (present && !st.active[i]) {
      st.active[i] = true;
      st.alerted[i] = false;
      st.since_ms[i] = in.now_ms;
    } else if (!present && st.active[i]) {
      st.active[i] = false;
      st.alerted[i] = false;
    }
  }

  bool healthy = in.bridge_running && in.connected_slots > 0 && !in.stop_unproven &&
                 !in.client_quarantined;
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

  if (!in.enabled || !st.uptime_ok) return r;

  bool suppressed = consecutive_reboots >= cfg.max_consecutive;

  // A pending reboot outranks a new alert so the grace timer cannot be starved.
  for (uint8_t i = 0; i < kCauseCount && !suppressed; i++) {
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
      r.reboot_suppressed = suppressed;
      return r;
    }
  }
  return r;
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

// "MQTT self-heal: <cause> for <N>m, rebooting in <M>m" or "... reboot limit reached".
static inline int formatAlert(const Result& r, const Config& cfg, char* buf, size_t len) {
  if (!buf || len == 0) return 0;
  unsigned held_min = (unsigned)(r.held_ms / 60000UL);
  if (r.reboot_suppressed) {
    return snprintf(buf, len, "MQTT self-heal: %s for %um, reboot limit reached",
                    causeLabel(r.cause), held_min);
  }
  return snprintf(buf, len, "MQTT self-heal: %s for %um, rebooting in %um",
                  causeLabel(r.cause), held_min, (unsigned)(cfg.alert_grace_ms / 60000UL));
}

}  // namespace MQTTSelfHealPolicy
