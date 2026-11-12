#pragma once

// Decoders for a reply body after the 4-byte tag has been split off. Multi-byte
// fields are read with shifts so a short or oddly aligned buffer is safe.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace poll {

// Prefix of RepeaterStats (examples/simple_repeater/MyMesh.h), in struct order.
// Bytes that are not present stay zero. has_uptime is set only when that
// field's four bytes were in the buffer, even if the value is 0.
struct StatusReading {
  uint16_t battery_mv;
  uint16_t tx_queue;
  int16_t noise_floor;
  int16_t last_rssi;
  uint32_t uptime_secs;
  bool has_uptime;

  uint32_t packets_recv;
  uint32_t packets_sent;
  uint32_t tx_air_secs;
  uint32_t sent_flood;
  uint32_t sent_direct;
  uint32_t recv_flood;
  uint32_t recv_direct;
  uint16_t err_events;
  int16_t last_snr_x4;
  uint16_t direct_dups;
  uint16_t flood_dups;
  uint32_t rx_air_secs;
  uint32_t recv_errors;
};

struct TelemetryReading {
  bool has_voltage;
  uint16_t battery_mv;
  bool has_temperature;
  int16_t temperature_c10;  // tenths of a degree C
};

static inline uint16_t leU16(const uint8_t* p) {
  return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static inline int16_t leI16(const uint8_t* p) {
  return (int16_t)leU16(p);
}

static inline uint32_t leU32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline uint16_t beU16(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline int16_t beI16(const uint8_t* p) {
  return (int16_t)beU16(p);
}

// Next field, or stop. A partial field is left unread.
static inline bool nextField(const uint8_t* data, size_t len, size_t* off, size_t n,
                            const uint8_t** out) {
  if (*off + n > len) return false;
  *out = data + *off;
  *off += n;
  return true;
}

static inline bool parseStatus(const uint8_t* data, size_t len, StatusReading* out) {
  if (!out || !data || len < 2) return false;
  memset(out, 0, sizeof(*out));
  size_t off = 0;
  const uint8_t* f;
  if (!nextField(data, len, &off, 2, &f)) return true;
  out->battery_mv = leU16(f);
  if (!nextField(data, len, &off, 2, &f)) return true;
  out->tx_queue = leU16(f);
  if (!nextField(data, len, &off, 2, &f)) return true;
  out->noise_floor = leI16(f);
  if (!nextField(data, len, &off, 2, &f)) return true;
  out->last_rssi = leI16(f);
  if (!nextField(data, len, &off, 4, &f)) return true;
  out->packets_recv = leU32(f);
  if (!nextField(data, len, &off, 4, &f)) return true;
  out->packets_sent = leU32(f);
  if (!nextField(data, len, &off, 4, &f)) return true;
  out->tx_air_secs = leU32(f);
  if (!nextField(data, len, &off, 4, &f)) return true;
  out->uptime_secs = leU32(f);
  out->has_uptime = true;
  if (!nextField(data, len, &off, 4, &f)) return true;
  out->sent_flood = leU32(f);
  if (!nextField(data, len, &off, 4, &f)) return true;
  out->sent_direct = leU32(f);
  if (!nextField(data, len, &off, 4, &f)) return true;
  out->recv_flood = leU32(f);
  if (!nextField(data, len, &off, 4, &f)) return true;
  out->recv_direct = leU32(f);
  if (!nextField(data, len, &off, 2, &f)) return true;
  out->err_events = leU16(f);
  if (!nextField(data, len, &off, 2, &f)) return true;
  out->last_snr_x4 = leI16(f);
  if (!nextField(data, len, &off, 2, &f)) return true;
  out->direct_dups = leU16(f);
  if (!nextField(data, len, &off, 2, &f)) return true;
  out->flood_dups = leU16(f);
  if (!nextField(data, len, &off, 4, &f)) return true;
  out->rx_air_secs = leU32(f);
  if (!nextField(data, len, &off, 4, &f)) return true;
  out->recv_errors = leU32(f);
  return true;
}

// CayenneLPP value sizes used by the plugin. Unknown types are absent on
// purpose: the repeater's getDataSize treats those as 1 byte and keeps going,
// which would swallow the next record. Channel 1 is the repeater itself.
static inline int lppValueSize(uint8_t type) {
  switch (type) {
    case 0:
    case 1:
    case 102:
    case 120:
    case 142:
      return 1;
    case 2:
    case 3:
    case 101:
    case 103:
    case 104:
    case 115:
    case 116:
    case 117:
    case 121:
    case 125:
    case 128:
    case 132:
      return 2;
    case 135:
      return 3;
    case 100:
    case 118:
    case 130:
    case 131:
    case 133:
      return 4;
    case 113:
    case 134:
      return 6;
    case 136:
      return 9;
    default:
      return -1;
  }
}

static const uint8_t kLppChannelSelf = 1;
static const uint8_t kLppTemperature = 103;
static const uint8_t kLppVoltage = 116;
static const int16_t kTempMinC10 = -600;  // -60.0 C
static const int16_t kTempMaxC10 = 1250;  // 125.0 C

// Channel 1 only. Voltage is u16 big-endian times 10 mV. Temperature is i16
// big-endian in tenths of a degree; outside [-60, 125] C is "no sensor".
// The first of each wins, including an out-of-range temperature.
static inline bool parseTelemetry(const uint8_t* data, size_t len, TelemetryReading* out) {
  if (!out || (!data && len > 0)) return false;
  memset(out, 0, sizeof(*out));
  if (!data || len == 0) return true;

  bool saw_voltage = false;
  bool saw_temperature = false;
  size_t i = 0;
  while (i + 2 <= len) {
    uint8_t channel = data[i];
    uint8_t type = data[i + 1];
    if (channel == 0) break;
    int sz = lppValueSize(type);
    if (sz < 0 || i + 2u + (size_t)sz > len) break;
    const uint8_t* raw = data + i + 2;
    if (channel == kLppChannelSelf && type == kLppVoltage && !saw_voltage) {
      saw_voltage = true;
      uint32_t mv = (uint32_t)beU16(raw) * 10u;
      if (mv <= 65535u) {
        out->has_voltage = true;
        out->battery_mv = (uint16_t)mv;
      }
    } else if (channel == kLppChannelSelf && type == kLppTemperature && !saw_temperature) {
      saw_temperature = true;
      int16_t c10 = beI16(raw);
      if (c10 >= kTempMinC10 && c10 <= kTempMaxC10) {
        out->has_temperature = true;
        out->temperature_c10 = c10;
      }
    }
    i += 2u + (size_t)sz;
  }
  return true;
}

}  // namespace poll
