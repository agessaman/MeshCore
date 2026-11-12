#pragma once

// CoreScope's polling policy: clamps, the kill switches, and per-node windows.
// No clock reads and no JSON; the caller fills Policy and passes now.
// Times are epoch seconds. A zero fetched_at or paused_until is a sentinel,
// not "the epoch", and is never subtracted from now.

#include "PollTypes.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace poll {

static const uint8_t  kMaxExcluded = 16;
static const uint8_t  kMaxNodeWindows = 8;

static const uint32_t kDirectMinS = 30UL * 60UL;
static const uint32_t kDirectMaxS = kDay;
static const uint32_t kDirectDefaultS = 3600;
static const uint32_t kOneHopMinS = 60UL * 60UL;
static const uint32_t kOneHopMaxS = kDay;
static const uint32_t kOneHopDefaultS = 7200;
static const uint32_t kGraceMinS = 10UL * 60UL;
static const uint32_t kGraceMaxS = 120UL * 60UL;
static const uint32_t kGraceDefaultS = 1800;
static const uint8_t  kMaxRequestsMin = 1;
static const uint8_t  kMaxRequestsMax = 3;
static const uint8_t  kMaxRequestsDefault = 2;
static const uint32_t kStalePolicyS = 6UL * 3600UL;

struct Policy {
  uint32_t direct_s;        // 3600, clamp [1800, 86400]
  uint32_t one_hop_s;       // 7200, clamp [3600, 86400]
  uint32_t grace_s;         // 1800, clamp [600, 7200]
  uint8_t  max_requests;    // 2, clamp [1, 3]
  char     version[16];     // echoed in claims; NUL-terminated
  bool     paused;
  uint32_t paused_until;    // 0 = until lifted
  bool     observer_blocked;
  bool     has_version_ok;  // CoreScope answered for the version that asked
  bool     version_ok;
  char     min_plugin_version[12];
  uint8_t  excluded[kMaxExcluded][kPubKeySize];
  uint8_t  excluded_count;
  struct NodeWindow {
    uint8_t  node[kPubKeySize];
    uint32_t window_s;
  } node_windows[kMaxNodeWindows];  // window clamped to the direct_s range
  uint8_t  node_window_count;
  // Fetch bookkeeping. The switches above stay as they are across a failed fetch.
  bool     ever_fetched;
  uint32_t fetched_at;      // last successful fetch; 0 = none
  int16_t  last_status;     // HTTP status of the last attempt; 0 = no answer
};

// Blocked outranks a bad version, which outranks a pause.
enum Gate : uint8_t { GATE_OPEN = 0, GATE_BLOCKED, GATE_VERSION, GATE_PAUSED };

// What another module keeps in flash so a reboot still honors the switches.
struct SavedGate {
  bool     blocked;
  bool     version_disabled;
  bool     paused;
  uint32_t paused_until;
};

static inline uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static inline void policyDefaults(Policy& p) {
  memset(&p, 0, sizeof(p));
  p.direct_s = kDirectDefaultS;
  p.one_hop_s = kOneHopDefaultS;
  p.grace_s = kGraceDefaultS;
  p.max_requests = kMaxRequestsDefault;
}

static inline void policyClamp(Policy& p) {
  p.direct_s = clampU32(p.direct_s, kDirectMinS, kDirectMaxS);
  p.one_hop_s = clampU32(p.one_hop_s, kOneHopMinS, kOneHopMaxS);
  p.grace_s = clampU32(p.grace_s, kGraceMinS, kGraceMaxS);
  if (p.max_requests < kMaxRequestsMin) p.max_requests = kMaxRequestsMin;
  if (p.max_requests > kMaxRequestsMax) p.max_requests = kMaxRequestsMax;
  // A full buffer with no NUL would run off in strcmp.
  p.version[sizeof(p.version) - 1] = '\0';
  p.min_plugin_version[sizeof(p.min_plugin_version) - 1] = '\0';
  if (p.excluded_count > kMaxExcluded) p.excluded_count = kMaxExcluded;
  if (p.node_window_count > kMaxNodeWindows) p.node_window_count = kMaxNodeWindows;
  // A zero window is unset, not a floor: drop it so the node keeps the hop default.
  uint8_t kept = 0;
  for (uint8_t i = 0; i < p.node_window_count; i++) {
    if (p.node_windows[i].window_s == 0) continue;
    if (kept != i) p.node_windows[kept] = p.node_windows[i];
    kept++;
  }
  p.node_window_count = kept;
  // Same floor and ceiling as direct_s, whatever direct_s itself was set to.
  for (uint8_t i = 0; i < p.node_window_count; i++) {
    p.node_windows[i].window_s =
        clampU32(p.node_windows[i].window_s, kDirectMinS, kDirectMaxS);
  }
}

// x.y.z, optional leading whitespace and one leading "v". Missing or
// malformed input is 0.0.0, so an empty minimum accepts every version and a
// garbage own version does not satisfy a real minimum.
struct ParsedVersion {
  uint32_t major;
  uint32_t minor;
  uint32_t patch;
};

static inline bool parseVersionComponent(const char*& s, uint32_t* out) {
  if (*s < '0' || *s > '9') return false;
  uint32_t v = 0;
  while (*s >= '0' && *s <= '9') {
    const uint32_t digit = (uint32_t)(*s - '0');
    if (v > (UINT32_MAX - digit) / 10U) v = UINT32_MAX;
    else v = v * 10U + digit;
    s++;
  }
  *out = v;
  return true;
}

static inline ParsedVersion parseVersion(const char* text) {
  ParsedVersion v;
  v.major = 0;
  v.minor = 0;
  v.patch = 0;
  if (!text) return v;
  while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r' ||
         *text == '\f' || *text == '\v') {
    text++;
  }
  if (*text == 'v') text++;
  const char* s = text;
  ParsedVersion parsed;
  if (!parseVersionComponent(s, &parsed.major) || *s != '.') return v;
  s++;
  if (!parseVersionComponent(s, &parsed.minor) || *s != '.') return v;
  s++;
  if (!parseVersionComponent(s, &parsed.patch)) return v;
  return parsed;
}

static inline int compareVersions(const char* a, const char* b) {
  const ParsedVersion va = parseVersion(a);
  const ParsedVersion vb = parseVersion(b);
  if (va.major != vb.major) return va.major < vb.major ? -1 : 1;
  if (va.minor != vb.minor) return va.minor < vb.minor ? -1 : 1;
  if (va.patch != vb.patch) return va.patch < vb.patch ? -1 : 1;
  return 0;
}

static inline bool versionAccepted(const Policy& p, const char* own_version) {
  // has_version_ok is only true when the saved answer is for this build.
  // After an upgrade the caller clears it and the minimum is judged instead.
  if (p.has_version_ok) return p.version_ok;
  return compareVersions(own_version, p.min_plugin_version) >= 0;
}

static inline const char* gateName(uint8_t gate) {
  switch (gate) {
    case GATE_BLOCKED: return "blocked";
    case GATE_VERSION: return "version_disabled";
    case GATE_PAUSED: return "paused";
    default: return "";
  }
}

static inline bool pauseActive(bool paused, uint32_t paused_until, uint32_t now) {
  if (!paused) return false;
  // No end time: stay paused. An end time lifts itself at that second.
  if (paused_until == 0) return true;
  return now < paused_until;
}

static inline uint8_t policyGate(const Policy& p, const char* own_version, uint32_t now) {
  if (p.observer_blocked) return GATE_BLOCKED;
  if (!versionAccepted(p, own_version)) return GATE_VERSION;
  if (pauseActive(p.paused, p.paused_until, now)) return GATE_PAUSED;
  return GATE_OPEN;
}

static inline bool policyExcluded(const Policy& p, const uint8_t node[kPubKeySize]) {
  if (!node) return false;
  uint8_t n = p.excluded_count;
  if (n > kMaxExcluded) n = kMaxExcluded;
  for (uint8_t i = 0; i < n; i++) {
    if (memcmp(p.excluded[i], node, kPubKeySize) == 0) return true;
  }
  return false;
}

static inline bool policyStale(const Policy& p, uint32_t now) {
  // A 404 answered (there is no policy). Never fetched: the defaults stand.
  if (!p.ever_fetched) return false;
  if (p.last_status == 200 || p.last_status == 404) return false;
  // No successful-fetch time, or one still in the future: not proven old.
  if (p.fetched_at == 0 || p.fetched_at > now) return false;
  return (now - p.fetched_at) > kStalePolicyS;
}

static inline uint32_t policyWindow(const Policy& p, const uint8_t node[kPubKeySize],
                                    uint8_t hops, uint32_t now) {
  uint32_t w = (hops == 0) ? p.direct_s : p.one_hop_s;
  if (node) {
    uint8_t n = p.node_window_count;
    if (n > kMaxNodeWindows) n = kMaxNodeWindows;
    for (uint8_t i = 0; i < n; i++) {
      if (memcmp(p.node_windows[i].node, node, kPubKeySize) == 0) {
        w = p.node_windows[i].window_s;
        break;
      }
    }
  }
  if (!policyStale(p, now)) return w;
  if (w > UINT32_MAX / 2U) return UINT32_MAX;
  return w * 2U;
}

static inline SavedGate savedGateFrom(const Policy& p, const char* own_version) {
  SavedGate g;
  g.blocked = p.observer_blocked;
  g.version_disabled = !versionAccepted(p, own_version);
  g.paused = p.paused;
  g.paused_until = p.paused_until;
  return g;
}

static inline uint8_t savedGate(const SavedGate& g, uint32_t now) {
  if (g.blocked) return GATE_BLOCKED;
  if (g.version_disabled) return GATE_VERSION;
  if (pauseActive(g.paused, g.paused_until, now)) return GATE_PAUSED;
  return GATE_OPEN;
}

// Days since 1970-01-01. Howard Hinnant's civil calendar, public domain.
static inline int32_t daysFromCivil(int32_t y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153U * (m + (m > 2 ? -3 : 9)) + 2U) / 5U + d - 1U;
  const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

static inline void civilFromDays(int32_t z, int32_t* y_out, unsigned* m_out, unsigned* d_out) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
  int32_t y = (int32_t)yoe + era * 400;
  const unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
  const unsigned mp = (5U * doy + 2U) / 153U;
  const unsigned d = doy - (153U * mp + 2U) / 5U + 1U;
  const unsigned m = mp < 10U ? mp + 3U : mp - 9U;
  y += (m <= 2);
  *y_out = y;
  *m_out = m;
  *d_out = d;
}

static inline bool civilYmdOk(int y, int m, int d) {
  if (m < 1 || m > 12 || d < 1) return false;
  static const uint8_t kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int dim = kDays[m];
  const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
  if (m == 2 && leap) dim = 29;
  return d <= dim;
}

static inline int parseNDigits(const char*& s, int n, int* out) {
  int v = 0;
  for (int i = 0; i < n; i++) {
    if (*s < '0' || *s > '9') return 0;
    v = v * 10 + (*s - '0');
    s++;
  }
  *out = v;
  return 1;
}

// UTC instant. Fractional seconds are dropped, not rounded. A zone is
// required (Z or a numeric offset); a bare local time is rejected.
static inline bool parseIsoUtc(const char* text, uint32_t* out) {
  if (!text || !out || !*text) return false;
  const char* s = text;
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (!parseNDigits(s, 4, &year) || *s != '-') return false;
  s++;
  if (!parseNDigits(s, 2, &month) || *s != '-') return false;
  s++;
  if (!parseNDigits(s, 2, &day)) return false;
  if (*s != 'T' && *s != ' ') return false;
  s++;
  if (!parseNDigits(s, 2, &hour) || *s != ':') return false;
  s++;
  if (!parseNDigits(s, 2, &minute) || *s != ':') return false;
  s++;
  if (!parseNDigits(s, 2, &second)) return false;
  if (*s == '.') {
    s++;
    if (*s < '0' || *s > '9') return false;
    while (*s >= '0' && *s <= '9') s++;
  }
  if (hour > 23 || minute > 59 || second > 59) return false;
  if (!civilYmdOk(year, month, day)) return false;

  int offset = 0;
  if (*s == 'Z' || *s == 'z') {
    s++;
  } else if (*s == '+' || *s == '-') {
    const int sign = (*s == '+') ? 1 : -1;
    s++;
    int oh = 0, om = 0, os = 0;
    if (!parseNDigits(s, 2, &oh)) return false;
    if (*s == ':') {
      s++;
      if (!parseNDigits(s, 2, &om)) return false;
      if (*s == ':') {
        s++;
        if (!parseNDigits(s, 2, &os)) return false;
      }
    } else if (*s >= '0' && *s <= '9') {
      if (!parseNDigits(s, 2, &om)) return false;
      if (*s >= '0' && *s <= '9') {
        if (!parseNDigits(s, 2, &os)) return false;
      }
    }
    if (oh > 23 || om > 59 || os > 59) return false;
    offset = sign * (oh * 3600 + om * 60 + os);
  } else {
    return false;
  }
  if (*s != '\0') return false;

  const int64_t days = daysFromCivil((int32_t)year, (unsigned)month, (unsigned)day);
  int64_t epoch = days * 86400LL + (int64_t)hour * 3600LL + (int64_t)minute * 60LL + second;
  epoch -= offset;
  if (epoch < 0 || epoch > (int64_t)UINT32_MAX) return false;
  *out = (uint32_t)epoch;
  return true;
}

static inline void write2(char*& p, unsigned v) {
  *p++ = (char)('0' + (v / 10U) % 10U);
  *p++ = (char)('0' + v % 10U);
}

static inline size_t formatIsoUtc(uint32_t epoch, char* out, size_t cap) {
  if (!out || cap < 21) return 0;
  const int32_t days = (int32_t)(epoch / 86400UL);
  const unsigned sod = (unsigned)(epoch % 86400UL);
  int32_t year = 0;
  unsigned month = 0, day = 0;
  civilFromDays(days, &year, &month, &day);
  if (year < 0 || year > 9999) return 0;
  char* p = out;
  write2(p, (unsigned)(year / 100));
  write2(p, (unsigned)(year % 100));
  *p++ = '-';
  write2(p, month);
  *p++ = '-';
  write2(p, day);
  *p++ = 'T';
  write2(p, sod / 3600U);
  *p++ = ':';
  write2(p, (sod % 3600U) / 60U);
  *p++ = ':';
  write2(p, sod % 60U);
  *p++ = 'Z';
  *p = '\0';
  return 20;
}

}  // namespace poll
