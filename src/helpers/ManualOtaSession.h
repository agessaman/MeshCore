#pragma once

#include <stdint.h>

// Pure state for the manual `start ota` web-upload lifecycle. The firmware owns
// the side effects (mesh drain, MQTT stop/start, HTTP server); this class keeps
// the timing and prior bridge state consistent across repeater and room-server
// roles and makes rollover/idempotence behavior host-testable.
class ManualOtaSession {
public:
  enum class State : uint8_t { Idle = 0, Pending, Active };

  static constexpr uint32_t kStartDelayMs = 2500;
  static constexpr uint32_t kSessionTimeoutMs = 15UL * 60UL * 1000UL;

  bool schedule(uint32_t now, bool force_ap) {
    if (_state != State::Idle) return false;
    _state = State::Pending;
    _force_ap = force_ap;
    _deadline = now + kStartDelayMs;
    return true;
  }

  bool startDue(uint32_t now) const {
    return _state == State::Pending && (int32_t)(now - _deadline) >= 0;
  }

  void markActive(uint32_t now, bool bridge_was_running) {
    _state = State::Active;
    _bridge_was_running = bridge_was_running;
    _deadline = now + kSessionTimeoutMs;
  }

  bool timeoutDue(uint32_t now, bool upload_in_progress) const {
    return _state == State::Active && !upload_in_progress &&
           (int32_t)(now - _deadline) >= 0;
  }

  void reset() {
    _state = State::Idle;
    _force_ap = false;
    _bridge_was_running = false;
    _deadline = 0;
  }

  State state() const { return _state; }
  bool isIdle() const { return _state == State::Idle; }
  bool isPending() const { return _state == State::Pending; }
  bool isActive() const { return _state == State::Active; }
  bool forceAp() const { return _force_ap; }
  bool bridgeWasRunning() const { return _bridge_was_running; }

private:
  State _state = State::Idle;
  bool _force_ap = false;
  bool _bridge_was_running = false;
  uint32_t _deadline = 0;
};
