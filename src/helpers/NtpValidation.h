#pragma once

#include <stdint.h>
#include <string.h>

// Client-side NTPv4 request building and response validation.
//
// Pure logic: no Arduino, WiFi or UDP headers, so every rejection case is
// host-testable. The caller owns the socket and supplies the datagram plus
// whether it arrived from the server it asked (see MQTTBridge::probeNtpServer).
//
// This exists because the NTPClient library accepted *any* non-empty datagram
// on its fixed local port as time: it ignored the read length, the mode, the
// stratum, the leap indicator and the request/response correlation, so a
// one-byte packet from anywhere set the clock — which then feeds JWT issuance,
// certificate validity and the RTC. Everything below is the validation that
// path never had.
namespace NtpValidation {

static const size_t kPacketSize = 48;

// Seconds between the NTP epoch (1900-01-01) and the Unix epoch (1970-01-01).
static const uint32_t kUnixEpochOffset = 2208988800UL;
// Wrap distance for NTP era 1, which starts 2036-02-07: era-1 seconds count
// from there, so a small NTP seconds field is a *future* time, not a past one.
static const uint32_t kEra1UnixOffset = 2085978496UL;

enum Reject : uint8_t {
  kAccepted = 0,
  kShortPacket,       // fewer than 48 bytes: not an NTP response at all
  kWrongSource,       // not from the address/port we queried
  kBadVersion,        // NTP version outside 3..4
  kBadMode,           // not mode 4 (server)
  kLeapAlarm,         // LI=3: the server itself is unsynchronised
  kBadStratum,        // 0 (kiss-of-death) or >= 16 (unsynchronised)
  kOriginMismatch,    // did not echo the transmit timestamp we sent
  kZeroTransmit,      // no transmit timestamp
  kEpochTooEarly,     // before the caller's floor
  kEpochTooLate,      // implausibly far in the future
};

// The transmit timestamp we send and the server must echo back in its originate
// field. Random, so an off-path sender cannot answer a query it never saw.
struct Nonce {
  uint32_t seconds;
  uint32_t fraction;
};

struct Result {
  Reject   reject;
  uint32_t epoch;     // Unix seconds; only meaningful when reject == kAccepted
};

static inline uint32_t readU32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void writeU32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

// LI=0 (no warning), VN=4, Mode=3 (client). The NTPClient request claimed LI=3,
// advertising an alarm condition it had no business asserting.
static inline void buildRequest(uint8_t out[kPacketSize], Nonce nonce) {
  memset(out, 0, kPacketSize);
  out[0] = 0x23;
  out[1] = 0;      // stratum: unspecified
  out[2] = 6;      // poll interval
  out[3] = 0xEC;   // precision
  writeU32(out + 40, nonce.seconds);     // transmit timestamp = our nonce
  writeU32(out + 44, nonce.fraction);
}

// Unix time for an NTP seconds field, handling the 2036 era rollover instead of
// wrapping into 1900 the way an unchecked subtraction does.
static inline uint32_t unixFromNtpSeconds(uint32_t ntp_seconds) {
  return ntp_seconds >= kUnixEpochOffset ? (ntp_seconds - kUnixEpochOffset)
                                         : (ntp_seconds + kEra1UnixOffset);
}

// `source_matches` is the caller's check that the datagram came from the
// address and port it queried; it is a parameter rather than a lookup so this
// stays free of socket types.
static inline Result validate(const uint8_t* data, size_t len, bool source_matches,
                              Nonce nonce, uint32_t min_epoch, uint32_t max_epoch) {
  Result r;
  r.epoch = 0;

  if (data == nullptr || len < kPacketSize) { r.reject = kShortPacket; return r; }
  if (!source_matches)                      { r.reject = kWrongSource; return r; }

  const uint8_t li = (uint8_t)((data[0] >> 6) & 0x03);
  const uint8_t version = (uint8_t)((data[0] >> 3) & 0x07);
  const uint8_t mode = (uint8_t)(data[0] & 0x07);
  const uint8_t stratum = data[1];

  if (version < 3 || version > 4) { r.reject = kBadVersion; return r; }
  if (mode != 4)                  { r.reject = kBadMode; return r; }
  if (li == 3)                    { r.reject = kLeapAlarm; return r; }
  // Stratum 0 carries a kiss-of-death code (RATE/DENY/RSTR) rather than time;
  // 16 and above means the server has no time to give.
  if (stratum == 0 || stratum >= 16) { r.reject = kBadStratum; return r; }

  if (readU32(data + 24) != nonce.seconds ||
      readU32(data + 28) != nonce.fraction) { r.reject = kOriginMismatch; return r; }

  const uint32_t transmit_seconds = readU32(data + 40);
  if (transmit_seconds == 0) { r.reject = kZeroTransmit; return r; }

  const uint32_t epoch = unixFromNtpSeconds(transmit_seconds);
  if (epoch < min_epoch) { r.reject = kEpochTooEarly; return r; }
  if (epoch > max_epoch) { r.reject = kEpochTooLate; return r; }

  r.reject = kAccepted;
  r.epoch = epoch;
  return r;
}

// Short enough for a LoRa-bounded diagnostic reply.
static inline const char* rejectReason(Reject reject) {
  switch (reject) {
    case kAccepted:       return "ok";
    case kShortPacket:    return "short packet";
    case kWrongSource:    return "wrong source";
    case kBadVersion:     return "bad version";
    case kBadMode:        return "not a server reply";
    case kLeapAlarm:      return "server unsynced";
    case kBadStratum:     return "bad stratum";
    case kOriginMismatch: return "unsolicited reply";
    case kZeroTransmit:   return "no timestamp";
    case kEpochTooEarly:  return "time too old";
    case kEpochTooLate:   return "time too far ahead";
    default:              return "invalid";
  }
}

} // namespace NtpValidation
