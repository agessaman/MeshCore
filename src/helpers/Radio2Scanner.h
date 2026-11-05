#pragma once

// Decides when an observer's radio visits radio2 and when it comes back. The
// caller performs each retune and reports how it went, so this is host-testable
// without a radio. See Radio2Profile.h for what radio2 is.

#include "Radio2Profile.h"

class Radio2Scanner {
public:
  enum RetuneResult : int8_t { RETUNE_OK = 0, RETUNE_BUSY = 1, RETUNE_FAILED = -1 };
  // Consecutive failed retunes before radio2 stops for this configuration.
  static constexpr uint8_t MaxConsecutiveFailures = 3;

  // Call every loop with the current settings. A change restarts the schedule
  // and clears a fault.
  void configure(bool on, const Radio2Params& primary, const Radio2Params& second) {
    if (on == _on && primary == _primary && second == _second) return;
    _on = on;
    _primary = primary;
    _second = second;
    _fault = false;
    _consecutive_failures = 0;
    _schedule_valid = false;
  }

  bool configured() const { return _on; }
  bool enabled() const { return _on && !_fault; }
  bool faulted() const { return _fault; }
  // 0 = the node's own radio settings, 1 = radio2.
  uint8_t current() const { return _current; }
  const Radio2Params& params(uint8_t profile) const { return profile ? _second : _primary; }

  // The profile to retune to now, or -1 to stay. can_leave_primary=false keeps
  // the radio on the node's own settings (a send is due, or another feature
  // has the radio).
  int step(uint32_t now_us, bool can_leave_primary) {
    if (_restore_primary || !enabled()) return _current == 1 || _restore_primary ? 0 : -1;
    if (_current == 0 && !can_leave_primary) return -1;
    if ((uint32_t)(now_us - _entered_us) < schedule().listen_us[_current]) return -1;
    return _current ^ 1;
  }

  void onRetune(uint8_t target, RetuneResult result, uint32_t now_us, uint32_t elapsed_us) {
    if (result == RETUNE_BUSY) {   // a packet is arriving or waiting; try again next loop
      _busy++;
      return;
    }
    if (result == RETUNE_FAILED) {
      _failures++;
      if (++_consecutive_failures >= MaxConsecutiveFailures) _fault = true;
      _current = 1;              // state unknown: keep trying to get back
      _restore_primary = true;
      return;
    }
    _current = target;
    _entered_us = now_us;
    _consecutive_failures = 0;
    if (target == 0) _restore_primary = false;
    _switches++;
    if (elapsed_us > _switch_max_us) {
      _switch_max_us = elapsed_us;
      _schedule_valid = false;
    }
  }

  // The radio was put back on the node's own settings by something else.
  void notePrimary(uint32_t now_us) {
    _current = 0;
    _entered_us = now_us;
    _restore_primary = false;
  }

  void noteHeard() { _heard++; }

  uint32_t switchBudgetUs() const {
    return _switch_max_us ? (uint32_t)(((uint64_t)_switch_max_us * 11 + 9) / 10)
                          : radio2::DefaultSwitchUs;
  }

  const radio2::Schedule& schedule() {
    if (!_schedule_valid) {
      _schedule = radio2::schedule(_primary, _second, switchBudgetUs());
      _schedule_valid = true;
    }
    return _schedule;
  }

  uint32_t heard() const { return _heard; }
  uint32_t switches() const { return _switches; }
  uint32_t busy() const { return _busy; }
  uint32_t failures() const { return _failures; }
  uint32_t switchMaxUs() const { return _switch_max_us; }

private:
  bool _on = false;
  bool _fault = false;
  bool _restore_primary = false;
  Radio2Params _primary, _second;
  uint8_t _current = 0;
  uint32_t _entered_us = 0;
  uint8_t _consecutive_failures = 0;
  uint32_t _switch_max_us = 0;
  radio2::Schedule _schedule;
  bool _schedule_valid = false;
  uint32_t _heard = 0, _switches = 0, _busy = 0, _failures = 0;
};
