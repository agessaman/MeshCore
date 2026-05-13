#include <Arduino.h>
#include "CommonCLI.h"
#include "TxtDataHelpers.h"
#include "AdvertDataHelpers.h"
#include "AlertReporter.h"  // for alertReporterBannedChannelMatch()
#include <RTClib.h>
#include <Utils.h>

// Tiny base64 encoder used by `set alert.hashtag` to render a derived 16-byte
// key into NodePrefs::alert_psk_b64. Kept inline so we don't drag the
// densaugeo/base64 PlatformIO dep into every CLI-using build.
static size_t alert_encode_base64(const uint8_t* in, size_t in_len, char* out, size_t out_size) {
  static const char TBL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t needed = ((in_len + 2) / 3) * 4 + 1;
  if (out_size < needed) return 0;
  size_t o = 0;
  for (size_t i = 0; i < in_len; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    int rem = (int)(in_len - i);
    if (rem > 1) v |= (uint32_t)in[i + 1] << 8;
    if (rem > 2) v |= (uint32_t)in[i + 2];
    out[o++] = TBL[(v >> 18) & 0x3F];
    out[o++] = TBL[(v >> 12) & 0x3F];
    out[o++] = (rem > 1) ? TBL[(v >> 6) & 0x3F] : '=';
    out[o++] = (rem > 2) ? TBL[v & 0x3F]        : '=';
  }
  out[o] = '\0';
  return o;
}

#ifndef BRIDGE_MAX_BAUD
#define BRIDGE_MAX_BAUD 115200
#endif
#ifdef ESP_PLATFORM
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#endif
#ifdef WITH_MQTT_BRIDGE
#include "bridges/MQTTBridge.h"

// Helper function to calculate total size of MQTT fields for file format compatibility
// Uses NodePrefs struct to get accurate field sizes
static size_t getMQTTFieldsSize(const NodePrefs* prefs) {
  return sizeof(prefs->mqtt_origin) + sizeof(prefs->mqtt_iata) +
         sizeof(prefs->mqtt_status_enabled) + sizeof(prefs->mqtt_packets_enabled) +
         sizeof(prefs->mqtt_raw_enabled) + sizeof(prefs->mqtt_tx_enabled) +
         sizeof(prefs->mqtt_status_interval) + sizeof(prefs->wifi_ssid) +
         sizeof(prefs->wifi_password) + sizeof(prefs->timezone_string) +
         sizeof(prefs->timezone_offset) + sizeof(prefs->mqtt_slot_preset) +
         sizeof(prefs->mqtt_slot_host) + sizeof(prefs->mqtt_slot_port) +
         sizeof(prefs->mqtt_slot_username) + sizeof(prefs->mqtt_slot_password) +
         sizeof(prefs->mqtt_owner_public_key) + sizeof(prefs->mqtt_email);
}
#endif

// Believe it or not, this std C function is busted on some platforms!
static uint32_t _atoi(const char* sp) {
  uint32_t n = 0;
  while (*sp && *sp >= '0' && *sp <= '9') {
    n *= 10;
    n += (*sp++ - '0');
  }
  return n;
}

#ifdef WITH_MQTT_BRIDGE
static int getMQTTPresetNameCount() {
  // Include virtual presets accepted by CLI parser.
  return MQTT_PRESET_COUNT + 2; // built-ins + custom + none
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

static bool isValidName(const char *n) {
  while (*n) {
    if (*n == '[' || *n == ']' || *n == '/' || *n == '\\' || *n == ':' || *n == ',' || *n == '?' || *n == '*') return false;
    n++;
  }
  return true;
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

void CommonCLI::loadPrefs(FILESYSTEM* fs) {
  bool is_fresh_install = false;
  bool is_upgrade = false;
  
  if (fs->exists("/com_prefs")) {
    loadPrefsInt(fs, "/com_prefs");   // new filename
  } else if (fs->exists("/node_prefs")) {
    loadPrefsInt(fs, "/node_prefs");
    is_upgrade = true;  // Migrating from old filename
    savePrefs(fs);  // save to new filename
    fs->remove("/node_prefs");  // remove old
  } else {
    // File doesn't exist - set default bridge settings for fresh installs
    is_fresh_install = true;
    _prefs->bridge_pkt_src = 1;  // Default to RX (logRx) for new installs
  }
#ifdef WITH_MQTT_BRIDGE
  // Load MQTT preferences from separate file
  loadMQTTPrefs(fs);
  // Sync MQTT prefs to NodePrefs so existing code (like MQTTBridge) can access them
  syncMQTTPrefsToNodePrefs();
  
  // For MQTT bridge, migrate bridge.source to RX (logRx) only on fresh installs or upgrades
  // so legacy "tx" is not the default. mqtt.rx / mqtt.tx are separate (fresh default: advert for TX)
  if ((is_fresh_install || is_upgrade) && _prefs->bridge_pkt_src == 0) {
    MESH_DEBUG_PRINTLN("MQTT Bridge: Migrating bridge.source from tx to rx (MQTT bridge default)");
    _prefs->bridge_pkt_src = 1;  // Set to RX (logRx)
    savePrefs(fs);  // Save the updated preference
  }
  // mqtt_rx_enabled: new field appended to end of MQTTPrefs. On upgrade from older firmware,
  // the shorter /mqtt_prefs file won't contain it, so it keeps the default value (1 = on)
  // set by setMQTTPrefsDefaults(). No explicit migration needed.
#endif
}

void CommonCLI::loadPrefsInt(FILESYSTEM* fs, const char* filename) {
#if defined(RP2040_PLATFORM)
  File file = fs->open(filename, "r");
#else
  File file = fs->open(filename);
#endif
  if (file) {
    uint8_t pad[8];

    file.read((uint8_t *)&_prefs->airtime_factor, sizeof(_prefs->airtime_factor));    // 0
    file.read((uint8_t *)&_prefs->node_name, sizeof(_prefs->node_name));              // 4
    file.read(pad, 4);                                                                // 36
    file.read((uint8_t *)&_prefs->node_lat, sizeof(_prefs->node_lat));                // 40
    file.read((uint8_t *)&_prefs->node_lon, sizeof(_prefs->node_lon));                // 48
    file.read((uint8_t *)&_prefs->password[0], sizeof(_prefs->password));             // 56
    file.read((uint8_t *)&_prefs->freq, sizeof(_prefs->freq));                        // 72
    file.read((uint8_t *)&_prefs->tx_power_dbm, sizeof(_prefs->tx_power_dbm));        // 76
    file.read((uint8_t *)&_prefs->disable_fwd, sizeof(_prefs->disable_fwd));          // 77
    file.read((uint8_t *)&_prefs->advert_interval, sizeof(_prefs->advert_interval));  // 78
    file.read(pad, 1);                                                                // 79 : 1 byte unused (was rx_boosted_gain in v1.14.1, moved to end for upgrade compat)
    file.read((uint8_t *)&_prefs->rx_delay_base, sizeof(_prefs->rx_delay_base));      // 80
    file.read((uint8_t *)&_prefs->tx_delay_factor, sizeof(_prefs->tx_delay_factor));  // 84
    file.read((uint8_t *)&_prefs->guest_password[0], sizeof(_prefs->guest_password)); // 88
    file.read((uint8_t *)&_prefs->direct_tx_delay_factor, sizeof(_prefs->direct_tx_delay_factor)); // 104
    file.read(pad, 4);                                                                             // 108
    file.read((uint8_t *)&_prefs->sf, sizeof(_prefs->sf));                                         // 112
    file.read((uint8_t *)&_prefs->cr, sizeof(_prefs->cr));                                         // 113
    file.read((uint8_t *)&_prefs->allow_read_only, sizeof(_prefs->allow_read_only));               // 114
    file.read((uint8_t *)&_prefs->multi_acks, sizeof(_prefs->multi_acks));                         // 115
    file.read((uint8_t *)&_prefs->bw, sizeof(_prefs->bw));                                         // 116
    file.read((uint8_t *)&_prefs->agc_reset_interval, sizeof(_prefs->agc_reset_interval));         // 120
    file.read((uint8_t *)&_prefs->path_hash_mode, sizeof(_prefs->path_hash_mode));                 // 121
    file.read((uint8_t *)&_prefs->loop_detect, sizeof(_prefs->loop_detect));                       // 122
    file.read(pad, 1);                                                                             // 123
    file.read((uint8_t *)&_prefs->flood_max, sizeof(_prefs->flood_max));                           // 124
    file.read((uint8_t *)&_prefs->flood_advert_interval, sizeof(_prefs->flood_advert_interval));   // 125
    file.read((uint8_t *)&_prefs->interference_threshold, sizeof(_prefs->interference_threshold)); // 126
    file.read((uint8_t *)&_prefs->bridge_enabled, sizeof(_prefs->bridge_enabled));                 // 127
    file.read((uint8_t *)&_prefs->bridge_delay, sizeof(_prefs->bridge_delay));                     // 128
    file.read((uint8_t *)&_prefs->bridge_pkt_src, sizeof(_prefs->bridge_pkt_src));                 // 130
    file.read((uint8_t *)&_prefs->bridge_baud, sizeof(_prefs->bridge_baud));                       // 131
    file.read((uint8_t *)&_prefs->bridge_channel, sizeof(_prefs->bridge_channel));                 // 135
    file.read((uint8_t *)&_prefs->bridge_secret, sizeof(_prefs->bridge_secret));                   // 136
    file.read((uint8_t *)&_prefs->powersaving_enabled, sizeof(_prefs->powersaving_enabled));       // 152
    file.read(pad, 3);                                                                             // 153
    file.read((uint8_t *)&_prefs->gps_enabled, sizeof(_prefs->gps_enabled));                       // 156
    file.read((uint8_t *)&_prefs->gps_interval, sizeof(_prefs->gps_interval));                     // 157
    file.read((uint8_t *)&_prefs->advert_loc_policy, sizeof (_prefs->advert_loc_policy));          // 161
    file.read((uint8_t *)&_prefs->discovery_mod_timestamp, sizeof(_prefs->discovery_mod_timestamp)); // 162
    file.read((uint8_t *)&_prefs->adc_multiplier, sizeof(_prefs->adc_multiplier));                 // 166
    file.read((uint8_t *)_prefs->owner_info, sizeof(_prefs->owner_info));                          // 170
    // MQTT settings - skip reading from main prefs file (now stored separately)
    // For backward compatibility, we'll skip these bytes if they exist in old files
    // The actual MQTT prefs will be loaded from /mqtt_prefs in loadMQTTPrefs()
    // Skip MQTT fields for file format compatibility (whether MQTT bridge is enabled or not)
#ifdef WITH_MQTT_BRIDGE
    size_t mqtt_fields_size = getMQTTFieldsSize(_prefs);
#else
    // If MQTT bridge not enabled, still skip these fields for file format compatibility
    size_t mqtt_fields_size =
      sizeof(_prefs->mqtt_origin) + sizeof(_prefs->mqtt_iata) +
      sizeof(_prefs->mqtt_status_enabled) + sizeof(_prefs->mqtt_packets_enabled) +
      sizeof(_prefs->mqtt_raw_enabled) + sizeof(_prefs->mqtt_tx_enabled) +
      sizeof(_prefs->mqtt_status_interval) + sizeof(_prefs->wifi_ssid) +
      sizeof(_prefs->wifi_password) + sizeof(_prefs->timezone_string) +
      sizeof(_prefs->timezone_offset) + sizeof(_prefs->mqtt_slot_preset) +
      sizeof(_prefs->mqtt_slot_host) + sizeof(_prefs->mqtt_slot_port) +
      sizeof(_prefs->mqtt_slot_username) + sizeof(_prefs->mqtt_slot_password) +
      sizeof(_prefs->mqtt_owner_public_key) + sizeof(_prefs->mqtt_email);
#endif
    uint8_t skip_buffer[512]; // Large enough buffer
    size_t remaining = mqtt_fields_size;
    while (remaining > 0) {
      size_t to_read = remaining > sizeof(skip_buffer) ? sizeof(skip_buffer) : remaining;
      file.read(skip_buffer, to_read);
      remaining -= to_read;
    }
    file.read((uint8_t *)&_prefs->rx_boosted_gain, sizeof(_prefs->rx_boosted_gain));              // 290
    file.read((uint8_t *)&_prefs->snmp_enabled, sizeof(_prefs->snmp_enabled));                    // 291
    file.read((uint8_t *)&_prefs->snmp_community, sizeof(_prefs->snmp_community));                // 292
    if (file.available() >= (int)sizeof(_prefs->radio_watchdog_minutes)) {
      file.read((uint8_t *)&_prefs->radio_watchdog_minutes, sizeof(_prefs->radio_watchdog_minutes)); // 316
    }
    // Alert channel fields (appended; older files won't have them — defaults from MyMesh ctor remain)
    if (file.available() >= (int)sizeof(_prefs->alert_enabled)) {
      file.read((uint8_t *)&_prefs->alert_enabled, sizeof(_prefs->alert_enabled));
    }
    if (file.available() >= (int)sizeof(_prefs->alert_psk_b64)) {
      file.read((uint8_t *)&_prefs->alert_psk_b64, sizeof(_prefs->alert_psk_b64));
    }
    if (file.available() >= (int)sizeof(_prefs->alert_wifi_minutes)) {
      file.read((uint8_t *)&_prefs->alert_wifi_minutes, sizeof(_prefs->alert_wifi_minutes));
    }
    if (file.available() >= (int)sizeof(_prefs->alert_mqtt_minutes)) {
      file.read((uint8_t *)&_prefs->alert_mqtt_minutes, sizeof(_prefs->alert_mqtt_minutes));
    }
    if (file.available() >= (int)sizeof(_prefs->alert_min_interval_min)) {
      file.read((uint8_t *)&_prefs->alert_min_interval_min, sizeof(_prefs->alert_min_interval_min));
    }
    if (file.available() >= (int)sizeof(_prefs->alert_hashtag)) {
      file.read((uint8_t *)&_prefs->alert_hashtag, sizeof(_prefs->alert_hashtag));
    }
    if (file.available() >= (int)sizeof(_prefs->alert_region)) {
      file.read((uint8_t *)&_prefs->alert_region, sizeof(_prefs->alert_region));
    }
    // ensure null termination after raw read
    _prefs->alert_psk_b64[sizeof(_prefs->alert_psk_b64) - 1] = '\0';
    _prefs->alert_hashtag[sizeof(_prefs->alert_hashtag) - 1] = '\0';
    _prefs->alert_region[sizeof(_prefs->alert_region) - 1] = '\0';

    // sanitise bad pref values
    _prefs->rx_delay_base = constrain(_prefs->rx_delay_base, 0, 20.0f);
    _prefs->tx_delay_factor = constrain(_prefs->tx_delay_factor, 0, 2.0f);
    _prefs->direct_tx_delay_factor = constrain(_prefs->direct_tx_delay_factor, 0, 2.0f);
    _prefs->airtime_factor = constrain(_prefs->airtime_factor, 0, 9.0f);
    _prefs->freq = constrain(_prefs->freq, 150.0f, 2500.0f);
    _prefs->bw = constrain(_prefs->bw, 7.8f, 500.0f);
    _prefs->sf = constrain(_prefs->sf, 5, 12);
    _prefs->cr = constrain(_prefs->cr, 5, 8);
    _prefs->tx_power_dbm = constrain(_prefs->tx_power_dbm, -9, 30);
    _prefs->multi_acks = constrain(_prefs->multi_acks, 0, 1);
    _prefs->adc_multiplier = constrain(_prefs->adc_multiplier, 0.0f, 10.0f);
    _prefs->path_hash_mode = constrain(_prefs->path_hash_mode, 0, 2);   // NOTE: mode 3 reserved for future
    _prefs->loop_detect = constrain(_prefs->loop_detect, 0, 3);          // LOOP_DETECT_OFF..LOOP_DETECT_STRICT

    // sanitise bad bridge pref values
    _prefs->bridge_enabled = constrain(_prefs->bridge_enabled, 0, 1);
    _prefs->bridge_delay = constrain(_prefs->bridge_delay, 0, 10000);
    _prefs->bridge_pkt_src = constrain(_prefs->bridge_pkt_src, 0, 1);
    _prefs->bridge_baud = constrain(_prefs->bridge_baud, 9600, BRIDGE_MAX_BAUD);
    _prefs->bridge_channel = constrain(_prefs->bridge_channel, 0, 14);

    _prefs->powersaving_enabled = constrain(_prefs->powersaving_enabled, 0, 1);

    _prefs->gps_enabled = constrain(_prefs->gps_enabled, 0, 1);
    _prefs->advert_loc_policy = constrain(_prefs->advert_loc_policy, 0, 2);

    _prefs->rx_boosted_gain = constrain(_prefs->rx_boosted_gain, 0, 1); // boolean
    _prefs->snmp_enabled = constrain(_prefs->snmp_enabled, 0, 1);
    _prefs->snmp_community[sizeof(_prefs->snmp_community) - 1] = '\0'; // ensure null terminated
    if (_prefs->radio_watchdog_minutes > 120) {
      _prefs->radio_watchdog_minutes = 5;
    }

    file.close();
  }
}

void CommonCLI::savePrefs(FILESYSTEM* fs) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  fs->remove("/com_prefs");
  File file = fs->open("/com_prefs", FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  File file = fs->open("/com_prefs", "w");
#else
  File file = fs->open("/com_prefs", "w", true);
#endif
  if (file) {
    uint8_t pad[8];
    memset(pad, 0, sizeof(pad));

    file.write((uint8_t *)&_prefs->airtime_factor, sizeof(_prefs->airtime_factor));    // 0
    file.write((uint8_t *)&_prefs->node_name, sizeof(_prefs->node_name));              // 4
    file.write(pad, 4);                                                                // 36
    file.write((uint8_t *)&_prefs->node_lat, sizeof(_prefs->node_lat));                // 40
    file.write((uint8_t *)&_prefs->node_lon, sizeof(_prefs->node_lon));                // 48
    file.write((uint8_t *)&_prefs->password[0], sizeof(_prefs->password));             // 56
    file.write((uint8_t *)&_prefs->freq, sizeof(_prefs->freq));                        // 72
    file.write((uint8_t *)&_prefs->tx_power_dbm, sizeof(_prefs->tx_power_dbm));        // 76
    file.write((uint8_t *)&_prefs->disable_fwd, sizeof(_prefs->disable_fwd));          // 77
    file.write((uint8_t *)&_prefs->advert_interval, sizeof(_prefs->advert_interval));  // 78
    file.write(pad, 1);                                                                // 79 : 1 byte unused (rx_boosted_gain moved to end)
    file.write((uint8_t *)&_prefs->rx_delay_base, sizeof(_prefs->rx_delay_base));      // 80
    file.write((uint8_t *)&_prefs->tx_delay_factor, sizeof(_prefs->tx_delay_factor));  // 84
    file.write((uint8_t *)&_prefs->guest_password[0], sizeof(_prefs->guest_password)); // 88
    file.write((uint8_t *)&_prefs->direct_tx_delay_factor, sizeof(_prefs->direct_tx_delay_factor)); // 104
    file.write(pad, 4);                                                                             // 108
    file.write((uint8_t *)&_prefs->sf, sizeof(_prefs->sf));                                         // 112
    file.write((uint8_t *)&_prefs->cr, sizeof(_prefs->cr));                                         // 113
    file.write((uint8_t *)&_prefs->allow_read_only, sizeof(_prefs->allow_read_only));               // 114
    file.write((uint8_t *)&_prefs->multi_acks, sizeof(_prefs->multi_acks));                         // 115
    file.write((uint8_t *)&_prefs->bw, sizeof(_prefs->bw));                                         // 116
    file.write((uint8_t *)&_prefs->agc_reset_interval, sizeof(_prefs->agc_reset_interval));         // 120
    file.write((uint8_t *)&_prefs->path_hash_mode, sizeof(_prefs->path_hash_mode));                 // 121
    file.write((uint8_t *)&_prefs->loop_detect, sizeof(_prefs->loop_detect));                       // 122
    file.write(pad, 1);                                                                             // 123
    file.write((uint8_t *)&_prefs->flood_max, sizeof(_prefs->flood_max));                           // 124
    file.write((uint8_t *)&_prefs->flood_advert_interval, sizeof(_prefs->flood_advert_interval));   // 125
    file.write((uint8_t *)&_prefs->interference_threshold, sizeof(_prefs->interference_threshold)); // 126
    file.write((uint8_t *)&_prefs->bridge_enabled, sizeof(_prefs->bridge_enabled));                 // 127
    file.write((uint8_t *)&_prefs->bridge_delay, sizeof(_prefs->bridge_delay));                     // 128
    file.write((uint8_t *)&_prefs->bridge_pkt_src, sizeof(_prefs->bridge_pkt_src));                 // 130
    file.write((uint8_t *)&_prefs->bridge_baud, sizeof(_prefs->bridge_baud));                       // 131
    file.write((uint8_t *)&_prefs->bridge_channel, sizeof(_prefs->bridge_channel));                 // 135
    file.write((uint8_t *)&_prefs->bridge_secret, sizeof(_prefs->bridge_secret));                   // 136
    file.write((uint8_t *)&_prefs->powersaving_enabled, sizeof(_prefs->powersaving_enabled));       // 152
    file.write(pad, 3);                                                                             // 153
    file.write((uint8_t *)&_prefs->gps_enabled, sizeof(_prefs->gps_enabled));                       // 156
    file.write((uint8_t *)&_prefs->gps_interval, sizeof(_prefs->gps_interval));                     // 157
    file.write((uint8_t *)&_prefs->advert_loc_policy, sizeof(_prefs->advert_loc_policy));           // 161
    file.write((uint8_t *)&_prefs->discovery_mod_timestamp, sizeof(_prefs->discovery_mod_timestamp)); // 162
    file.write((uint8_t *)&_prefs->adc_multiplier, sizeof(_prefs->adc_multiplier));                 // 166
    file.write((uint8_t *)_prefs->owner_info, sizeof(_prefs->owner_info));                          // 170
    // MQTT settings - no longer saved here (stored in separate /mqtt_prefs file)
    // Write zeros/padding to maintain file format compatibility
#ifdef WITH_MQTT_BRIDGE
    size_t mqtt_fields_size = getMQTTFieldsSize(_prefs);
#else
    // If MQTT bridge not enabled, still write zeros for file format compatibility
    size_t mqtt_fields_size =
      sizeof(_prefs->mqtt_origin) + sizeof(_prefs->mqtt_iata) +
      sizeof(_prefs->mqtt_status_enabled) + sizeof(_prefs->mqtt_packets_enabled) +
      sizeof(_prefs->mqtt_raw_enabled) + sizeof(_prefs->mqtt_tx_enabled) +
      sizeof(_prefs->mqtt_status_interval) + sizeof(_prefs->wifi_ssid) +
      sizeof(_prefs->wifi_password) + sizeof(_prefs->timezone_string) +
      sizeof(_prefs->timezone_offset) + sizeof(_prefs->mqtt_slot_preset) +
      sizeof(_prefs->mqtt_slot_host) + sizeof(_prefs->mqtt_slot_port) +
      sizeof(_prefs->mqtt_slot_username) + sizeof(_prefs->mqtt_slot_password) +
      sizeof(_prefs->mqtt_owner_public_key) + sizeof(_prefs->mqtt_email);
#endif
    memset(pad, 0, sizeof(pad));
    size_t remaining = mqtt_fields_size;
    while (remaining > 0) {
      size_t to_write = remaining > sizeof(pad) ? sizeof(pad) : remaining;
      file.write(pad, to_write);
      remaining -= to_write;
    }
    file.write((uint8_t *)&_prefs->rx_boosted_gain, sizeof(_prefs->rx_boosted_gain));              // 290
    file.write((uint8_t *)&_prefs->snmp_enabled, sizeof(_prefs->snmp_enabled));                    // 291
    file.write((uint8_t *)&_prefs->snmp_community, sizeof(_prefs->snmp_community));                // 292
    file.write((uint8_t *)&_prefs->radio_watchdog_minutes, sizeof(_prefs->radio_watchdog_minutes)); // 316
    // Alert channel fields (appended)
    file.write((uint8_t *)&_prefs->alert_enabled, sizeof(_prefs->alert_enabled));
    file.write((uint8_t *)&_prefs->alert_psk_b64, sizeof(_prefs->alert_psk_b64));
    file.write((uint8_t *)&_prefs->alert_wifi_minutes, sizeof(_prefs->alert_wifi_minutes));
    file.write((uint8_t *)&_prefs->alert_mqtt_minutes, sizeof(_prefs->alert_mqtt_minutes));
    file.write((uint8_t *)&_prefs->alert_min_interval_min, sizeof(_prefs->alert_min_interval_min));
    file.write((uint8_t *)&_prefs->alert_hashtag, sizeof(_prefs->alert_hashtag));
    file.write((uint8_t *)&_prefs->alert_region, sizeof(_prefs->alert_region));

    file.close();
  }
#ifdef WITH_MQTT_BRIDGE
  // Save MQTT preferences to separate file
  syncNodePrefsToMQTTPrefs();  // Sync any changes from NodePrefs to MQTTPrefs
  saveMQTTPrefs(fs);
#endif
}

#ifdef WITH_MQTT_BRIDGE
// Set default values for MQTT preferences (used when file doesn't exist or is corrupted)
static void setMQTTPrefsDefaults(MQTTPrefs* prefs) {
  memset(prefs, 0, sizeof(MQTTPrefs));
  // Set sensible defaults matching MQTTBridge expectations
  prefs->mqtt_status_enabled = 1;    // enabled by default
  prefs->mqtt_packets_enabled = 1;   // enabled by default
  prefs->mqtt_raw_enabled = 0;       // disabled by default
  prefs->mqtt_tx_enabled = 2;        // advert: own adverts only, by default
  prefs->mqtt_rx_enabled = 1;        // RX packets enabled by default
  prefs->mqtt_status_interval = 300000; // 5 minutes default
  // Slot presets: analyzer-us and analyzer-eu enabled by default, rest = none
  strncpy(prefs->mqtt_slot_preset[0], "analyzer-us", sizeof(prefs->mqtt_slot_preset[0]) - 1);
  prefs->mqtt_slot_preset[0][sizeof(prefs->mqtt_slot_preset[0]) - 1] = '\0';
  strncpy(prefs->mqtt_slot_preset[1], "analyzer-eu", sizeof(prefs->mqtt_slot_preset[1]) - 1);
  prefs->mqtt_slot_preset[1][sizeof(prefs->mqtt_slot_preset[1]) - 1] = '\0';
  for (int i = 2; i < MAX_MQTT_SLOTS; i++) {
    strncpy(prefs->mqtt_slot_preset[i], "none", sizeof(prefs->mqtt_slot_preset[i]) - 1);
    prefs->mqtt_slot_preset[i][sizeof(prefs->mqtt_slot_preset[i]) - 1] = '\0';
  }
  prefs->wifi_power_save = 1; // Default to none (0=min, 1=none, 2=max)
  // String fields are already zero-initialized by memset
}

void CommonCLI::loadMQTTPrefs(FILESYSTEM* fs) {
  // Initialize with defaults first
  setMQTTPrefsDefaults(&_mqtt_prefs);

  bool file_existed = fs->exists("/mqtt_prefs");
  if (file_existed) {
    // Load from separate MQTT prefs file
#if defined(RP2040_PLATFORM)
    File file = fs->open("/mqtt_prefs", "r");
#else
    File file = fs->open("/mqtt_prefs");
#endif
    if (file) {
      size_t file_size = file.size();

      // Detect old (pre-slot) format by file size.
      // Old MQTTPrefs was ~472 bytes (no slot fields). New is ~1464 bytes.
      // If the file is smaller than the new struct but close to OldMQTTPrefs size,
      // read it with the old layout and migrate.
      if (file_size > 0 && file_size <= sizeof(OldMQTTPrefs)) {
        OldMQTTPrefs old_prefs;
        memset(&old_prefs, 0, sizeof(old_prefs));
        size_t bytes_read = file.read((uint8_t *)&old_prefs, file_size < sizeof(old_prefs) ? file_size : sizeof(old_prefs));
        file.close();

        if (bytes_read > 0) {
          MESH_DEBUG_PRINTLN("MQTT: Migrating old-format prefs to slot-based layout");

          // Copy common fields (identical layout at start of both structs)
          memcpy(_mqtt_prefs.mqtt_origin, old_prefs.mqtt_origin, sizeof(_mqtt_prefs.mqtt_origin));
          memcpy(_mqtt_prefs.mqtt_iata, old_prefs.mqtt_iata, sizeof(_mqtt_prefs.mqtt_iata));
          _mqtt_prefs.mqtt_status_enabled = old_prefs.mqtt_status_enabled;
          _mqtt_prefs.mqtt_packets_enabled = old_prefs.mqtt_packets_enabled;
          _mqtt_prefs.mqtt_raw_enabled = old_prefs.mqtt_raw_enabled;
          _mqtt_prefs.mqtt_tx_enabled = old_prefs.mqtt_tx_enabled;
          _mqtt_prefs.mqtt_status_interval = old_prefs.mqtt_status_interval;
          memcpy(_mqtt_prefs.wifi_ssid, old_prefs.wifi_ssid, sizeof(_mqtt_prefs.wifi_ssid));
          memcpy(_mqtt_prefs.wifi_password, old_prefs.wifi_password, sizeof(_mqtt_prefs.wifi_password));
          _mqtt_prefs.wifi_power_save = old_prefs.wifi_power_save;
          memcpy(_mqtt_prefs.timezone_string, old_prefs.timezone_string, sizeof(_mqtt_prefs.timezone_string));
          _mqtt_prefs.timezone_offset = old_prefs.timezone_offset;

          // Migrate shared auth fields
          memcpy(_mqtt_prefs.mqtt_owner_public_key, old_prefs.mqtt_owner_public_key, sizeof(_mqtt_prefs.mqtt_owner_public_key));
          memcpy(_mqtt_prefs.mqtt_email, old_prefs.mqtt_email, sizeof(_mqtt_prefs.mqtt_email));

          // Migrate analyzer presets to slots
          if (old_prefs.mqtt_analyzer_us_enabled == 1) {
            strncpy(_mqtt_prefs.mqtt_slot_preset[0], "analyzer-us", sizeof(_mqtt_prefs.mqtt_slot_preset[0]) - 1);
          } else {
            strncpy(_mqtt_prefs.mqtt_slot_preset[0], "none", sizeof(_mqtt_prefs.mqtt_slot_preset[0]) - 1);
          }
          if (old_prefs.mqtt_analyzer_eu_enabled == 1) {
            strncpy(_mqtt_prefs.mqtt_slot_preset[1], "analyzer-eu", sizeof(_mqtt_prefs.mqtt_slot_preset[1]) - 1);
          } else {
            strncpy(_mqtt_prefs.mqtt_slot_preset[1], "none", sizeof(_mqtt_prefs.mqtt_slot_preset[1]) - 1);
          }

          // Migrate custom server to slot 3
          if (old_prefs.mqtt_server[0] != '\0' && old_prefs.mqtt_port > 0) {
            strncpy(_mqtt_prefs.mqtt_slot_preset[2], "custom", sizeof(_mqtt_prefs.mqtt_slot_preset[2]) - 1);
            strncpy(_mqtt_prefs.mqtt_slot_host[2], old_prefs.mqtt_server, sizeof(_mqtt_prefs.mqtt_slot_host[2]) - 1);
            _mqtt_prefs.mqtt_slot_port[2] = old_prefs.mqtt_port;
            strncpy(_mqtt_prefs.mqtt_slot_username[2], old_prefs.mqtt_username, sizeof(_mqtt_prefs.mqtt_slot_username[2]) - 1);
            strncpy(_mqtt_prefs.mqtt_slot_password[2], old_prefs.mqtt_password, sizeof(_mqtt_prefs.mqtt_slot_password[2]) - 1);
          } else {
            strncpy(_mqtt_prefs.mqtt_slot_preset[2], "none", sizeof(_mqtt_prefs.mqtt_slot_preset[2]) - 1);
          }

          // Save migrated prefs in new format
          saveMQTTPrefs(fs);
        }
      } else if (file_size > 0 && file_size <= sizeof(ThreeSlotMQTTPrefs)) {
        // 3-slot format → 6-slot migration
        // Array sizes changed from [3] to [6], shifting all field offsets.
        // Read into old layout struct and field-copy to new layout.
        ThreeSlotMQTTPrefs old3;
        memset(&old3, 0, sizeof(old3));
        size_t bytes_to_read = file_size < sizeof(old3) ? file_size : sizeof(old3);
        size_t bytes_read = file.read((uint8_t *)&old3, bytes_to_read);
        file.close();

        if (bytes_read > 0) {
          MESH_DEBUG_PRINTLN("MQTT: Migrating 3-slot prefs to 6-slot layout");

          // Copy non-slot fields (identical layout)
          memcpy(_mqtt_prefs.mqtt_origin, old3.mqtt_origin, sizeof(_mqtt_prefs.mqtt_origin));
          memcpy(_mqtt_prefs.mqtt_iata, old3.mqtt_iata, sizeof(_mqtt_prefs.mqtt_iata));
          _mqtt_prefs.mqtt_status_enabled = old3.mqtt_status_enabled;
          _mqtt_prefs.mqtt_packets_enabled = old3.mqtt_packets_enabled;
          _mqtt_prefs.mqtt_raw_enabled = old3.mqtt_raw_enabled;
          _mqtt_prefs.mqtt_tx_enabled = old3.mqtt_tx_enabled;
          _mqtt_prefs.mqtt_status_interval = old3.mqtt_status_interval;
          memcpy(_mqtt_prefs.wifi_ssid, old3.wifi_ssid, sizeof(_mqtt_prefs.wifi_ssid));
          memcpy(_mqtt_prefs.wifi_password, old3.wifi_password, sizeof(_mqtt_prefs.wifi_password));
          _mqtt_prefs.wifi_power_save = old3.wifi_power_save;
          memcpy(_mqtt_prefs.timezone_string, old3.timezone_string, sizeof(_mqtt_prefs.timezone_string));
          _mqtt_prefs.timezone_offset = old3.timezone_offset;

          // Copy slot fields for indices 0-2 from old layout
          for (int i = 0; i < 3; i++) {
            memcpy(_mqtt_prefs.mqtt_slot_preset[i], old3.mqtt_slot_preset[i], sizeof(_mqtt_prefs.mqtt_slot_preset[i]));
            memcpy(_mqtt_prefs.mqtt_slot_host[i], old3.mqtt_slot_host[i], sizeof(_mqtt_prefs.mqtt_slot_host[i]));
            _mqtt_prefs.mqtt_slot_port[i] = old3.mqtt_slot_port[i];
            memcpy(_mqtt_prefs.mqtt_slot_username[i], old3.mqtt_slot_username[i], sizeof(_mqtt_prefs.mqtt_slot_username[i]));
            memcpy(_mqtt_prefs.mqtt_slot_password[i], old3.mqtt_slot_password[i], sizeof(_mqtt_prefs.mqtt_slot_password[i]));
            memcpy(_mqtt_prefs.mqtt_slot_token[i], old3.mqtt_slot_token[i], sizeof(_mqtt_prefs.mqtt_slot_token[i]));
            memcpy(_mqtt_prefs.mqtt_slot_topic[i], old3.mqtt_slot_topic[i], sizeof(_mqtt_prefs.mqtt_slot_topic[i]));
          }
          // Slots 3-5 keep defaults ("none") from setMQTTPrefsDefaults()

          // Copy shared auth fields
          memcpy(_mqtt_prefs.mqtt_owner_public_key, old3.mqtt_owner_public_key, sizeof(_mqtt_prefs.mqtt_owner_public_key));
          memcpy(_mqtt_prefs.mqtt_email, old3.mqtt_email, sizeof(_mqtt_prefs.mqtt_email));

          // Save migrated prefs in new 6-slot format
          saveMQTTPrefs(fs);
        }
      } else if (file_size > 0) {
        // 6-slot format: read directly
        size_t bytes_to_read = file_size < sizeof(_mqtt_prefs) ? file_size : sizeof(_mqtt_prefs);
        size_t bytes_read = file.read((uint8_t *)&_mqtt_prefs, bytes_to_read);
        file.close();
        if (bytes_read != bytes_to_read) {
          setMQTTPrefsDefaults(&_mqtt_prefs);
        }
      } else {
        file.close();
        setMQTTPrefsDefaults(&_mqtt_prefs);
      }
    }
  } else {
    // No /mqtt_prefs file — defaults already set
    // (Legacy /com_prefs migration removed: the old offset-based approach was fragile
    // and the pre-MQTT firmware never wrote MQTT fields to /com_prefs anyway.)
  }
}

void CommonCLI::saveMQTTPrefs(FILESYSTEM* fs) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  fs->remove("/mqtt_prefs");
  File file = fs->open("/mqtt_prefs", FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  File file = fs->open("/mqtt_prefs", "w");
#else
  File file = fs->open("/mqtt_prefs", "w", true);
#endif
  if (file) {
    file.write((uint8_t *)&_mqtt_prefs, sizeof(_mqtt_prefs));
    file.close();
  }
}

void CommonCLI::syncMQTTPrefsToNodePrefs() {
  // Copy MQTT prefs to NodePrefs so existing code can access them
  // Use StrHelper::strncpy to ensure proper null termination
  StrHelper::strncpy(_prefs->mqtt_origin, _mqtt_prefs.mqtt_origin, sizeof(_prefs->mqtt_origin));
  StrHelper::strncpy(_prefs->mqtt_iata, _mqtt_prefs.mqtt_iata, sizeof(_prefs->mqtt_iata));
  _prefs->mqtt_status_enabled = _mqtt_prefs.mqtt_status_enabled;
  _prefs->mqtt_packets_enabled = _mqtt_prefs.mqtt_packets_enabled;
  _prefs->mqtt_raw_enabled = _mqtt_prefs.mqtt_raw_enabled;
  _prefs->mqtt_tx_enabled = _mqtt_prefs.mqtt_tx_enabled;
  _prefs->mqtt_rx_enabled = _mqtt_prefs.mqtt_rx_enabled;
  _prefs->mqtt_status_interval = _mqtt_prefs.mqtt_status_interval;
  StrHelper::strncpy(_prefs->wifi_ssid, _mqtt_prefs.wifi_ssid, sizeof(_prefs->wifi_ssid));
  StrHelper::strncpy(_prefs->wifi_password, _mqtt_prefs.wifi_password, sizeof(_prefs->wifi_password));
  _prefs->wifi_power_save = _mqtt_prefs.wifi_power_save;
  StrHelper::strncpy(_prefs->timezone_string, _mqtt_prefs.timezone_string, sizeof(_prefs->timezone_string));
  _prefs->timezone_offset = _mqtt_prefs.timezone_offset;
  // Slot-based fields
  for (int i = 0; i < MAX_MQTT_SLOTS; i++) {
    StrHelper::strncpy(_prefs->mqtt_slot_preset[i], _mqtt_prefs.mqtt_slot_preset[i], sizeof(_prefs->mqtt_slot_preset[i]));
    StrHelper::strncpy(_prefs->mqtt_slot_host[i], _mqtt_prefs.mqtt_slot_host[i], sizeof(_prefs->mqtt_slot_host[i]));
    _prefs->mqtt_slot_port[i] = _mqtt_prefs.mqtt_slot_port[i];
    StrHelper::strncpy(_prefs->mqtt_slot_username[i], _mqtt_prefs.mqtt_slot_username[i], sizeof(_prefs->mqtt_slot_username[i]));
    StrHelper::strncpy(_prefs->mqtt_slot_password[i], _mqtt_prefs.mqtt_slot_password[i], sizeof(_prefs->mqtt_slot_password[i]));
    StrHelper::strncpy(_prefs->mqtt_slot_token[i], _mqtt_prefs.mqtt_slot_token[i], sizeof(_prefs->mqtt_slot_token[i]));
    StrHelper::strncpy(_prefs->mqtt_slot_topic[i], _mqtt_prefs.mqtt_slot_topic[i], sizeof(_prefs->mqtt_slot_topic[i]));
    StrHelper::strncpy(_prefs->mqtt_slot_audience[i], _mqtt_prefs.mqtt_slot_audience[i], sizeof(_prefs->mqtt_slot_audience[i]));
  }
  StrHelper::strncpy(_prefs->mqtt_owner_public_key, _mqtt_prefs.mqtt_owner_public_key, sizeof(_prefs->mqtt_owner_public_key));
  StrHelper::strncpy(_prefs->mqtt_email, _mqtt_prefs.mqtt_email, sizeof(_prefs->mqtt_email));
}

void CommonCLI::syncNodePrefsToMQTTPrefs() {
  // Copy NodePrefs to MQTT prefs (used when saving after changes via CLI)
  // Use StrHelper::strncpy to ensure proper null termination
  StrHelper::strncpy(_mqtt_prefs.mqtt_origin, _prefs->mqtt_origin, sizeof(_mqtt_prefs.mqtt_origin));
  StrHelper::strncpy(_mqtt_prefs.mqtt_iata, _prefs->mqtt_iata, sizeof(_mqtt_prefs.mqtt_iata));
  _mqtt_prefs.mqtt_status_enabled = _prefs->mqtt_status_enabled;
  _mqtt_prefs.mqtt_packets_enabled = _prefs->mqtt_packets_enabled;
  _mqtt_prefs.mqtt_raw_enabled = _prefs->mqtt_raw_enabled;
  _mqtt_prefs.mqtt_tx_enabled = _prefs->mqtt_tx_enabled;
  _mqtt_prefs.mqtt_rx_enabled = _prefs->mqtt_rx_enabled;
  _mqtt_prefs.mqtt_status_interval = _prefs->mqtt_status_interval;
  StrHelper::strncpy(_mqtt_prefs.wifi_ssid, _prefs->wifi_ssid, sizeof(_mqtt_prefs.wifi_ssid));
  StrHelper::strncpy(_mqtt_prefs.wifi_password, _prefs->wifi_password, sizeof(_mqtt_prefs.wifi_password));
  _mqtt_prefs.wifi_power_save = _prefs->wifi_power_save;
  StrHelper::strncpy(_mqtt_prefs.timezone_string, _prefs->timezone_string, sizeof(_mqtt_prefs.timezone_string));
  _mqtt_prefs.timezone_offset = _prefs->timezone_offset;
  // Slot-based fields
  for (int i = 0; i < MAX_MQTT_SLOTS; i++) {
    StrHelper::strncpy(_mqtt_prefs.mqtt_slot_preset[i], _prefs->mqtt_slot_preset[i], sizeof(_mqtt_prefs.mqtt_slot_preset[i]));
    StrHelper::strncpy(_mqtt_prefs.mqtt_slot_host[i], _prefs->mqtt_slot_host[i], sizeof(_mqtt_prefs.mqtt_slot_host[i]));
    _mqtt_prefs.mqtt_slot_port[i] = _prefs->mqtt_slot_port[i];
    StrHelper::strncpy(_mqtt_prefs.mqtt_slot_username[i], _prefs->mqtt_slot_username[i], sizeof(_mqtt_prefs.mqtt_slot_username[i]));
    StrHelper::strncpy(_mqtt_prefs.mqtt_slot_password[i], _prefs->mqtt_slot_password[i], sizeof(_mqtt_prefs.mqtt_slot_password[i]));
    StrHelper::strncpy(_mqtt_prefs.mqtt_slot_token[i], _prefs->mqtt_slot_token[i], sizeof(_mqtt_prefs.mqtt_slot_token[i]));
    StrHelper::strncpy(_mqtt_prefs.mqtt_slot_topic[i], _prefs->mqtt_slot_topic[i], sizeof(_mqtt_prefs.mqtt_slot_topic[i]));
    StrHelper::strncpy(_mqtt_prefs.mqtt_slot_audience[i], _prefs->mqtt_slot_audience[i], sizeof(_mqtt_prefs.mqtt_slot_audience[i]));
  }
  StrHelper::strncpy(_mqtt_prefs.mqtt_owner_public_key, _prefs->mqtt_owner_public_key, sizeof(_mqtt_prefs.mqtt_owner_public_key));
  StrHelper::strncpy(_mqtt_prefs.mqtt_email, _prefs->mqtt_email, sizeof(_mqtt_prefs.mqtt_email));
}
#endif

#define MIN_LOCAL_ADVERT_INTERVAL   60

void CommonCLI::savePrefs() {
  uint8_t old_advert_interval = _prefs->advert_interval;
  if (_prefs->advert_interval * 2 < MIN_LOCAL_ADVERT_INTERVAL) {
    _prefs->advert_interval = 0;  // turn it off, now that device has been manually configured
  }
  // If advert_interval was changed, update the timer to reflect the change
  if (old_advert_interval != _prefs->advert_interval) {
    _callbacks->updateAdvertTimer();
  }
  _callbacks->savePrefs();
}

uint8_t CommonCLI::buildAdvertData(uint8_t node_type, uint8_t* app_data) {
  if (_prefs->advert_loc_policy == ADVERT_LOC_NONE) {
    AdvertDataBuilder builder(node_type, _prefs->node_name);
    return builder.encodeTo(app_data);
  } else if (_prefs->advert_loc_policy == ADVERT_LOC_SHARE) {
    AdvertDataBuilder builder(node_type, _prefs->node_name, _sensors->node_lat, _sensors->node_lon);
    return builder.encodeTo(app_data);
  } else {
    AdvertDataBuilder builder(node_type, _prefs->node_name, _prefs->node_lat, _prefs->node_lon);
    return builder.encodeTo(app_data);
  }
}

void CommonCLI::handleCommand(uint32_t sender_timestamp, char* command, char* reply) {
    if (memcmp(command, "poweroff", 8) == 0 || memcmp(command, "shutdown", 8) == 0) {
      _board->powerOff();  // doesn't return
    } else if (memcmp(command, "reboot", 6) == 0) {
      _board->reboot();  // doesn't return
    } else if (memcmp(command, "clkreboot", 9) == 0) {
      // Reset clock
      getRTCClock()->setCurrentTime(1715770351);  // 15 May 2024, 8:50pm
      _board->reboot();  // doesn't return
     } else if (memcmp(command, "advert.zerohop", 14) == 0 && (command[14] == 0 || command[14] == ' ')) {
      // send zerohop advert
      _callbacks->sendSelfAdvertisement(1500, false);  // longer delay, give CLI response time to be sent first
      strcpy(reply, "OK - zerohop advert sent");
    } else if (memcmp(command, "advert", 6) == 0) {
      // send flood advert
      _callbacks->sendSelfAdvertisement(1500, true);  // longer delay, give CLI response time to be sent first
      strcpy(reply, "OK - Advert sent");
    } else if (memcmp(command, "clock sync", 10) == 0) {
      uint32_t curr = getRTCClock()->getCurrentTime();
      if (sender_timestamp > curr) {
        getRTCClock()->setCurrentTime(sender_timestamp + 1);
        uint32_t now = getRTCClock()->getCurrentTime();
        DateTime dt = DateTime(now);
        sprintf(reply, "OK - clock set: %02d:%02d - %d/%d/%d UTC", dt.hour(), dt.minute(), dt.day(), dt.month(), dt.year());
      } else {
        strcpy(reply, "ERR: clock cannot go backwards");
      }
    } else if (memcmp(command, "memory", 6) == 0) {
      sprintf(reply, "Free: %d, Min: %d, Max: %d, Queue: %d, IntFree: %d, IntMax: %d, PSRAM: %d/%d",
              ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(),
              _callbacks->getQueueSize(),
              (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
              (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
              (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
              (int)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    } else if (memcmp(command, "tls.bundletest ", 15) == 0) {
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
    } else if (memcmp(command, "start ota", 9) == 0) {
      if (!_board->startOTAUpdate(_prefs->node_name, reply)) {
        strcpy(reply, "Error");
      }
    } else if (memcmp(command, "clock", 5) == 0) {
      uint32_t now = getRTCClock()->getCurrentTime();
      DateTime dt = DateTime(now);
      sprintf(reply, "%02d:%02d - %d/%d/%d UTC", dt.hour(), dt.minute(), dt.day(), dt.month(), dt.year());
    } else if (memcmp(command, "time ", 5) == 0) {  // set time (to epoch seconds)
      uint32_t secs = _atoi(&command[5]);
      uint32_t curr = getRTCClock()->getCurrentTime();
      if (secs > curr) {
        getRTCClock()->setCurrentTime(secs);
        uint32_t now = getRTCClock()->getCurrentTime();
        DateTime dt = DateTime(now);
        sprintf(reply, "OK - clock set: %02d:%02d - %d/%d/%d UTC", dt.hour(), dt.minute(), dt.day(), dt.month(), dt.year());
      } else {
        strcpy(reply, "(ERR: clock cannot go backwards)");
      }
    } else if (memcmp(command, "neighbors", 9) == 0) {
      _callbacks->formatNeighborsReply(reply);
    } else if (memcmp(command, "neighbor.remove ", 16) == 0) {
      const char* hex = &command[16];
      uint8_t pubkey[PUB_KEY_SIZE];
      int hex_len = min((int)strlen(hex), PUB_KEY_SIZE*2);
      int pubkey_len = hex_len / 2;
      if (mesh::Utils::fromHex(pubkey, pubkey_len, hex)) {
        _callbacks->removeNeighbor(pubkey, pubkey_len);
        strcpy(reply, "OK");
      } else {
        strcpy(reply, "ERR: bad pubkey");
      }
    } else if (memcmp(command, "tempradio ", 10) == 0) {
      strcpy(tmp, &command[10]);
      const char *parts[5];
      int num = mesh::Utils::parseTextParts(tmp, parts, 5);
      float freq  = num > 0 ? strtof(parts[0], nullptr) : 0.0f;
      float bw    = num > 1 ? strtof(parts[1], nullptr) : 0.0f;
      uint8_t sf  = num > 2 ? atoi(parts[2]) : 0;
      uint8_t cr  = num > 3 ? atoi(parts[3]) : 0;
      int temp_timeout_mins  = num > 4 ? atoi(parts[4]) : 0;
      if (freq >= 150.0f && freq <= 2500.0f && sf >= 5 && sf <= 12 && cr >= 5 && cr <= 8 && bw >= 7.0f && bw <= 500.0f && temp_timeout_mins > 0) {
        _callbacks->applyTempRadioParams(freq, bw, sf, cr, temp_timeout_mins);
        sprintf(reply, "OK - temp params for %d mins", temp_timeout_mins);
      } else {
        strcpy(reply, "Error, invalid params");
      }
    } else if (memcmp(command, "password ", 9) == 0) {
      // change admin password
      StrHelper::strncpy(_prefs->password, &command[9], sizeof(_prefs->password));
      savePrefs();
      sprintf(reply, "password now: %s", _prefs->password);   // echo back just to let admin know for sure!!
    } else if (memcmp(command, "clear stats", 11) == 0) {
      _callbacks->clearStats();
      strcpy(reply, "(OK - stats reset)");
    } else if (memcmp(command, "alert test", 10) == 0 && (command[10] == 0 || command[10] == ' ')) {
      // Send a one-off test alert on the configured alert channel.
      const char* extra = command[10] == ' ' ? &command[11] : "";
      char text[120];
      if (*extra) {
        snprintf(text, sizeof(text), "[test] %s", extra);
      } else {
        strcpy(text, "[test] alert channel ok");
      }
      if (!_prefs->alert_psk_b64[0]) {
        strcpy(reply, "Error: alert channel not configured (set alert.psk or set alert.hashtag)");
      } else {
        bool ok = _callbacks->sendAlertText(text);
        strcpy(reply, ok ? "OK - alert sent" : "Error: alert send failed (bad PSK or PUBLIC key refused?)");
      }
    } else if (memcmp(command, "get ", 4) == 0) {
      handleGetCmd(sender_timestamp, command, reply);
    } else if (memcmp(command, "set ", 4) == 0) {
      handleSetCmd(sender_timestamp, command, reply);
    } else if (sender_timestamp == 0 && strcmp(command, "erase") == 0) {
      bool s = _callbacks->formatFileSystem();
      sprintf(reply, "File system erase: %s", s ? "OK" : "Err");
    } else if (memcmp(command, "ver", 3) == 0) {
      sprintf(reply, "%s (Build: %s)", _callbacks->getFirmwareVer(), _callbacks->getBuildDate());
    } else if (memcmp(command, "board", 5) == 0) {
      sprintf(reply, "%s", _board->getManufacturerName());
    } else if (memcmp(command, "sensor get ", 11) == 0) {
      const char* key = command + 11;
      const char* val = _sensors->getSettingByKey(key);
      if (val != NULL) {
        sprintf(reply, "> %s", val);
      } else {
        strcpy(reply, "null");
      }
    } else if (memcmp(command, "sensor set ", 11) == 0) {
      strcpy(tmp, &command[11]);
      const char *parts[2];
      int num = mesh::Utils::parseTextParts(tmp, parts, 2, ' ');
      const char *key = (num > 0) ? parts[0] : "";
      const char *value = (num > 1) ? parts[1] : "null";
      if (_sensors->setSettingValue(key, value)) {
        strcpy(reply, "ok");
      } else {
        strcpy(reply, "can't find custom var");
      }
    } else if (memcmp(command, "sensor list", 11) == 0) {
      char* dp = reply;
      int start = 0;
      int end = _sensors->getNumSettings();
      if (strlen(command) > 11) {
        start = _atoi(command+12);
      }
      if (start >= end) {
        strcpy(reply, "no custom var");
      } else {
        sprintf(dp, "%d vars\n", end);
        dp = strchr(dp, 0);
        int i;
        for (i = start; i < end && (dp-reply < 134); i++) {
          sprintf(dp, "%s=%s\n",
            _sensors->getSettingName(i),
            _sensors->getSettingValue(i));
          dp = strchr(dp, 0);
        }
        if (i < end) {
          sprintf(dp, "... next:%d", i);
        } else {
          *(dp-1) = 0; // remove last CR
        }
      }
    } else if (memcmp(command, "region", 6) == 0) {
      handleRegionCmd(command, reply);
#if ENV_INCLUDE_GPS == 1
    } else if (memcmp(command, "gps on", 6) == 0) {
      if (_sensors->setSettingValue("gps", "1")) {
        _prefs->gps_enabled = 1;
        savePrefs();
        strcpy(reply, "ok");
      } else {
        strcpy(reply, "gps toggle not found");
      }
    } else if (memcmp(command, "gps off", 7) == 0) {
      if (_sensors->setSettingValue("gps", "0")) {
        _prefs->gps_enabled = 0;
        savePrefs();
        strcpy(reply, "ok");
      } else {
        strcpy(reply, "gps toggle not found");
      }
    } else if (memcmp(command, "gps sync", 8) == 0) {
      LocationProvider * l = _sensors->getLocationProvider();
      if (l != NULL) {
        l->syncTime();
        strcpy(reply, "ok");
      } else {
        strcpy(reply, "gps provider not found");
      }
    } else if (memcmp(command, "gps setloc", 10) == 0) {
      _prefs->node_lat = _sensors->node_lat;
      _prefs->node_lon = _sensors->node_lon;
      savePrefs();
      strcpy(reply, "ok");
    } else if (memcmp(command, "gps advert", 10) == 0) {
      if (strlen(command) == 10) {
        switch (_prefs->advert_loc_policy) {
          case ADVERT_LOC_NONE:
            strcpy(reply, "> none");
            break;
          case ADVERT_LOC_PREFS:
            strcpy(reply, "> prefs");
            break;
          case ADVERT_LOC_SHARE:
            strcpy(reply, "> share");
            break;
          default:
            strcpy(reply, "error");
        }
      } else if (memcmp(command+11, "none", 4) == 0) {
        _prefs->advert_loc_policy = ADVERT_LOC_NONE;
        savePrefs();
        strcpy(reply, "ok");
      } else if (memcmp(command+11, "share", 5) == 0) {
        _prefs->advert_loc_policy = ADVERT_LOC_SHARE;
        savePrefs();
        strcpy(reply, "ok");
      } else if (memcmp(command+11, "prefs", 5) == 0) {
        _prefs->advert_loc_policy = ADVERT_LOC_PREFS;
        savePrefs();
        strcpy(reply, "ok");
      } else {
        strcpy(reply, "error");
      }
    } else if (memcmp(command, "gps", 3) == 0) {
      LocationProvider * l = _sensors->getLocationProvider();
      if (l != NULL) {
        bool enabled = l->isEnabled(); // is EN pin on ?
        bool fix = l->isValid();       // has fix ?
        int sats = l->satellitesCount();
        bool active = !strcmp(_sensors->getSettingByKey("gps"), "1");
        if (enabled) {
          sprintf(reply, "on, %s, %s, %d sats",
            active?"active":"deactivated",
            fix?"fix":"no fix",
            sats);
        } else {
          strcpy(reply, "off");
        }
      } else {
        strcpy(reply, "Can't find GPS");
      }
#endif
    } else if (memcmp(command, "powersaving on", 14) == 0) {
      _prefs->powersaving_enabled = 1;
      savePrefs();
      strcpy(reply, "ok"); // TODO: to return Not supported if required
    } else if (memcmp(command, "powersaving off", 15) == 0) {
      _prefs->powersaving_enabled = 0;
      savePrefs();
      strcpy(reply, "ok");
    } else if (memcmp(command, "powersaving", 11) == 0) {
      if (_prefs->powersaving_enabled) {
        strcpy(reply, "on");
      } else {
        strcpy(reply, "off");
      }
    } else if (memcmp(command, "log start", 9) == 0) {
      _callbacks->setLoggingOn(true);
      strcpy(reply, "   logging on");
    } else if (memcmp(command, "log stop", 8) == 0) {
      _callbacks->setLoggingOn(false);
      strcpy(reply, "   logging off");
    } else if (memcmp(command, "log erase", 9) == 0) {
      _callbacks->eraseLogFile();
      strcpy(reply, "   log erased");
    } else if (sender_timestamp == 0 && memcmp(command, "log", 3) == 0) {
      _callbacks->dumpLogFile();
      strcpy(reply, "   EOF");
    } else if (sender_timestamp == 0 && memcmp(command, "stats-packets", 13) == 0 && (command[13] == 0 || command[13] == ' ')) {
      _callbacks->formatPacketStatsReply(reply);
    } else if (sender_timestamp == 0 && memcmp(command, "stats-radio-diag", 16) == 0 && (command[16] == 0 || command[16] == ' ')) {
      _callbacks->formatRadioDiagReply(reply);
    } else if (sender_timestamp == 0 && memcmp(command, "stats-radio", 11) == 0 && (command[11] == 0 || command[11] == ' ')) {
      _callbacks->formatRadioStatsReply(reply);
    } else if (sender_timestamp == 0 && memcmp(command, "stats-core", 10) == 0 && (command[10] == 0 || command[10] == ' ')) {
      _callbacks->formatStatsReply(reply);
    } else {
      strcpy(reply, "Unknown command");
    }
}

void CommonCLI::handleSetCmd(uint32_t sender_timestamp, char* command, char* reply) {
  const char* config = &command[4];
  if (memcmp(config, "dutycycle ", 10) == 0) {
    float dc = atof(&config[10]);
    if (dc < 1 || dc > 100) {
      strcpy(reply, "ERROR: dutycycle must be 1-100");
    } else {
      _prefs->airtime_factor = (100.0f / dc) - 1.0f;
      savePrefs();
      float actual = 100.0f / (_prefs->airtime_factor + 1.0f);
      int a_int = (int)actual;
      int a_frac = (int)((actual - a_int) * 10.0f + 0.5f);
      sprintf(reply, "OK - %d.%d%%", a_int, a_frac);
    }
  } else if (memcmp(config, "af ", 3) == 0) {
    _prefs->airtime_factor = atof(&config[3]);
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "int.thresh ", 11) == 0) {
    _prefs->interference_threshold = atoi(&config[11]);
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "agc.reset.interval ", 19) == 0) {
    _prefs->agc_reset_interval = atoi(&config[19]) / 4;
    savePrefs();
    sprintf(reply, "OK - interval rounded to %d", ((uint32_t) _prefs->agc_reset_interval) * 4);
  } else if (memcmp(config, "radio.watchdog ", 15) == 0) {
    const char* val = &config[15];
    if (*val == 0) {
      strcpy(reply, "Error: missing radio.watchdog minutes");
      return;
    }
    for (const char* sp = val; *sp; sp++) {
      if (*sp < '0' || *sp > '9') {
        strcpy(reply, "Error: radio.watchdog must be an integer 0-120");
        return;
      }
    }
    int mins = atoi(val);
    if (mins > 120) {
      strcpy(reply, "Error: radio.watchdog must be 0-120 minutes");
    } else {
      _prefs->radio_watchdog_minutes = (uint8_t)mins;
      savePrefs();
      if (mins == 0) {
        strcpy(reply, "OK - radio watchdog disabled");
      } else {
        sprintf(reply, "OK - radio watchdog %d min", mins);
      }
    }
  } else if (memcmp(config, "multi.acks ", 11) == 0) {
    _prefs->multi_acks = atoi(&config[11]);
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "allow.read.only ", 16) == 0) {
    _prefs->allow_read_only = memcmp(&config[16], "on", 2) == 0;
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "flood.advert.interval ", 22) == 0) {
    int hours = _atoi(&config[22]);
    if ((hours > 0 && hours < 3) || (hours > 168)) {
      strcpy(reply, "Error: interval range is 3-168 hours");
    } else {
      _prefs->flood_advert_interval = (uint8_t)(hours);
      _callbacks->updateFloodAdvertTimer();
      savePrefs();
      strcpy(reply, "OK");
    }
  } else if (memcmp(config, "advert.interval ", 16) == 0) {
    int mins = _atoi(&config[16]);
    if ((mins > 0 && mins < MIN_LOCAL_ADVERT_INTERVAL) || (mins > 240)) {
      sprintf(reply, "Error: interval range is %d-240 minutes", MIN_LOCAL_ADVERT_INTERVAL);
    } else {
      _prefs->advert_interval = (uint8_t)(mins / 2);
      _callbacks->updateAdvertTimer();
      savePrefs();
      strcpy(reply, "OK");
    }
  } else if (memcmp(config, "guest.password ", 15) == 0) {
    StrHelper::strncpy(_prefs->guest_password, &config[15], sizeof(_prefs->guest_password));
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "prv.key ", 8) == 0) {
    uint8_t prv_key[PRV_KEY_SIZE];
    bool success = mesh::Utils::fromHex(prv_key, PRV_KEY_SIZE, &config[8]);
    // only allow rekey if key is valid
    if (success && mesh::LocalIdentity::validatePrivateKey(prv_key)) {
      mesh::LocalIdentity new_id;
      new_id.readFrom(prv_key, PRV_KEY_SIZE);
      _callbacks->saveIdentity(new_id);
      strcpy(reply, "OK, reboot to apply! New pubkey: ");
      mesh::Utils::toHex(&reply[33], new_id.pub_key, PUB_KEY_SIZE);
    } else {
      strcpy(reply, "Error, bad key");
    }
  } else if (memcmp(config, "name ", 5) == 0) {
    if (isValidName(&config[5])) {
      StrHelper::strncpy(_prefs->node_name, &config[5], sizeof(_prefs->node_name));
      savePrefs();
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Error, bad chars");
    }
  } else if (memcmp(config, "repeat ", 7) == 0) {
    _prefs->disable_fwd = memcmp(&config[7], "off", 3) == 0;
    savePrefs();
    strcpy(reply, _prefs->disable_fwd ? "OK - repeat is now OFF" : "OK - repeat is now ON");
#if defined(USE_SX1262) || defined(USE_SX1268)
  } else if (memcmp(config, "radio.rxgain ", 13) == 0) {
    _prefs->rx_boosted_gain = memcmp(&config[13], "on", 2) == 0;
    strcpy(reply, "OK");
    savePrefs();
    _callbacks->setRxBoostedGain(_prefs->rx_boosted_gain);
#endif
  } else if (memcmp(config, "radio ", 6) == 0) {
    strcpy(tmp, &config[6]);
    const char *parts[4];
    int num = mesh::Utils::parseTextParts(tmp, parts, 4);
    float freq  = num > 0 ? strtof(parts[0], nullptr) : 0.0f;
    float bw    = num > 1 ? strtof(parts[1], nullptr) : 0.0f;
    uint8_t sf  = num > 2 ? atoi(parts[2]) : 0;
    uint8_t cr  = num > 3 ? atoi(parts[3]) : 0;
    if (freq >= 150.0f && freq <= 2500.0f && sf >= 5 && sf <= 12 && cr >= 5 && cr <= 8 && bw >= 7.0f && bw <= 500.0f) {
      _prefs->sf = sf;
      _prefs->cr = cr;
      _prefs->freq = freq;
      _prefs->bw = bw;
      _callbacks->savePrefs();
      strcpy(reply, "OK - reboot to apply");
    } else {
      strcpy(reply, "Error, invalid radio params");
    }
  } else if (memcmp(config, "lat ", 4) == 0) {
    _prefs->node_lat = atof(&config[4]);
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "lon ", 4) == 0) {
    _prefs->node_lon = atof(&config[4]);
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "rxdelay ", 8) == 0) {
    float db = atof(&config[8]);
    if (db >= 0) {
      _prefs->rx_delay_base = db;
      savePrefs();
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Error, cannot be negative");
    }
  } else if (memcmp(config, "txdelay ", 8) == 0) {
    float f = atof(&config[8]);
    if (f >= 0) {
      _prefs->tx_delay_factor = f;
      savePrefs();
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Error, cannot be negative");
    }
  } else if (memcmp(config, "flood.max ", 10) == 0) {
    uint8_t m = atoi(&config[10]);
    if (m <= 64) {
      _prefs->flood_max = m;
      savePrefs();
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Error, max 64");
    }
  } else if (memcmp(config, "direct.txdelay ", 15) == 0) {
    float f = atof(&config[15]);
    if (f >= 0) {
      _prefs->direct_tx_delay_factor = f;
      savePrefs();
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Error, cannot be negative");
    }
  } else if (memcmp(config, "owner.info ", 11) == 0) {
    config += 11;
    char *dp = _prefs->owner_info;
    while (*config && dp - _prefs->owner_info < sizeof(_prefs->owner_info)-1) {
      *dp++ = (*config == '|') ? '\n' : *config;    // translate '|' to newline chars
      config++;
    }
    *dp = 0;
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "path.hash.mode ", 15) == 0) {
    config += 15;
    uint8_t mode = atoi(config);
    if (mode < 3) {
      _prefs->path_hash_mode = mode;
      savePrefs();
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Error, must be 0,1, or 2");
    }
  } else if (memcmp(config, "loop.detect ", 12) == 0) {
    config += 12;
    uint8_t mode;
    if (memcmp(config, "off", 3) == 0) {
      mode = LOOP_DETECT_OFF;
    } else if (memcmp(config, "minimal", 7) == 0) {
      mode = LOOP_DETECT_MINIMAL;
    } else if (memcmp(config, "moderate", 8) == 0) {
      mode = LOOP_DETECT_MODERATE;
    } else if (memcmp(config, "strict", 6) == 0) {
      mode = LOOP_DETECT_STRICT;
    } else {
      mode = 0xFF;
      strcpy(reply, "Error, must be: off, minimal, moderate, or strict");
    }
    if (mode != 0xFF) {
      _prefs->loop_detect = mode;
      savePrefs();
      strcpy(reply, "OK");
    }
  } else if (memcmp(config, "snmp.community ", 15) == 0) {
    StrHelper::strncpy(_prefs->snmp_community, &config[15], sizeof(_prefs->snmp_community));
    savePrefs();
    strcpy(reply, "OK - restart to apply");
  } else if (memcmp(config, "snmp ", 5) == 0) {
    _prefs->snmp_enabled = memcmp(&config[5], "on", 2) == 0;
    savePrefs();
    strcpy(reply, "OK - restart to apply");
  } else if (memcmp(config, "tx ", 3) == 0) {
    _prefs->tx_power_dbm = atoi(&config[3]);
    savePrefs();
    _callbacks->setTxPower(_prefs->tx_power_dbm);
    strcpy(reply, "OK");
  } else if (sender_timestamp == 0 && memcmp(config, "freq ", 5) == 0) {
    _prefs->freq = atof(&config[5]);
    savePrefs();
    strcpy(reply, "OK - reboot to apply");
#ifdef WITH_BRIDGE
  } else if (memcmp(config, "bridge.enabled ", 15) == 0) {
    _prefs->bridge_enabled = memcmp(&config[15], "on", 2) == 0;
    _callbacks->setBridgeState(_prefs->bridge_enabled);
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "bridge.delay ", 13) == 0) {
    int delay = _atoi(&config[13]);
    if (delay >= 0 && delay <= 10000) {
      _prefs->bridge_delay = (uint16_t)delay;
      savePrefs();
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Error: delay must be between 0-10000 ms");
    }
  } else if (memcmp(config, "bridge.source ", 14) == 0) {
    _prefs->bridge_pkt_src = memcmp(&config[14], "rx", 2) == 0;
#ifdef WITH_MQTT_BRIDGE
    if (_prefs->bridge_pkt_src == 1) {
      _prefs->mqtt_rx_enabled = 1;
      _prefs->mqtt_tx_enabled = 0;
    } else {
      _prefs->mqtt_rx_enabled = 0;
      _prefs->mqtt_tx_enabled = 1;
    }
#endif
    savePrefs();
    strcpy(reply, "OK");
#endif
#ifdef WITH_RS232_BRIDGE
  } else if (memcmp(config, "bridge.baud ", 12) == 0) {
    uint32_t baud = atoi(&config[12]);
    if (baud >= 9600 && baud <= BRIDGE_MAX_BAUD) {
      _prefs->bridge_baud = (uint32_t)baud;
      _callbacks->restartBridge();
      savePrefs();
      strcpy(reply, "OK");
    } else {
      sprintf(reply, "Error: baud rate must be between 9600-%d",BRIDGE_MAX_BAUD);
    }
#endif
#ifdef WITH_ESPNOW_BRIDGE
  } else if (memcmp(config, "bridge.channel ", 15) == 0) {
    int ch = atoi(&config[15]);
    if (ch > 0 && ch < 15) {
      _prefs->bridge_channel = (uint8_t)ch;
      _callbacks->restartBridge();
      savePrefs();
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Error: channel must be between 1-14");
    }
  } else if (memcmp(config, "bridge.secret ", 14) == 0) {
    StrHelper::strncpy(_prefs->bridge_secret, &config[14], sizeof(_prefs->bridge_secret));
    _callbacks->restartBridge();
    savePrefs();
    strcpy(reply, "OK");
#endif
#ifdef WITH_MQTT_BRIDGE
  } else if (strcmp(config, "mqtt.origin") == 0) {
    _prefs->mqtt_origin[0] = '\0';
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.origin ", 12) == 0) {
    StrHelper::strncpy(_prefs->mqtt_origin, &config[12], sizeof(_prefs->mqtt_origin));
    StrHelper::stripSurroundingQuotes(_prefs->mqtt_origin, sizeof(_prefs->mqtt_origin));
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.iata ", 10) == 0) {
    StrHelper::strncpy(_prefs->mqtt_iata, &config[10], sizeof(_prefs->mqtt_iata));
    for (int i = 0; _prefs->mqtt_iata[i]; i++) {
      _prefs->mqtt_iata[i] = toupper(_prefs->mqtt_iata[i]);
    }
    savePrefs();
    _callbacks->restartBridge();
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.status ", 12) == 0) {
    _prefs->mqtt_status_enabled = memcmp(&config[12], "on", 2) == 0;
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.packets ", 13) == 0) {
    _prefs->mqtt_packets_enabled = memcmp(&config[13], "on", 2) == 0;
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.raw ", 9) == 0) {
    _prefs->mqtt_raw_enabled = memcmp(&config[9], "on", 2) == 0;
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.tx ", 8) == 0) {
    if (memcmp(&config[8], "advert", 6) == 0) {
      _prefs->mqtt_tx_enabled = 2;
    } else {
      _prefs->mqtt_tx_enabled = memcmp(&config[8], "on", 2) == 0 ? 1 : 0;
    }
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.rx ", 8) == 0) {
    _prefs->mqtt_rx_enabled = memcmp(&config[8], "on", 2) == 0 ? 1 : 0;
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.interval ", 14) == 0) {
    uint32_t minutes = _atoi(&config[14]);
    if (minutes >= 1 && minutes <= 60) {
      _prefs->mqtt_status_interval = minutes * 60000;
      savePrefs();
      _callbacks->restartBridge();
      sprintf(reply, "OK - interval set to %u minutes (%lu ms), bridge restarted", minutes, (unsigned long)_prefs->mqtt_status_interval);
    } else {
      strcpy(reply, "Error: interval must be between 1-60 minutes");
    }
  } else if (memcmp(config, "wifi.ssid ", 10) == 0) {
    StrHelper::strncpy(_prefs->wifi_ssid, &config[10], sizeof(_prefs->wifi_ssid));
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "wifi.pwd ", 9) == 0) {
    StrHelper::strncpy(_prefs->wifi_password, &config[9], sizeof(_prefs->wifi_password));
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
      _prefs->wifi_power_save = ps_value;
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
    StrHelper::strncpy(_prefs->timezone_string, &config[9], sizeof(_prefs->timezone_string));
    savePrefs();
    strcpy(reply, "OK");
  } else if (memcmp(config, "timezone.offset ", 16) == 0) {
    int8_t offset = _atoi(&config[16]);
    if (offset >= -12 && offset <= 14) {
      _prefs->timezone_offset = offset;
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
            if (s != slot && strcmp(_prefs->mqtt_slot_preset[s], preset_name) == 0) {
              dup_slot = s;
              break;
            }
          }
        }
        if (dup_slot >= 0) {
          sprintf(reply, "Error: preset '%s' is already assigned to slot %d", preset_name, dup_slot + 1);
        } else {
          StrHelper::strncpy(_prefs->mqtt_slot_preset[slot], preset_name, sizeof(_prefs->mqtt_slot_preset[slot]));
          savePrefs();
          _callbacks->restartBridgeSlot(slot);
          // Check if the slot has everything it needs to connect
          const MQTTPresetDef* p = findMQTTPreset(preset_name);
          if (p && p->topic_style == MQTT_TOPIC_MESHRANK && _prefs->mqtt_slot_token[slot][0] == '\0') {
            sprintf(reply, "OK - slot %d preset: %s (run 'set mqtt%d.token <your_token>' to connect)", slot + 1, preset_name, slot + 1);
          } else if (p && p->topic_style == MQTT_TOPIC_MESHCORE &&
                     (strlen(_prefs->mqtt_iata) == 0 || strcmp(_prefs->mqtt_iata, "XXX") == 0)) {
            sprintf(reply, "OK - slot %d preset: %s (run 'set mqtt.iata <airport_code>' to publish)", slot + 1, preset_name);
          } else {
            sprintf(reply, "OK - slot %d preset: %s", slot + 1, preset_name);
          }
        }
      } else {
        strcpy(reply, "Error: unknown preset. Use 'get mqtt.presets'");
      }
    } else if (memcmp(subcmd, "server ", 7) == 0) {
      StrHelper::strncpy(_prefs->mqtt_slot_host[slot], &subcmd[7], sizeof(_prefs->mqtt_slot_host[slot]));
      savePrefs();
      strcpy(reply, "OK");
    } else if (memcmp(subcmd, "port ", 5) == 0) {
      int port = atoi(&subcmd[5]);
      if (port > 0 && port <= 65535) {
        _prefs->mqtt_slot_port[slot] = port;
        savePrefs();
        strcpy(reply, "OK");
      } else {
        strcpy(reply, "Error: port must be between 1 and 65535");
      }
    } else if (memcmp(subcmd, "username ", 9) == 0) {
      StrHelper::strncpy(_prefs->mqtt_slot_username[slot], &subcmd[9], sizeof(_prefs->mqtt_slot_username[slot]));
      savePrefs();
      strcpy(reply, "OK");
    } else if (memcmp(subcmd, "password ", 9) == 0) {
      StrHelper::strncpy(_prefs->mqtt_slot_password[slot], &subcmd[9], sizeof(_prefs->mqtt_slot_password[slot]));
      savePrefs();
      strcpy(reply, "OK");
    } else if (memcmp(subcmd, "token ", 6) == 0) {
      StrHelper::strncpy(_prefs->mqtt_slot_token[slot], &subcmd[6], sizeof(_prefs->mqtt_slot_token[slot]));
      savePrefs();
      _callbacks->restartBridgeSlot(slot);
      sprintf(reply, "OK - slot %d token set", slot + 1);
    } else if (memcmp(subcmd, "topic ", 6) == 0) {
      if (strcmp(_prefs->mqtt_slot_preset[slot], "custom") != 0) {
        sprintf(reply, "Error: topic template only applies to custom preset slots");
      } else {
        StrHelper::strncpy(_prefs->mqtt_slot_topic[slot], &subcmd[6], sizeof(_prefs->mqtt_slot_topic[slot]));
        savePrefs();
        _callbacks->restartBridgeSlot(slot);
        sprintf(reply, "OK - slot %d topic: %s", slot + 1, _prefs->mqtt_slot_topic[slot]);
      }
    } else if (memcmp(subcmd, "audience ", 9) == 0) {
      StrHelper::strncpy(_prefs->mqtt_slot_audience[slot], &subcmd[9], sizeof(_prefs->mqtt_slot_audience[slot]));
      savePrefs();
      _callbacks->restartBridgeSlot(slot);
      if (_prefs->mqtt_slot_audience[slot][0] != '\0') {
        sprintf(reply, "OK - slot %d JWT audience: %s", slot + 1, _prefs->mqtt_slot_audience[slot]);
      } else {
        sprintf(reply, "OK - slot %d JWT audience cleared (using username/password auth)", slot + 1);
      }
    } else if (memcmp(subcmd, "audience", 8) == 0 && subcmd[8] == '\0') {
      // "set mqttN.audience" with no value — clear the audience
      _prefs->mqtt_slot_audience[slot][0] = '\0';
      savePrefs();
      _callbacks->restartBridgeSlot(slot);
      sprintf(reply, "OK - slot %d JWT audience cleared (using username/password auth)", slot + 1);
    } else {
      sprintf(reply, "unknown config: %s", config);
    }
  } else if (memcmp(config, "mqtt.analyzer.us ", 17) == 0) {
    const int slot = 0;
    if (memcmp(&config[17], "on", 2) == 0) {
      StrHelper::strncpy(_prefs->mqtt_slot_preset[slot], "analyzer-us", sizeof(_prefs->mqtt_slot_preset[slot]));
    } else {
      StrHelper::strncpy(_prefs->mqtt_slot_preset[slot], MQTT_PRESET_NONE, sizeof(_prefs->mqtt_slot_preset[slot]));
    }
    savePrefs();
    _callbacks->restartBridgeSlot(slot);
    strcpy(reply, "OK");
  } else if (memcmp(config, "mqtt.analyzer.eu ", 17) == 0) {
    const int slot = 1;
    if (memcmp(&config[17], "on", 2) == 0) {
      StrHelper::strncpy(_prefs->mqtt_slot_preset[slot], "analyzer-eu", sizeof(_prefs->mqtt_slot_preset[slot]));
    } else {
      StrHelper::strncpy(_prefs->mqtt_slot_preset[slot], MQTT_PRESET_NONE, sizeof(_prefs->mqtt_slot_preset[slot]));
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
        StrHelper::strncpy(_prefs->mqtt_owner_public_key, owner_key, sizeof(_prefs->mqtt_owner_public_key));
        savePrefs();
        strcpy(reply, "OK");
      } else {
        strcpy(reply, "Error: invalid hex characters in public key");
      }
    } else {
      strcpy(reply, "Error: public key must be 64 hex characters (32 bytes)");
    }
  } else if (memcmp(config, "mqtt.email ", 11) == 0) {
    StrHelper::strncpy(_prefs->mqtt_email, &config[11], sizeof(_prefs->mqtt_email));
    savePrefs();
    strcpy(reply, "OK");
#endif
  } else if (memcmp(config, "alert ", 6) == 0) {
    // set alert on|off
    const char* val = &config[6];
    if (memcmp(val, "on", 2) == 0 && (val[2] == 0 || val[2] == ' ')) {
      _prefs->alert_enabled = 1;
      savePrefs();
      _callbacks->onAlertConfigChanged();
      strcpy(reply, "OK - alerts on");
    } else if (memcmp(val, "off", 3) == 0 && (val[3] == 0 || val[3] == ' ')) {
      _prefs->alert_enabled = 0;
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
      _prefs->alert_psk_b64[0] = '\0';
      _prefs->alert_hashtag[0] = '\0';
      savePrefs();
      _callbacks->onAlertConfigChanged();
      strcpy(reply, "OK - alert.psk cleared (alerts disabled until configured)");
    } else if (len >= sizeof(_prefs->alert_psk_b64)) {
      strcpy(reply, "Error: PSK too long (max 47 chars)");
    } else if (val[0] == '#') {
      strcpy(reply, "Error: use 'set alert.hashtag' for hashtag channels");
    } else {
      // Accept either:
      //   - base64: 24 chars (16-byte secret) or 44 chars (32-byte secret) —
      //     the format `BaseChatMesh::addChannel` already expects.
      //   - hex:    32 chars (16-byte secret) or 64 chars (32-byte secret) —
      //     what the MeshCore mobile app's "share" button emits for both
      //     private channels and hashtag channels.
      // Hex input is converted to base64 once at CLI time so the on-disk
      // representation (and AlertReporter's decode path) stays unchanged.
      char canonical_b64[48];
      const char* store = nullptr;

      if (len == 32 || len == 64) {
        bool all_hex = true;
        for (size_t i = 0; i < len; i++) {
          if (!mesh::Utils::isHexChar(val[i])) { all_hex = false; break; }
        }
        if (all_hex) {
          uint8_t raw[32];
          int raw_len = (int)(len / 2);
          if (mesh::Utils::fromHex(raw, raw_len, val)) {
            size_t b64_len = alert_encode_base64(raw, (size_t)raw_len, canonical_b64, sizeof(canonical_b64));
            if (b64_len > 0 && b64_len < sizeof(_prefs->alert_psk_b64)) {
              store = canonical_b64;
            }
          }
        }
      }
      if (!store && (len == 24 || len == 44)) {
        store = val;
      }

      if (!store) {
        strcpy(reply, "Error: PSK must be 32/64 hex chars or 24/44 chars base64 (16- or 32-byte secret)");
      } else if (const char* banned = alertReporterBannedChannelMatchB64(store)) {
        // Refuse any key on the banned channel list (Public PSK, well-known
        // auto-responder hashtags like #test/#bot, etc.). Fault alerts on
        // those channels would spam every node in the area.
        sprintf(reply, "Error: refusing banned channel '%s'; pick a private key or hashtag", banned);
      } else {
        StrHelper::strncpy(_prefs->alert_psk_b64, store, sizeof(_prefs->alert_psk_b64));
        // The new PSK is operator-supplied, so any previously-derived hashtag
        // name is no longer accurate provenance — drop it.
        _prefs->alert_hashtag[0] = '\0';
        savePrefs();
        _callbacks->onAlertConfigChanged();
        strcpy(reply, "OK - alert.psk updated");
      }
    }
  } else if (memcmp(config, "alert.hashtag", 13) == 0 && (config[13] == 0 || config[13] == ' ')) {
    const char* val = (config[13] == ' ') ? &config[14] : "";
    while (*val == ' ') val++;
    size_t in_len = strlen(val);
    if (in_len == 0) {
      _prefs->alert_psk_b64[0] = '\0';
      _prefs->alert_hashtag[0] = '\0';
      savePrefs();
      _callbacks->onAlertConfigChanged();
      strcpy(reply, "OK - alert.hashtag cleared (alerts disabled until configured)");
    } else {
      // Canonical stored form is "#name" because the leading '#' is part of
      // the sha256 input (matching the companion-app hashtag-channel
      // derivation in docs/companion_protocol.md). Accept the user typing
      // either "alerts" or "#alerts".
      char hashtag[sizeof(_prefs->alert_hashtag)];
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
        // then base64-encode and store in alert_psk_b64. We don't re-derive
        // on every send — operators can later override with `set alert.psk`
        // without leaving stale hashtag text behind.
        uint8_t digest[32];
        mesh::Utils::sha256(digest, sizeof(digest),
                            (const uint8_t*)hashtag, (int)strlen(hashtag));
        if (const char* banned = alertReporterBannedChannelMatch(digest)) {
          // Hashtag derives to a banned key (e.g. `set alert.hashtag test`
          // hits the #test entry). Refuse before clobbering existing config.
          sprintf(reply, "Error: refusing banned channel '%s'", banned);
        } else {
          char b64[48];
          size_t b64_len = alert_encode_base64(digest, 16, b64, sizeof(b64));
          if (b64_len == 0 || b64_len >= sizeof(_prefs->alert_psk_b64)) {
            strcpy(reply, "Error: failed to derive PSK from hashtag");
          } else {
            StrHelper::strncpy(_prefs->alert_hashtag, hashtag, sizeof(_prefs->alert_hashtag));
            StrHelper::strncpy(_prefs->alert_psk_b64, b64, sizeof(_prefs->alert_psk_b64));
            savePrefs();
            _callbacks->onAlertConfigChanged();
            sprintf(reply, "OK - alert.hashtag: %s", _prefs->alert_hashtag);
          }
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
      _prefs->alert_region[0] = '\0';
      savePrefs();
      _callbacks->onAlertConfigChanged();
      strcpy(reply, "OK - alert.region cleared (using default scope)");
    } else if (len >= sizeof(_prefs->alert_region)) {
      strcpy(reply, "Error: alert.region too long");
    } else {
      StrHelper::strncpy(_prefs->alert_region, val, sizeof(_prefs->alert_region));
      StrHelper::stripSurroundingQuotes(_prefs->alert_region, sizeof(_prefs->alert_region));
      savePrefs();
      _callbacks->onAlertConfigChanged();
      sprintf(reply, "OK - alert.region: %s", _prefs->alert_region);
    }
  } else if (memcmp(config, "alert.wifi ", 11) == 0) {
    int mins = (int)_atoi(&config[11]);
    if (mins < 0 || mins > 1440) {
      strcpy(reply, "Error: alert.wifi must be 0-1440 minutes (0=off)");
    } else {
      _prefs->alert_wifi_minutes = (uint16_t)mins;
      savePrefs();
      sprintf(reply, "OK - alert.wifi %d min%s", mins, mins == 0 ? " (disabled)" : "");
    }
  } else if (memcmp(config, "alert.mqtt ", 11) == 0) {
    int mins = (int)_atoi(&config[11]);
    if (mins < 0 || mins > 10080) {
      strcpy(reply, "Error: alert.mqtt must be 0-10080 minutes (0=off)");
    } else {
      _prefs->alert_mqtt_minutes = (uint16_t)mins;
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
      _prefs->alert_min_interval_min = (uint16_t)mins;
      savePrefs();
      sprintf(reply, "OK - alert.interval %d min", mins);
    }
  } else if (memcmp(config, "adc.multiplier ", 15) == 0) {
    _prefs->adc_multiplier = atof(&config[15]);
    if (_board->setAdcMultiplier(_prefs->adc_multiplier)) {
      savePrefs();
      if (_prefs->adc_multiplier == 0.0f) {
        strcpy(reply, "OK - using default board multiplier");
      } else {
        sprintf(reply, "OK - multiplier set to %.3f", _prefs->adc_multiplier);
      }
    } else {
      _prefs->adc_multiplier = 0.0f;
      strcpy(reply, "Error: unsupported by this board");
    };
  } else {
    sprintf(reply, "unknown config: %s", config);
  }
}

void CommonCLI::handleGetCmd(uint32_t sender_timestamp, char* command, char* reply) {
  const char* config = &command[4];
  if (memcmp(config, "dutycycle", 9) == 0) {
    float dc = 100.0f / (_prefs->airtime_factor + 1.0f);
    int dc_int = (int)dc;
    int dc_frac = (int)((dc - dc_int) * 10.0f + 0.5f);
    sprintf(reply, "> %d.%d%%", dc_int, dc_frac);
  } else if (memcmp(config, "af", 2) == 0) {
    sprintf(reply, "> %s", StrHelper::ftoa(_prefs->airtime_factor));
  } else if (memcmp(config, "int.thresh", 10) == 0) {
    sprintf(reply, "> %d", (uint32_t) _prefs->interference_threshold);
  } else if (memcmp(config, "agc.reset.interval", 18) == 0) {
    sprintf(reply, "> %d", ((uint32_t) _prefs->agc_reset_interval) * 4);
  } else if (memcmp(config, "radio.watchdog", 14) == 0) {
    sprintf(reply, "> %d", (uint32_t)_prefs->radio_watchdog_minutes);
  } else if (memcmp(config, "multi.acks", 10) == 0) {
    sprintf(reply, "> %d", (uint32_t) _prefs->multi_acks);
  } else if (memcmp(config, "allow.read.only", 15) == 0) {
    sprintf(reply, "> %s", _prefs->allow_read_only ? "on" : "off");
  } else if (memcmp(config, "flood.advert.interval", 21) == 0) {
    sprintf(reply, "> %d", ((uint32_t) _prefs->flood_advert_interval));
  } else if (memcmp(config, "advert.interval", 15) == 0) {
    sprintf(reply, "> %d", ((uint32_t) _prefs->advert_interval) * 2);
  } else if (memcmp(config, "guest.password", 14) == 0) {
    sprintf(reply, "> %s", _prefs->guest_password);
  } else if (sender_timestamp == 0 && memcmp(config, "prv.key", 7) == 0) {  // from serial command line only
    uint8_t prv_key[PRV_KEY_SIZE];
    int len = _callbacks->getSelfId().writeTo(prv_key, PRV_KEY_SIZE);
    mesh::Utils::toHex(tmp, prv_key, len);
    sprintf(reply, "> %s", tmp);
  } else if (memcmp(config, "name", 4) == 0) {
    sprintf(reply, "> %s", _prefs->node_name);
  } else if (memcmp(config, "repeat", 6) == 0) {
    sprintf(reply, "> %s", _prefs->disable_fwd ? "off" : "on");
  } else if (memcmp(config, "lat", 3) == 0) {
    sprintf(reply, "> %s", StrHelper::ftoa(_prefs->node_lat));
  } else if (memcmp(config, "lon", 3) == 0) {
    sprintf(reply, "> %s", StrHelper::ftoa(_prefs->node_lon));
#if defined(USE_SX1262) || defined(USE_SX1268)
  } else if (memcmp(config, "radio.rxgain", 12) == 0) {
    sprintf(reply, "> %s", _prefs->rx_boosted_gain ? "on" : "off");
#endif
  } else if (memcmp(config, "radio", 5) == 0) {
    char freq[16], bw[16];
    strcpy(freq, StrHelper::ftoa(_prefs->freq));
    strcpy(bw, StrHelper::ftoa3(_prefs->bw));
    sprintf(reply, "> %s,%s,%d,%d", freq, bw, (uint32_t)_prefs->sf, (uint32_t)_prefs->cr);
  } else if (memcmp(config, "rxdelay", 7) == 0) {
    sprintf(reply, "> %s", StrHelper::ftoa(_prefs->rx_delay_base));
  } else if (memcmp(config, "txdelay", 7) == 0) {
    sprintf(reply, "> %s", StrHelper::ftoa(_prefs->tx_delay_factor));
  } else if (memcmp(config, "flood.max", 9) == 0) {
    sprintf(reply, "> %d", (uint32_t)_prefs->flood_max);
  } else if (memcmp(config, "direct.txdelay", 14) == 0) {
    sprintf(reply, "> %s", StrHelper::ftoa(_prefs->direct_tx_delay_factor));
  } else if (memcmp(config, "owner.info", 10) == 0) {
    *reply++ = '>';
    *reply++ = ' ';
    const char* sp = _prefs->owner_info;
    while (*sp) {
      *reply++ = (*sp == '\n') ? '|' : *sp;    // translate newline back to orig '|'
      sp++;
    }
    *reply = 0;  // set null terminator
  } else if (memcmp(config, "path.hash.mode", 14) == 0) {
    sprintf(reply, "> %d", (uint32_t)_prefs->path_hash_mode);
  } else if (memcmp(config, "loop.detect", 11) == 0) {
    if (_prefs->loop_detect == LOOP_DETECT_OFF) {
      strcpy(reply, "> off");
    } else if (_prefs->loop_detect == LOOP_DETECT_MINIMAL) {
      strcpy(reply, "> minimal");
    } else if (_prefs->loop_detect == LOOP_DETECT_MODERATE) {
      strcpy(reply, "> moderate");
    } else {
      strcpy(reply, "> strict");
    }
  } else if (memcmp(config, "snmp.community", 14) == 0) {
    sprintf(reply, "> %s", _prefs->snmp_community);
  } else if (memcmp(config, "snmp", 4) == 0 && (config[4] == '\0' || config[4] == '\n' || config[4] == '\r')) {
    strcpy(reply, _prefs->snmp_enabled ? "> on" : "> off");
  } else if (memcmp(config, "tx", 2) == 0 && (config[2] == 0 || config[2] == ' ')) {
    sprintf(reply, "> %d", (int32_t) _prefs->tx_power_dbm);
  } else if (memcmp(config, "freq", 4) == 0) {
    sprintf(reply, "> %s", StrHelper::ftoa(_prefs->freq));
  } else if (memcmp(config, "public.key", 10) == 0) {
    strcpy(reply, "> ");
    mesh::Utils::toHex(&reply[2], _callbacks->getSelfId().pub_key, PUB_KEY_SIZE);
  } else if (memcmp(config, "role", 4) == 0) {
    sprintf(reply, "> %s", _callbacks->getRole());
  } else if (memcmp(config, "bridge.type", 11) == 0) {
    sprintf(reply, "> %s",
#ifdef WITH_RS232_BRIDGE
            "rs232"
#elif WITH_ESPNOW_BRIDGE
            "espnow"
#else
            "none"
#endif
    );
#ifdef WITH_BRIDGE
  } else if (memcmp(config, "bridge.enabled", 14) == 0) {
    sprintf(reply, "> %s", _prefs->bridge_enabled ? "on" : "off");
  } else if (memcmp(config, "bridge.delay", 12) == 0) {
    sprintf(reply, "> %d", (uint32_t)_prefs->bridge_delay);
  } else if (memcmp(config, "bridge.source", 13) == 0) {
    sprintf(reply, "> %s", _prefs->bridge_pkt_src ? "logRx" : "logTx");
#endif
#ifdef WITH_RS232_BRIDGE
  } else if (memcmp(config, "bridge.baud", 11) == 0) {
    sprintf(reply, "> %d", (uint32_t)_prefs->bridge_baud);
#endif
#ifdef WITH_ESPNOW_BRIDGE
  } else if (memcmp(config, "bridge.channel", 14) == 0) {
    sprintf(reply, "> %d", (uint32_t)_prefs->bridge_channel);
  } else if (memcmp(config, "bridge.secret", 13) == 0) {
    sprintf(reply, "> %s", _prefs->bridge_secret);
#endif
#ifdef WITH_MQTT_BRIDGE
  } else if (memcmp(config, "mqtt.origin", 11) == 0) {
    char effective_origin[32];
    MQTTBridge::getEffectiveMqttOrigin(_prefs, effective_origin, sizeof(effective_origin));
    sprintf(reply, "> %s", effective_origin);
  } else if (memcmp(config, "mqtt.iata", 9) == 0) {
    sprintf(reply, "> %s", _prefs->mqtt_iata);
  } else if (memcmp(config, "mqtt.presets", 12) == 0 && (config[12] == '\0' || config[12] == ' ')) {
    int start = 0;
    if (config[12] == ' ') {
      const char* start_arg = &config[13];
      if (*start_arg == '\0') {
        strcpy(reply, "Error: usage get mqtt.presets [start]");
        return;
      }
      for (const char* sp = start_arg; *sp; sp++) {
        if (*sp < '0' || *sp > '9') {
          strcpy(reply, "Error: usage get mqtt.presets [start]");
          return;
        }
      }
      start = (int)_atoi(start_arg);
    }
    formatMQTTPresetListReply(reply, 160, start);
  } else if (memcmp(config, "mqtt.status", 11) == 0) {
    MQTTBridge::formatMqttStatusReply(reply, 160, _prefs);
  } else if (memcmp(config, "mqtt.packets", 12) == 0) {
    sprintf(reply, "> %s", _prefs->mqtt_packets_enabled ? "on" : "off");
  } else if (memcmp(config, "mqtt.raw", 8) == 0) {
    sprintf(reply, "> %s", _prefs->mqtt_raw_enabled ? "on" : "off");
  } else if (memcmp(config, "mqtt.tx", 7) == 0) {
    const char* tx_str = _prefs->mqtt_tx_enabled == 2 ? "advert" : (_prefs->mqtt_tx_enabled ? "on" : "off");
    sprintf(reply, "> %s", tx_str);
  } else if (memcmp(config, "mqtt.rx", 7) == 0) {
    sprintf(reply, "> %s", _prefs->mqtt_rx_enabled ? "on" : "off");
  } else if (memcmp(config, "mqtt.interval", 13) == 0) {
    uint32_t minutes = (_prefs->mqtt_status_interval + 29999) / 60000;
    sprintf(reply, "> %u minutes (%lu ms)", minutes, (unsigned long)_prefs->mqtt_status_interval);
  } else if (config[0] == 'm' && config[1] == 'q' && config[2] == 't' && config[3] == 't' &&
             config[4] >= '1' && config[4] <= ('0' + MAX_MQTT_SLOTS) && config[5] == '.') {
    // Slot-based commands: get mqtt1.preset, get mqtt1.server, etc.
    int slot = config[4] - '1'; // 0-5
    const char* subcmd = &config[6];
    if (memcmp(subcmd, "preset", 6) == 0) {
      sprintf(reply, "> %s", _prefs->mqtt_slot_preset[slot]);
    } else if (memcmp(subcmd, "server", 6) == 0) {
      sprintf(reply, "> %s", _prefs->mqtt_slot_host[slot]);
    } else if (memcmp(subcmd, "port", 4) == 0) {
      sprintf(reply, "> %d", _prefs->mqtt_slot_port[slot]);
    } else if (memcmp(subcmd, "username", 8) == 0) {
      sprintf(reply, "> %s", _prefs->mqtt_slot_username[slot]);
    } else if (memcmp(subcmd, "password", 8) == 0) {
      sprintf(reply, "> %s", _prefs->mqtt_slot_password[slot]);
    } else if (memcmp(subcmd, "token", 5) == 0) {
      if (_prefs->mqtt_slot_token[slot][0] != '\0') {
        sprintf(reply, "> %s", _prefs->mqtt_slot_token[slot]);
      } else {
        strcpy(reply, "> (not set)");
      }
    } else if (memcmp(subcmd, "topic", 5) == 0) {
      if (_prefs->mqtt_slot_topic[slot][0] != '\0') {
        sprintf(reply, "> %s", _prefs->mqtt_slot_topic[slot]);
      } else {
        strcpy(reply, "> (default: meshcore/{iata}/{device}/{type})");
      }
    } else if (memcmp(subcmd, "audience", 8) == 0) {
      if (_prefs->mqtt_slot_audience[slot][0] != '\0') {
        sprintf(reply, "> %s", _prefs->mqtt_slot_audience[slot]);
      } else {
        strcpy(reply, "> (not set — custom slots use username/password auth)");
      }
    } else if (memcmp(subcmd, "diag", 4) == 0) {
      MQTTBridge::formatSlotDiagReply(reply, 160, slot);
    } else {
      sprintf(reply, "??: %s", config);
    }
  } else if (memcmp(config, "wifi.ssid", 9) == 0) {
    sprintf(reply, "> %s", _prefs->wifi_ssid);
  } else if (memcmp(config, "wifi.pwd", 8) == 0) {
    sprintf(reply, "> %s", _prefs->wifi_password);
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
        size_t len = strlen(reply);
        const size_t reply_remaining = 128;
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
    uint8_t ps = _prefs->wifi_power_save;
    const char* ps_name = (ps == 1) ? "none" : (ps == 2) ? "max" : "min";
    sprintf(reply, "> %s", ps_name);
  } else if (memcmp(config, "timezone", 8) == 0) {
    sprintf(reply, "> %s", _prefs->timezone_string);
  } else if (memcmp(config, "timezone.offset", 15) == 0) {
    sprintf(reply, "> %d", _prefs->timezone_offset);
  } else if (memcmp(config, "mqtt.analyzer.us", 17) == 0) {
    sprintf(reply, "> %s", strcmp(_prefs->mqtt_slot_preset[0], "analyzer-us") == 0 ? "on" : "off");
  } else if (memcmp(config, "mqtt.analyzer.eu", 17) == 0) {
    sprintf(reply, "> %s", strcmp(_prefs->mqtt_slot_preset[1], "analyzer-eu") == 0 ? "on" : "off");
  } else if (sender_timestamp == 0 && memcmp(config, "mqtt.owner", 10) == 0) {
    if (_prefs->mqtt_owner_public_key[0] != '\0') {
      sprintf(reply, "> %s", _prefs->mqtt_owner_public_key);
    } else {
      strcpy(reply, "> (not set)");
    }
  } else if (sender_timestamp == 0 && memcmp(config, "mqtt.email", 10) == 0) {
    if (_prefs->mqtt_email[0] != '\0') {
      sprintf(reply, "> %s", _prefs->mqtt_email);
    } else {
      strcpy(reply, "> (not set)");
    }
  } else if (memcmp(config, "mqtt.config.valid", 17) == 0) {
    bool valid = MQTTBridge::isConfigValid(_prefs);
    sprintf(reply, "> %s", valid ? "valid" : "invalid");
#endif
  } else if (memcmp(config, "bootloader.ver", 14) == 0) {
  #ifdef NRF52_PLATFORM
      char ver[32];
      if (_board->getBootloaderVersion(ver, sizeof(ver))) {
          sprintf(reply, "> %s", ver);
      } else {
          strcpy(reply, "> unknown");
      }
  #else
      strcpy(reply, "ERROR: unsupported");
  #endif
  } else if (memcmp(config, "alert.hashtag", 13) == 0) {
    sprintf(reply, "> %s", _prefs->alert_hashtag[0] ? _prefs->alert_hashtag : "(unset)");
  } else if (memcmp(config, "alert.psk", 9) == 0) {
    sprintf(reply, "> %s", _prefs->alert_psk_b64[0] ? _prefs->alert_psk_b64 : "(unset)");
  } else if (memcmp(config, "alert.region", 12) == 0) {
    sprintf(reply, "> %s", _prefs->alert_region[0] ? _prefs->alert_region : "(unset, using default scope)");
  } else if (memcmp(config, "alert.wifi", 10) == 0) {
    sprintf(reply, "> %u min%s", (unsigned)_prefs->alert_wifi_minutes,
            _prefs->alert_wifi_minutes == 0 ? " (disabled)" : "");
  } else if (memcmp(config, "alert.mqtt", 10) == 0) {
    sprintf(reply, "> %u min%s", (unsigned)_prefs->alert_mqtt_minutes,
            _prefs->alert_mqtt_minutes == 0 ? " (disabled)" : "");
  } else if (memcmp(config, "alert.interval", 14) == 0) {
    sprintf(reply, "> %u min", (unsigned)_prefs->alert_min_interval_min);
  } else if (memcmp(config, "alert", 5) == 0 && (config[5] == 0 || config[5] == '\n' || config[5] == '\r')) {
    sprintf(reply, "> %s", _prefs->alert_enabled ? "on" : "off");
  } else if (memcmp(config, "adc.multiplier", 14) == 0) {
    float adc_mult = _board->getAdcMultiplier();
    if (adc_mult == 0.0f) {
      strcpy(reply, "Error: unsupported by this board");
    } else {
      sprintf(reply, "> %.3f", adc_mult);
    }
  // Power management commands
  } else if (memcmp(config, "pwrmgt.support", 14) == 0) {
#ifdef NRF52_POWER_MANAGEMENT
    strcpy(reply, "> supported");
#else
    strcpy(reply, "> unsupported");
#endif
  } else if (memcmp(config, "pwrmgt.source", 13) == 0) {
#ifdef NRF52_POWER_MANAGEMENT
    strcpy(reply, _board->isExternalPowered() ? "> external" : "> battery");
#else
    strcpy(reply, "ERROR: Power management not supported");
#endif
  } else if (memcmp(config, "pwrmgt.bootreason", 17) == 0) {
#ifdef NRF52_POWER_MANAGEMENT
    sprintf(reply, "> Reset: %s; Shutdown: %s",
      _board->getResetReasonString(_board->getResetReason()),
      _board->getShutdownReasonString(_board->getShutdownReason()));
#else
    strcpy(reply, "ERROR: Power management not supported");
#endif
  } else if (memcmp(config, "pwrmgt.bootmv", 13) == 0) {
#ifdef NRF52_POWER_MANAGEMENT
    sprintf(reply, "> %u mV", _board->getBootVoltage());
#else
    strcpy(reply, "ERROR: Power management not supported");
#endif
  } else {
    sprintf(reply, "??: %s", config);
  }
}

void CommonCLI::handleRegionCmd(char* command, char* reply) {
  reply[0] = 0;

  const char* parts[4];
  int n = mesh::Utils::parseTextParts(command, parts, 4, ' ');
  if (n == 1) {
    _region_map->exportTo(reply, 160);
  } else if (n >= 2 && strcmp(parts[1], "load") == 0) {
    _callbacks->startRegionsLoad();
  } else if (n >= 2 && strcmp(parts[1], "save") == 0) {
    _prefs->discovery_mod_timestamp = getRTCClock()->getCurrentTime();   // this node is now 'modified' (for discovery info)
    savePrefs();
    bool success = _callbacks->saveRegions();
    strcpy(reply, success ? "OK" : "Err - save failed");
  } else if (n >= 3 && strcmp(parts[1], "allowf") == 0) {
    auto region = _region_map->findByNamePrefix(parts[2]);
    if (region) {
      region->flags &= ~REGION_DENY_FLOOD;
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - unknown region");
    }
  } else if (n >= 3 && strcmp(parts[1], "denyf") == 0) {
    auto region = _region_map->findByNamePrefix(parts[2]);
    if (region) {
      region->flags |= REGION_DENY_FLOOD;
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - unknown region");
    }
  } else if (n >= 3 && strcmp(parts[1], "get") == 0) {
    auto region = _region_map->findByNamePrefix(parts[2]);
    if (region) {
      auto parent = _region_map->findById(region->parent);
      if (parent && parent->id != 0) {
        sprintf(reply, " %s (%s) %s", region->name, parent->name, (region->flags & REGION_DENY_FLOOD) ? "" : "F");
      } else {
        sprintf(reply, " %s %s", region->name, (region->flags & REGION_DENY_FLOOD) ? "" : "F");
      }
    } else {
      strcpy(reply, "Err - unknown region");
    }
  } else if (n >= 3 && strcmp(parts[1], "home") == 0) {
    auto home = _region_map->findByNamePrefix(parts[2]);
    if (home) {
      _region_map->setHomeRegion(home);
      sprintf(reply, " home is now %s", home->name);
    } else {
      strcpy(reply, "Err - unknown region");
    }
  } else if (n == 2 && strcmp(parts[1], "home") == 0) {
    auto home = _region_map->getHomeRegion();
    sprintf(reply, " home is %s", home ? home->name : "*");
  } else if (n >= 3 && strcmp(parts[1], "default") == 0) {
    if (strcmp(parts[2], "<null>") == 0) {
      _region_map->setDefaultRegion(NULL);
      _callbacks->onDefaultRegionChanged(NULL);
      _callbacks->saveRegions();  // persist in one atomic step
      sprintf(reply, " default scope is now <null>");
    } else {
      auto def = _region_map->findByNamePrefix(parts[2]);
      if (def == NULL) {
        def = _region_map->putRegion(parts[2], 0);  // auto-create the default region
      }
      if (def) {
        def->flags = 0;   // make sure allow flood enabled
        _region_map->setDefaultRegion(def);
        _callbacks->onDefaultRegionChanged(def);
        _callbacks->saveRegions();  // persist in one atomic step
        sprintf(reply, " default scope is now %s", def->name);
      } else {
        strcpy(reply, "Err - region table full");
      }
    }
  } else if (n == 2 && strcmp(parts[1], "default") == 0) {
    auto def = _region_map->getDefaultRegion();
    sprintf(reply, " default scope is %s", def ? def->name : "<null>");
  } else if (n >= 3 && strcmp(parts[1], "put") == 0) {
    auto parent = n >= 4 ? _region_map->findByNamePrefix(parts[3]) : &(_region_map->getWildcard());
    if (parent == NULL) {
      strcpy(reply, "Err - unknown parent");
    } else {
      auto region = _region_map->putRegion(parts[2], parent->id);
      if (region == NULL) {
        strcpy(reply, "Err - unable to put");
      } else {
        region->flags = 0;   // New default: enable flood
        strcpy(reply, "OK - (flood allowed)");
      }
    }
  } else if (n >= 3 && strcmp(parts[1], "remove") == 0) {
    auto region = _region_map->findByName(parts[2]);
    if (region) {
      if (_region_map->removeRegion(*region)) {
        strcpy(reply, "OK");
      } else {
        strcpy(reply, "Err - not empty");
      }
    } else {
      strcpy(reply, "Err - not found");
    }
  } else if (n >= 3 && strcmp(parts[1], "list") == 0) {
    uint8_t mask = 0;
    bool invert = false;
    
    if (strcmp(parts[2], "allowed") == 0) {
      mask = REGION_DENY_FLOOD;
      invert = false;  // list regions that DON'T have DENY flag
    } else if (strcmp(parts[2], "denied") == 0) {
      mask = REGION_DENY_FLOOD;
      invert = true;   // list regions that DO have DENY flag
    } else {
      strcpy(reply, "Err - use 'allowed' or 'denied'");
      return;
    }
    
    int len = _region_map->exportNamesTo(reply, 160, mask, invert);
    if (len == 0) {
      strcpy(reply, "-none-");
    }
  } else {
    strcpy(reply, "Err - ??");
  }
}
