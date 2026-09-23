#pragma once
#include <stdint.h>

// `ota update` was issued while the manifest check was still queued or running.
// The app loop polls the board until that check settles, then either arms the
// ordinary deferred flash or reports why it will not run. Bounded so a check
// that never completes cannot leave an update armed for the rest of the boot.
class OtaUpdateFollowUp {
 public:
  enum class Action : uint8_t { None, Start, Refuse, Timeout };
  enum class Check : uint8_t { Applicable, NotApplicable, Requeued };
  static constexpr uint32_t kTimeoutMs = 120000;

  bool armed() const { return _armed; }
  void arm(uint32_t now) {
    _armed = true;
    _deadline = now + kTimeoutMs;
  }
  void clear() { _armed = false; }

  // dry_run runs only once the board reports no check in progress. Requeued
  // means the cached result had already expired and the board started another
  // check; stay armed and wait for that one.
  template<class DryRun>
  Action poll(uint32_t now, bool check_in_progress, DryRun dry_run) {
    if (!_armed) return Action::None;
    if ((int32_t)(now - _deadline) >= 0) {
      _armed = false;
      return Action::Timeout;
    }
    if (check_in_progress) return Action::None;
    switch (dry_run()) {
      case Check::Applicable: _armed = false; return Action::Start;
      case Check::Requeued: return Action::None;
      default: _armed = false; return Action::Refuse;
    }
  }

 private:
  bool _armed = false;
  uint32_t _deadline = 0;
};
