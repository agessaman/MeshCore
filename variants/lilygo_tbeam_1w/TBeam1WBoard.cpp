#include "TBeam1WBoard.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

static const int FAN_PWM_MAX = (1 << FAN_PWM_RES_BITS) - 1;

void TBeam1WBoard::begin() {
  ESP32Board::begin();

  // Power on radio module (must be done before radio init)
  pinMode(SX126X_POWER_EN, OUTPUT);
  digitalWrite(SX126X_POWER_EN, HIGH);
  radio_powered = true;
  delay(10);  // Allow radio to power up

  // RF switch RXEN pin handled by RadioLib via setRfSwitchPins()

  // Initialize LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // NTC ADC (PA-adjacent thermistor)
  pinMode(NTC_PIN, INPUT);
  analogSetPinAttenuation(NTC_PIN, ADC_11db);
  analogReadResolution(12);

  // Fan: auto/onoff. Thermal on at 36C / off below 30C; TX still forces a cooldown.
  pinMode(FAN_CTRL_PIN, OUTPUT);
  digitalWrite(FAN_CTRL_PIN, HIGH);
  _temp_c = readNtcTempC();
  applyDuty(100);
  startFanTask();
}

void TBeam1WBoard::startFanTask() {
  if (_fan_task) return;
  xTaskCreate(fanTaskThunk, "tbeam1w_fan", 4096, this, 1, &_fan_task);
}

void TBeam1WBoard::fanTaskThunk(void* arg) {
  auto* self = static_cast<TBeam1WBoard*>(arg);
  for (;;) {
    if (!self->_stopped) {
      self->updateFan();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void TBeam1WBoard::onBeforeTransmit() {
  digitalWrite(LED_PIN, HIGH);  // TX LED on
  _tx_active = true;
  if (_mode == FAN_AUTO && _manual_duty < 0 && !_stopped) {
    applyDuty(FAN_TX_FLOOR_PCT);
  }
}

void TBeam1WBoard::onAfterTransmit() {
  digitalWrite(LED_PIN, LOW);   // TX LED off
  _tx_until_ms = millis() + FAN_TX_COOLDOWN_MS;
  _tx_cooldown_active = true;
  _tx_active = false;
}

uint16_t TBeam1WBoard::getBattMilliVolts() {
  // T-Beam 1W uses 7.4V battery with voltage divider
  analogReadResolution(12);
  uint32_t raw = 0;
  for (int i = 0; i < 8; i++) {
    raw += analogRead(BATTERY_PIN);
  }
  raw = raw / 8;
  return static_cast<uint16_t>((raw * 3300 * ADC_MULTIPLIER) / 4095);
}

const char* TBeam1WBoard::getManufacturerName() const {
  return "LilyGo T-Beam 1W";
}

void TBeam1WBoard::powerOff() {
  _stopped = true;
  applyDuty(0);

  // Turn off radio LNA (CTRL pin must be LOW when not receiving)
  digitalWrite(SX126X_RXEN, LOW);

  // Turn off radio power
  digitalWrite(SX126X_POWER_EN, LOW);
  radio_powered = false;

  digitalWrite(LED_PIN, LOW);

  ESP32Board::powerOff();
}

void TBeam1WBoard::setFanEnabled(bool enabled) {
  applyDuty(enabled ? 100 : 0);
}

bool TBeam1WBoard::isFanEnabled() const {
  return _duty_pct > 0;
}

float TBeam1WBoard::readNtcTempC() {
  analogReadMilliVolts(NTC_PIN);  // settle
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) {
    uint32_t sample_mv = analogReadMilliVolts(NTC_PIN);
    // GPIO14 is ADC2 on ESP32-S3. Wi-Fi/ESP-NOW arbitration failures are
    // reported by Arduino as 0 mV; never average a failed sample into a
    // plausible-but-low temperature.
    if (sample_mv == 0 || sample_mv >= NTC_VCC_MV) return NAN;
    sum += sample_mv;
  }
  float mv = sum / 8.0f;

  // R_ntc = R_fixed * (Vcc - V) / V  for 3.3V-NTC-ADC-10k-GND
  float r_ntc = NTC_R_FIXED * (NTC_VCC_MV - mv) / mv;
  if (r_ntc <= 0.0f) return NAN;

  float temp_k = 1.0f / (1.0f / 298.15f + (1.0f / NTC_B) * logf(r_ntc / NTC_R25));
  return temp_k - 273.15f;
}

bool TBeam1WBoard::ntcImplausible(float temp_c) const {
  return isnan(temp_c) || temp_c < -20.0f || temp_c > 120.0f;
}

int TBeam1WBoard::rampDuty(float temp_c) const {
  if (temp_c < (float)_lo_c) return 0;
  if (temp_c >= (float)_hi_c) return 100;
  float span = (float)(_hi_c - _lo_c);
  if (span <= 0.0f) return 100;
  float t = (temp_c - (float)_lo_c) / span;
  return FAN_MIN_DUTY_PCT + (int)((100 - FAN_MIN_DUTY_PCT) * t + 0.5f);
}

bool TBeam1WBoard::isTxCooling(uint32_t now) {
  if (_tx_active) return true;
  if (!_tx_cooldown_active) return false;
  if ((int32_t)(now - _tx_until_ms) >= 0) {
    _tx_cooldown_active = false;
    return false;
  }
  return true;
}

int TBeam1WBoard::cooldownSecs() {
  if (_tx_active) return (FAN_TX_COOLDOWN_MS + 999) / 1000;
  if (!_tx_cooldown_active) return 0;
  int32_t remain_ms = (int32_t)(_tx_until_ms - millis());
  if (remain_ms <= 0) {
    _tx_cooldown_active = false;
    return 0;
  }
  return (remain_ms + 999) / 1000;
}

void TBeam1WBoard::applyDuty(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  _duty_pct = pct;

  if (_drive == FAN_DRIVE_PWM) {
    if (!_pwm_attached) {
      ledcSetup(FAN_PWM_CHANNEL, FAN_PWM_FREQ_HZ, FAN_PWM_RES_BITS);
      ledcAttachPin(FAN_CTRL_PIN, FAN_PWM_CHANNEL);
      _pwm_attached = true;
    }
    uint32_t ticks = ((uint32_t)pct * FAN_PWM_MAX + 50) / 100;
    ledcWrite(FAN_PWM_CHANNEL, ticks);
  } else {
    if (_pwm_attached) {
      ledcDetachPin(FAN_CTRL_PIN);
      _pwm_attached = false;
      pinMode(FAN_CTRL_PIN, OUTPUT);
    }
    digitalWrite(FAN_CTRL_PIN, pct > 0 ? HIGH : LOW);
  }
}

void TBeam1WBoard::updateFan() {
  float t = readNtcTempC();
  _temp_c = t;
  bool tx_cooling = isTxCooling(millis());

  int duty;
  if (_manual_duty >= 0) {
    duty = _manual_duty;
  } else if (_mode == FAN_ON) {
    duty = 100;
  } else if (_mode == FAN_OFF) {
    duty = 0;
  } else if (ntcImplausible(t)) {
    duty = 100;  // fail-safe: treat bad NTC as hot
  } else if (_drive == FAN_DRIVE_PWM) {
    duty = rampDuty(t);
  } else {
    // Thermal hysteresis only: on at hi, off below lo. TX cooldown is applied
    // after this and must not latch _thermal_on, or a TX at 30C keeps the fan
    // running until temp dips under lo.
    if (t >= (float)_hi_c) _thermal_on = true;
    else if (t < (float)_lo_c) _thermal_on = false;
    duty = _thermal_on ? 100 : 0;
  }

  if (_mode == FAN_AUTO && _manual_duty < 0 && !ntcImplausible(t)) {
    if (tx_cooling && duty < FAN_TX_FLOOR_PCT) duty = FAN_TX_FLOOR_PCT;
  }

  applyDuty(duty);
}

bool TBeam1WBoard::persistKey(const char* key, const char* value) {
  return _prefs && _prefs->setByKey(key, value);
}

bool TBeam1WBoard::parseIntArg(const char* text, int& value) {
  if (!text || !*text) return false;
  errno = 0;
  char* end = nullptr;
  long parsed = strtol(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX) {
    return false;
  }
  value = (int)parsed;
  return true;
}

void TBeam1WBoard::loadFanPrefs() {
  if (!_prefs) return;

  char buf[12];
  buf[0] = 0;
  if (_prefs->getByKey("fan", buf, 11)) {
    if (strcmp(buf, "auto") == 0) _mode = FAN_AUTO;
    else if (strcmp(buf, "off") == 0) _mode = FAN_OFF;
    else if (strcmp(buf, "on") == 0) _mode = FAN_ON;
  }

  buf[0] = 0;
  if (_prefs->getByKey("fan_drv", buf, 11)) {
    if (strcmp(buf, "onoff") == 0) _drive = FAN_DRIVE_ONOFF;
    else if (strcmp(buf, "pwm") == 0) _drive = FAN_DRIVE_PWM;
  }

  int lo = _lo_c;
  int hi = _hi_c;
  buf[0] = 0;
  if (_prefs->getByKey("fan_lo", buf, 11)) lo = atoi(buf);
  buf[0] = 0;
  if (_prefs->getByKey("fan_hi", buf, 11)) hi = atoi(buf);
  if (lo >= 0 && hi <= 120 && lo < hi) {
    _lo_c = lo;
    _hi_c = hi;
  }

  _manual_duty = -1;
}

void TBeam1WBoard::attachDynamicPrefs(KeyValueStore* prefs) {
  _prefs = prefs;
  loadFanPrefs();
  updateFan();
}

const char* TBeam1WBoard::modeName() const {
  if (_manual_duty >= 0) return "manual";
  if (_mode == FAN_AUTO) return "auto";
  if (_mode == FAN_OFF) return "off";
  return "on";
}

const char* TBeam1WBoard::driveName() const {
  return _drive == FAN_DRIVE_ONOFF ? "onoff" : "pwm";
}

bool TBeam1WBoard::handleCommand(const char* command, uint32_t sender_timestamp, char* reply) {
  (void)sender_timestamp;

  if (strcmp(command, "get fan") == 0) {
    int cd = cooldownSecs();
    if (ntcImplausible(_temp_c)) {
      sprintf(reply, "> %s n/a duty=%d%% %s cd=%ds",
              modeName(), (int)_duty_pct, driveName(), cd);
    } else {
      sprintf(reply, "> %s %.1fC duty=%d%% %s cd=%ds",
              modeName(), (double)_temp_c, (int)_duty_pct, driveName(), cd);
    }
    return true;
  }

  if (strncmp(command, "set fan.lo ", 11) == 0) {
    int lo;
    if (!parseIntArg(&command[11], lo) || lo < 0 || lo >= _hi_c || lo > 100) {
      strcpy(reply, "Error: fan.lo must be 0..100 and < fan.hi");
    } else if (!persistKey("fan_lo", &command[11])) {
      strcpy(reply, "Error: failed to save fan.lo");
    } else {
      _lo_c = lo;
      sprintf(reply, "OK - fan.lo %d", lo);
    }
    return true;
  }

  if (strncmp(command, "set fan.hi ", 11) == 0) {
    int hi;
    if (!parseIntArg(&command[11], hi) || hi <= _lo_c || hi > 120) {
      strcpy(reply, "Error: fan.hi must be > fan.lo and <= 120");
    } else if (!persistKey("fan_hi", &command[11])) {
      strcpy(reply, "Error: failed to save fan.hi");
    } else {
      _hi_c = hi;
      sprintf(reply, "OK - fan.hi %d", hi);
    }
    return true;
  }

  if (strncmp(command, "set fan.drive ", 14) == 0) {
    const char* arg = &command[14];
    if (strcmp(arg, "pwm") == 0) {
      if (!persistKey("fan_drv", "pwm")) {
        strcpy(reply, "Error: failed to save fan.drive");
      } else {
        _drive = FAN_DRIVE_PWM;
        applyDuty(_duty_pct);
        strcpy(reply, "OK - fan.drive pwm");
      }
    } else if (strcmp(arg, "onoff") == 0) {
      if (!persistKey("fan_drv", "onoff")) {
        strcpy(reply, "Error: failed to save fan.drive");
      } else {
        _drive = FAN_DRIVE_ONOFF;
        applyDuty(_duty_pct);
        strcpy(reply, "OK - fan.drive onoff");
      }
    } else {
      strcpy(reply, "Error: fan.drive must be pwm or onoff");
    }
    return true;
  }

  if (strncmp(command, "set fan.duty ", 13) == 0) {
    int duty;
    if (!parseIntArg(&command[13], duty) || duty < 0 || duty > 100) {
      strcpy(reply, "Error: fan.duty must be 0-100");
    } else {
      _manual_duty = duty;
      applyDuty(duty);
      sprintf(reply, "OK - fan.duty %d (not saved)", duty);
    }
    return true;
  }

  if (strncmp(command, "set fan ", 8) == 0) {
    const char* arg = &command[8];
    if (strcmp(arg, "on") == 0) {
      if (!persistKey("fan", "on")) {
        strcpy(reply, "Error: failed to save fan mode");
      } else {
        _mode = FAN_ON;
        _manual_duty = -1;
        applyDuty(100);
        strcpy(reply, "OK - fan on");
      }
    } else if (strcmp(arg, "off") == 0) {
      if (!persistKey("fan", "off")) {
        strcpy(reply, "Error: failed to save fan mode");
      } else {
        _mode = FAN_OFF;
        _manual_duty = -1;
        applyDuty(0);
        strcpy(reply, "OK - fan off");
      }
    } else if (strcmp(arg, "auto") == 0) {
      if (!persistKey("fan", "auto")) {
        strcpy(reply, "Error: failed to save fan mode");
      } else {
        _mode = FAN_AUTO;
        _manual_duty = -1;
        updateFan();
        strcpy(reply, "OK - fan auto");
      }
    } else {
      strcpy(reply, "Error: fan must be on, off, or auto");
    }
    return true;
  }

  return false;
}
