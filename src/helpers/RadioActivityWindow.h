#pragma once

#include <stdint.h>
#include <string.h>

// Fixed-memory rolling window of RF receive activity, bucketed by minute.
//
// Pure logic: no Arduino, radio, display or role headers. Callers supply a
// millisecond counter and the per-packet values.
//
// The caller's 32-bit millis() is extended to a monotonic 64-bit clock on entry
// (see tick()), so nothing downstream has a rollover case. Unsigned-subtraction
// tricks are not enough here: they only survive a single wrap crossing, while an
// always-on observer accumulates uptime past 2^32 ms (~49.7 days), at which
// point a 32-bit tracker age would collapse back to a small value and the
// window would re-enter warm-up and divide 20 minutes of traffic by seconds.

#define RADIO_ACTIVITY_BUCKETS    20
#define RADIO_ACTIVITY_BUCKET_MS  60000UL

// Beyond this, "time since last packet" stops being reported rather than shown
// as a stale or (after a 49-day rollover) nonsensical age.
#define RADIO_ACTIVITY_MAX_AGE_MS (100UL * RADIO_ACTIVITY_BUCKET_MS)

// How far a timestamp may run behind the previous one and still count as an
// out-of-order reading rather than a very long forward gap.
#define RADIO_ACTIVITY_BACKSTEP_TOLERANCE_MS 5000UL

struct RadioActivitySnapshot {
  uint32_t packets;
  uint32_t wire_bytes;
  uint32_t airtime_ms;
  int32_t  snr_q4_sum;
  int32_t  rssi_sum;

  // Span the totals actually cover: 19 whole minutes plus the elapsed part of
  // the current one, so it tops out just under 20 minutes and never claims
  // coverage the ring does not have.
  uint32_t window_ms;
  uint32_t tracking_ms;        // how long the tracker has been running

  uint32_t last_packet_age_ms;
  bool     has_last_packet;    // false until the first packet, and once stale

  uint16_t peak_per_min;
  uint16_t buckets[RADIO_ACTIVITY_BUCKETS];   // [0] oldest .. [N-1] current minute

  bool isEmpty() const { return packets == 0; }

  bool isWarmingUp() const {
    return tracking_ms < (uint32_t)RADIO_ACTIVITY_BUCKETS * RADIO_ACTIVITY_BUCKET_MS;
  }
  uint32_t warmupMinutes() const { return tracking_ms / RADIO_ACTIVITY_BUCKET_MS; }

  // Derived values, in integer fixed point so host tests are exact and the
  // formatting path stays off the FPU. All are zero when the window is empty.
  uint32_t packetsPerMinuteX10() const {
    if (window_ms == 0) return 0;
    return (uint32_t)(((uint64_t)packets * RADIO_ACTIVITY_BUCKET_MS * 10) / window_ms);
  }
  uint32_t bytesPerSecondX10() const {
    if (window_ms == 0) return 0;
    return (uint32_t)(((uint64_t)wire_bytes * 1000 * 10) / window_ms);
  }
  uint32_t avgBytesPerPacket() const {
    if (packets == 0) return 0;
    return (wire_bytes + packets / 2) / packets;
  }
  // Receive airtime as tenths of a percent of the window.
  uint32_t airtimePercentX10() const {
    if (window_ms == 0) return 0;
    return (uint32_t)(((uint64_t)airtime_ms * 1000) / window_ms);
  }
  // Average SNR in tenths of a dB (sums are quarter-dB units).
  int32_t avgSnrX10() const {
    if (packets == 0) return 0;
    return (snr_q4_sum * 10) / ((int32_t)packets * 4);
  }
  int32_t avgRssi() const {
    if (packets == 0) return 0;
    return rssi_sum / (int32_t)packets;
  }
};

class RadioActivityWindow {
public:
  RadioActivityWindow() { reset(0); }

  void reset(uint32_t now_ms) {
    memset(_buckets, 0, sizeof(_buckets));
    _head = 0;
    _now_ms = 0;
    _last_input_ms = now_ms;
    _bucket_start_ms = 0;
    _tracking_since_ms = 0;
    _last_packet_ms = 0;
    _ever_received = false;
  }

  void recordPacket(uint32_t now_ms, uint16_t wire_bytes, uint32_t airtime_ms, int8_t snr_q4,
                    int16_t rssi_dbm) {
    advance(now_ms);

    Bucket& b = _buckets[_head];
    // Saturated: drop the event whole so the bucket's averages stay consistent
    // with its packet count. Unreachable at any real LoRa packet rate.
    if (b.packets == 0xFFFF) return;

    b.packets++;
    b.wire_bytes += wire_bytes;
    b.airtime_ms += airtime_ms;
    b.snr_q4_sum += snr_q4;
    b.rssi_sum += rssi_dbm;

    _last_packet_ms = _now_ms;
    _ever_received = true;
  }

  void snapshot(uint32_t now_ms, RadioActivitySnapshot* out) {
    advance(now_ms);
    memset(out, 0, sizeof(*out));

    for (int i = 0; i < RADIO_ACTIVITY_BUCKETS; i++) {
      const Bucket& b = _buckets[(_head + 1 + i) % RADIO_ACTIVITY_BUCKETS];
      out->buckets[i] = b.packets;
      out->packets += b.packets;
      out->wire_bytes += b.wire_bytes;
      out->airtime_ms += b.airtime_ms;
      out->snr_q4_sum += b.snr_q4_sum;
      out->rssi_sum += b.rssi_sum;
      if (b.packets > out->peak_per_min) out->peak_per_min = b.packets;
    }

    uint64_t elapsed_in_current = _now_ms - _bucket_start_ms;   // < BUCKET_MS after advance()
    uint64_t max_span =
        (uint64_t)(RADIO_ACTIVITY_BUCKETS - 1) * RADIO_ACTIVITY_BUCKET_MS + elapsed_in_current;
    uint64_t tracking = _now_ms - _tracking_since_ms;
    const uint64_t full_span = (uint64_t)RADIO_ACTIVITY_BUCKETS * RADIO_ACTIVITY_BUCKET_MS;

    // Clamped, so the 32-bit snapshot fields stay in range on a long-lived node.
    // Past full_span the exact tracker age is not needed: the window is warm.
    out->tracking_ms = (uint32_t)(tracking < full_span ? tracking : full_span);
    out->window_ms = (uint32_t)(tracking < max_span ? tracking : max_span);

    if (_ever_received) {
      uint64_t age = _now_ms - _last_packet_ms;
      if (age <= RADIO_ACTIVITY_MAX_AGE_MS) {
        out->last_packet_age_ms = (uint32_t)age;
        out->has_last_packet = true;
      }
    }
  }

private:
  struct Bucket {
    uint32_t wire_bytes;
    uint32_t airtime_ms;
    int32_t  snr_q4_sum;
    int32_t  rssi_sum;
    uint16_t packets;
    uint16_t _reserved;
  };

  Bucket   _buckets[RADIO_ACTIVITY_BUCKETS];
  uint64_t _now_ms;               // monotonic clock, extended from the caller's
  uint64_t _bucket_start_ms;      // start of the current (newest) minute
  uint64_t _tracking_since_ms;
  uint64_t _last_packet_ms;
  uint32_t _last_input_ms;        // last 32-bit value the caller handed in
  uint8_t  _head;                 // ring index of the current minute
  bool     _ever_received;

  // Accumulates the unsigned delta since the previous call, which is correct
  // across one millis() wrap.
  //
  // A delta past the halfway mark is ambiguous: it is either a slightly stale
  // reading or a genuine gap of more than ~24.8 days. recordPacket() and
  // snapshot() sample millis() microseconds apart, so a real stale reading is
  // tiny - anything larger is treated as the long gap it is, which matters
  // because rejecting it outright would freeze the ring and leave a
  // month-old packet looking recently received.
  void tick(uint32_t now_ms) {
    uint32_t delta = now_ms - _last_input_ms;
    if (delta > 0x80000000u &&
        (uint32_t)(_last_input_ms - now_ms) <= RADIO_ACTIVITY_BACKSTEP_TOLERANCE_MS) {
      return;   // out-of-order reading: no time has passed
    }
    _now_ms += delta;
    _last_input_ms = now_ms;
  }

  // Retires expired buckets lazily, advancing the boundary by whole BUCKET_MS
  // steps so the minute phase is preserved across gaps.
  void advance(uint32_t now_ms) {
    tick(now_ms);

    uint64_t elapsed = _now_ms - _bucket_start_ms;
    if (elapsed < RADIO_ACTIVITY_BUCKET_MS) return;

    uint64_t steps = elapsed / RADIO_ACTIVITY_BUCKET_MS;
    _bucket_start_ms += steps * RADIO_ACTIVITY_BUCKET_MS;

    if (steps >= RADIO_ACTIVITY_BUCKETS) {
      memset(_buckets, 0, sizeof(_buckets));
      _head = 0;
      _tracking_since_ms = _bucket_start_ms;
      return;
    }

    for (uint64_t i = 0; i < steps; i++) {
      _head = (uint8_t)((_head + 1) % RADIO_ACTIVITY_BUCKETS);
      memset(&_buckets[_head], 0, sizeof(Bucket));
    }
  }
};

static_assert(sizeof(RadioActivityWindow) <= 1024, "RadioActivityWindow must stay under 1 KiB");
