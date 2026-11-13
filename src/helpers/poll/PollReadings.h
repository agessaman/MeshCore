#pragma once

// One decoded reading, the battery (v1) and telemetry (v2) envelopes that carry
// it, and the in-memory hold for readings the slot could not publish yet. Pure:
// no heap, no clock reads, and no JSON dependency beyond the claim's writers, so
// both envelopes escape, format times and print SNR exactly alike.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "PollClaims.h"
#include "PollReplyParsers.h"
#include "PollTypes.h"

namespace poll {

// Plain values for ReadingFormat in PollPolicy.h: battery for v1, telemetry for
// v2. Zero there means the encrypted-reply path, which publishes nothing.
static const uint8_t kReadingV1 = 1;
static const uint8_t kReadingV2 = 2;

static const uint8_t kReadingLppMax = 160;
static const uint8_t kReadingHashSize = 8;
static const uint16_t kReadingJsonMax = 1664;  // v2 with every field and a fully escaped origin
static const uint8_t kReadingHoldMax = kMaxTargets;

// What one poll learned. The counters live in status and their presence in
// status_len; the telemetry records are copied so a v2 envelope can carry them.
struct Reading {
  uint8_t  node[kPubKeySize];
  uint8_t  method;             // ReqKind: which request the reply answered
  uint32_t sampled_at;         // epoch seconds when the reply arrived
  bool     has_battery;
  uint16_t battery_mv;
  bool     has_temperature;
  int16_t  temperature_c10;    // tenths of a degree C
  bool     has_snr;
  int16_t  snr_x4;             // quarter dB, as in the claim
  bool     has_tag;
  uint32_t tag;
  bool     has_hash;
  uint8_t  hash[kReadingHashSize];
  StatusReading status;
  uint8_t  status_len;         // status data bytes present in the reply
  bool     other_records;      // an LPP record besides channel-1 voltage/temperature
  uint8_t  lpp[kReadingLppMax];
  uint8_t  lpp_len;
};

// v1 carries a status reply or a telemetry reply with a voltage. v2 carries any
// successful reply; anything else is the encrypted-reply path and publishes none.
static inline bool readingPublishable(uint8_t format, const Reading& r) {
  if (format == kReadingV2) return true;
  if (format != kReadingV1) return false;
  if (r.method == REQ_STATUS) return true;
  return r.has_battery;
}

// A reply with nothing worth pacing: no voltage at all, and no extra record
// that a later poll could have measured instead.
static inline bool replyEmptyForPacing(const Reading& r) {
  if (r.has_battery && r.battery_mv != 0) return false;
  return !r.other_records;
}

// Tenths of a degree as a decimal with one fraction digit.
static inline void readingTenths(detail::Out& o, int32_t tenths) {
  if (tenths < 0) {
    o.ch('-');
    tenths = -tenths;
  }
  detail::writeUint(o, (uint32_t)(tenths / 10));
  o.ch('.');
  o.ch((char)('0' + tenths % 10));
}

// Quarter dB as dB with two fraction digits, which the quarter steps allow exactly.
static inline void readingQuarterDb(detail::Out& o, int32_t snr_x4) {
  int32_t hundredths = snr_x4 * 25;
  if (hundredths < 0) {
    o.ch('-');
    hundredths = -hundredths;
  }
  detail::writeUint(o, (uint32_t)(hundredths / 100));
  o.ch('.');
  o.ch((char)('0' + (hundredths / 10) % 10));
  o.ch((char)('0' + hundredths % 10));
}

static inline void readingInt(detail::Out& o, int32_t v) {
  if (v < 0) {
    o.ch('-');
    v = -v;
  }
  detail::writeUint(o, (uint32_t)v);
}

static inline void readingStatSep(detail::Out& o, bool& first) {
  if (!first) o.ch(',');
  first = false;
}

// Only the W47 fields whose bytes were present, in the plugin's stats order.
// The battery is the reading's own field and is never repeated here.
static inline void readingStats(detail::Out& o, const Reading& r) {
  o.ch('{');
  bool first = true;
  if (r.status_len >= 24) {
    readingStatSep(o, first);
    o.lit("\"uptime_secs\":");
    detail::writeUint(o, r.status.uptime_secs);
  }
  if (r.status_len >= 6) {
    readingStatSep(o, first);
    o.lit("\"noise_floor\":");
    readingInt(o, r.status.noise_floor);
  }
  if (r.status_len >= 8) {
    readingStatSep(o, first);
    o.lit("\"last_rssi\":");
    readingInt(o, r.status.last_rssi);
  }
  if (r.status_len >= 44) {
    readingStatSep(o, first);
    o.lit("\"last_snr\":");
    readingQuarterDb(o, r.status.last_snr_x4);
  }
  if (r.status_len >= 4) {
    readingStatSep(o, first);
    o.lit("\"tx_queue\":");
    detail::writeUint(o, r.status.tx_queue);
  }
  if (r.status_len >= 12) {
    readingStatSep(o, first);
    o.lit("\"packets_recv\":");
    detail::writeUint(o, r.status.packets_recv);
  }
  if (r.status_len >= 16) {
    readingStatSep(o, first);
    o.lit("\"packets_sent\":");
    detail::writeUint(o, r.status.packets_sent);
  }
  if (r.status_len >= 36) {
    readingStatSep(o, first);
    o.lit("\"recv_flood\":");
    detail::writeUint(o, r.status.recv_flood);
  }
  if (r.status_len >= 40) {
    readingStatSep(o, first);
    o.lit("\"recv_direct\":");
    detail::writeUint(o, r.status.recv_direct);
  }
  if (r.status_len >= 28) {
    readingStatSep(o, first);
    o.lit("\"sent_flood\":");
    detail::writeUint(o, r.status.sent_flood);
  }
  if (r.status_len >= 32) {
    readingStatSep(o, first);
    o.lit("\"sent_direct\":");
    detail::writeUint(o, r.status.sent_direct);
  }
  if (r.status_len >= 20) {
    readingStatSep(o, first);
    o.lit("\"tx_air_secs\":");
    detail::writeUint(o, r.status.tx_air_secs);
  }
  if (r.status_len >= 52) {
    readingStatSep(o, first);
    o.lit("\"rx_air_secs\":");
    detail::writeUint(o, r.status.rx_air_secs);
  }
  if (r.status_len >= 48) {
    readingStatSep(o, first);
    o.lit("\"flood_dups\":");
    detail::writeUint(o, r.status.flood_dups);
  }
  if (r.status_len >= 46) {
    readingStatSep(o, first);
    o.lit("\"direct_dups\":");
    detail::writeUint(o, r.status.direct_dups);
  }
  if (r.status_len >= 56) {
    readingStatSep(o, first);
    o.lit("\"recv_errors\":");
    detail::writeUint(o, r.status.recv_errors);
  }
  if (r.status_len >= 42) {
    readingStatSep(o, first);
    o.lit("\"err_events\":");
    detail::writeUint(o, r.status.err_events);
  }
  o.ch('}');
}

// One envelope, exactly one reading. Compact separators and the plugin's field
// order; origin_id and the packet hash are uppercase like their packet
// envelope, node_id and lpp lowercase. Returns 0 rather than a truncated payload.
static inline size_t buildReadingJson(char* out, size_t cap, uint8_t format, const char* origin,
                                      const uint8_t origin_pub[kPubKeySize], uint32_t now,
                                      const char* plugin_tag, const Reading& r) {
  if (!out || cap == 0 || !origin_pub) return 0;
  const bool v2 = format == kReadingV2;
  if (format != kReadingV1 && !v2) return 0;

  detail::Out o(out, cap);
  o.lit("{\"v\":");
  detail::writeUint(o, v2 ? 2u : 1u);
  o.lit(",\"origin\":");
  detail::jsonString(o, origin);
  o.lit(",\"origin_id\":\"");
  detail::hex(o, origin_pub, kPubKeySize, true);
  o.lit("\",\"timestamp\":\"");
  detail::jsonTimestamp(o, now);
  o.lit("\",\"plugin\":");
  detail::jsonString(o, plugin_tag);
  o.lit(",\"readings\":[{\"node_id\":\"");
  detail::hex(o, r.node, kPubKeySize, false);
  o.ch('"');

  if (v2) {
    if (r.has_battery) {
      o.lit(",\"battery_mv\":");
      detail::writeUint(o, r.battery_mv);
    }
  } else {
    o.lit(",\"battery_mv\":");
    detail::writeUint(o, r.has_battery ? r.battery_mv : 0);
  }

  o.lit(",\"source\":\"poll\",\"reply_hops\":0,\"method\":\"");
  o.lit(r.method == REQ_STATUS ? "status" : "telemetry");
  o.lit("\",\"sampled_at\":\"");
  detail::jsonTimestamp(o, r.sampled_at);
  o.ch('"');

  if (r.has_snr) {
    o.lit(",\"snr\":");
    detail::jsonSnr(o, r.snr_x4);
  }
  if (r.has_temperature) {
    o.lit(",\"temperature_c\":");
    readingTenths(o, r.temperature_c10);
  }
  if (v2 && r.method != REQ_STATUS) {
    const uint8_t n = r.lpp_len > kReadingLppMax ? kReadingLppMax : r.lpp_len;
    o.lit(",\"lpp\":\"");
    detail::hex(o, r.lpp, n, false);
    o.ch('"');
  }
  if (r.method == REQ_STATUS) {
    o.lit(",\"stats\":");
    readingStats(o, r);
  }
  if (r.has_tag) {
    o.lit(",\"tag\":");
    detail::writeUint(o, r.tag);
  }
  if (r.has_hash) {
    o.lit(",\"packet_hash\":\"");
    detail::hex(o, r.hash, kReadingHashSize, true);
    o.ch('"');
  }
  o.lit("}]}");
  return o.ok ? o.len : 0;
}

// Newest unpublished reading per target, oldest first. A new reading for a
// target already held replaces it and becomes the newest; a full hold drops the
// oldest entry to make room. No heap: the array is fixed at kReadingHoldMax.
struct ReadingHold {
  Reading entries[kReadingHoldMax];
  uint8_t count;
};

static inline void readingHoldRemoveAt(ReadingHold& h, uint8_t i) {
  for (uint8_t k = i; k + 1 < h.count; k++) h.entries[k] = h.entries[k + 1];
  h.count--;
}

static inline void readingHoldPut(ReadingHold& h, const Reading& r) {
  for (uint8_t i = 0; i < h.count; i++) {
    if (memcmp(h.entries[i].node, r.node, kPubKeySize) == 0) {
      readingHoldRemoveAt(h, i);
      break;
    }
  }
  if (h.count >= kReadingHoldMax) readingHoldRemoveAt(h, 0);
  h.entries[h.count++] = r;
}

// The oldest reading, or null when the hold is empty.
static inline const Reading* readingHoldPeek(const ReadingHold& h) {
  return h.count ? &h.entries[0] : nullptr;
}

// Copies out the oldest reading and removes it.
static inline bool readingHoldTake(ReadingHold& h, Reading& out) {
  if (h.count == 0) return false;
  out = h.entries[0];
  readingHoldRemoveAt(h, 0);
  return true;
}

static inline bool readingHoldDrop(ReadingHold& h, const uint8_t node[kPubKeySize]) {
  if (!node) return false;
  for (uint8_t i = 0; i < h.count; i++) {
    if (memcmp(h.entries[i].node, node, kPubKeySize) == 0) {
      readingHoldRemoveAt(h, i);
      return true;
    }
  }
  return false;
}

static inline void readingHoldClear(ReadingHold& h) { h.count = 0; }

}  // namespace poll
