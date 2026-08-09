#pragma once

#include <helpers/ConfigSerializer.h>
#include <helpers/MQTTObserverValidation.h>
#include <helpers/MQTTPresets.h>
#include <helpers/MQTTPrefsStorage.h>

#ifdef WITH_MQTT_BRIDGE

static const int32_t MQTT_PREFS_JSON_FORMAT_VERSION = 1;

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
    int32_t _power_save;
    bool _seen_ssid = false, _seen_password = false, _seen_power_save = false;
  protected:
    void structure() override {
      defStrict("ssid", _prefs->wifi_ssid, sizeof(_prefs->wifi_ssid), _seen_ssid);
      defStrict("password", _prefs->wifi_password, sizeof(_prefs->wifi_password), _seen_password);
      defStrict("power_save", _power_save, _seen_power_save);
    }
  public:
    explicit WifiPrefs(MQTTPrefs* prefs) : _prefs(prefs), _power_save(prefs->wifi_power_save) {}
    bool apply(bool* repaired) {
      if (_power_save < 0 || _power_save > 2) {
        _power_save = 1;
        *repaired = true;
      }
      _prefs->wifi_power_save = static_cast<uint8_t>(_power_save);
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

  class NeighborPrefs : public ConfigSerializer {
    MQTTPrefs* _prefs;
    int32_t _enabled, _interval_ms;
    bool _seen_enabled = false, _seen_interval = false;
  protected:
    void structure() override {
      defStrict("enabled", _enabled, _seen_enabled);
      defStrict("interval_ms", _interval_ms, _seen_interval);
    }
  public:
    explicit NeighborPrefs(MQTTPrefs* prefs)
        : _prefs(prefs), _enabled(prefs->mqtt_neighbors_enabled),
          _interval_ms(static_cast<int32_t>(prefs->mqtt_neighbors_interval)) {}
    void apply(bool* repaired) {
      if (_enabled < 0 || _enabled > 1) { _enabled = 0; *repaired = true; }
      if (_interval_ms < static_cast<int32_t>(MQTT_NEIGHBORS_MIN_INTERVAL_MS) ||
          _interval_ms > static_cast<int32_t>(MQTT_NEIGHBORS_MAX_INTERVAL_MS)) {
        _interval_ms = static_cast<int32_t>(MQTT_NEIGHBORS_DEFAULT_INTERVAL_MS);
        *repaired = true;
      }
      _prefs->mqtt_neighbors_enabled = static_cast<uint8_t>(_enabled);
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
    int32_t _port, _filter;
    bool _seen_preset = false, _seen_host = false, _seen_port = false;
    bool _seen_username = false, _seen_password = false, _seen_token = false;
    bool _seen_topic = false, _seen_audience = false, _seen_filter = false;
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
    }
  public:
    SlotPrefs(MQTTPrefs* prefs, int index)
        : _prefs(prefs), _index(index), _port(prefs->mqtt_slot_port[index]),
          _filter(prefs->mqtt_slot_packet_filter[index]) {}
    void apply(bool* repaired) {
      if (_port < 0 || _port > 65535) { _port = 0; *repaired = true; }
      if (_filter < 0 || _filter > 65535) { _filter = 0xffff; *repaired = true; }
      const char* preset = _prefs->mqtt_slot_preset[_index];
      if (strcmp(preset, MQTT_PRESET_NONE) != 0 &&
          strcmp(preset, MQTT_PRESET_CUSTOM) != 0 && findMQTTPreset(preset) == nullptr) {
        strcpy(_prefs->mqtt_slot_preset[_index], MQTT_PRESET_NONE);
        *repaired = true;
      }
      _prefs->mqtt_slot_port[_index] = static_cast<uint16_t>(_port);
      _prefs->mqtt_slot_packet_filter[_index] = static_cast<uint16_t>(_filter);
    }
  };

  class MqttPrefs : public ConfigSerializer {
    MQTTPrefs* _prefs;
    int32_t _packets_enabled, _raw_enabled, _tx_enabled, _rx_enabled;
    char _default_iata[sizeof(MQTTPrefs::mqtt_iata)];
    bool _seen_origin = false, _seen_iata = false, _seen_packets = false;
    bool _seen_raw = false, _seen_tx = false, _seen_rx = false;
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
          _rx_enabled(prefs->mqtt_rx_enabled), _status(prefs), _neighbors(prefs),
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
      _prefs->mqtt_packets_enabled = static_cast<uint8_t>(_packets_enabled);
      _prefs->mqtt_raw_enabled = static_cast<uint8_t>(_raw_enabled);
      _prefs->mqtt_tx_enabled = static_cast<uint8_t>(_tx_enabled);
      _prefs->mqtt_rx_enabled = static_cast<uint8_t>(_rx_enabled);
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

  MQTTPrefs* _prefs;
  int32_t _version = MQTT_PREFS_JSON_FORMAT_VERSION;
  bool _seen_version = false;
  WifiPrefs _wifi;
  TimePrefs _time;
  MqttPrefs _mqtt;
  SnmpPrefs _snmp;
  RadioPrefs _radio;
  AlertPrefs _alert;

protected:
  void structure() override {
    defStrict("version", _version, _seen_version);
    def("wifi", _wifi);
    def("time", _time);
    def("mqtt", _mqtt);
    def("snmp", _snmp);
    def("radio", _radio);
    def("alert", _alert);
  }

public:
  explicit MQTTPrefsSerializer(MQTTPrefs* prefs, const MQTTPrefs* repair_defaults = nullptr)
      : _prefs(prefs), _wifi(prefs),
        _time(prefs, repair_defaults ? repair_defaults : prefs),
        _mqtt(prefs, repair_defaults ? repair_defaults : prefs), _snmp(prefs),
        _radio(prefs), _alert(prefs) {}

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
    return true;
  }

  bool apply(bool* repaired) {
    return hasSupportedVersion() && normalize(repaired);
  }
};

#endif  // WITH_MQTT_BRIDGE
