#pragma once

// Receive-only second radio profile ("radio2") for observer builds.
//
// One LoRa transceiver alternates between the node's own radio settings and a
// second set, and publishes what it hears on the second one. Packets heard on
// radio2 are logged and published only: they are never routed or repeated, and
// nothing is transmitted on radio2. Command replies and MQTT keys match Keymind
// Cascade's radio2, so tools and analyzers treat both firmwares alike.
//
// Pure logic with no Arduino or radio dependencies, so it is host-testable.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Radio2Params {
  float freq = 0;   // MHz
  float bw = 0;     // kHz
  uint8_t sf = 0;
  uint8_t cr = 0;   // 5..8 (4/5..4/8)

  bool operator==(const Radio2Params& o) const {
    return freq == o.freq && bw == o.bw && sf == o.sf && cr == o.cr;
  }
  bool operator!=(const Radio2Params& o) const { return !(*this == o); }
};

namespace radio2 {

enum Mode : uint8_t { MODE_OFF = 0, MODE_RX = 1 };

// Same 4.6-symbol minimum visit as Keymind Cascade's scanner.
static constexpr double MinListenSymbols = 4.6;
// Switch cost assumed until real retunes have been measured.
static constexpr uint32_t DefaultSwitchUs = 1000;

inline bool validBandwidth(float bw) {
  static const float bandwidths[] = {7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f,
                                     62.5f, 125.0f, 250.0f, 500.0f};
  for (float b : bandwidths) if (fabsf(bw - b) < 0.01f) return true;
  return false;
}

inline bool valid(const Radio2Params& p) {
  return p.freq >= 137.0f && p.freq <= 1020.0f && validBandwidth(p.bw)
      && p.sf >= 5 && p.sf <= 12 && p.cr >= 5 && p.cr <= 8;
}

// Persisted as integers so the JSON prefs never round-trip floats.
inline uint32_t freqToKhz(float mhz) { return (uint32_t)lroundf(mhz * 1000.0f); }
inline float khzToFreq(uint32_t khz) { return khz / 1000.0f; }
inline uint32_t bwToHz(float khz) { return (uint32_t)lroundf(khz * 1000.0f); }
inline float hzToBw(uint32_t hz) { return hz / 1000.0f; }

// Settings are kHz/Hz integers at heart; print them from those so a float
// like 911.15f shows as 911.150000, not 911.150024.
inline double exactMhz(float mhz) { return freqToKhz(mhz) / 1000.0; }
inline double exactKhz(float khz) { return bwToHz(khz) / 1000.0; }

inline Radio2Params fromStored(uint32_t freq_khz, uint32_t bw_hz, uint8_t sf, uint8_t cr) {
  Radio2Params p;
  p.freq = khzToFreq(freq_khz);
  p.bw = hzToBw(bw_hz);
  p.sf = sf;
  p.cr = cr;
  return p;
}

inline double symbolUs(const Radio2Params& p) {
  return p.bw > 0 && p.sf <= 12 ? (double)(1UL << p.sf) * 1000.0 / p.bw : 0;
}

// Preamble that stock MeshCore senders use (RadioLibWrapper::preambleLengthForSF).
inline uint16_t senderPreamble(uint8_t sf) { return sf <= 8 ? 32 : 16; }

inline uint32_t minimumListenUs(const Radio2Params& p) {
  return (uint32_t)ceil(MinListenSymbols * symbolUs(p));
}

// How long to listen on each profile per visit. Index 0 is the node's own
// radio, index 1 is radio2.
//
// Keymind Cascade's policy: the profile with longer symbols ("slow") gets the
// minimum visit; the other gets whatever leaves the slow profile two chances
// to catch a preamble that starts while the radio is away. MediaMesh senders
// size their own preambles for this same cycle.
struct Schedule {
  uint32_t listen_us[2] = {0, 0};
  uint8_t slow = 0;
  bool fits = false;   // false: the fast visit had to be stretched to its minimum
};

inline Schedule schedule(const Radio2Params& primary, const Radio2Params& second,
                         uint32_t switch_us) {
  Schedule s;
  const Radio2Params* p[2] = {&primary, &second};
  s.slow = symbolUs(primary) >= symbolUs(second) ? 0 : 1;
  const uint8_t fast = s.slow ^ 1;
  s.listen_us[s.slow] = minimumListenUs(*p[s.slow]);
  const uint32_t fast_min = minimumListenUs(*p[fast]);
  const double available = senderPreamble(p[s.slow]->sf) / 2.0 * symbolUs(*p[s.slow])
      - s.listen_us[s.slow] - 2.0 * switch_us;
  s.fits = available >= fast_min;
  s.listen_us[fast] = s.fits ? (uint32_t)available : fast_min;
  return s;
}

enum ParseResult : uint8_t {
  PARSE_OK = 0,
  PARSE_OFF,
  PARSE_INVALID,
  PARSE_TX_UNSUPPORTED,   // "rxtx": observer firmware only listens on radio2
};

// Accepts "off", a preset name ("mediamesh", "mediamesh-range"), or
// "freq,bw,sf,cr[,rx][,preamble|auto]". A trailing preamble is accepted for
// compatibility with Keymind Cascade commands and ignored: receiving does not
// depend on it.
inline ParseResult parse(const char* text, Radio2Params& out) {
  while (*text == ' ') text++;
  if (strcmp(text, "off") == 0) return PARSE_OFF;
  if (strcmp(text, "mediamesh") == 0) text = "911.150,250,7,5,rx";
  else if (strcmp(text, "mediamesh-range") == 0) text = "911.150,125,7,5,rx";

  char buf[64];
  if (strlen(text) >= sizeof(buf)) return PARSE_INVALID;
  strcpy(buf, text);

  char* fields[6];
  int n = 0;
  for (char* tok = strtok(buf, ","); tok; tok = strtok(nullptr, ",")) {
    if (n == 6) return PARSE_INVALID;
    fields[n++] = tok;
  }
  if (n < 4) return PARSE_INVALID;

  Radio2Params p;
  char* end;
  p.freq = strtof(fields[0], &end); if (*end) return PARSE_INVALID;
  p.bw = strtof(fields[1], &end);   if (*end) return PARSE_INVALID;
  long sf = strtol(fields[2], &end, 10); if (*end) return PARSE_INVALID;
  long cr = strtol(fields[3], &end, 10); if (*end) return PARSE_INVALID;
  if (sf < 0 || sf > 255 || cr < 0 || cr > 255) return PARSE_INVALID;
  p.sf = (uint8_t)sf;
  p.cr = (uint8_t)cr;

  if (n >= 5) {
    if (strcmp(fields[4], "rxtx") == 0) return PARSE_TX_UNSUPPORTED;
    if (strcmp(fields[4], "rx") != 0) return PARSE_INVALID;
  }
  if (n == 6 && strcmp(fields[5], "auto") != 0) {
    long pre = strtol(fields[5], &end, 10);
    if (*end || pre < 8 || pre > 65535) return PARSE_INVALID;
  }
  if (!valid(p)) return PARSE_INVALID;
  out = p;
  return PARSE_OK;
}

// `get radio2` reply, the shape Keymind Cascade prints: "> off" or
// "> 911.150,250.000,7,5,rx,32".
inline void formatReply(char* out, size_t n, bool on, const Radio2Params& p) {
  if (!on) { snprintf(out, n, "> off"); return; }
  snprintf(out, n, "> %.3f,%.3f,%u,%u,rx,%u", exactMhz(p.freq), exactKhz(p.bw), (unsigned)p.sf, (unsigned)p.cr,
           (unsigned)senderPreamble(p.sf));
}

// The `radio` value in the MQTT status `radios` list, the same format as the
// status message's own `radio` field.
inline void formatStatusRadio(char* out, size_t n, const Radio2Params& p) {
  snprintf(out, n, "%.6f,%.1f,%u,%u", exactMhz(p.freq), exactKhz(p.bw), (unsigned)p.sf, (unsigned)p.cr);
}

}  // namespace radio2
