#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "TouchTapDetector.h"

// Minimal polled driver for the CHSC6X capacitive touch controller on the
// Heltec V4 R8 Expansion Kit V2 panel.
//
// Only "is a finger down" is needed to toggle the display, so no coordinates
// and no calibration are read. TP_INT is not used: on this board it is an
// optional link (R13) on GPIO 43, which is also U0TXD - see HeltecV4R8Board.cpp
// for the verified pin map.

#ifndef CHSC6X_I2C_ADDR
#define CHSC6X_I2C_ADDR 0x2E
#endif

#define CHSC6X_READ_LEN 5

class CHSC6XTouch {
public:
  // Probes the bus for diagnostics only. The result must NOT gate polling: this
  // controller NACKs its address whenever it has nothing to report, so a single
  // idle probe at boot is indistinguishable from absent hardware. checkTap()
  // keeps polling either way, and a NACK there costs one quiet, fast bus cycle.
  bool begin(TwoWire& wire = Wire) {
    _wire = &wire;
    _wire->beginTransmission((uint8_t)CHSC6X_I2C_ADDR);
    _present = (_wire->endTransmission() == 0);
    _detector.reset(millis());

  #if defined(DISPLAY_TOUCH_DEBUG) && defined(PIN_TOUCH_INT)
    pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
  #endif

    if (_present) {
      Serial.printf("Touch: CHSC6X found at 0x%02X\n", CHSC6X_I2C_ADDR);
    } else {
      // Report what is actually on the bus, so an unexpected controller or
      // address can be identified from a normal boot log.
      Serial.printf("Touch: nothing at 0x%02X; I2C bus holds:", CHSC6X_I2C_ADDR);
      for (uint8_t addr = 8; addr < 0x78; addr++) {
        _wire->beginTransmission(addr);
        if (_wire->endTransmission() == 0) Serial.printf(" 0x%02X", addr);
      }
      Serial.println();
    }
    return _present;
  }

  bool isPresent() const { return _present; }

  // True exactly once per new touch.
  bool checkTap(uint32_t now_ms) {
    bool pressed = readPressed();
    if (pressed && !_present) {
      _present = true;   // answered late; the boot probe caught it mid-idle
      Serial.println("Touch: CHSC6X responding");
    }
    return _detector.update(now_ms, pressed);
  }

private:
  TwoWire* _wire = NULL;
  bool _present = false;
  TouchTapDetector _detector;

  bool readPressed() {
    // Probe the address first. This controller NACKs while it has nothing to
    // report, and going straight to requestFrom() turns that into a logged bus
    // error every poll plus, once the bus wedges, a ~1 s timeout stall in the
    // UI loop. endTransmission() reports the same NACK quietly and cheaply.
    _wire->beginTransmission((uint8_t)CHSC6X_I2C_ADDR);
    if (_wire->endTransmission() != 0) return false;

    uint8_t got = _wire->requestFrom((uint8_t)CHSC6X_I2C_ADDR, (uint8_t)CHSC6X_READ_LEN);
    uint8_t buf[CHSC6X_READ_LEN];
    for (uint8_t i = 0; i < CHSC6X_READ_LEN; i++) {
      buf[i] = i < got ? (uint8_t)_wire->read() : 0xFF;
    }
    while (_wire->available()) _wire->read();   // drain a short read

    if (got != CHSC6X_READ_LEN) {
      logRaw(got, NULL);
      return false;
    }
    logRaw(got, buf);

    // Measured on the Expansion Kit V2 panel: byte 0 reads 0x00 while idle and
    // 0x1F while a finger is down - not the 0x01 point count the reference
    // CHSC6X drivers document, so testing for a count of 1 never fires. A
    // partly-failed read leaves 0xFF, which must not register as a press.
    return buf[0] != 0x00 && buf[0] != 0xFF;
  }

#ifdef DISPLAY_TOUCH_DEBUG
  int16_t _logged = -1;
  uint8_t _log_budget = 5;   // always show the first few frames, then on change

  // Mostly logs on change, so a normal boot stays quiet - but the opening
  // frames are unconditional so an idle read that never changes is still
  // visible in the log.
  void logRaw(uint8_t got, const uint8_t* buf) {
    int16_t key = buf ? (int16_t)buf[0] : (int16_t)(-2 - (int16_t)got);
    if (key == _logged && _log_budget == 0) return;
    if (_log_budget > 0) _log_budget--;
    _logged = key;

    if (!buf) {
      Serial.printf("Touch: short read (%u of %u bytes)\n", got, CHSC6X_READ_LEN);
      return;
    }
    Serial.printf("Touch: raw %02X %02X %02X %02X %02X", buf[0], buf[1], buf[2], buf[3], buf[4]);
  #ifdef PIN_TOUCH_INT
    // Pulled up, so an unfitted R13 sits steady HIGH and a wired INT pulses LOW.
    Serial.printf("  INT=%d", digitalRead(PIN_TOUCH_INT));
  #endif
    Serial.println();
  }
#else
  void logRaw(uint8_t, const uint8_t*) {}
#endif
};
