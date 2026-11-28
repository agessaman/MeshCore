#pragma once

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "MQTTTopicRouter.h"

// Pure routing for polling publications: reading kinds and their topics, and
// the broker-host match that picks the slot. Shared by MQTTBridge and the native tests.

// Reading kinds accepted by MQTTBridge::requestPublishPollReading.
enum MQTTPollReadingKind : uint8_t {
  MQTT_POLL_READING_BATTERY = 0,     // battery topic, reading envelope v1
  MQTT_POLL_READING_TELEMETRY = 1,   // telemetry topic, reading envelope v2
};

static const size_t kMQTTPollReadingJsonMax = 1664;   // one reading with LPP hex and full stats (poll::kReadingJsonMax)

// Publication type for a reading kind, or -1 for an unknown kind.
static inline int mqttPollReadingPublication(uint8_t kind) {
  switch (kind) {
    case MQTT_POLL_READING_BATTERY: return MQTT_PUBLICATION_BATTERY;
    case MQTT_POLL_READING_TELEMETRY: return MQTT_PUBLICATION_TELEMETRY;
    default: return -1;
  }
}

static const uint8_t kMQTTPollHostsMax = 4;      // host entries an analyzer may name
static const size_t kMQTTPollHostSize = 64;      // bytes per entry, NUL included

// One slot as the matcher sees it: its enabled flag and its effective broker,
// either a bare host or a URI such as wss://host:443/path.
struct MQTTPollSlotHost {
  bool enabled;
  const char* broker;
};

// Finds the host inside a bare host or URI: skips scheme and user info, stops
// at port, path, query or fragment, unwraps [IPv6], and drops one trailing dot.
static inline bool mqttBrokerHostSpan(const char* broker, const char** start, size_t* len) {
  if (!broker || !start || !len) return false;
  const char* scheme = strstr(broker, "://");
  const char* p = scheme ? scheme + 3 : broker;
  const char* end = p + strcspn(p, "/?#");
  for (const char* q = p; q < end; q++) {
    if (*q == '@') p = q + 1;
  }
  if (p < end && *p == '[') {
    const char* close = (const char*)memchr(p, ']', (size_t)(end - p));
    if (!close) return false;
    p++;
    end = close;
  } else {
    // A bare IPv6 literal has several colons and no port to strip.
    const char* colon = (const char*)memchr(p, ':', (size_t)(end - p));
    if (colon && (scheme || !memchr(colon + 1, ':', (size_t)(end - colon - 1)))) end = colon;
  }
  if (end > p && end[-1] == '.') end--;
  if (end <= p) return false;
  *start = p;
  *len = (size_t)(end - p);
  return true;
}

// Case-insensitive host equality; one trailing dot on the entry is ignored.
static inline bool mqttPollHostMatches(const char* entry, size_t entry_size,
                                       const char* host, size_t host_len) {
  if (!entry || !host) return false;
  size_t n = 0;
  while (n < entry_size && entry[n] != '\0') n++;
  if (n > 0 && entry[n - 1] == '.') n--;
  if (n == 0 || n != host_len) return false;
  for (size_t i = 0; i < n; i++) {
    if (tolower((unsigned char)entry[i]) != tolower((unsigned char)host[i])) return false;
  }
  return true;
}

// 1-based number of the lowest enabled slot whose broker host equals one of
// the first kMQTTPollHostsMax entries, or 0 when none does.
static inline uint8_t mqttPollSlotForHosts(const char hosts[][kMQTTPollHostSize], uint8_t count,
                                           const MQTTPollSlotHost* slots, uint8_t slot_count) {
  if (!hosts || !slots) return 0;
  if (count > kMQTTPollHostsMax) count = kMQTTPollHostsMax;
  for (uint8_t i = 0; i < slot_count; i++) {
    if (!slots[i].enabled) continue;
    const char* host;
    size_t host_len;
    if (!mqttBrokerHostSpan(slots[i].broker, &host, &host_len)) continue;
    for (uint8_t j = 0; j < count; j++) {
      if (mqttPollHostMatches(hosts[j], kMQTTPollHostSize, host, host_len)) {
        return (uint8_t)(i + 1);
      }
    }
  }
  return 0;
}
