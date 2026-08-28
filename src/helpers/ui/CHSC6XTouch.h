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
#define CHSC6X_MAX_POINTS 1

class CHSC6XTouch {
public:
  // Probes the bus. Returns false (and disables itself) when nothing answers,
  // so a board without the touch panel simply carries on without it.
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
    if (!_present) return false;
    return _detector.update(now_ms, readPressed());
  }

private:
  TwoWire* _wire = NULL;
  bool _present = false;
  TouchTapDetector _detector;

  bool readPressed() {
    uint8_t got = _wire->requestFrom((uint8_t)CHSC6X_I2C_ADDR, (uint8_t)CHSC6X_READ_LEN);
    if (got != CHSC6X_READ_LEN) {
      while (_wire->available()) _wire->read();   // drain a short read
      logRaw(got, NULL);
      return false;
    }

    uint8_t buf[CHSC6X_READ_LEN];
    for (uint8_t i = 0; i < CHSC6X_READ_LEN; i++) buf[i] = (uint8_t)_wire->read();
    logRaw(got, buf);

    // buf[0] is the reported touch-point count (buf[2]/buf[4] are x/y). It must
    // be tested against a *valid* count, not merely against zero: an idle or
    // NACKed read can come back as 0xFF, which "non-zero" reads as a finger
    // held down forever - the tap detector then fires once and, seeing no
    // release, never fires again.
    return buf[0] >= 1 && buf[0] <= CHSC6X_MAX_POINTS;
  }

#ifdef DISPLAY_TOUCH_DEBUG
  int16_t _logged = -1;

  // Logs on change only, so a normal boot stays quiet.
  void logRaw(uint8_t got, const uint8_t* buf) {
    int16_t key = buf ? (int16_t)buf[0] : (int16_t)(-2 - (int16_t)got);
    if (key == _logged) return;
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
