#include "AlertReporter.h"

#include <Utils.h>
#include <Packet.h>
#include <string.h>
#include <stdio.h>

// Minimal base64 decoder — kept local to avoid dragging the densaugeo/base64
// PlatformIO dependency into every repeater env that doesn't otherwise need
// it (only chat builds with MAX_GROUP_CHANNELS pulled it in via BaseChatMesh).
// Returns the number of decoded bytes, or 0 on error. Output buffer must be
// at least (in_len * 3 / 4) bytes.
static int alert_decode_base64(const char* in, size_t in_len, uint8_t* out) {
  static const int8_t TBL[128] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
  };
  size_t pad = 0;
  while (in_len > 0 && in[in_len - 1] == '=') { in_len--; pad++; }
  if ((in_len + pad) % 4 != 0) return 0;
  if (pad > 2) return 0;

  size_t out_pos = 0;
  uint32_t buffer = 0;
  int bits = 0;
  for (size_t i = 0; i < in_len; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c >= 128) return 0;
    int v = TBL[c];
    if (v < 0) return 0;
    buffer = (buffer << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[out_pos++] = (uint8_t)((buffer >> bits) & 0xFF);
    }
  }
  return (int)out_pos;
}

// Header layout for PAYLOAD_TYPE_GRP_TXT before encryption:
//   [0..3] timestamp (uint32_t LE) — also helps make packet_hash unique
//   [4]    TXT_TYPE_PLAIN
//   [5..]  "<sender>: <text>"  (null-terminated by sender for legacy parsers)
#ifndef MAX_ALERT_TEXT_LEN
// Conservative ceiling: matches BaseChatMesh::MAX_TEXT_LEN (10 * 16 = 160) and
// stays under MAX_PACKET_PAYLOAD - 4(timestamp) - 1(type) - CIPHER_MAC_SIZE - 1.
#define MAX_ALERT_TEXT_LEN 160
#endif

#ifndef ALERT_TXT_TYPE_PLAIN
#define ALERT_TXT_TYPE_PLAIN 0
#endif

#ifdef MQTT_DEBUG
#include <Arduino.h>
#define ALERT_DEBUG_PRINTLN(...) Serial.printf("Alert: " __VA_ARGS__); Serial.println()
#else
#define ALERT_DEBUG_PRINTLN(...) do {} while (0)
#endif

AlertReporter::AlertReporter()
    : _prefs(nullptr), _mesh(nullptr), _callbacks(nullptr),
#ifdef WITH_MQTT_BRIDGE
      _bridge(nullptr),
#endif
      _next_check_ms(0) {
#ifdef WITH_MQTT_BRIDGE
  memset(&_wifi, 0, sizeof(_wifi));
  memset(&_mqtt, 0, sizeof(_mqtt));
#endif
}

void AlertReporter::begin(NodePrefs* prefs, mesh::Mesh* mesh, CommonCLICallbacks* callbacks) {
  _prefs = prefs;
  _mesh = mesh;
  _callbacks = callbacks;
  onConfigChanged();
}

#ifdef WITH_MQTT_BRIDGE
void AlertReporter::setBridge(MQTTBridge* bridge) {
  _bridge = bridge;
}
#endif

// Channels banned as fault-alert destinations. Fault alerts are noisy
// operator-infrastructure messages; routing them to community channels would
// flood every nearby companion app (and amplify via well-known auto-responder
// bots), so the firmware refuses these keys at both CLI set-time and at
// runtime in resolveChannel.
//
// Provenance for each row can be re-derived with:
//   printf '#name' | openssl dgst -sha256 | cut -c1-32
// or for raw b64 PSKs:
//   echo 'izOH6cXN6mrJ5e26oRXNcg==' | base64 -d | xxd -p
//
// To ban an additional channel: append one new row; no other code changes
// required. The matcher converts the candidate's 16-byte secret to a
// lowercase hex string and does a linear strcmp — N is tiny.
struct BannedAlertChannel {
  const char* label;
  const char* secret_hex;  // 32 lowercase hex chars (no 0x, no separators)
};

static const BannedAlertChannel BANNED_ALERT_CHANNELS[] = {
  // Public group PSK ("izOH6cXN6mrJ5e26oRXNcg==")
  { "PUBLIC", "8b3387e9c5cdea6ac9e5edbaa115cd72" },
  // sha256("#test")[0..15] — auto-responders in many regions
  { "#test",  "9cd8fcf22a47333b591d96a2b848b73f" },
  // sha256("#bot")[0..15] — generic bot channel, frequent auto-responders
  { "#bot",   "eb50a1bcb3e4e5d7bf69a57c9dada211" },
};

const char* alertReporterBannedChannelMatch(const uint8_t* secret16) {
  char hex[33];
  static const char* H = "0123456789abcdef";
  for (int i = 0; i < 16; i++) {
    hex[i*2]   = H[(secret16[i] >> 4) & 0xF];
    hex[i*2+1] = H[secret16[i] & 0xF];
  }
  hex[32] = '\0';
  for (size_t i = 0; i < sizeof(BANNED_ALERT_CHANNELS) / sizeof(BANNED_ALERT_CHANNELS[0]); i++) {
    if (strcmp(hex, BANNED_ALERT_CHANNELS[i].secret_hex) == 0) {
      return BANNED_ALERT_CHANNELS[i].label;
    }
  }
  return nullptr;
}

const char* alertReporterBannedChannelMatchB64(const char* psk_b64) {
  if (!psk_b64) return nullptr;
  size_t len_in = strlen(psk_b64);
  if (len_in == 0) return nullptr;
  uint8_t secret[32];
  int n = alert_decode_base64(psk_b64, len_in, secret);
  if (n != 16) return nullptr;  // banned table only contains 16-byte secrets
  return alertReporterBannedChannelMatch(secret);
}

bool AlertReporter::resolveChannel(mesh::GroupChannel& out) const {
  if (!_prefs) return false;

  // alert_psk_b64 is the single source of truth — `set alert.hashtag`
  // pre-derives the base64 PSK from sha256("#name")[0..15] at CLI time.
  const char* psk = _prefs->alert_psk_b64;
  size_t psk_len = strlen(psk);
  if (psk_len == 0 || psk_len >= sizeof(_prefs->alert_psk_b64)) return false;

  memset(out.secret, 0, sizeof(out.secret));
  int len = alert_decode_base64(psk, psk_len, out.secret);
  if (len != 32 && len != 16) return false;

  // Belt-and-suspenders against an operator pasting a banned PSK directly
  // into alert.psk, or a hashtag whose hash somehow collides with one of the
  // banned 16-byte secrets (astronomically improbable, but free to check).
  if (len == 16) {
    const char* banned = alertReporterBannedChannelMatch(out.secret);
    if (banned) {
      ALERT_DEBUG_PRINTLN("refused banned channel '%s' for alert", banned);
      return false;
    }
  }

  // PATH_HASH_SIZE bytes — same scheme used by addChannel().
  mesh::Utils::sha256(out.hash, sizeof(out.hash), out.secret, len);
  return true;
}

void AlertReporter::onConfigChanged() {
  // Reset transient state so a config change re-arms the edge detector.
#ifdef WITH_MQTT_BRIDGE
  _wifi.state = OK;
  _wifi.fired_at_ms = 0;
  for (size_t i = 0; i < sizeof(_mqtt) / sizeof(_mqtt[0]); i++) {
    _mqtt[i].state = OK;
    _mqtt[i].fired_at_ms = 0;
  }
#endif
}

bool AlertReporter::sendChannel(const char* text) {
  if (!_mesh || !_prefs) return false;

  mesh::GroupChannel channel;
  if (!resolveChannel(channel)) return false;

  // Build "<sender>: <text>" plaintext payload. Sender = node name (current).
  uint8_t buf[5 + MAX_ALERT_TEXT_LEN + 32];
  uint32_t timestamp = _mesh->getRTCClock()->getCurrentTime();
  memcpy(buf, &timestamp, 4);
  buf[4] = ALERT_TXT_TYPE_PLAIN;

  const char* sender = _prefs->node_name[0] ? _prefs->node_name : "node";
  int n = snprintf((char*)&buf[5], MAX_ALERT_TEXT_LEN, "%s: %s", sender, text);
  if (n < 0) return false;
  if (n >= MAX_ALERT_TEXT_LEN) n = MAX_ALERT_TEXT_LEN - 1;

  mesh::Packet* pkt = _mesh->createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channel,
                                                 buf, 5 + (size_t)n);
  if (!pkt) {
    ALERT_DEBUG_PRINTLN("createGroupDatagram failed (pool empty?)");
    return false;
  }

  // Ride the repeater's default scope (or `alert.region` override) when the
  // host MyMesh provides one — same path MyMesh uses for adverts and
  // broadcast channel messages. Falls back to plain (unscoped) flood when
  // no callbacks are wired or no scope is configured, matching the
  // pre-scoped behavior on builds without RegionMap.
  TransportKey scope;
  bool have_scope = _callbacks && _callbacks->resolveAlertScope(scope) && !scope.isNull();
  if (have_scope) {
    uint16_t codes[2];
    codes[0] = scope.calcTransportCode(pkt);
    codes[1] = 0;
    _mesh->sendFlood(pkt, codes);
  } else {
    _mesh->sendFlood(pkt);
  }
  ALERT_DEBUG_PRINTLN("sent: %s", text);
  return true;
}

bool AlertReporter::sendText(const char* text) {
  // sendText() is the manual entry point (`alert test` CLI). Deliberately
  // does NOT check alert_enabled so operators can verify the PSK / hashtag
  // setup without enabling automatic fault firing.
  if (!_prefs || !text || !*text) return false;
  return sendChannel(text);
}

void AlertReporter::formatAge(unsigned long age_ms, char* out, size_t out_size) const {
  unsigned long secs = age_ms / 1000UL;
  unsigned long h = secs / 3600UL;
  unsigned long m = (secs % 3600UL) / 60UL;
  if (h > 0) {
    snprintf(out, out_size, "%luh%lum", h, m);
  } else {
    snprintf(out, out_size, "%lum", m);
  }
}

void AlertReporter::onLoop(unsigned long now_ms) {
  if (!_prefs || !_prefs->alert_enabled) return;
  if (!_mesh) return;

  // Throttle: ~5 s cadence. The thresholds are minutes-scale so this is fine.
  if ((long)(now_ms - _next_check_ms) < 0) return;
  _next_check_ms = now_ms + 5000UL;

#ifdef WITH_MQTT_BRIDGE
  // Clamp to a 60-minute floor regardless of what's in NodePrefs. The CLI
  // already enforces this on set, but a stale prefs file or future field
  // tweak shouldn't be able to drag the floor below 1 hour and let a
  // flapping link spam the mesh.
  uint16_t cfg_min = _prefs->alert_min_interval_min;
  if (cfg_min < 60) cfg_min = 60;
  unsigned long min_interval_ms = (unsigned long)cfg_min * 60000UL;

  // -------- WiFi fault --------
  if (_prefs->alert_wifi_minutes > 0) {
    unsigned long wifi_disc_ms = MQTTBridge::getLastWifiDisconnectTime();
    unsigned long wifi_conn_ms = MQTTBridge::getWifiConnectedAtMillis();
    bool wifi_down = (wifi_disc_ms != 0 && wifi_conn_ms == 0);
    unsigned long down_ms = wifi_down ? (now_ms - wifi_disc_ms) : 0;
    unsigned long thresh_ms = (unsigned long)_prefs->alert_wifi_minutes * 60000UL;

    if (_wifi.state == OK) {
      if (wifi_down && down_ms >= thresh_ms &&
          (now_ms - _wifi.fired_at_ms) >= min_interval_ms) {
        char age[16];
        formatAge(down_ms, age, sizeof(age));
        uint8_t reason = MQTTBridge::getLastWifiDisconnectReason();
        char text[80];
        if (reason != 0) {
          snprintf(text, sizeof(text), "WiFi down %s (reason %u)", age, (unsigned)reason);
        } else {
          snprintf(text, sizeof(text), "WiFi down %s", age);
        }
        if (sendChannel(text)) {
          _wifi.state = FIRING;
          _wifi.fired_at_ms = now_ms;
          _wifi.last_outage_started_ms = wifi_disc_ms;
        }
      }
    } else { // FIRING
      if (!wifi_down) {
        unsigned long total = (wifi_conn_ms != 0 && _wifi.last_outage_started_ms != 0)
            ? (wifi_conn_ms - _wifi.last_outage_started_ms) : 0;
        char age[16];
        formatAge(total, age, sizeof(age));
        char text[80];
        snprintf(text, sizeof(text), "WiFi recovered after %s", age);
        sendChannel(text);
        _wifi.state = OK;
      }
    }
  } else if (_wifi.state == FIRING) {
    _wifi.state = OK; // threshold disabled mid-fault: silently re-arm
  }

  // -------- MQTT slot faults --------
  if (_prefs->alert_mqtt_minutes > 0 && _bridge != nullptr) {
    int n = MQTTBridge::getRuntimeSlotCount();
    if (n > (int)(sizeof(_mqtt) / sizeof(_mqtt[0]))) n = (int)(sizeof(_mqtt) / sizeof(_mqtt[0]));
    unsigned long thresh_ms = (unsigned long)_prefs->alert_mqtt_minutes * 60000UL;

    for (int i = 0; i < n; i++) {
      Fault& f = _mqtt[i];
      if (!_bridge->isSlotEnabledAndAttempted(i)) {
        if (f.state == FIRING) f.state = OK; // slot disabled mid-fault
        continue;
      }
      unsigned long outage_start = _bridge->getSlotCurrentOutageStartMs(i);
      bool down = (outage_start != 0);
      unsigned long down_ms = down ? (now_ms - outage_start) : 0;

      if (f.state == OK) {
        if (down && down_ms >= thresh_ms &&
            (now_ms - f.fired_at_ms) >= min_interval_ms) {
          char age[16];
          formatAge(down_ms, age, sizeof(age));
          char text[100];
          snprintf(text, sizeof(text), "MQTT slot %d (%s) down %s",
                   i + 1, _bridge->getSlotPresetName(i), age);
          if (sendChannel(text)) {
            f.state = FIRING;
            f.fired_at_ms = now_ms;
            f.last_outage_started_ms = outage_start;
          }
        }
      } else { // FIRING
        if (!down) {
          unsigned long total = (f.last_outage_started_ms != 0)
              ? (now_ms - f.last_outage_started_ms) : 0;
          char age[16];
          formatAge(total, age, sizeof(age));
          char text[100];
          snprintf(text, sizeof(text), "MQTT slot %d (%s) recovered after %s",
                   i + 1, _bridge->getSlotPresetName(i), age);
          sendChannel(text);
          f.state = OK;
        }
      }
    }
  }
#else
  (void)now_ms;
#endif
}
