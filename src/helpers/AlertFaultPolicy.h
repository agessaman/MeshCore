#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Pure WiFi/MQTT fault-alert policy used by AlertReporter. Keeping the edge
// detector, duration math, rate-limit floor, and message formatting here lets
// host tests drive a fake clock without Mesh, MQTTBridge, or a radio.
//
// WiFi outage state is an OutageSnapshot: down, started_ms, and the initiating
// disconnect reason. That is the only data AlertReporter passes into tick()
// and formatWifiAlert(). Last ESP-IDF DISCONNECTED event time/reason are not
// inputs — using them made "WiFi down" quote uptime and reason 8 (ASSOC_LEAVE
// from STA-backoff WiFi.disconnect()).
namespace AlertFaultPolicy {

static const uint32_t kCheckIntervalMs = 5000UL;
static const uint16_t kMinIntervalMinutes = 60;
static const uint32_t kMsPerMinute = 60000UL;

enum class State : uint8_t { OK, FIRING };

struct Fault {
  State state;
  uint32_t fired_at_ms;             // millis() of last successful "down" send; 0 = never
  uint32_t last_outage_started_ms;  // remembered so recovered can quote duration
};

// Current-outage view consumed by tick() and formatWifiAlert().
// `down` is the authority: started_ms may be 0 when the STA dropped at
// millis()==0. While !down, packOutageSnapshot canonicalizes start/reason to 0.
struct OutageSnapshot {
  bool down;
  uint32_t started_ms;
  uint8_t reason;  // initiating reason; 0 = omit from the down message
};

enum class Action : uint8_t { None, FireDown, FireRecovered };

struct TickResult {
  Action action;
  uint32_t duration_ms;
};

static inline OutageSnapshot fromStartMs(uint32_t started_ms) {
  OutageSnapshot snap{};
  snap.down = started_ms != 0;
  snap.started_ms = started_ms;
  return snap;
}

// Single published word for the Core 0 writers (WiFi event / MQTT task) and
// the Core 1 reader (AlertReporter). 32-bit started_ms + 8-bit reason + down
// fit in 64 bits; atomic load/store of this word is the cross-task boundary.
static const uint64_t kOutageDownBit = 1ULL << 40;

static inline uint64_t packOutageSnapshot(OutageSnapshot s) {
  if (!s.down) {
    s.started_ms = 0;
    s.reason = 0;
  }
  uint64_t v = (uint64_t)s.started_ms;
  v |= (uint64_t)s.reason << 32;
  if (s.down) v |= kOutageDownBit;
  return v;
}

static inline OutageSnapshot unpackOutageSnapshot(uint64_t v) {
  OutageSnapshot s{};
  s.started_ms = (uint32_t)v;
  s.reason = (uint8_t)(v >> 32);
  s.down = (v & kOutageDownBit) != 0;
  if (!s.down) {
    s.started_ms = 0;
    s.reason = 0;
  }
  return s;
}

// Status poll (handleWiFiConnection). `down` is independent of started_ms so a
// drop at millis()==0 is still down. Uses snapshot.down — not a separate
// last_connected flag — so a DISCONNECTED event that already opened the
// outage is not treated as a fresh connected→down edge on the next poll.
static inline OutageSnapshot applyWifiStatus(uint32_t now, bool connected,
                                             OutageSnapshot cur,
                                             bool initialized) {
  if (connected) {
    if (!initialized || cur.down) {
      return OutageSnapshot{};
    }
    return cur;
  }
  if (!initialized || !cur.down) {
    OutageSnapshot snap{};
    snap.down = true;
    snap.started_ms = now;  // 0 is a legal start
    snap.reason = cur.reason;
    return snap;
  }
  return cur;
}

// STA_GOT_IP (and any observe-connected between 10 s polls). A disconnect +
// reconnect that never shares a status poll with the drop must still close
// the outage, or the next DISCONNECTED event keeps the first start/reason.
static inline OutageSnapshot applyWifiGotIp(OutageSnapshot /*cur*/) {
  return OutageSnapshot{};
}

// ESP-IDF STA_DISCONNECTED event. Opens the outage if needed. While already
// down, the start is frozen and the first non-zero reason is kept — later
// reason 8 (ASSOC_LEAVE from WiFi.disconnect()) cannot replace it.
static inline OutageSnapshot applyWifiDisconnectEvent(uint32_t now, uint8_t reason,
                                                      OutageSnapshot cur) {
  if (!cur.down) {
    OutageSnapshot snap{};
    snap.down = true;
    snap.started_ms = now;
    snap.reason = reason;
    return snap;
  }
  if (cur.reason == 0 && reason != 0) {
    cur.reason = reason;
  }
  return cur;
}

// Unsigned subtraction is the standard millis() idiom and remains correct
// across one 32-bit counter rollover.
static inline uint32_t elapsedMs(uint32_t now, uint32_t then) {
  return now - then;
}

// Construction leaves next_check_ms at 0, so the first poll is always due.
// The signed delta matches AlertReporter's original (long)(now - next) < 0
// skip, which is wrap-safe for intervals well under 2^31 ms.
static inline bool checkDue(uint32_t now, uint32_t next_check_ms) {
  return (int32_t)(now - next_check_ms) >= 0;
}

static inline uint32_t nextCheckMs(uint32_t now) {
  return now + kCheckIntervalMs;
}

// Stale prefs or a future field tweak cannot drag the floor below 1 hour.
static inline uint32_t minIntervalMs(uint16_t cfg_minutes) {
  uint16_t minutes = cfg_minutes < kMinIntervalMinutes ? kMinIntervalMinutes
                                                       : cfg_minutes;
  return (uint32_t)minutes * kMsPerMinute;
}

static inline uint32_t thresholdMs(uint16_t minutes) {
  return (uint32_t)minutes * kMsPerMinute;
}

static inline uint32_t downDurationMs(uint32_t now, const OutageSnapshot& snap) {
  return snap.down ? elapsedMs(now, snap.started_ms) : 0;
}

static inline bool rateLimitAllows(uint32_t now, uint32_t fired_at_ms,
                                   uint32_t min_interval_ms) {
  // fired_at_ms == 0 means never fired since boot/config change. Treating it
  // as a send at millis()==0 would suppress the first alert until uptime
  // reached min_interval.
  return fired_at_ms == 0 || elapsedMs(now, fired_at_ms) >= min_interval_ms;
}

// Decide whether to emit a down/recovered message. Does not mutate `f`:
// FireDown is committed only after a successful send; FireRecovered is
// committed after the send attempt (success or not), matching production.
static inline TickResult tick(const Fault& f, uint32_t now,
                              const OutageSnapshot& snap, uint32_t thresh_ms,
                              uint32_t min_interval_ms) {
  TickResult result = {Action::None, 0};
  if (f.state == State::OK) {
    const uint32_t down_ms = downDurationMs(now, snap);
    if (snap.down && down_ms >= thresh_ms &&
        rateLimitAllows(now, f.fired_at_ms, min_interval_ms)) {
      result.action = Action::FireDown;
      result.duration_ms = down_ms;
    }
  } else if (!snap.down) {
    result.action = Action::FireRecovered;
    // FIRING always went through commitDown, so last_outage_started_ms is a
    // real start — including 0 when the outage began at millis()==0.
    result.duration_ms = elapsedMs(now, f.last_outage_started_ms);
  }
  return result;
}

static inline void commitDown(Fault& f, uint32_t now, uint32_t outage_start_ms) {
  f.state = State::FIRING;
  f.fired_at_ms = now;
  f.last_outage_started_ms = outage_start_ms;
}

static inline void commitRecovered(Fault& f) {
  f.state = State::OK;
}

static inline void reset(Fault& f) {
  f.state = State::OK;
  f.fired_at_ms = 0;
}

static inline void rearmIfDisabled(Fault& f) {
  if (f.state == State::FIRING) f.state = State::OK;
}

static inline void formatAge(uint32_t age_ms, char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  uint32_t secs = age_ms / 1000U;
  uint32_t h = secs / 3600U;
  uint32_t m = (secs % 3600U) / 60U;
  if (h > 0) {
    snprintf(out, out_size, "%uh%um", (unsigned)h, (unsigned)m);
  } else {
    snprintf(out, out_size, "%um", (unsigned)m);
  }
}

static inline void formatWifiDown(char* out, size_t out_size, uint32_t duration_ms,
                                  uint8_t reason) {
  if (!out || out_size == 0) return;
  char age[16];
  formatAge(duration_ms, age, sizeof(age));
  if (reason != 0) {
    snprintf(out, out_size, "WiFi down %s (reason %u)", age, (unsigned)reason);
  } else {
    snprintf(out, out_size, "WiFi down %s", age);
  }
}

static inline void formatWifiRecovered(char* out, size_t out_size,
                                       uint32_t duration_ms) {
  if (!out || out_size == 0) return;
  char age[16];
  formatAge(duration_ms, age, sizeof(age));
  snprintf(out, out_size, "WiFi recovered after %s", age);
}

// Production formatting entry: the same (TickResult, OutageSnapshot) pair
// AlertReporter feeds after tick(). Returns false when there is no message.
static inline bool formatWifiAlert(char* out, size_t out_size, const TickResult& r,
                                   const OutageSnapshot& snap) {
  if (r.action == Action::FireDown) {
    formatWifiDown(out, out_size, r.duration_ms, snap.reason);
    return true;
  }
  if (r.action == Action::FireRecovered) {
    formatWifiRecovered(out, out_size, r.duration_ms);
    return true;
  }
  return false;
}

static inline void formatMqttDown(char* out, size_t out_size, int slot_1based,
                                  const char* preset_name, uint32_t duration_ms) {
  if (!out || out_size == 0) return;
  char age[16];
  formatAge(duration_ms, age, sizeof(age));
  snprintf(out, out_size, "MQTT slot %d (%s) down %s", slot_1based,
           preset_name ? preset_name : "?", age);
}

static inline void formatMqttRecovered(char* out, size_t out_size, int slot_1based,
                                       const char* preset_name,
                                       uint32_t duration_ms) {
  if (!out || out_size == 0) return;
  char age[16];
  formatAge(duration_ms, age, sizeof(age));
  snprintf(out, out_size, "MQTT slot %d (%s) recovered after %s", slot_1based,
           preset_name ? preset_name : "?", age);
}

}  // namespace AlertFaultPolicy
