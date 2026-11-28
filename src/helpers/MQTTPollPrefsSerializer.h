#pragma once

#include <helpers/ConfigSerializer.h>
#include <helpers/poll/PollPrefsValidation.h>
#include <helpers/poll/PollWipe.h>
#include <string.h>

// Absent poll object keeps the preloaded defaults and is not a repair, so
// upgrading does not rewrite every node's file. Out-of-range values are.
class MQTTPollPrefsSerializer : public ConfigSerializer {
  MQTTPrefs* _prefs;
  int32_t _enabled, _consent, _slot, _interval, _status_hours, _community, _requests, _limit;
  int32_t _pw[MQTT_POLL_MAX_TARGETS];
  char _url[MQTT_POLL_URL_LEN];
  char _t[MQTT_POLL_MAX_TARGETS][65];
  char _pass[MQTT_POLL_MAX_TARGETS][poll::kPwBufSize];
  bool _seen_enabled = false, _seen_consent = false, _seen_url = false;
  bool _seen_slot = false, _seen_interval = false, _seen_status = false;
  bool _seen_community = false, _seen_requests = false, _seen_limit = false;
  bool _seen_t[MQTT_POLL_MAX_TARGETS] = {};
  bool _seen_pw[MQTT_POLL_MAX_TARGETS] = {};
  bool _seen_pass[MQTT_POLL_MAX_TARGETS] = {};
protected:
  void structure() override {
    defStrict("enabled", _enabled, _seen_enabled);
    defStrict("consent", _consent, _seen_consent);
    defStrict("url", _url, sizeof(_url), _seen_url);
    defStrict("slot", _slot, _seen_slot);
    defStrict("interval_min", _interval, _seen_interval);
    defStrict("status_hours", _status_hours, _seen_status);
    defStrict("community", _community, _seen_community);
    defStrict("t1", _t[0], sizeof(_t[0]), _seen_t[0]);
    defStrict("pw1", _pw[0], _seen_pw[0]);
    defStrict("t2", _t[1], sizeof(_t[1]), _seen_t[1]);
    defStrict("pw2", _pw[1], _seen_pw[1]);
    defStrict("t3", _t[2], sizeof(_t[2]), _seen_t[2]);
    defStrict("pw3", _pw[2], _seen_pw[2]);
    defStrict("t4", _t[3], sizeof(_t[3]), _seen_t[3]);
    defStrict("pw4", _pw[3], _seen_pw[3]);
    defStrict("t5", _t[4], sizeof(_t[4]), _seen_t[4]);
    defStrict("pw5", _pw[4], _seen_pw[4]);
    defStrict("t6", _t[5], sizeof(_t[5]), _seen_t[5]);
    defStrict("pw6", _pw[5], _seen_pw[5]);
    defStrict("t7", _t[6], sizeof(_t[6]), _seen_t[6]);
    defStrict("pw7", _pw[6], _seen_pw[6]);
    defStrict("t8", _t[7], sizeof(_t[7]), _seen_t[7]);
    defStrict("pw8", _pw[7], _seen_pw[7]);
    // Later additions; older firmware skips these keys.
    defStrict("requests", _requests, _seen_requests);
    defStrict("pass1", _pass[0], sizeof(_pass[0]), _seen_pass[0]);
    defStrict("pass2", _pass[1], sizeof(_pass[1]), _seen_pass[1]);
    defStrict("pass3", _pass[2], sizeof(_pass[2]), _seen_pass[2]);
    defStrict("pass4", _pass[3], sizeof(_pass[3]), _seen_pass[3]);
    defStrict("pass5", _pass[4], sizeof(_pass[4]), _seen_pass[4]);
    defStrict("pass6", _pass[5], sizeof(_pass[5]), _seen_pass[5]);
    defStrict("pass7", _pass[6], sizeof(_pass[6]), _seen_pass[6]);
    defStrict("pass8", _pass[7], sizeof(_pass[7]), _seen_pass[7]);
    defStrict("limit", _limit, _seen_limit);
  }
public:
  explicit MQTTPollPrefsSerializer(MQTTPrefs* prefs)
      : _prefs(prefs), _enabled(prefs->poll_enabled), _consent(prefs->poll_consent),
        _slot(prefs->poll_slot), _interval(prefs->poll_interval_min),
        _status_hours(prefs->poll_status_hours), _community(prefs->poll_community),
        _requests(prefs->poll_requests), _limit(prefs->poll_limit) {
    memcpy(_url, prefs->poll_url, sizeof(_url));
    for (int i = 0; i < MQTT_POLL_MAX_TARGETS; i++) {
      _pw[i] = prefs->poll_pw_mode[i];
      memcpy(_pass[i], prefs->poll_pw[i], sizeof(_pass[i]));
      _pass[i][sizeof(_pass[i]) - 1] = '\0';
      if (poll::targetIsEmpty(prefs->poll_targets[i])) {
        _t[i][0] = '\0';
      } else {
        poll::formatTargetHex(prefs->poll_targets[i], _t[i]);
      }
    }
  }
  ~MQTTPollPrefsSerializer() { poll::detail::wipe(_pass, sizeof(_pass)); }
  void apply(bool* repaired) {
    if (_interval < static_cast<int32_t>(MQTT_POLL_INTERVAL_MIN) ||
        _interval > static_cast<int32_t>(MQTT_POLL_INTERVAL_MAX)) {
      _interval = MQTT_POLL_INTERVAL_DEFAULT;
      *repaired = true;
    }
    if (!poll::pollStatusHoursValid(_status_hours)) {
      _status_hours = MQTT_POLL_STATUS_HOURS_DEFAULT;
      *repaired = true;
    }
    if (_slot < 0 || _slot > MQTT_PREFS_SLOT_COUNT) {
      _slot = 0;
      *repaired = true;
    }
    if (_consent != 0 && _consent != 1) {
      _consent = 0;
      *repaired = true;
    }
    if (_community != 0 && _community != 1) {
      _community = MQTT_POLL_COMMUNITY_DEFAULT;
      *repaired = true;
    }
    if (_enabled != 0 && _enabled != 1) {
      _enabled = 0;
      *repaired = true;
    }
    if (_requests != 0 && _requests != 1) {
      _requests = MQTT_POLL_REQUESTS_DEFAULT;
      *repaired = true;
    }
    // A file without the key, or prefs never defaulted, take eight without a rewrite.
    if (!poll::pollLimitValid(_limit)) {
      _limit = MQTT_POLL_LIMIT_DEFAULT;
      if (_seen_limit) *repaired = true;
    }
    bool url_changed = false;
    if (!poll::normalizePollUrl(_url, &url_changed)) {
      *repaired = true;
    } else if (url_changed) {
      *repaired = true;
    }
    for (int i = 0; i < MQTT_POLL_MAX_TARGETS; i++) {
      uint8_t key[MQTT_POLL_PUBKEY_SIZE];
      bool folded = false;
      const poll::TargetHex parsed = poll::parseTargetHex(_t[i], key, &folded);
      if (_t[i][0] == '\0') {
        memset(_prefs->poll_targets[i], 0, MQTT_POLL_PUBKEY_SIZE);
      } else if (parsed != poll::TargetHex::Ok) {
        memset(_prefs->poll_targets[i], 0, MQTT_POLL_PUBKEY_SIZE);
        _t[i][0] = '\0';
        _pw[i] = MQTT_POLL_PW_AUTO;
        memset(_pass[i], 0, sizeof(_pass[i]));
        *repaired = true;
      } else {
        memcpy(_prefs->poll_targets[i], key, MQTT_POLL_PUBKEY_SIZE);
        if (folded) {
          poll::formatTargetHex(key, _t[i]);
          *repaired = true;
        }
      }
      if (_pw[i] < 0 || _pw[i] > MQTT_POLL_PW_CUSTOM) {
        _pw[i] = MQTT_POLL_PW_AUTO;
        *repaired = true;
      }
      // An empty entry keeps no password; a password needs custom mode and vice versa.
      uint8_t mode = static_cast<uint8_t>(_pw[i]);
      if (_t[i][0] == '\0' && (mode == MQTT_POLL_PW_CUSTOM || _pass[i][0] != '\0')) {
        mode = MQTT_POLL_PW_AUTO;
        memset(_pass[i], 0, sizeof(_pass[i]));
        *repaired = true;
      }
      if (poll::normalizeTargetPassword(&mode, _pass[i])) *repaired = true;
      _pw[i] = mode;
      _prefs->poll_pw_mode[i] = mode;
      memcpy(_prefs->poll_pw[i], _pass[i], sizeof(_prefs->poll_pw[i]));
    }
    if (_enabled == 1 &&
        !poll::pollCanEnable(_url, _slot, MQTT_PREFS_SLOT_COUNT)) {
      _enabled = 0;
      *repaired = true;
    }
    memcpy(_prefs->poll_url, _url, sizeof(_prefs->poll_url));
    _prefs->poll_enabled = static_cast<uint8_t>(_enabled);
    _prefs->poll_consent = static_cast<uint8_t>(_consent);
    _prefs->poll_slot = static_cast<uint8_t>(_slot);
    _prefs->poll_interval_min = static_cast<uint16_t>(_interval);
    _prefs->poll_status_hours = static_cast<uint8_t>(_status_hours);
    _prefs->poll_community = static_cast<uint8_t>(_community);
    _prefs->poll_requests = static_cast<uint8_t>(_requests);
    _prefs->poll_limit = static_cast<uint8_t>(_limit);
  }
};
