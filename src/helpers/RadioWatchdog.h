#pragma once

#include <stdint.h>

// Decides when the observer radio watchdog should kick a radio that is parked in
// RX but hearing nothing (see Dispatcher::loop()).
//
// Pure logic: no Arduino, radio or mesh headers. The caller feeds the raw 32-bit
// activity timestamps it can see (last receive, last radio interrupt, last
// successful transmit) plus its millis() counter.
//
// Why this is not a max() of those three timestamps: millis() wraps every ~49.7
// days, and a pre-wrap timestamp is numerically larger than every fresh
// post-wrap one. Picking the largest therefore latches onto an old transmit and
// reports the radio silent while it is receiving continuously, resetting a
// healthy radio once per watchdog interval until that timestamp stops winning.
//
// Instead the caller's timestamps are only ever compared with their own previous
// values: any change is fresh activity, stamped against a monotonic 64-bit clock
// extended from millis(). Nothing downstream has a rollover case, and evidence
// older than one 32-bit cycle stays old instead of aliasing back to "recent".
//
// How far a timestamp may run behind the previous one and still count as an
// out-of-order reading rather than a very long forward gap (same rule as
// RadioActivityWindow).
#define RADIO_WATCHDOG_BACKSTEP_TOLERANCE_MS 5000UL

struct RadioWatchdogDecision {
  bool     recover;      // trip now: idle + restart receive
  bool     measurable;   // false until some activity has been observed
  uint64_t silent_ms;    // age of the most recent activity (0 when !measurable)
};

class RadioWatchdog {
public:
  RadioWatchdog() { reset(0); }

  void reset(uint32_t now_ms) {
    _now_ms = now_ms;
    _last_input_ms = now_ms;
    _seeded = false;
    _has_activity = false;
    _last_recv = _last_irq = _last_tx = 0;
    _last_activity_ms = now_ms;
    _recovered = false;
    _last_recovery_ms = 0;
  }

  // Call once per loop with the current activity timestamps. `in_recv_mode` and
  // `watchdog_ms` (0 disables) gate the decision; the silence measurement is
  // updated either way so a disabled or transmitting radio does not accumulate
  // phantom silence.
  RadioWatchdogDecision update(uint32_t now_ms, bool in_recv_mode, uint32_t watchdog_ms,
                               uint32_t last_recv, uint32_t last_irq, uint32_t last_tx) {
    tick(now_ms);

    if (!_seeded) {
      // First observation: the timestamps carry activity from before this
      // tracker existed, so date it to now rather than trusting a raw value we
      // cannot place on our own clock. Worst case that delays the first
      // possible trip by one watchdog interval.
      _seeded = true;
      _has_activity = (last_recv != 0 || last_irq != 0 || last_tx != 0);
      _last_activity_ms = _now_ms;
    } else if (last_recv != _last_recv || last_irq != _last_irq || last_tx != _last_tx) {
      _has_activity = true;
      _last_activity_ms = _now_ms;
    }
    _last_recv = last_recv;
    _last_irq = last_irq;
    _last_tx = last_tx;

    RadioWatchdogDecision d;
    d.recover = false;
    d.measurable = _has_activity;
    d.silent_ms = _has_activity ? (_now_ms - _last_activity_ms) : 0;

    if (!_has_activity || !in_recv_mode || watchdog_ms == 0) return d;
    if (d.silent_ms <= watchdog_ms) return d;
    // One recovery per watchdog interval: a radio that stays silent through a
    // reset must not be reset every loop.
    if (_recovered && (_now_ms - _last_recovery_ms) <= watchdog_ms) return d;

    d.recover = true;
    return d;
  }

  // Call after acting on a decision with recover set.
  void noteRecovery() {
    _recovered = true;
    _last_recovery_ms = _now_ms;
  }

  uint64_t nowMs() const { return _now_ms; }

private:
  // Extends the caller's 32-bit millis() to a monotonic 64-bit clock.
  void tick(uint32_t now_ms) {
    uint32_t delta = now_ms - _last_input_ms;
    if (delta > 0x80000000u &&
        (uint32_t)(_last_input_ms - now_ms) <= RADIO_WATCHDOG_BACKSTEP_TOLERANCE_MS) {
      return;   // out-of-order reading: no time has passed
    }
    _now_ms += delta;
    _last_input_ms = now_ms;
  }

  uint64_t _now_ms;
  uint32_t _last_input_ms;
  bool     _seeded;
  bool     _has_activity;
  uint32_t _last_recv, _last_irq, _last_tx;
  uint64_t _last_activity_ms;
  bool     _recovered;
  uint64_t _last_recovery_ms;
};
