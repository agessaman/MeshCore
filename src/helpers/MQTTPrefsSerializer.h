#pragma once

#include <helpers/ConfigSerializer.h>
#include <helpers/MQTTConnectionPolicy.h>
#include <helpers/MQTTObserverValidation.h>
#include <helpers/MQTTPresets.h>
#include <helpers/MQTTNeighborsPolicy.h>
#include <helpers/MQTTPrefsStorage.h>
#include <helpers/poll/PollPrefsPolicy.h>
#include <helpers/WifiPowerSavePolicy.h>
#include <helpers/WifiTxPowerPolicy.h>

#ifdef WITH_MQTT_BRIDGE

static const int32_t MQTT_PREFS_JSON_FORMAT_VERSION = 1;

// Format invariant, binding on every future schema version: `version` is the
// FIRST property of the root object. The probe below reads it with this
// firmware's grammar and stops at the first construct that grammar cannot
// tokenize, so a newer file that introduces unknown syntax ahead of its version
// field is indistinguishable from a corrupt one here. That costs it the
// opaque-future preservation guarantee: as a temp left by an interrupted commit
// it would be classified as discardable rather than retained. Keep the version
// first when adding fields or bumping the version; a host test pins it.
//
// Reads only the mandatory root version while ignoring every other schema
// field. Recovery uses this before the v1 decoder so a syntactically valid
// future file remains opaque even when that schema changes a v1 field's type.
class MQTTPrefsVersionProbe : public ConfigSerializer {
  int32_t _version = 0;
  bool _seen_version = false;
protected:
  void structure() override { defStrict("version", _version, _seen_version); }
public:
  bool hasFutureVersion() const {
    return _seen_version && _version > MQTT_PREFS_JSON_FORMAT_VERSION;
  }
};

// ConfigSerializer adapter for the observer preference POD. Keeping the
// serializer separate avoids making the runtime object layout part of the JSON
// format and adds no permanent per-slot serializer state to CommonCLI.
class MQTTPrefsSerializer : public ConfigSerializer {

  class WifiPrefs : public ConfigSerializer {
    MQTTPrefs* _prefs;
    int32_t _power_save, _setup_complete, _tx_power;
    bool _seen_ssid = false, _seen_password = false, _seen_power_save = false;
    bool _seen_setup_complete = false, _seen_tx_power = false;
  protected:
    void structure() override {
      defStrict("ssid", _prefs->wifi_ssid, sizeof(_prefs->wifi_ssid), _seen_ssid);
      defStrict("password", _prefs->wifi_password, sizeof(_prefs->wifi_password), _seen_password);
      defStrict("power_save", _power_save, _seen_power_save);
      defStrict("setup_complete", _setup_complete, _seen_setup_complete);
      defStrict("tx_power", _tx_power, _seen_tx_power);
    }
  public:
    explicit WifiPrefs(MQTTPrefs* prefs)
        : _prefs(prefs), _power_save(prefs->wifi_power_save),
          _setup_complete(prefs->network_setup_complete),
          _tx_power(prefs->wifi_tx_power) {}
    bool apply(bool* repaired) {
      if (_power_save < 0 || _power_save > WifiPowerSavePolicy::kMaxStoredValue) {
        _power_save = 1;
        *repaired = true;
      }
      if (_setup_complete < 0 || _setup_complete > 1) {
        _setup_complete = 0;
        *repaired = true;
      }
      if (!WifiTxPowerPolicy::isValidStored(_tx_power)) {
        _tx_power = WifiTxPowerPolicy::kBoardDefault;
        *repaired = true;
      }
      _prefs->wifi_tx_power = static_cast<uint8_t>(_tx_power);
      _prefs->wifi_power_save = static_cast<uint8_t>(_power_save);
      _prefs->network_setup_complete = static_cast<uint8_t>(_setup_complete);
      return true;
    }
  };

  class TimePrefs : public ConfigSerializer {
    MQTTPrefs* _prefs;
    int32_t _utc_offset, _default_utc_offset;
    char _default_ntp_server[sizeof(MQTTPrefs::mqtt_ntp_server)];
    bool _seen_timezone = false, _seen_utc_offset = false, _seen_ntp_server = false;
  protected:
    void structure() override {
      defStrict("timezone", _prefs->timezone_string, sizeof(_prefs->timezone_string), _seen_timezone);
      defStrict("utc_offset", _utc_offset, _seen_utc_offset);
      defStrict("ntp_server", _prefs->mqtt_ntp_server, sizeof(_prefs->mqtt_ntp_server), _seen_ntp_server);
    }
  public:
    TimePrefs(MQTTPrefs* prefs, const MQTTPrefs* defaults)
        : _prefs(prefs), _utc_offset(prefs->timezone_offset),
          _default_utc_offset(defaults->timezone_offset) {
      if (_default_utc_offset < -12 || _default_utc_offset > 14) {
        _default_utc_offset = 0;
      }
      memcpy(_default_ntp_server, defaults->mqtt_ntp_server, sizeof(_default_ntp_server));
      if (_default_ntp_server[0] != '\0' &&
          !mqttNtpHostnameValid(_default_ntp_server)) {
        _default_ntp_server[0] = '\0';
      }
    }
    bool apply(bool* repaired) {
      if (_utc_offset < -12 || _utc_offset > 14) {
        _utc_offset = _default_utc_offset;
        *repaired = true;
      }
      if (_prefs->mqtt_ntp_server[0] != '\0' &&
          !mqttNtpHostnameValid(_prefs->mqtt_ntp_server)) {
        memcpy(_prefs->mqtt_ntp_server, _default_ntp_server, sizeof(_default_ntp_server));
        *repaired = true;
      }
      _prefs->timezone_offset = static_cast<int8_t>(_utc_offset);
      return true;
    }
  };

  class StatusPrefs : public ConfigSerializer {
    MQTTPrefs* _prefs;
    int32_t _enabled, _interval_ms;
    bool _seen_enabled = false, _seen_interval = false;
  protected:
    void structure() override {
      defStrict("enabled", _enabled, _seen_enabled);
      defStrict("interval_ms", _interval_ms, _seen_interval);
    }
  public:
    explicit StatusPrefs(MQTTPrefs* prefs)
        : _prefs(prefs), _enabled(prefs->mqtt_status_enabled),
          _interval_ms(static_cast<int32_t>(prefs->mqtt_status_interval)) {}
    void apply(bool* repaired) {
      if (_enabled < 0 || _enabled > 1) { _enabled = 1; *repaired = true; }
      if (_interval_ms < 60000 || _interval_ms > 3600000) {
        _interval_ms = 300000;
        *repaired = true;
      }
      _prefs->mqtt_status_enabled = static_cast<uint8_t>(_enabled);
      _prefs->mqtt_status_interval = static_cast<uint32_t>(_interval_ms);
    }
  };

  // "mode" holds auto/on/off. "enabled" stays the 0/1 older firmware reads:
  // 1 only for on, so a rollback sees off for both auto and off and repairs
  // nothing — the file, and with it "mode", survives unless the old firmware
  // rewrites it for another reason.
  class NeighborPrefs : public ConfigSerializer {
    MQTTPrefs* _prefs;
    int32_t _enabled, _mode, _interval_ms;
    bool _seen_enabled = false, _seen_mode = false, _seen_interval = false;
  protected:
    void structure() override {
      defStrict("enabled", _enabled, _seen_enabled);
      defStrict("mode", _mode, _seen_mode);
      defStrict("interval_ms", _interval_ms, _seen_interval);
    }
  public:
    explicit NeighborPrefs(MQTTPrefs* prefs)
        : _prefs(prefs),
          _enabled(prefs->mqtt_neighbors_enabled == MQTT_NEIGHBORS_ON ? 1 : 0),
          _mode(prefs->mqtt_neighbors_enabled),
          _interval_ms(static_cast<int32_t>(prefs->mqtt_neighbors_interval)) {}
    void apply(bool* repaired) {
      if (_seen_mode) {
        if (_mode < 0 || _mode > MQTT_NEIGHBORS_MODE_MAX) {
          _mode = MQTT_NEIGHBORS_OFF;
          *repaired = true;
        }
        // Keep "enabled" truthful for older firmware.
        if (_seen_enabled && _enabled != (_mode == MQTT_NEIGHBORS_ON ? 1 : 0)) *repaired = true;
      } else if (_seen_enabled) {
        // Written before "mode" existed: 0 was never set or off there, now auto.
        // Not a repair, so upgrading does not rewrite every node's file; the
        // key is added on the next save.
        if (_enabled == 1) {
          _mode = MQTT_NEIGHBORS_ON;
        } else if (_enabled == 0) {
          _mode = MQTT_NEIGHBORS_AUTO;
        } else {
          _mode = MQTT_NEIGHBORS_OFF;
          *repaired = true;
        }
      } else if (_mode < 0 || _mode > MQTT_NEIGHBORS_MODE_MAX) {
        _mode = MQTT_NEIGHBORS_OFF;
        *repaired = true;
      }
      if (_interval_ms < static_cast<int32_t>(MQTT_NEIGHBORS_MIN_INTERVAL_MS) ||
          _interval_ms > static_cast<int32_t>(MQTT_NEIGHBORS_MAX_INTERVAL_MS)) {
        _interval_ms = static_cast<int32_t>(MQTT_NEIGHBORS_DEFAULT_INTERVAL_MS);
        *repaired = true;
      }
      _prefs->mqtt_neighbors_enabled = static_cast<uint8_t>(_mode);
      _prefs->mqtt_neighbors_interval = static_cast<uint32_t>(_interval_ms);
    }
  };

  class OwnerPrefs : public ConfigSerializer {
    MQTTPrefs* _prefs;
    bool _seen_public_key = false, _seen_email = false;
  protected:
    void structure() override {
      defStrict("public_key", _prefs->mqtt_owner_public_key,
                sizeof(_prefs->mqtt_owner_public_key), _seen_public_key);
      defStrict("email", _prefs->mqtt_email, sizeof(_prefs->mqtt_email), _seen_email);
    }
  public:
    explicit OwnerPrefs(MQTTPrefs* prefs) : _prefs(prefs) {}
    void apply(bool* repaired) {
      if (_prefs->mqtt_owner_public_key[0] != '\0' &&
          !mqttOwnerKeyValid(_prefs->mqtt_owner_public_key)) {
        _prefs->mqtt_owner_public_key[0] = '\0';
        *repaired = true;
      }
    }
  };

  class SlotPrefs : public ConfigSerializer {
    MQTTPrefs* _prefs;
    int _index;
    int32_t _port, _filter, _neighbors;
    bool _seen_preset = false, _seen_host = false, _seen_port = false;
    bool _seen_username = false, _seen_password = false, _seen_token = false;
    bool _seen_topic = false, _seen_audience = false, _seen_filter = false;
    bool _seen_iata = false, _seen_neighbors = false;
  protected:
    void structure() override {
      defStrict("preset", _prefs->mqtt_slot_preset[_index],
                sizeof(_prefs->mqtt_slot_preset[_index]), _seen_preset);
      defStrict("host", _prefs->mqtt_slot_host[_index],
                sizeof(_prefs->mqtt_slot_host[_index]), _seen_host);
      defStrict("port", _port, _seen_port);
      defStrict("username", _prefs->mqtt_slot_username[_index],
                sizeof(_prefs->mqtt_slot_username[_index]), _seen_username);
      defStrict("password", _prefs->mqtt_slot_password[_index],
                sizeof(_prefs->mqtt_slot_password[_index]), _seen_password);
      defStrict("token", _prefs->mqtt_slot_token[_index],
                sizeof(_prefs->mqtt_slot_token[_index]), _seen_token);
      defStrict("topic", _prefs->mqtt_slot_topic[_index],
                sizeof(_prefs->mqtt_slot_topic[_index]), _seen_topic);
      defStrict("audience", _prefs->mqtt_slot_audience[_index],
                sizeof(_prefs->mqtt_slot_audience[_index]), _seen_audience);
      defStrict("packet_filter", _filter, _seen_filter);
      defStrict("iata", _prefs->mqtt_slot_iata[_index],
                sizeof(_prefs->mqtt_slot_iata[_index]), _seen_iata);
      defStrict("neighbors", _neighbors, _seen_neighbors);
    }
  public:
    SlotPrefs(MQTTPrefs* prefs, int index)
        : _prefs(prefs), _index(index), _port(prefs->mqtt_slot_port[index]),
          _filter(prefs->mqtt_slot_packet_filter[index]),
          _neighbors(prefs->mqtt_slot_neighbors[index]) {}
    void apply(bool* repaired) {
      if (_port < 0 || _port > 65535) { _port = 0; *repaired = true; }
      // Missing (older file) keeps the auto default; corrupt fails safe to off.
      if (_neighbors < 0 || _neighbors > MQTT_NEIGHBORS_MODE_MAX) {
        _neighbors = MQTT_NEIGHBORS_OFF;
        *repaired = true;
      }
      _prefs->mqtt_slot_neighbors[_index] = static_cast<uint8_t>(_neighbors);
      if (_filter < 0 || _filter > 65535) { _filter = 0xffff; *repaired = true; }
      const char* preset = _prefs->mqtt_slot_preset[_index];
      if (strcmp(preset, MQTT_PRESET_NONE) != 0 &&
          strcmp(preset, MQTT_PRESET_CUSTOM) != 0 && findMQTTPreset(preset) == nullptr) {
        strcpy(_prefs->mqtt_slot_preset[_index], MQTT_PRESET_NONE);
        *repaired = true;
      }
      _prefs->mqtt_slot_port[_index] = static_cast<uint16_t>(_port);
      _prefs->mqtt_slot_packet_filter[_index] = static_cast<uint16_t>(_filter);
      // An unusable override is dropped rather than repaired to a default: the
      // slot then falls back to the global IATA, the same as never setting one.
      char* iata = _prefs->mqtt_slot_iata[_index];
      if (iata[0] != '\0') {
        if (!mqttIataValid(iata)) {
          iata[0] = '\0';
          *repaired = true;
        } else {
          for (char* p = iata; *p; ++p) {
            if (*p >= 'a' && *p <= 'z') {
              *p = static_cast<char>(*p - ('a' - 'A'));
              *repaired = true;
            }
          }
        }
      }
    }
  };

  class MqttPrefs : public ConfigSerializer {
    MQTTPrefs* _prefs;
    int32_t _packets_enabled, _raw_enabled, _tx_enabled, _rx_enabled, _advert_verify;
    int32_t _breaker_min;
    int32_t _selfheal;
    char _default_iata[sizeof(MQTTPrefs::mqtt_iata)];
    bool _seen_origin = false, _seen_iata = false, _seen_packets = false;
    bool _seen_raw = false, _seen_tx = false, _seen_rx = false, _seen_advert_verify = false;
    bool _seen_breaker = false;
    bool _seen_selfheal = false;
    StatusPrefs _status;
    NeighborPrefs _neighbors;
    OwnerPrefs _owner;
    static_assert(MQTT_PREFS_SLOT_COUNT == 6,
                  "MQTTPrefsSerializer slot members and keys must be updated");
    SlotPrefs _slot1, _slot2, _slot3, _slot4, _slot5, _slot6;
  protected:
    void structure() override {
      defStrict("origin", _prefs->mqtt_origin, sizeof(_prefs->mqtt_origin), _seen_origin);
      defStrict("iata", _prefs->mqtt_iata, sizeof(_prefs->mqtt_iata), _seen_iata);
      defStrict("packets_enabled", _packets_enabled, _seen_packets);
      defStrict("raw_enabled", _raw_enabled, _seen_raw);
      defStrict("tx_enabled", _tx_enabled, _seen_tx);
      defStrict("rx_enabled", _rx_enabled, _seen_rx);
      defStrict("advert_verify", _advert_verify, _seen_advert_verify);
      defStrict("breaker_min", _breaker_min, _seen_breaker);
      defStrict("selfheal", _selfheal, _seen_selfheal);
      def("status", _status);
      def("neighbors", _neighbors);
      def("owner", _owner);
      def("slot1", _slot1);
      def("slot2", _slot2);
      def("slot3", _slot3);
      def("slot4", _slot4);
      def("slot5", _slot5);
      def("slot6", _slot6);
    }
  public:
    MqttPrefs(MQTTPrefs* prefs, const MQTTPrefs* defaults)
        : _prefs(prefs), _packets_enabled(prefs->mqtt_packets_enabled),
          _raw_enabled(prefs->mqtt_raw_enabled), _tx_enabled(prefs->mqtt_tx_enabled),
          _rx_enabled(prefs->mqtt_rx_enabled), _advert_verify(prefs->mqtt_advert_verify),
          _breaker_min(prefs->mqtt_breaker_probe_min), _selfheal(prefs->mqtt_selfheal_enabled),
          _status(prefs), _neighbors(prefs),
          _owner(prefs), _slot1(prefs, 0), _slot2(prefs, 1),
          _slot3(prefs, 2), _slot4(prefs, 3),
          _slot5(prefs, 4), _slot6(prefs, 5) {
      memcpy(_default_iata, defaults->mqtt_iata, sizeof(_default_iata));
      if (_default_iata[0] != '\0' && !mqttIataValid(_default_iata)) {
        _default_iata[0] = '\0';
      }
      for (char* p = _default_iata; *p; ++p) {
        if (*p >= 'a' && *p <= 'z') *p = static_cast<char>(*p - ('a' - 'A'));
      }
    }
    void apply(bool* repaired) {
      if (_packets_enabled < 0 || _packets_enabled > 1) { _packets_enabled = 1; *repaired = true; }
      if (_raw_enabled < 0 || _raw_enabled > 1) { _raw_enabled = 0; *repaired = true; }
      if (_tx_enabled < 0 || _tx_enabled > 2) { _tx_enabled = 2; *repaired = true; }
      if (_rx_enabled < 0 || _rx_enabled > 1) { _rx_enabled = 1; *repaired = true; }
      if (_advert_verify < 0 || _advert_verify > 1) { _advert_verify = 0; *repaired = true; }
      if (!MQTTConnectionPolicy::breakerProbeMinutesValid(_breaker_min)) {
        _breaker_min = MQTTConnectionPolicy::kBreakerProbeDefaultMinutes;
        *repaired = true;
      }
      _prefs->mqtt_packets_enabled = static_cast<uint8_t>(_packets_enabled);
      _prefs->mqtt_raw_enabled = static_cast<uint8_t>(_raw_enabled);
      _prefs->mqtt_tx_enabled = static_cast<uint8_t>(_tx_enabled);
      _prefs->mqtt_rx_enabled = static_cast<uint8_t>(_rx_enabled);
      _prefs->mqtt_advert_verify = static_cast<uint8_t>(_advert_verify);
      if (_selfheal < 0 || _selfheal > 1) { _selfheal = 0; *repaired = true; }
      _prefs->mqtt_breaker_probe_min = static_cast<uint8_t>(_breaker_min);
      _prefs->mqtt_selfheal_enabled = static_cast<uint8_t>(_selfheal);
      if (_prefs->mqtt_iata[0] != '\0') {
        if (!mqttIataValid(_prefs->mqtt_iata)) {
          memcpy(_prefs->mqtt_iata, _default_iata, sizeof(_default_iata));
          *repaired = true;
        } else {
          for (char* p = _prefs->mqtt_iata; *p; ++p) {
            if (*p >= 'a' && *p <= 'z') {
              *p = static_cast<char>(*p - ('a' - 'A'));
              *repaired = true;
            }
          }
        }
      }
      _status.apply(repaired);
      _neighbors.apply(repaired);
      _owner.apply(repaired);
      _slot1.apply(repaired); _slot2.apply(repaired); _slot3.apply(repaired);
      _slot4.apply(repaired); _slot5.apply(repaired); _slot6.apply(repaired);
    }
  };

  class SnmpPrefs : public ConfigSerializer {
    MQTTPrefs* _prefs;
    int32_t _enabled;
    bool _seen_enabled = false, _seen_community = false;
  protected:
    void structure() override {
      defStrict("enabled", _enabled, _seen_enabled);
      defStrict("community", _prefs->snmp_community,
                sizeof(_prefs->snmp_community), _seen_community);
    }
  public:
    explicit SnmpPrefs(MQTTPrefs* prefs) : _prefs(prefs), _enabled(prefs->snmp_enabled) {}
    void apply(bool* repaired) {
      if (_enabled < 0 || _enabled > 1) { _enabled = 0; *repaired = true; }
      _prefs->snmp_enabled = static_cast<uint8_t>(_enabled);
    }
  };

  class RadioPrefs : public ConfigSerializer {
    MQTTPrefs* _prefs;
    int32_t _watchdog_min;
    bool _seen_watchdog = false;
  protected:
    void structure() override { defStrict("watchdog_min", _watchdog_min, _seen_watchdog); }
  public:
    explicit RadioPrefs(MQTTPrefs* prefs)
        : _prefs(prefs), _watchdog_min(prefs->radio_watchdog_minutes) {}
    void apply(bool* repaired) {
      if (_watchdog_min < 0 || _watchdog_min > 120) { _watchdog_min = 5; *repaired = true; }
      _prefs->radio_watchdog_minutes = static_cast<uint8_t>(_watchdog_min);
    }
  };

  class AlertPrefs : public ConfigSerializer {
    MQTTPrefs* _prefs;
    int32_t _enabled, _wifi_minutes, _mqtt_minutes, _rate_limit_min;
    bool _seen_enabled = false, _seen_psk = false, _seen_wifi = false;
    bool _seen_mqtt = false, _seen_rate = false, _seen_hashtag = false, _seen_region = false;
  protected:
    void structure() override {
      defStrict("enabled", _enabled, _seen_enabled);
      defStrict("psk_hex", _prefs->alert_psk_hex, sizeof(_prefs->alert_psk_hex), _seen_psk);
      defStrict("wifi_minutes", _wifi_minutes, _seen_wifi);
      defStrict("mqtt_minutes", _mqtt_minutes, _seen_mqtt);
      defStrict("rate_limit_min", _rate_limit_min, _seen_rate);
      defStrict("hashtag", _prefs->alert_hashtag, sizeof(_prefs->alert_hashtag), _seen_hashtag);
      defStrict("region", _prefs->alert_region, sizeof(_prefs->alert_region), _seen_region);
    }
  public:
    explicit AlertPrefs(MQTTPrefs* prefs)
        : _prefs(prefs), _enabled(prefs->alert_enabled),
          _wifi_minutes(prefs->alert_wifi_minutes), _mqtt_minutes(prefs->alert_mqtt_minutes),
          _rate_limit_min(prefs->alert_min_interval_min) {}
    void apply(bool* repaired) {
      if (_enabled < 0 || _enabled > 1) { _enabled = 0; *repaired = true; }
      if (_wifi_minutes < 0 || _wifi_minutes > 1440) { _wifi_minutes = 30; *repaired = true; }
      if (_mqtt_minutes < 0 || _mqtt_minutes > 10080) { _mqtt_minutes = 240; *repaired = true; }
      if (_rate_limit_min < 60 || _rate_limit_min > 10080) { _rate_limit_min = 60; *repaired = true; }
      if (_prefs->alert_psk_hex[0] != '\0') {
        const size_t len = strlen(_prefs->alert_psk_hex);
        bool valid_hex = len == 32;
        for (size_t i = 0; valid_hex && i < len; ++i) {
          const char c = _prefs->alert_psk_hex[i];
          valid_hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                      (c >= 'a' && c <= 'f');
        }
        if (!valid_hex) {
          _prefs->alert_psk_hex[0] = '\0';
          _prefs->alert_hashtag[0] = '\0';
          *repaired = true;
        }
      }
      _prefs->alert_enabled = static_cast<uint8_t>(_enabled);
      _prefs->alert_wifi_minutes = static_cast<uint16_t>(_wifi_minutes);
      _prefs->alert_mqtt_minutes = static_cast<uint16_t>(_mqtt_minutes);
      _prefs->alert_min_interval_min = static_cast<uint16_t>(_rate_limit_min);
    }
  };

  class DisplayPrefs : public ConfigSerializer {
    MQTTPrefs* _prefs;
    int32_t _timeout_s, _flip;
    bool _seen_timeout = false, _seen_flip = false;
  protected:
    void structure() override {
      defStrict("timeout_s", _timeout_s, _seen_timeout);
      defStrict("flip", _flip, _seen_flip);
    }
  public:
    explicit DisplayPrefs(MQTTPrefs* prefs)
        : _prefs(prefs), _timeout_s(prefs->display_timeout_secs), _flip(prefs->display_flip) {}
    void apply(bool* repaired) {
      if (_timeout_s < 0 || _timeout_s > DISPLAY_TIMEOUT_MAX_SECS) {
        _timeout_s = DISPLAY_TIMEOUT_DEFAULT_SECS;
        *repaired = true;
      }
      if (_flip < 0 || _flip > 1) { _flip = 0; *repaired = true; }
      _prefs->display_timeout_secs = static_cast<uint16_t>(_timeout_s);
      _prefs->display_flip = static_cast<uint8_t>(_flip);
    }
  };

  // Absent poll object keeps the preloaded defaults and is not a repair, so
  // upgrading does not rewrite every node's file. Out-of-range values are.
  class PollPrefs : public ConfigSerializer {
    MQTTPrefs* _prefs;
    int32_t _enabled, _consent, _slot, _interval, _status_hours, _community;
    int32_t _pw[MQTT_POLL_MAX_TARGETS];
    char _url[MQTT_POLL_URL_LEN];
    char _t[MQTT_POLL_MAX_TARGETS][65];
    bool _seen_enabled = false, _seen_consent = false, _seen_url = false;
    bool _seen_slot = false, _seen_interval = false, _seen_status = false;
    bool _seen_community = false;
    bool _seen_t[MQTT_POLL_MAX_TARGETS] = {};
    bool _seen_pw[MQTT_POLL_MAX_TARGETS] = {};
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
    }
  public:
    explicit PollPrefs(MQTTPrefs* prefs)
        : _prefs(prefs), _enabled(prefs->poll_enabled), _consent(prefs->poll_consent),
          _slot(prefs->poll_slot), _interval(prefs->poll_interval_min),
          _status_hours(prefs->poll_status_hours), _community(prefs->poll_community) {
      memcpy(_url, prefs->poll_url, sizeof(_url));
      for (int i = 0; i < MQTT_POLL_MAX_TARGETS; i++) {
        _pw[i] = prefs->poll_pw_mode[i];
        if (poll::targetIsEmpty(prefs->poll_targets[i])) {
          _t[i][0] = '\0';
        } else {
          poll::formatTargetHex(prefs->poll_targets[i], _t[i]);
        }
      }
    }
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
          *repaired = true;
        } else {
          memcpy(_prefs->poll_targets[i], key, MQTT_POLL_PUBKEY_SIZE);
          if (folded) {
            poll::formatTargetHex(key, _t[i]);
            *repaired = true;
          }
        }
        if (_pw[i] < 0 || _pw[i] > MQTT_POLL_PW_HELLO) {
          _pw[i] = MQTT_POLL_PW_AUTO;
          *repaired = true;
        }
        _prefs->poll_pw_mode[i] = static_cast<uint8_t>(_pw[i]);
      }
      if (_enabled == 1 &&
          !poll::pollCanEnable(_url, _slot, _consent, MQTT_PREFS_SLOT_COUNT)) {
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
    }
  };

  MQTTPrefs* _prefs;
  int32_t _version = MQTT_PREFS_JSON_FORMAT_VERSION;
  bool _seen_version = false;
  WifiPrefs _wifi;
  TimePrefs _time;
  MqttPrefs _mqtt;
  SnmpPrefs _snmp;
  RadioPrefs _radio;
  AlertPrefs _alert;
  DisplayPrefs _display;
  PollPrefs _poll;

protected:
  void structure() override {
    defStrict("version", _version, _seen_version);  // must stay first; see above
    def("wifi", _wifi);
    def("time", _time);
    def("mqtt", _mqtt);
    def("snmp", _snmp);
    def("radio", _radio);
    def("alert", _alert);
    def("display", _display);
    def("poll", _poll);
  }

public:
  explicit MQTTPrefsSerializer(MQTTPrefs* prefs, const MQTTPrefs* repair_defaults = nullptr)
      : _prefs(prefs), _wifi(prefs),
        _time(prefs, repair_defaults ? repair_defaults : prefs),
        _mqtt(prefs, repair_defaults ? repair_defaults : prefs), _snmp(prefs),
        _radio(prefs), _alert(prefs), _display(prefs), _poll(prefs) {}

  bool hasSupportedVersion() const {
    return _seen_version && _version == MQTT_PREFS_JSON_FORMAT_VERSION;
  }
  bool hasFutureVersion() const {
    return _seen_version && _version > MQTT_PREFS_JSON_FORMAT_VERSION;
  }

  bool normalize(bool* repaired) {
    if (repaired == nullptr) return false;
    *repaired = false;
    _wifi.apply(repaired);
    _time.apply(repaired);
    _mqtt.apply(repaired);
    _snmp.apply(repaired);
    _radio.apply(repaired);
    _alert.apply(repaired);
    _display.apply(repaired);
    _poll.apply(repaired);
    return true;
  }

  bool apply(bool* repaired) {
    return hasSupportedVersion() && normalize(repaired);
  }
};

#endif  // WITH_MQTT_BRIDGE
