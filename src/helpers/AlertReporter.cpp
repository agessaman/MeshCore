#include "AlertReporter.h"

#include <Utils.h>
#include <Packet.h>
#include <string.h>
#include <stdio.h>
#ifdef WITH_MQTT_BRIDGE
#include "AlertFaultPolicy.h"
#include "NetworkInterface.h"
#endif

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

#ifdef WITH_MQTT_BRIDGE
AlertReporter::AlertReporter()
    : _prefs(nullptr), _obs(nullptr), _mesh(nullptr), _callbacks(nullptr),
#ifdef WITH_MQTT_BRIDGE
      _bridge(nullptr),
#endif
      _next_check_ms(0) {
#ifdef WITH_MQTT_BRIDGE
  memset(&_wifi, 0, sizeof(_wifi));
  memset(&_mqtt, 0, sizeof(_mqtt));
#endif
}

void AlertReporter::begin(NodePrefs* prefs, MQTTPrefs* obs, mesh::Mesh* mesh, CommonCLICallbacks* callbacks) {
  _prefs = prefs;
  _obs = obs;
  _mesh = mesh;
  _callbacks = callbacks;
  onConfigChanged();
}

void AlertReporter::setBridge(MQTTBridge* bridge) {
  _bridge = bridge;
}
#endif // WITH_MQTT_BRIDGE (AlertReporter methods, part 1)

// Channels banned as fault-alert destinations. Fault alerts are noisy
// operator-infrastructure messages; routing them to community channels would
// flood every nearby companion app (and amplify via well-known auto-responder
// bots), so the firmware refuses these keys at both CLI set-time and at
// runtime in resolveChannel.
//
// Provenance for each row can be re-derived with:
//   printf '#name' | openssl dgst -sha256 | cut -c1-32
// or for the Public PSK:
//   echo 'izOH6cXN6mrJ5e26oRXNcg==' | base64 -d | xxd -p -c 16
//
// To ban an additional channel: append one new row; no other code changes
// required. Both the table entries and `alert_psk_hex` are 32 lowercase hex
// chars (16-byte secret), so the matcher is a direct strcmp.
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
  mesh::Utils::toHex(hex, secret16, 16);
  for (size_t i = 0; i < sizeof(BANNED_ALERT_CHANNELS) / sizeof(BANNED_ALERT_CHANNELS[0]); i++) {
    if (strcmp(hex, BANNED_ALERT_CHANNELS[i].secret_hex) == 0) {
      return BANNED_ALERT_CHANNELS[i].label;
    }
  }
  return nullptr;
}

const char* alertReporterBannedChannelMatchHex(const char* psk_hex) {
  if (!psk_hex || strlen(psk_hex) != 32) return nullptr;
  uint8_t secret[16];
  if (!mesh::Utils::fromHex(secret, 16, psk_hex)) return nullptr;
  return alertReporterBannedChannelMatch(secret);
}

#ifdef WITH_MQTT_BRIDGE
bool AlertReporter::resolveChannel(mesh::GroupChannel& out) const {
  if (!_prefs) return false;

  // alert_psk_hex is the single source of truth — `set alert.hashtag`
  // pre-derives the hex-encoded PSK from sha256("#name")[0..15] at CLI time.
  // Only 16-byte secrets (32 hex chars) are supported; 32-byte channel keys
  // are not used anywhere in MeshCore practice and not represented in the
  // banned table either.
  const char* psk = _obs->alert_psk_hex;
  if (strlen(psk) != 32) return false;

  memset(out.secret, 0, sizeof(out.secret));
  if (!mesh::Utils::fromHex(out.secret, 16, psk)) return false;

  // Belt-and-suspenders against an operator pasting a banned PSK directly
  // into alert.psk, or a hashtag whose hash somehow collides with one of the
  // banned 16-byte secrets (astronomically improbable, but free to check).
  const char* banned = alertReporterBannedChannelMatch(out.secret);
  if (banned) {
    ALERT_DEBUG_PRINTLN("refused banned channel '%s' for alert", banned);
    return false;
  }

  mesh::Utils::sha256(out.hash, sizeof(out.hash), out.secret, 16);
  return true;
}

void AlertReporter::onConfigChanged() {
  // Reset transient state so a config change re-arms the edge detector.
#ifdef WITH_MQTT_BRIDGE
  AlertFaultPolicy::reset(_wifi);
  for (size_t i = 0; i < sizeof(_mqtt) / sizeof(_mqtt[0]); i++) {
    AlertFaultPolicy::reset(_mqtt[i]);
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
  //
  // path_hash_size must honor the repeater's configured path.hash.mode (1, 2,
  // or 3-byte hashes); the Mesh.h default of 1 would silently downgrade
  // observers running on 2/3-byte regional meshes.
  const uint8_t path_hash_size = (uint8_t)(_prefs->path_hash_mode + 1);
  TransportKey scope;
  bool have_scope = _callbacks && _callbacks->resolveAlertScope(scope) && !scope.isNull();
  if (have_scope) {
    uint16_t codes[2];
    codes[0] = scope.calcTransportCode(pkt);
    codes[1] = 0;
    _mesh->sendFlood(pkt, codes, 0, path_hash_size);
  } else {
    _mesh->sendFlood(pkt, 0, path_hash_size);
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

void AlertReporter::onLoop(unsigned long now_ms) {
  if (!_prefs || !_obs || !_obs->alert_enabled) return;
  if (!_mesh) return;

  const uint32_t now = (uint32_t)now_ms;
  if (!AlertFaultPolicy::checkDue(now, (uint32_t)_next_check_ms)) return;
  _next_check_ms = AlertFaultPolicy::nextCheckMs(now);

#ifdef WITH_MQTT_BRIDGE
  const uint32_t min_interval_ms =
      AlertFaultPolicy::minIntervalMs(_obs->alert_min_interval_min);

  // -------- WiFi fault --------
  if (_obs->alert_wifi_minutes > 0) {
    if (_bridge != nullptr) {
      const AlertFaultPolicy::OutageSnapshot snap = _bridge->getWifiOutageSnapshot();
      AlertFaultPolicy::TickResult r = AlertFaultPolicy::tick(
          _wifi, now, snap,
          AlertFaultPolicy::thresholdMs(_obs->alert_wifi_minutes),
          min_interval_ms);
      if (r.action == AlertFaultPolicy::Action::FireDown) {
        char text[80];
        AlertFaultPolicy::formatNetworkAlert(
            text, sizeof(text),
            strcmp(activeNetworkInterface().mediumName(), "ethernet") == 0
                ? "Ethernet" : "WiFi",
            r, snap);
        if (sendChannel(text)) {
          AlertFaultPolicy::commitDown(_wifi, now, snap.started_ms);
        }
      } else if (r.action == AlertFaultPolicy::Action::FireRecovered) {
        char text[80];
        AlertFaultPolicy::formatNetworkAlert(
            text, sizeof(text),
            strcmp(activeNetworkInterface().mediumName(), "ethernet") == 0
                ? "Ethernet" : "WiFi",
            r, snap);
        sendChannel(text);
        AlertFaultPolicy::commitRecovered(_wifi);
      }
    }
  } else {
    AlertFaultPolicy::rearmIfDisabled(_wifi);
  }

  // -------- MQTT slot faults --------
  if (_obs->alert_mqtt_minutes > 0 && _bridge != nullptr) {
    int n = MQTTBridge::getRuntimeSlotCount();
    if (n > (int)(sizeof(_mqtt) / sizeof(_mqtt[0]))) n = (int)(sizeof(_mqtt) / sizeof(_mqtt[0]));
    const uint32_t thresh_ms = AlertFaultPolicy::thresholdMs(_obs->alert_mqtt_minutes);

    for (int i = 0; i < n; i++) {
      AlertFaultPolicy::Fault& f = _mqtt[i];
      if (!_bridge->isSlotEnabledAndAttempted(i)) {
        AlertFaultPolicy::rearmIfDisabled(f);
        continue;
      }
      const uint32_t outage_start = (uint32_t)_bridge->getSlotCurrentOutageStartMs(i);
      const AlertFaultPolicy::OutageSnapshot snap =
          AlertFaultPolicy::fromStartMs(outage_start);
      AlertFaultPolicy::TickResult r = AlertFaultPolicy::tick(
          f, now, snap, thresh_ms, min_interval_ms);
      if (r.action == AlertFaultPolicy::Action::FireDown) {
        char text[100];
        AlertFaultPolicy::formatMqttDown(text, sizeof(text), i + 1,
                                         _bridge->getSlotPresetName(i),
                                         r.duration_ms);
        if (sendChannel(text)) {
          AlertFaultPolicy::commitDown(f, now, outage_start);
        }
      } else if (r.action == AlertFaultPolicy::Action::FireRecovered) {
        char text[100];
        AlertFaultPolicy::formatMqttRecovered(text, sizeof(text), i + 1,
                                              _bridge->getSlotPresetName(i),
                                              r.duration_ms);
        sendChannel(text);
        AlertFaultPolicy::commitRecovered(f);
      }
    }
  }
#else
  (void)now_ms;
#endif
}
#endif // WITH_MQTT_BRIDGE (AlertReporter methods, part 2)
