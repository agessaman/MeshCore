#pragma once

// Decoders for a reply body after the 4-byte tag has been split off. Multi-byte
// fields are read with shifts so a short or oddly aligned buffer is safe.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace poll {

// Prefix of RepeaterStats (examples/simple_repeater/MyMesh.h), in struct order.
// Bytes that are not present stay zero. has_uptime is set only when that
// field's four bytes were in the buffer, even if the value is 0. data_len is
// how many data bytes the reply carried, so a caller can tell which trailing
// fields exist rather than reading an absent one as a measured zero.
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
  uint8_t data_len;  // bytes present, capped at 255
};

struct TelemetryReading {
  bool has_voltage;
  uint16_t battery_mv;
  bool has_temperature;
  int16_t temperature_c10;  // tenths of a degree C
  uint8_t lpp_len;          // leading bytes that are whole LPP records
  bool other_records;       // a record other than channel-1 voltage or temperature
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
  out->data_len = len > 255 ? 255 : (uint8_t)len;
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
//
// lpp_len covers the leading whole records: it stops at the first channel-0
// byte, or at the first record that cannot be walked, whose own bytes and the
// zero padding after them are left out. An unknown type counts as another
// record, since it may be a sensor the poller does not decode.
static inline bool parseTelemetry(const uint8_t* data, size_t len, TelemetryReading* out) {
  if (!out || (!data && len > 0)) return false;
  memset(out, 0, sizeof(*out));
  if (!data || len == 0) return true;

  bool saw_voltage = false;
  bool saw_temperature = false;
  size_t i = 0;
  uint8_t stop = 0;  // 0 ran out cleanly, 1 channel 0, 2 a record that cannot be walked
  while (i + 2 <= len) {
    uint8_t channel = data[i];
    uint8_t type = data[i + 1];
    if (channel == 0) {
      stop = 1;
      break;
    }
    int sz = lppValueSize(type);
    const bool self_voltage = channel == kLppChannelSelf && type == kLppVoltage;
    const bool self_temperature = channel == kLppChannelSelf && type == kLppTemperature;
    if (sz < 0 || i + 2u + (size_t)sz > len) {
      stop = 2;
      if (!self_voltage && !self_temperature) out->other_records = true;
      break;
    }
    if (!self_voltage && !self_temperature) out->other_records = true;
    const uint8_t* raw = data + i + 2;
    if (self_voltage && !saw_voltage) {
      saw_voltage = true;
      uint32_t mv = (uint32_t)beU16(raw) * 10u;
      if (mv <= 65535u) {
        out->has_voltage = true;
        out->battery_mv = (uint16_t)mv;
      }
    } else if (self_temperature && !saw_temperature) {
      saw_temperature = true;
      int16_t c10 = beI16(raw);
      if (c10 >= kTempMinC10 && c10 <= kTempMaxC10) {
        out->has_temperature = true;
        out->temperature_c10 = c10;
      }
    }
    i += 2u + (size_t)sz;
  }
  if (stop == 0 && i < len) stop = data[i] == 0 ? 1 : 2;
  if (stop == 1) {
    out->lpp_len = (uint8_t)i;
  } else if (stop == 2) {
    size_t n = len;
    while (n > 0 && data[n - 1] == 0) n--;
    out->lpp_len = n > 255 ? 255 : (uint8_t)n;
  } else {
    out->lpp_len = len > 255 ? 255 : (uint8_t)len;
  }
  return true;
}

}  // namespace poll
