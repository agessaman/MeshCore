#pragma once

#include <stdint.h>

// Owned by the network worker. Completion, rather than a pre-probe loop sample,
// arms the next deadline so slow or failed probes cannot trigger a refresh storm.
class NtpSchedule {
public:
  enum class Result : uint8_t { NetworkTime, Holdover, Failed };

  bool due(uint32_t now) {
    if (!_active && (!_scheduled || (int32_t)(now - _next_attempt_ms) >= 0)) {
      _ready = true;
    }
    return !_active && _ready;
  }

  bool begin(uint32_t now, bool force = false) {
    if (_active || (!force && !due(now))) return false;
    _active = true;
    _ready = false;
    _last_attempt_ms = now;
    return true;
  }

  void complete(uint32_t now, Result result) {
    if (!_active) return;
    _active = false;
    _scheduled = true;
    _result = result;
    uint32_t interval;
    if (result == Result::NetworkTime) {
      _has_network_time = true;
      _last_success_ms = now;
      _failures = 0;
      interval = 3600000;
    } else {
      // A usable RTC keeps MQTT available, but is not a successful NTP probe.
      if (_failures < 5) ++_failures;
      interval = result == Result::Holdover ? 300000 : retryDelay(_failures);
    }
    _next_attempt_ms = now + interval;
  }

  bool active() const { return _active; }
  bool hasNetworkTime() const { return _has_network_time; }
  uint32_t lastAttemptMs() const { return _last_attempt_ms; }
  uint32_t lastSuccessMs() const { return _last_success_ms; }
  Result result() const { return _result; }

private:
  static uint32_t retryDelay(uint8_t failures) {
    const uint32_t delays[] = {30000, 60000, 120000, 240000, 300000};
    return delays[failures ? failures - 1 : 0];
  }

  bool _active = false;
  bool _ready = true;
  bool _scheduled = false;
  bool _has_network_time = false;
  uint8_t _failures = 0;
  uint32_t _last_attempt_ms = 0;
  uint32_t _last_success_ms = 0;
  uint32_t _next_attempt_ms = 0;
  Result _result = Result::Failed;
};
