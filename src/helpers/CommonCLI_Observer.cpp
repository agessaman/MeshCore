// CommonCLI_Observer.cpp — fork-owned observer/MQTT/WiFi/timezone/alert/SNMP CLI
// command handling, split out of CommonCLI.cpp so the upstream-tracked file carries
// only two small delegation hooks. These are CommonCLI member functions, so they
// retain full access to _prefs/_callbacks/_board/savePrefs() with no re-plumbing.
//
// Behavior is intentionally identical to the previously-inlined branches: MQTT
// commands keep their WITH_MQTT_BRIDGE guard; alert/SNMP commands remain unguarded.
// Each handler returns true if it recognized the command, false to fall through to
// the base get/set parser in CommonCLI.cpp.

#include <Arduino.h>
#include "CommonCLI.h"
#include "TxtDataHelpers.h"
#include "AlertReporter.h"  // for alertReporterBannedChannelMatch[Hex]()
#include <Utils.h>
#ifdef ESP_PLATFORM
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#endif
#ifdef WITH_MQTT_BRIDGE
#include "bridges/MQTTBridge.h"
#include "MQTTDefaults.h"
#endif

// Local copy of the busted-libc-safe atoi (the original in CommonCLI.cpp is static).
static uint32_t _atoi(const char* sp) {
  uint32_t n = 0;
  while (*sp && *sp >= '0' && *sp <= '9') {
    n *= 10;
    n += (*sp++ - '0');
  }
  return n;
}

#ifdef ESP_PLATFORM
// Optional embedded CA bundle symbols produced by board_build.embed_files.
// Weak linkage keeps non-bundle builds linkable.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_src_certs_x509_crt_bundle_bin_start") __attribute__((weak));
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_src_certs_x509_crt_bundle_bin_end") __attribute__((weak));

static bool parseTlsBundleTarget(const char* input, char* host_out, size_t host_out_size, uint16_t* port_out) {
  if (!input || !host_out || host_out_size == 0 || !port_out) return false;

  while (*input == ' ') input++;
  if (*input == '\0') return false;

  const char* start = input;
  const char* scheme = strstr(input, "://");
  if (scheme) start = scheme + 3;

  const char* end = start;
  while (*end && *end != '/' && *end != '?' && *end != '#') end++;
  if (end <= start) return false;

  uint16_t port = 443;
  const char* host_start = start;
  const char* host_end = end;

  if (*host_start == '[') {
    const char* close = (const char*)memchr(host_start, ']', host_end - host_start);
    if (!close) return false;
    if ((close + 1) < host_end && *(close + 1) == ':') {
      int p = atoi(close + 2);
      if (p <= 0 || p > 65535) return false;
      port = (uint16_t)p;
    }
    host_start++;
    host_end = close;
  } else {
    const char* colon = (const char*)memchr(host_start, ':', host_end - host_start);
    if (colon) {
      int p = atoi(colon + 1);
      if (p <= 0 || p > 65535) return false;
      port = (uint16_t)p;
      host_end = colon;
    }
  }

  size_t host_len = (size_t)(host_end - host_start);
  if (host_len == 0 || host_len >= host_out_size) return false;
  memcpy(host_out, host_start, host_len);
  host_out[host_len] = '\0';
  *port_out = port;
  return true;
}
#endif

#ifdef WITH_MQTT_BRIDGE
static int getMQTTPresetNameCount() {
  // Include virtual presets accepted by CLI parser.
  return MQTT_PRESET_COUNT + 2; // built-ins + custom + none
}

static bool isValidNtpHostname(const char* host) {
  if (!host || host[0] == '\0') return false;
  size_t len = strlen(host);
  if (len > 63) return false;
  if (host[0] == '.' || host[len - 1] == '.') return false;
  for (size_t i = 0; i < len; i++) {
    char c = host[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '-')) {
      return false;
    }
  }
  return true;
}

static const char* getMQTTPresetNameByIndex(int index) {
  if (index < MQTT_PRESET_COUNT) return MQTT_PRESETS[index].name;
  if (index == MQTT_PRESET_COUNT) return MQTT_PRESET_CUSTOM;
  if (index == MQTT_PRESET_COUNT + 1) return MQTT_PRESET_NONE;
  return nullptr;
}

static void formatMQTTPresetListReply(char* reply, size_t reply_size, int start) {
  if (!reply || reply_size == 0) return;
  reply[0] = '\0';

  const int total = getMQTTPresetNameCount();
  if (start < 0 || start >= total) {
    snprintf(reply, reply_size, "Error: preset list start must be 0-%d", total - 1);
    return;
  }

  // Keep room for continuation marker and null terminator.
  const size_t reserve_for_next = 18;
  size_t used = 0;
  bool wrote_any = false;

  int index = start;
  while (index < total) {
    const char* name = getMQTTPresetNameByIndex(index);
    if (!name) break;
    size_t name_len = strlen(name);
    size_t room = reply_size - used;
    if (room <= reserve_for_next) break;
    size_t needed = name_len + (wrote_any ? 1 : 0); // comma separator
    if (needed >= room - reserve_for_next) break;
    if (wrote_any) {
      reply[used++] = ',';
    }
    memcpy(reply + used, name, name_len);
    used += name_len;
    reply[used] = '\0';
    wrote_any = true;
    index++;
  }

  if (!wrote_any) {
    strcpy(reply, "Error: list page too small");
    return;
  }

  if (index < total) {
    snprintf(reply + used, reply_size - used, "... next:%d", index);
  }
}
#endif

bool CommonCLI::handleObserverSetCmd(uint32_t sender_timestamp, const char* config, char* reply) {
#ifdef WITH_MQTT_BRIDGE
  bool handled = true;
  if (memcmp(config, "snmp.community ", 15) == 0) {
    StrHelper::strncpy(_mqtt_prefs.snmp_community, &config[15], sizeof(_mqtt_prefs.snmp_community));
    savePrefs();
    strcpy(reply, "OK - restart to apply");
  } else if (memcmp(config, "snmp ", 5) == 0) {
    _mqtt_prefs.snmp_enabled = memcmp(&config[5], "on", 2) == 0;
    savePrefs();
    strcpy(reply, "OK - restart to apply");
  } else if (memcmp(config, "radio.watchdog ", 15) == 0) {
    const char* val = &config[15];
    bool all_digits = (*val != '\0');
    for (const char* sp = val; *sp; sp++) {
      if (*sp < '0' || *sp > '9') { all_digits = false; break; }
    }
    if (*val == '\0') {
      strcpy(reply, "Error: missing radio.watchdog minutes");
    } else if (!all_digits) {
      strcpy(reply, "Error: radio.watchdog must be an integer 0-120");
    } else {
      int mins = atoi(val);
      if (mins > 120) {
        strcpy(reply, "Error: radio.watchdog must be 0-120 minutes");
      } else {
        _mqtt_prefs.radio_watchdog_minutes = (uint8_t)mins;
        savePrefs();
        if (mins == 0) {
          strcpy(reply, "OK - radio watchdog disabled");
        } else {
          sprintf(reply, "OK - radio watchdog %d min", mins);
        }
      }
    }
#ifdef WITH_MQTT_BRIDGE
  } else if (strcmp(config, "mqtt.origin") == 0) {
    _mqtt_prefs.mqtt_origin[0] = '\0';
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.origin ", 12) == 0) {
    StrHelper::strncpy(_mqtt_prefs.mqtt_origin, &config[12], sizeof(_mqtt_prefs.mqtt_origin));
    StrHelper::stripSurroundingQuotes(_mqtt_prefs.mqtt_origin, sizeof(_mqtt_prefs.mqtt_origin));
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.iata ", 10) == 0) {
    StrHelper::strncpy(_mqtt_prefs.mqtt_iata, &config[10], sizeof(_mqtt_prefs.mqtt_iata));
    for (int i = 0; _mqtt_prefs.mqtt_iata[i]; i++) {
      _mqtt_prefs.mqtt_iata[i] = toupper(_mqtt_prefs.mqtt_iata[i]);
    }
    savePrefs();
    _callbacks->restartBridge();
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.status ", 12) == 0) {
    _mqtt_prefs.mqtt_status_enabled = memcmp(&config[12], "on", 2) == 0;
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.packets ", 13) == 0) {
    _mqtt_prefs.mqtt_packets_enabled = memcmp(&config[13], "on", 2) == 0;
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.raw ", 9) == 0) {
    _mqtt_prefs.mqtt_raw_enabled = memcmp(&config[9], "on", 2) == 0;
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.tx ", 8) == 0) {
    if (memcmp(&config[8], "advert", 6) == 0) {
      _mqtt_prefs.mqtt_tx_enabled = 2;
    } else {
      _mqtt_prefs.mqtt_tx_enabled = memcmp(&config[8], "on", 2) == 0 ? 1 : 0;
    }
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.rx ", 8) == 0) {
    _mqtt_prefs.mqtt_rx_enabled = memcmp(&config[8], "on", 2) == 0 ? 1 : 0;
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.interval ", 14) == 0) {
    uint32_t minutes = _atoi(&config[14]);
    if (minutes >= 1 && minutes <= 60) {
      _mqtt_prefs.mqtt_status_interval = minutes * 60000;
      savePrefs();
      _callbacks->restartBridge();
      sprintf(reply, "OK - interval set to %u minutes (%lu ms), bridge restarted", minutes, (unsigned long)_mqtt_prefs.mqtt_status_interval);
    } else {
      strcpy(reply, "Error: interval must be between 1-60 minutes");
    }
  } else if (memcmp(config, "mqtt.ntp ", 9) == 0) {
    const char* host = &config[9];
    while (*host == ' ') host++;
    bool clearing = strcmp(host, "none") == 0;
    if (!clearing && !isValidNtpHostname(host)) {
      strcpy(reply, "Error: invalid NTP hostname");
    } else {
      if (clearing) {
        _mqtt_prefs.mqtt_ntp_server[0] = '\0';
      } else {
        StrHelper::strncpy(_mqtt_prefs.mqtt_ntp_server, host, sizeof(_mqtt_prefs.mqtt_ntp_server));
      }
      savePrefs();
#ifdef ESP_PLATFORM
      // Validate by running an immediate sync. syncMqttNtp() marshals onto the MQTT
      // task (Core 0) so no NTP I/O happens on this (Core 1) CLI thread.
      if (WiFi.status() != WL_CONNECTED) {
        strcpy(reply, "OK - saved (WiFi not connected; NTP sync pending)");
      } else if (!_callbacks->isMqttBridgeRunning()) {
        strcpy(reply, "OK - saved (MQTT bridge not running)");
      } else if (_callbacks->syncMqttNtp()) {
        strcpy(reply, "OK - time synced");
      } else {
        strcpy(reply, "Error: NTP sync failed");
      }
#else
      strcpy(reply, "OK - saved");
#endif
    }
  } else if (memcmp(config, "wifi.ssid ", 10) == 0) {
    StrHelper::strncpy(_mqtt_prefs.wifi_ssid, &config[10], sizeof(_mqtt_prefs.wifi_ssid));
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "wifi.pwd ", 9) == 0) {
    StrHelper::strncpy(_mqtt_prefs.wifi_password, &config[9], sizeof(_mqtt_prefs.wifi_password));
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "wifi.powersave ", 15) == 0) {
    const char* value = &config[15];
    uint8_t ps_value;
    bool valid = false;
    if (memcmp(value, "min", 3) == 0 && (value[3] == 0 || value[3] == ' ')) {
      ps_value = 0;
      valid = true;
    } else if (memcmp(value, "none", 4) == 0 && (value[4] == 0 || value[4] == ' ')) {
      ps_value = 1;
      valid = true;
    } else if (memcmp(value, "max", 3) == 0 && (value[3] == 0 || value[3] == ' ')) {
      ps_value = 2;
      valid = true;
    }
    if (!valid) {
      strcpy(reply, "Error: must be none, min, or max");
    } else {
      _mqtt_prefs.wifi_power_save = ps_value;
      savePrefs();
#ifdef ESP_PLATFORM
      if (WiFi.status() == WL_CONNECTED) {
        wifi_ps_type_t ps_mode = (ps_value == 1) ? WIFI_PS_NONE :
                                (ps_value == 2) ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM;
        esp_err_t ps_result = esp_wifi_set_ps(ps_mode);
        if (ps_result == ESP_OK) {
          const char* ps_name = (ps_value == 1) ? "none" : (ps_value == 2) ? "max" : "min";
          sprintf(reply, "OK - power save set to %s", ps_name);
        } else {
          sprintf(reply, "OK - saved, but failed to apply: %d", ps_result);
        }
      } else {
        const char* ps_name = (ps_value == 1) ? "none" : (ps_value == 2) ? "max" : "min";
        sprintf(reply, "OK - saved as %s (will apply on next WiFi connection)", ps_name);
      }
#else
      const char* ps_name = (ps_value == 1) ? "none" : (ps_value == 2) ? "max" : "min";
      sprintf(reply, "OK - saved as %s", ps_name);
#endif
    }
  } else if (memcmp(config, "timezone ", 9) == 0) {
    StrHelper::strncpy(_mqtt_prefs.timezone_string, &config[9], sizeof(_mqtt_prefs.timezone_string));
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "timezone.offset ", 16) == 0) {
    int8_t offset = _atoi(&config[16]);
    if (offset >= -12 && offset <= 14) {
      _mqtt_prefs.timezone_offset = offset;
      savePrefs();
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Error: timezone offset must be between -12 and +14");
    }
  } else if (config[0] == 'm' && config[1] == 'q' && config[2] == 't' && config[3] == 't' &&
             config[4] >= '1' && config[4] <= ('0' + MAX_MQTT_SLOTS) && config[5] == '.') {
    // Slot-based commands: set mqtt1.preset <name>, set mqtt1.server <host>, etc.
    int slot = config[4] - '1'; // 0-5
    const char* subcmd = &config[6];
    if (memcmp(subcmd, "preset ", 7) == 0) {
      const char* preset_name = &subcmd[7];
      // Validate preset name
      if (findMQTTPreset(preset_name) != nullptr ||
          strcmp(preset_name, MQTT_PRESET_CUSTOM) == 0 ||
          strcmp(preset_name, MQTT_PRESET_NONE) == 0) {
        // Reject duplicate presets (except "none" and "custom")
        int dup_slot = -1;
        if (findMQTTPreset(preset_name) != nullptr) {
          for (int s = 0; s < MAX_MQTT_SLOTS; s++) {
            if (s != slot && strcmp(_mqtt_prefs.mqtt_slot_preset[s], preset_name) == 0) {
              dup_slot = s;
              break;
            }
          }
        }
        if (dup_slot >= 0) {
          sprintf(reply, "Error: preset '%s' is already assigned to slot %d", preset_name, dup_slot + 1);
        } else {
          StrHelper::strncpy(_mqtt_prefs.mqtt_slot_preset[slot], preset_name, sizeof(_mqtt_prefs.mqtt_slot_preset[slot]));
          savePrefs();
          _callbacks->restartBridgeSlot(slot);
          // Check if the slot has everything it needs to connect
          const MQTTPresetDef* p = findMQTTPreset(preset_name);
          if (p && p->topic_style == MQTT_TOPIC_MESHRANK && _mqtt_prefs.mqtt_slot_token[slot][0] == '\0') {
            sprintf(reply, "OK - slot %d preset: %s (run 'set mqtt%d.token <your_token>' to connect)", slot + 1, preset_name, slot + 1);
          } else if (p && p->topic_style == MQTT_TOPIC_MESHCORE &&
                     (strlen(_mqtt_prefs.mqtt_iata) == 0 || strcmp(_mqtt_prefs.mqtt_iata, "XXX") == 0)) {
            sprintf(reply, "OK - slot %d preset: %s (run 'set mqtt.iata <airport_code>' to publish)", slot + 1, preset_name);
          } else if (p && mqttPresetNeedsSlotCredentials(p) &&
                     (_mqtt_prefs.mqtt_slot_username[slot][0] == '\0' ||
                      _mqtt_prefs.mqtt_slot_password[slot][0] == '\0')) {
            sprintf(reply,
                    "OK - slot %d preset: %s (run 'set mqtt%d.username <user>' and 'set mqtt%d.password <pass>' to connect)",
                    slot + 1, preset_name, slot + 1, slot + 1);
          } else {
            sprintf(reply, "OK - slot %d preset: %s", slot + 1, preset_name);
          }
        }
      } else {
        strcpy(reply, "Error: unknown preset. Use 'get mqtt.presets'");
      }
    } else if (memcmp(subcmd, "server ", 7) == 0) {
      StrHelper::strncpy(_mqtt_prefs.mqtt_slot_host[slot], &subcmd[7], sizeof(_mqtt_prefs.mqtt_slot_host[slot]));
      savePrefs();
      strcpy(reply, "OK");
    } else if (memcmp(subcmd, "port ", 5) == 0) {
      int port = atoi(&subcmd[5]);
      if (port > 0 && port <= 65535) {
        _mqtt_prefs.mqtt_slot_port[slot] = port;
        savePrefs();
        strcpy(reply, "OK");
      } else {
        strcpy(reply, "Error: port must be between 1 and 65535");
      }
    } else if (memcmp(subcmd, "username ", 9) == 0) {
      StrHelper::strncpy(_mqtt_prefs.mqtt_slot_username[slot], &subcmd[9], sizeof(_mqtt_prefs.mqtt_slot_username[slot]));
      savePrefs();
      _callbacks->restartBridgeSlot(slot);
      strcpy(reply, "OK");
    } else if (memcmp(subcmd, "password ", 9) == 0) {
      StrHelper::strncpy(_mqtt_prefs.mqtt_slot_password[slot], &subcmd[9], sizeof(_mqtt_prefs.mqtt_slot_password[slot]));
      savePrefs();
      _callbacks->restartBridgeSlot(slot);
      strcpy(reply, "OK");
    } else if (memcmp(subcmd, "token ", 6) == 0) {
      StrHelper::strncpy(_mqtt_prefs.mqtt_slot_token[slot], &subcmd[6], sizeof(_mqtt_prefs.mqtt_slot_token[slot]));
      savePrefs();
      _callbacks->restartBridgeSlot(slot);
      sprintf(reply, "OK - slot %d token set", slot + 1);
    } else if (memcmp(subcmd, "topic ", 6) == 0) {
      if (strcmp(_mqtt_prefs.mqtt_slot_preset[slot], "custom") != 0) {
        sprintf(reply, "Error: topic template only applies to custom preset slots");
      } else {
        StrHelper::strncpy(_mqtt_prefs.mqtt_slot_topic[slot], &subcmd[6], sizeof(_mqtt_prefs.mqtt_slot_topic[slot]));
        savePrefs();
        _callbacks->restartBridgeSlot(slot);
        sprintf(reply, "OK - slot %d topic: %s", slot + 1, _mqtt_prefs.mqtt_slot_topic[slot]);
      }
    } else if (memcmp(subcmd, "audience ", 9) == 0) {
      StrHelper::strncpy(_mqtt_prefs.mqtt_slot_audience[slot], &subcmd[9], sizeof(_mqtt_prefs.mqtt_slot_audience[slot]));
      savePrefs();
      _callbacks->restartBridgeSlot(slot);
      if (_mqtt_prefs.mqtt_slot_audience[slot][0] != '\0') {
        sprintf(reply, "OK - slot %d JWT audience: %s", slot + 1, _mqtt_prefs.mqtt_slot_audience[slot]);
      } else {
        sprintf(reply, "OK - slot %d JWT audience cleared (using username/password auth)", slot + 1);
      }
    } else if (memcmp(subcmd, "audience", 8) == 0 && subcmd[8] == '\0') {
      // "set mqttN.audience" with no value — clear the audience
      _mqtt_prefs.mqtt_slot_audience[slot][0] = '\0';
      savePrefs();
      _callbacks->restartBridgeSlot(slot);
      sprintf(reply, "OK - slot %d JWT audience cleared (using username/password auth)", slot + 1);
    } else {
      sprintf(reply, "unknown config: %s", config);
    }
  } else if (memcmp(config, "mqtt.analyzer.us ", 17) == 0) {
    const int slot = 0;
    if (memcmp(&config[17], "on", 2) == 0) {
      StrHelper::strncpy(_mqtt_prefs.mqtt_slot_preset[slot], "analyzer-us", sizeof(_mqtt_prefs.mqtt_slot_preset[slot]));
    } else {
      StrHelper::strncpy(_mqtt_prefs.mqtt_slot_preset[slot], MQTT_PRESET_NONE, sizeof(_mqtt_prefs.mqtt_slot_preset[slot]));
    }
    savePrefs();
    _callbacks->restartBridgeSlot(slot);
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.analyzer.eu ", 17) == 0) {
    const int slot = 1;
    if (memcmp(&config[17], "on", 2) == 0) {
      StrHelper::strncpy(_mqtt_prefs.mqtt_slot_preset[slot], "analyzer-eu", sizeof(_mqtt_prefs.mqtt_slot_preset[slot]));
    } else {
      StrHelper::strncpy(_mqtt_prefs.mqtt_slot_preset[slot], MQTT_PRESET_NONE, sizeof(_mqtt_prefs.mqtt_slot_preset[slot]));
    }
    savePrefs();
    _callbacks->restartBridgeSlot(slot);
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.owner ", 11) == 0) {
    const char* owner_key = &config[11];
    int key_len = strlen(owner_key);
    if (key_len == 64) {
      bool valid_key = true;
      for (int i = 0; i < key_len; i++) {
        if (!((owner_key[i] >= '0' && owner_key[i] <= '9') ||
              (owner_key[i] >= 'A' && owner_key[i] <= 'F') ||
              (owner_key[i] >= 'a' && owner_key[i] <= 'f'))) {
          valid_key = false;
          break;
        }
      }
      if (valid_key) {
        StrHelper::strncpy(_mqtt_prefs.mqtt_owner_public_key, owner_key, sizeof(_mqtt_prefs.mqtt_owner_public_key));
        savePrefs();
        strcpy(reply, "OK");
      } else {
        strcpy(reply, "Error: invalid hex characters in public key");
      }
    } else {
      strcpy(reply, "Error: public key must be 64 hex characters (32 bytes)");
    }
  } else if (memcmp(config, "mqtt.email ", 11) == 0) {
    StrHelper::strncpy(_mqtt_prefs.mqtt_email, &config[11], sizeof(_mqtt_prefs.mqtt_email));
    savePrefs();
    strcpy(reply, "OK");
#endif
  } else if (memcmp(config, "alert ", 6) == 0) {
    // set alert on|off
    const char* val = &config[6];
    if (memcmp(val, "on", 2) == 0 && (val[2] == 0 || val[2] == ' ')) {
      _mqtt_prefs.alert_enabled = 1;
      savePrefs();
      _callbacks->onAlertConfigChanged();
      strcpy(reply, "OK - alerts on");
    } else if (memcmp(val, "off", 3) == 0 && (val[3] == 0 || val[3] == ' ')) {
      _mqtt_prefs.alert_enabled = 0;
      savePrefs();
      _callbacks->onAlertConfigChanged();
      strcpy(reply, "OK - alerts off");
    } else {
      strcpy(reply, "Error: usage set alert on|off");
    }
  } else if (memcmp(config, "alert.psk", 9) == 0 && (config[9] == 0 || config[9] == ' ')) {
    // `set alert.psk` with no argument clears the field (alerts then disabled
    // until a new psk/hashtag is configured).
    const char* val = (config[9] == ' ') ? &config[10] : "";
    while (*val == ' ') val++;
    size_t len = strlen(val);
    if (len == 0) {
      _mqtt_prefs.alert_psk_hex[0] = '\0';
      _mqtt_prefs.alert_hashtag[0] = '\0';
      savePrefs();
      _callbacks->onAlertConfigChanged();
      strcpy(reply, "OK - alert.psk cleared (alerts disabled until configured)");
    } else if (val[0] == '#') {
      strcpy(reply, "Error: use 'set alert.hashtag' for hashtag channels");
    } else if (len != 32) {
      // 16-byte channel secret = 32 hex chars. This is what the mobile app's
      // "Share Channel" emits, what `set alert.hashtag` derives, and what the
      // BANNED_ALERT_CHANNELS table holds. 32-byte channels aren't used
      // anywhere in MeshCore practice.
      strcpy(reply, "Error: PSK must be 32 hex chars (16-byte channel secret)");
    } else {
      // Validate all-hex, then normalize via fromHex/toHex so the stored
      // form is always lowercase regardless of input case.
      uint8_t raw[16];
      bool all_hex = true;
      for (size_t i = 0; i < len; i++) {
        if (!mesh::Utils::isHexChar(val[i])) { all_hex = false; break; }
      }
      if (!all_hex || !mesh::Utils::fromHex(raw, 16, val)) {
        strcpy(reply, "Error: PSK must be 32 hex chars (16-byte channel secret)");
      } else {
        char normalized[33];
        mesh::Utils::toHex(normalized, raw, 16);
        if (const char* banned = alertReporterBannedChannelMatchHex(normalized)) {
          // Refuse any key on the banned channel list (Public PSK, well-known
          // auto-responder hashtags like #test/#bot, etc.). Fault alerts on
          // those channels would spam every node in the area.
          sprintf(reply, "Error: refusing banned channel '%s'; pick a private key or hashtag", banned);
        } else {
          StrHelper::strncpy(_mqtt_prefs.alert_psk_hex, normalized, sizeof(_mqtt_prefs.alert_psk_hex));
          // The new PSK is operator-supplied, so any previously-derived
          // hashtag name is no longer accurate provenance — drop it.
          _mqtt_prefs.alert_hashtag[0] = '\0';
          savePrefs();
          _callbacks->onAlertConfigChanged();
          strcpy(reply, "OK - alert.psk updated");
        }
      }
    }
  } else if (memcmp(config, "alert.hashtag", 13) == 0 && (config[13] == 0 || config[13] == ' ')) {
    const char* val = (config[13] == ' ') ? &config[14] : "";
    while (*val == ' ') val++;
    size_t in_len = strlen(val);
    if (in_len == 0) {
      _mqtt_prefs.alert_psk_hex[0] = '\0';
      _mqtt_prefs.alert_hashtag[0] = '\0';
      savePrefs();
      _callbacks->onAlertConfigChanged();
      strcpy(reply, "OK - alert.hashtag cleared (alerts disabled until configured)");
    } else {
      // Canonical stored form is "#name" because the leading '#' is part of
      // the sha256 input (matching the companion-app hashtag-channel
      // derivation in docs/companion_protocol.md). Accept the user typing
      // either "alerts" or "#alerts".
      char hashtag[sizeof(_mqtt_prefs.alert_hashtag)];
      size_t need = (val[0] == '#') ? in_len : in_len + 1;
      if (need >= sizeof(hashtag)) {
        strcpy(reply, "Error: hashtag too long");
      } else {
        if (val[0] == '#') {
          StrHelper::strncpy(hashtag, val, sizeof(hashtag));
        } else {
          hashtag[0] = '#';
          StrHelper::strncpy(&hashtag[1], val, sizeof(hashtag) - 1);
        }

        // Derive the channel key once: first 16 bytes of sha256("#name"),
        // store hex-encoded in alert_psk_hex. We don't re-derive on every
        // send — operators can later override with `set alert.psk` without
        // leaving stale hashtag text behind.
        uint8_t digest[32];
        mesh::Utils::sha256(digest, sizeof(digest),
                            (const uint8_t*)hashtag, (int)strlen(hashtag));
        if (const char* banned = alertReporterBannedChannelMatch(digest)) {
          // Hashtag derives to a banned key (e.g. `set alert.hashtag test`
          // hits the #test entry). Refuse before clobbering existing config.
          sprintf(reply, "Error: refusing banned channel '%s'", banned);
        } else {
          char hex[33];
          mesh::Utils::toHex(hex, digest, 16);
          StrHelper::strncpy(_mqtt_prefs.alert_hashtag, hashtag, sizeof(_mqtt_prefs.alert_hashtag));
          StrHelper::strncpy(_mqtt_prefs.alert_psk_hex, hex, sizeof(_mqtt_prefs.alert_psk_hex));
          savePrefs();
          _callbacks->onAlertConfigChanged();
          sprintf(reply, "OK - alert.hashtag: %s", _mqtt_prefs.alert_hashtag);
        }
      }
    }
  } else if (memcmp(config, "alert.region", 12) == 0 && (config[12] == 0 || config[12] == ' ')) {
    // `set alert.region <name>` overrides the repeater's default_scope for
    // alert sends only. `set alert.region` (no arg) clears it. The name is
    // looked up lazily via RegionMap at send time; we deliberately don't
    // mutate the region map here, so naming an unknown region is allowed
    // but will silently fall back to default_scope until the operator runs
    // `region put` for it.
    const char* val = (config[12] == ' ') ? &config[13] : "";
    while (*val == ' ') val++;
    size_t len = strlen(val);
    if (len == 0) {
      _mqtt_prefs.alert_region[0] = '\0';
      savePrefs();
      _callbacks->onAlertConfigChanged();
      strcpy(reply, "OK - alert.region cleared (using default scope)");
    } else if (len >= sizeof(_mqtt_prefs.alert_region)) {
      strcpy(reply, "Error: alert.region too long");
    } else {
      StrHelper::strncpy(_mqtt_prefs.alert_region, val, sizeof(_mqtt_prefs.alert_region));
      StrHelper::stripSurroundingQuotes(_mqtt_prefs.alert_region, sizeof(_mqtt_prefs.alert_region));
      savePrefs();
      _callbacks->onAlertConfigChanged();
      sprintf(reply, "OK - alert.region: %s", _mqtt_prefs.alert_region);
    }
  } else if (memcmp(config, "alert.wifi ", 11) == 0) {
    int mins = (int)_atoi(&config[11]);
    if (mins < 0 || mins > 1440) {
      strcpy(reply, "Error: alert.wifi must be 0-1440 minutes (0=off)");
    } else {
      _mqtt_prefs.alert_wifi_minutes = (uint16_t)mins;
      savePrefs();
      sprintf(reply, "OK - alert.wifi %d min%s", mins, mins == 0 ? " (disabled)" : "");
    }
  } else if (memcmp(config, "alert.mqtt ", 11) == 0) {
    int mins = (int)_atoi(&config[11]);
    if (mins < 0 || mins > 10080) {
      strcpy(reply, "Error: alert.mqtt must be 0-10080 minutes (0=off)");
    } else {
      _mqtt_prefs.alert_mqtt_minutes = (uint16_t)mins;
      savePrefs();
      sprintf(reply, "OK - alert.mqtt %d min%s", mins, mins == 0 ? " (disabled)" : "");
    }
  } else if (memcmp(config, "alert.interval ", 15) == 0) {
    int mins = (int)_atoi(&config[15]);
    // Floor at 60 min: faster re-fires would let a flapping link spam the
    // mesh with a fresh GRP_TXT flood every minute — terrible for airtime.
    if (mins < 60 || mins > 10080) {
      strcpy(reply, "Error: alert.interval must be 60-10080 minutes");
    } else {
      _mqtt_prefs.alert_min_interval_min = (uint16_t)mins;
      savePrefs();
      sprintf(reply, "OK - alert.interval %d min", mins);
    }
  } else {
    handled = false;
  }
  return handled;
#else
  (void)sender_timestamp; (void)config; (void)reply;
  return false;
#endif
}

bool CommonCLI::handleObserverGetCmd(uint32_t sender_timestamp, const char* config, char* reply) {
#ifdef WITH_MQTT_BRIDGE
  bool handled = true;
  if (memcmp(config, "snmp.community", 14) == 0) {
    sprintf(reply, "> %s", _mqtt_prefs.snmp_community);
  } else if (memcmp(config, "snmp", 4) == 0 && (config[4] == '\0' || config[4] == '\n' || config[4] == '\r')) {
    strcpy(reply, _mqtt_prefs.snmp_enabled ? "> on" : "> off");
  } else if (memcmp(config, "radio.watchdog", 14) == 0) {
    sprintf(reply, "> %d", (uint32_t)_mqtt_prefs.radio_watchdog_minutes);
#ifdef WITH_MQTT_BRIDGE
  } else if (memcmp(config, "mqtt.origin", 11) == 0) {
    char effective_origin[32];
    MQTTBridge::getEffectiveMqttOrigin(_prefs, &_mqtt_prefs, effective_origin, sizeof(effective_origin));
    sprintf(reply, "> %s", effective_origin);
  } else if (memcmp(config, "mqtt.iata", 9) == 0) {
    sprintf(reply, "> %s", _mqtt_prefs.mqtt_iata);
  } else if (memcmp(config, "mqtt.presets", 12) == 0 && (config[12] == '\0' || config[12] == ' ')) {
    int start = 0;
    if (config[12] == ' ') {
      const char* start_arg = &config[13];
      if (*start_arg == '\0') {
        strcpy(reply, "Error: usage get mqtt.presets [start]");
        return true;
      }
      for (const char* sp = start_arg; *sp; sp++) {
        if (*sp < '0' || *sp > '9') {
          strcpy(reply, "Error: usage get mqtt.presets [start]");
          return true;
        }
      }
      start = (int)_atoi(start_arg);
    }
    formatMQTTPresetListReply(reply, 160, start);
  } else if (memcmp(config, "mqtt.stats", 10) == 0) {
    MQTTBridge::formatMqttStatsReply(reply, 160);
  } else if (memcmp(config, "mqtt.status", 11) == 0) {
    MQTTBridge::formatMqttStatusReply(reply, 160, &_mqtt_prefs);
  } else if (memcmp(config, "mqtt.packets", 12) == 0) {
    sprintf(reply, "> %s", _mqtt_prefs.mqtt_packets_enabled ? "on" : "off");
  } else if (memcmp(config, "mqtt.raw", 8) == 0) {
    sprintf(reply, "> %s", _mqtt_prefs.mqtt_raw_enabled ? "on" : "off");
  } else if (memcmp(config, "mqtt.tx", 7) == 0) {
    const char* tx_str = _mqtt_prefs.mqtt_tx_enabled == 2 ? "advert" : (_mqtt_prefs.mqtt_tx_enabled ? "on" : "off");
    sprintf(reply, "> %s", tx_str);
  } else if (memcmp(config, "mqtt.rx", 7) == 0) {
    sprintf(reply, "> %s", _mqtt_prefs.mqtt_rx_enabled ? "on" : "off");
  } else if (memcmp(config, "mqtt.interval", 13) == 0) {
    uint32_t minutes = (_mqtt_prefs.mqtt_status_interval + 29999) / 60000;
    sprintf(reply, "> %u minutes (%lu ms)", minutes, (unsigned long)_mqtt_prefs.mqtt_status_interval);
  } else if (memcmp(config, "mqtt.ntp.diag", 13) == 0 && (config[13] == '\0' || config[13] == ' ')) {
#ifdef ESP_PLATFORM
    // Connectivity probe across all configured NTP servers; never updates the clock.
    // Serial console (sender_timestamp == 0) gets a detailed table; LoRa gets a compact list.
    if (WiFi.status() != WL_CONNECTED) {
      strcpy(reply, "Error: WiFi not connected");
    } else if (!_callbacks->isMqttBridgeRunning()) {
      strcpy(reply, "Error: MQTT bridge not running");
    } else if (!_callbacks->runMqttNtpDiag(reply, 160, sender_timestamp == 0)) {
      strcpy(reply, "Error: NTP diag unavailable");
    }
#else
    strcpy(reply, "Error: not supported on this platform");
#endif
  } else if (memcmp(config, "mqtt.ntp", 8) == 0 && (config[8] == '\0' || config[8] == ' ')) {
    sprintf(reply, "> %s", MQTTBridge::effectiveNtpPrimary(&_mqtt_prefs));
  } else if (config[0] == 'm' && config[1] == 'q' && config[2] == 't' && config[3] == 't' &&
             config[4] >= '1' && config[4] <= ('0' + MAX_MQTT_SLOTS) && config[5] == '.') {
    // Slot-based commands: get mqtt1.preset, get mqtt1.server, etc.
    int slot = config[4] - '1'; // 0-5
    const char* subcmd = &config[6];
    if (memcmp(subcmd, "preset", 6) == 0) {
      sprintf(reply, "> %s", _mqtt_prefs.mqtt_slot_preset[slot]);
    } else if (memcmp(subcmd, "server", 6) == 0) {
      sprintf(reply, "> %s", _mqtt_prefs.mqtt_slot_host[slot]);
    } else if (memcmp(subcmd, "port", 4) == 0) {
      sprintf(reply, "> %d", _mqtt_prefs.mqtt_slot_port[slot]);
    } else if (memcmp(subcmd, "username", 8) == 0) {
      sprintf(reply, "> %s", _mqtt_prefs.mqtt_slot_username[slot]);
    } else if (memcmp(subcmd, "password", 8) == 0) {
      sprintf(reply, "> %s", _mqtt_prefs.mqtt_slot_password[slot]);
    } else if (memcmp(subcmd, "token", 5) == 0) {
      if (_mqtt_prefs.mqtt_slot_token[slot][0] != '\0') {
        sprintf(reply, "> %s", _mqtt_prefs.mqtt_slot_token[slot]);
      } else {
        strcpy(reply, "> (not set)");
      }
    } else if (memcmp(subcmd, "topic", 5) == 0) {
      if (_mqtt_prefs.mqtt_slot_topic[slot][0] != '\0') {
        sprintf(reply, "> %s", _mqtt_prefs.mqtt_slot_topic[slot]);
      } else {
        strcpy(reply, "> (default: meshcore/{iata}/{device}/{type})");
      }
    } else if (memcmp(subcmd, "audience", 8) == 0) {
      if (_mqtt_prefs.mqtt_slot_audience[slot][0] != '\0') {
        sprintf(reply, "> %s", _mqtt_prefs.mqtt_slot_audience[slot]);
      } else {
        strcpy(reply, "> (not set — custom slots use username/password auth)");
      }
    } else if (memcmp(subcmd, "diag", 4) == 0) {
      MQTTBridge::formatSlotDiagReply(reply, 160, slot);
    } else {
      sprintf(reply, "??: %s", config);
    }
  } else if (memcmp(config, "wifi.ssid", 9) == 0) {
    sprintf(reply, "> %s", _mqtt_prefs.wifi_ssid);
  } else if (memcmp(config, "wifi.pwd", 8) == 0) {
    sprintf(reply, "> %s", _mqtt_prefs.wifi_password);
  } else if (memcmp(config, "wifi.status", 11) == 0) {
    wl_status_t status = WiFi.status();
    const char* status_str;
    switch (status) {
      case WL_CONNECTED: status_str = "connected"; break;
      case WL_NO_SSID_AVAIL: status_str = "no_ssid"; break;
      case WL_CONNECT_FAILED: status_str = "connect_failed"; break;
      case WL_CONNECTION_LOST: status_str = "connection_lost"; break;
      case WL_DISCONNECTED: status_str = "disconnected"; break;
      case 255: status_str = "not_started"; break;
      default: status_str = "unknown"; break;
    }
    if (status == WL_CONNECTED) {
      sprintf(reply, "> %s, IP: %s, RSSI: %d dBm", status_str, WiFi.localIP().toString().c_str(), WiFi.RSSI());
#ifdef WITH_MQTT_BRIDGE
      unsigned long connect_at = MQTTBridge::getWifiConnectedAtMillis();
      if (connect_at != 0) {
        unsigned long uptime_ms = millis() - connect_at;
        unsigned long uptime_sec = uptime_ms / 1000;
        unsigned long d = uptime_sec / 86400;
        unsigned long h = (uptime_sec % 86400) / 3600;
        unsigned long m = (uptime_sec % 3600) / 60;
        unsigned long s = uptime_sec % 60;
        // reply points at the caller's char[160] command buffer (see main.cpp);
        // compute the actual remaining space instead of assuming 128.
        const size_t kReplyBufSize = 160;
        size_t len = strlen(reply);
        const size_t reply_remaining = (len < kReplyBufSize) ? (kReplyBufSize - len) : 0;
        if (d > 0) {
          snprintf(reply + len, reply_remaining, ", uptime: %lud %luh %lum %lus", d, h, m, s);
        } else if (h > 0) {
          snprintf(reply + len, reply_remaining, ", uptime: %luh %lum %lus", h, m, s);
        } else if (m > 0) {
          snprintf(reply + len, reply_remaining, ", uptime: %lum %lus", m, s);
        } else {
          snprintf(reply + len, reply_remaining, ", uptime: %lus", s);
        }
      }
#endif
    } else {
#ifdef WITH_MQTT_BRIDGE
      uint8_t reason = MQTTBridge::getLastWifiDisconnectReason();
      if (reason != 0) {
        const char* desc = MQTTBridge::wifiReasonStr(reason);
        if (desc) {
          sprintf(reply, "> %s: %s (reason: %d)", status_str, desc, reason);
        } else {
          sprintf(reply, "> %s: reason %d", status_str, reason);
        }
      } else {
        sprintf(reply, "> %s (code: %d)", status_str, status);
      }
#else
      sprintf(reply, "> %s (code: %d)", status_str, status);
#endif
    }
  } else if (memcmp(config, "wifi.powersave", 14) == 0) {
    uint8_t ps = _mqtt_prefs.wifi_power_save;
    const char* ps_name = (ps == 1) ? "none" : (ps == 2) ? "max" : "min";
    sprintf(reply, "> %s", ps_name);
  } else if (memcmp(config, "timezone", 8) == 0) {
    sprintf(reply, "> %s", _mqtt_prefs.timezone_string);
  } else if (memcmp(config, "timezone.offset", 15) == 0) {
    sprintf(reply, "> %d", _mqtt_prefs.timezone_offset);
  } else if (memcmp(config, "mqtt.analyzer.us", 17) == 0) {
    sprintf(reply, "> %s", strcmp(_mqtt_prefs.mqtt_slot_preset[0], "analyzer-us") == 0 ? "on" : "off");
  } else if (memcmp(config, "mqtt.analyzer.eu", 17) == 0) {
    sprintf(reply, "> %s", strcmp(_mqtt_prefs.mqtt_slot_preset[1], "analyzer-eu") == 0 ? "on" : "off");
  } else if (sender_timestamp == 0 && memcmp(config, "mqtt.owner", 10) == 0) {
    if (_mqtt_prefs.mqtt_owner_public_key[0] != '\0') {
      sprintf(reply, "> %s", _mqtt_prefs.mqtt_owner_public_key);
    } else {
      strcpy(reply, "> (not set)");
    }
  } else if (sender_timestamp == 0 && memcmp(config, "mqtt.email", 10) == 0) {
    if (_mqtt_prefs.mqtt_email[0] != '\0') {
      sprintf(reply, "> %s", _mqtt_prefs.mqtt_email);
    } else {
      strcpy(reply, "> (not set)");
    }
  } else if (memcmp(config, "mqtt.config.valid", 17) == 0) {
    bool valid = MQTTBridge::isConfigValid(&_mqtt_prefs);
    sprintf(reply, "> %s", valid ? "valid" : "invalid");
#endif
  } else if (memcmp(config, "alert.hashtag", 13) == 0) {
    sprintf(reply, "> %s", _mqtt_prefs.alert_hashtag[0] ? _mqtt_prefs.alert_hashtag : "(unset)");
  } else if (sender_timestamp == 0 && memcmp(config, "alert.psk", 9) == 0) {  // from serial command line only
    sprintf(reply, "> %s", _mqtt_prefs.alert_psk_hex[0] ? _mqtt_prefs.alert_psk_hex : "(unset)");
  } else if (memcmp(config, "alert.region", 12) == 0) {
    sprintf(reply, "> %s", _mqtt_prefs.alert_region[0] ? _mqtt_prefs.alert_region : "(unset, using default scope)");
  } else if (memcmp(config, "alert.wifi", 10) == 0) {
    sprintf(reply, "> %u min%s", (unsigned)_mqtt_prefs.alert_wifi_minutes,
            _mqtt_prefs.alert_wifi_minutes == 0 ? " (disabled)" : "");
  } else if (memcmp(config, "alert.mqtt", 10) == 0) {
    sprintf(reply, "> %u min%s", (unsigned)_mqtt_prefs.alert_mqtt_minutes,
            _mqtt_prefs.alert_mqtt_minutes == 0 ? " (disabled)" : "");
  } else if (memcmp(config, "alert.interval", 14) == 0) {
    sprintf(reply, "> %u min", (unsigned)_mqtt_prefs.alert_min_interval_min);
  } else if (memcmp(config, "alert", 5) == 0 && (config[5] == 0 || config[5] == '\n' || config[5] == '\r')) {
    sprintf(reply, "> %s", _mqtt_prefs.alert_enabled ? "on" : "off");
  } else {
    handled = false;
  }
  return handled;
#else
  (void)sender_timestamp; (void)config; (void)reply;
  return false;
#endif
}

bool CommonCLI::handleObserverCommand(uint32_t sender_timestamp, char* command, char* reply) {
#ifdef WITH_MQTT_BRIDGE
  if (memcmp(command, "tls.bundletest ", 15) == 0) {
#ifdef ESP_PLATFORM
    if (WiFi.status() != WL_CONNECTED) {
      strcpy(reply, "ERR: WiFi not connected");
    } else {
      size_t bundle_len = 0;
      if (rootca_crt_bundle_start != nullptr &&
          rootca_crt_bundle_end != nullptr &&
          rootca_crt_bundle_end > rootca_crt_bundle_start) {
        bundle_len = static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start);
      }
      if (bundle_len == 0) {
        strcpy(reply, "ERR: no embedded cert bundle");
      } else {
        char host[96];
        uint16_t port = 443;
        if (!parseTlsBundleTarget(command + 15, host, sizeof(host), &port)) {
          strcpy(reply, "ERR: usage tls.bundletest <host[:port]|url>");
        } else {
          WiFiClientSecure client;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
          client.setCACertBundle(rootca_crt_bundle_start, bundle_len);
#else
          client.setCACertBundle(rootca_crt_bundle_start);
#endif
          client.setTimeout(8000);
          bool ok = client.connect(host, port);
          if (ok) {
            client.stop();
            snprintf(reply, 160, "OK: TLS bundle verified %s:%u", host, (unsigned)port);
          } else {
            snprintf(reply, 160, "ERR: TLS bundle failed %s:%u", host, (unsigned)port);
          }
        }
      }
    }
#else
    strcpy(reply, "ERR: unsupported on this platform");
#endif
    return true;
  } else if (memcmp(command, "ota check", 9) == 0 || memcmp(command, "ota update", 10) == 0) {
    // Observer pull-OTA: fetch this variant's build from the baked-in manifest
    // and flash it. Intentionally a separate command from "start ota" (the
    // manual ElegantOTA web-upload SoftAP) so a remote/online update is never
    // triggered by someone expecting to hand-upload a binary.
    //   ota check  -> report available build, do not flash
    //   ota update -> download and flash, then reboot
#if defined(WITH_MQTT_BRIDGE) && defined(OTA_MANIFEST_BASE)
    if (WiFi.status() != WL_CONNECTED) {
      strcpy(reply, "ERR: WiFi not connected");
    } else if (memcmp(command, "ota check", 9) == 0) {
      // Check is synchronous so its result lands in this reply, and runs with the
      // MQTT bridge UP: the slim per-variant manifest is tiny, so the fetch only
      // costs a single TLS handshake (no large JSON doc) — which fits alongside
      // the live MQTT sessions even on no-PSRAM boards. No bridge bounce needed.
      _board->otaFromManifest(_callbacks->getFirmwareVer(), true, reply);
    } else {
      // `ota update`: cheap pre-check first (plain HTTP, bridge stays up). Only
      // schedule the real update — which tears the bridge down, flashes, and
      // reboots — when an applicable build actually exists. otaFromManifest(dry)
      // returns true iff so; otherwise it leaves the explanation (up to date /
      // cable flash / error) in reply, which we send without disturbing the
      // bridge or misleading the user with a "Beginning update..." that no-ops.
      if (_board->otaFromManifest(_callbacks->getFirmwareVer(), true, reply)) {
        // reply now holds "update available: <cur> -> <target> (N behind|new base)",
        // where <target> is "vX.Y.Z.B (hash)". Pull <target> out for a friendlier
        // start message. The "-> " ... trailing " (" framing is produced by
        // ESP32Board::otaFromManifestImpl; <target> ends at the LAST " (" (the
        // "(N behind)"/"(new base)" suffix), since the version's own hash-paren
        // comes before it.
        char target[48] = {0};
        const char* arrow = strstr(reply, "-> ");
        if (arrow) {
          arrow += 3;
          const char* suffix = nullptr;
          for (const char* p = arrow; (p = strstr(p, " (")) != nullptr; p++) suffix = p;
          size_t len = suffix ? (size_t)(suffix - arrow) : strlen(arrow);
          if (len >= sizeof(target)) len = sizeof(target) - 1;
          memcpy(target, arrow, len);
          target[len] = 0;
        }
        // Update is DEFERRED so this ack goes out over the mesh before the flash
        // blocks the loop and reboots (the app loop runs it shortly).
        if (_callbacks->beginDeferredOtaUpdate()) {
          if (target[0]) {
            snprintf(reply, 160, "Updating to %s; reboots when done (~30s offline). Check 'ver' after.", target);
          } else {
            strcpy(reply, "Beginning update... (node will reboot if successful)");
          }
        } else {
          strcpy(reply, "ERR: online OTA not available");
        }
      }
    }
#else
    strcpy(reply, "ERR: online OTA not supported on this build");
#endif
    return true;
  } else if (memcmp(command, "alert test", 10) == 0 && (command[10] == 0 || command[10] == ' ')) {
    // Send a one-off test alert on the configured alert channel.
    const char* extra = command[10] == ' ' ? &command[11] : "";
    char text[120];
    if (*extra) {
      snprintf(text, sizeof(text), "[test] %s", extra);
    } else {
      strcpy(text, "[test] alert channel ok");
    }
    if (!_mqtt_prefs.alert_psk_hex[0]) {
      strcpy(reply, "Error: alert channel not configured (set alert.psk or set alert.hashtag)");
    } else {
      bool ok = _callbacks->sendAlertText(text);
      strcpy(reply, ok ? "OK - alert sent" : "Error: alert send failed (bad PSK or PUBLIC key refused?)");
    }
    return true;
  }
  return false;
#else
  (void)sender_timestamp; (void)command; (void)reply;
  return false;
#endif
}
