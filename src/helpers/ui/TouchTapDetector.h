#pragma once

#include <stdint.h>

// Debounced rising-edge detector for a polled touch panel.
//
// Pure logic: no Arduino, no I2C. The caller polls the panel and hands over a
// raw "finger down" reading; this decides when that counts as a new tap. All
// elapsed-time comparisons are unsigned subtractions, so millis() rollover is
// a non-event.

#ifndef TOUCH_TAP_DEBOUNCE_MS
#define TOUCH_TAP_DEBOUNCE_MS 40
#endif

// Ignores a second tap arriving this soon after an accepted one, so a bouncy
// panel or a slightly long press cannot toggle the display twice.
#ifndef TOUCH_TAP_MIN_GAP_MS
#define TOUCH_TAP_MIN_GAP_MS 400
#endif

class TouchTapDetector {
public:
  TouchTapDetector() { reset(0); }

  void reset(uint32_t now_ms = 0) {
    _raw = false;
    _stable = false;
    _changed_at = now_ms;
    _last_tap = now_ms;
    _tapped_before = false;
  }

  // Returns true exactly once per accepted finger-down.
  bool update(uint32_t now_ms, bool pressed) {
    if (pressed != _raw) {   // reading moved; restart the settling window
      _raw = pressed;
      _changed_at = now_ms;
      return false;
    }
    if (now_ms - _changed_at < TOUCH_TAP_DEBOUNCE_MS) return false;   // not settled
    if (_raw == _stable) return false;                               // nothing new

    _stable = _raw;
    if (!_stable) return false;   // this is the release, not a tap

    if (_tapped_before && (now_ms - _last_tap) < TOUCH_TAP_MIN_GAP_MS) return false;
    _last_tap = now_ms;
    _tapped_before = true;
    return true;
  }

  bool isTouched() const { return _stable; }

private:
  uint32_t _changed_at;
  uint32_t _last_tap;
  bool _raw;
  bool _stable;
  bool _tapped_before;
};
