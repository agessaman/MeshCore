#include "helpers/RadioActivityWindow.h"

#include <gtest/gtest.h>

namespace {

const uint32_t MINUTE = RADIO_ACTIVITY_BUCKET_MS;
const int N = RADIO_ACTIVITY_BUCKETS;

// Representative packet: 32 wire bytes, 100 ms airtime, +7.0 dB SNR, -95 dBm.
void recordTypical(RadioActivityWindow& w, uint32_t at_ms, uint16_t bytes = 32) {
  w.recordPacket(at_ms, bytes, 100, 28, -95);
}

RadioActivitySnapshot snapshotAt(RadioActivityWindow& w, uint32_t at_ms) {
  RadioActivitySnapshot s;
  w.snapshot(at_ms, &s);
  return s;
}

} // namespace

TEST(RadioActivityWindow, EmptySnapshotHasNoTotalsAndNoDivisionByZero) {
  RadioActivityWindow w;
  w.reset(0);

  RadioActivitySnapshot s = snapshotAt(w, 0);

  EXPECT_TRUE(s.isEmpty());
  EXPECT_EQ(0u, s.packets);
  EXPECT_EQ(0u, s.wire_bytes);
  EXPECT_EQ(0u, s.window_ms);
  EXPECT_FALSE(s.has_last_packet);
  EXPECT_EQ(0u, s.peak_per_min);

  // Every derived value must be defined with a zero denominator.
  EXPECT_EQ(0u, s.packetsPerMinuteX10());
  EXPECT_EQ(0u, s.bytesPerSecondX10());
  EXPECT_EQ(0u, s.avgBytesPerPacket());
  EXPECT_EQ(0u, s.airtimePercentX10());
  EXPECT_EQ(0, s.avgSnrX10());
  EXPECT_EQ(0, s.avgRssi());

  for (int i = 0; i < N; i++) EXPECT_EQ(0u, s.buckets[i]);
}

TEST(RadioActivityWindow, SingleEventProducesExactTotalsAndRates) {
  RadioActivityWindow w;
  w.reset(0);
  recordTypical(w, 1000);

  RadioActivitySnapshot s = snapshotAt(w, 2000);

  EXPECT_EQ(1u, s.packets);
  EXPECT_EQ(32u, s.wire_bytes);
  EXPECT_EQ(100u, s.airtime_ms);
  EXPECT_EQ(2000u, s.window_ms);
  EXPECT_EQ(2000u, s.tracking_ms);

  EXPECT_EQ(300u, s.packetsPerMinuteX10());   // 30.0 packets/min
  EXPECT_EQ(160u, s.bytesPerSecondX10());     // 16.0 B/s
  EXPECT_EQ(32u, s.avgBytesPerPacket());
  EXPECT_EQ(50u, s.airtimePercentX10());      // 5.0 %
  EXPECT_EQ(70, s.avgSnrX10());               // +7.0 dB
  EXPECT_EQ(-95, s.avgRssi());

  EXPECT_TRUE(s.has_last_packet);
  EXPECT_EQ(1000u, s.last_packet_age_ms);

  // The current minute is the rightmost bucket.
  EXPECT_EQ(1u, s.buckets[N - 1]);
  for (int i = 0; i < N - 1; i++) EXPECT_EQ(0u, s.buckets[i]);
}

TEST(RadioActivityWindow, MultipleEventsInOneMinuteAccumulate) {
  RadioActivityWindow w;
  w.reset(0);
  recordTypical(w, 1000, 10);
  recordTypical(w, 2000, 20);
  recordTypical(w, 3000, 30);

  RadioActivitySnapshot s = snapshotAt(w, 4000);

  EXPECT_EQ(3u, s.packets);
  EXPECT_EQ(60u, s.wire_bytes);
  EXPECT_EQ(300u, s.airtime_ms);
  EXPECT_EQ(20u, s.avgBytesPerPacket());
  EXPECT_EQ(3u, s.buckets[N - 1]);
  EXPECT_EQ(3u, s.peak_per_min);
  EXPECT_EQ(1000u, s.last_packet_age_ms);
}

TEST(RadioActivityWindow, EventsRotateIntoTheNextBucketAtTheMinuteBoundary) {
  RadioActivityWindow w;
  w.reset(0);
  recordTypical(w, 30000);           // minute 0
  recordTypical(w, MINUTE);          // exactly on the boundary: minute 1
  recordTypical(w, MINUTE + 5000);   // minute 1

  RadioActivitySnapshot s = snapshotAt(w, MINUTE + 10000);

  EXPECT_EQ(3u, s.packets);
  EXPECT_EQ(2u, s.buckets[N - 1]);   // current minute
  EXPECT_EQ(1u, s.buckets[N - 2]);   // previous minute
  EXPECT_EQ(2u, s.peak_per_min);
}

TEST(RadioActivityWindow, BucketsAreOrderedOldestToNewest) {
  RadioActivityWindow w;
  w.reset(0);

  // Minute m gets (m + 1) packets.
  for (int m = 0; m < N; m++) {
    for (int i = 0; i <= m; i++) recordTypical(w, m * MINUTE + 1000 + i);
  }

  RadioActivitySnapshot s = snapshotAt(w, (N - 1) * MINUTE + 30000);

  for (int i = 0; i < N; i++) {
    EXPECT_EQ((uint16_t)(i + 1), s.buckets[i]) << "bucket " << i;
  }
  EXPECT_EQ((uint16_t)N, s.peak_per_min);
  EXPECT_EQ((uint32_t)(N * (N + 1) / 2), s.packets);
}

TEST(RadioActivityWindow, OldestBucketExpiresOnceItLeavesTheWindow) {
  RadioActivityWindow w;
  w.reset(0);

  for (int m = 0; m < N; m++) recordTypical(w, m * MINUTE + 1000);

  // Still inside the window: all 20 minutes are represented.
  RadioActivitySnapshot before = snapshotAt(w, (N - 1) * MINUTE + 59999);
  EXPECT_EQ((uint32_t)N, before.packets);
  EXPECT_EQ(1u, before.buckets[0]);

  // One tick past the boundary: the oldest minute is gone, and the new current
  // minute is empty.
  RadioActivitySnapshot after = snapshotAt(w, N * MINUTE);
  EXPECT_EQ((uint32_t)(N - 1), after.packets);
  EXPECT_EQ(1u, after.buckets[0]);       // what was minute 1
  EXPECT_EQ(0u, after.buckets[N - 1]);   // the fresh current minute
}

TEST(RadioActivityWindow, MoreThanTwentyMinutesOfSilenceClearsTheRing) {
  RadioActivityWindow w;
  w.reset(0);
  recordTypical(w, 1000);

  uint32_t now = 21 * MINUTE;
  RadioActivitySnapshot s = snapshotAt(w, now);

  EXPECT_TRUE(s.isEmpty());
  for (int i = 0; i < N; i++) EXPECT_EQ(0u, s.buckets[i]);

  // Tracking restarts at the current minute, so the window reports itself as
  // warming up again rather than claiming 20 minutes of empty coverage.
  EXPECT_EQ(0u, s.tracking_ms);
  EXPECT_EQ(0u, s.window_ms);
  EXPECT_TRUE(s.isWarmingUp());

  // The last-packet age survives the ring clear: it is still the most useful
  // thing to show when nothing is arriving.
  EXPECT_TRUE(s.has_last_packet);
  EXPECT_EQ(now - 1000, s.last_packet_age_ms);
}

TEST(RadioActivityWindow, LastPacketAgeIsDroppedOnceItGoesStale) {
  RadioActivityWindow w;
  w.reset(0);
  recordTypical(w, 1000);

  RadioActivitySnapshot fresh = snapshotAt(w, 1000 + RADIO_ACTIVITY_MAX_AGE_MS);
  EXPECT_TRUE(fresh.has_last_packet);

  RadioActivitySnapshot stale = snapshotAt(w, 1000 + RADIO_ACTIVITY_MAX_AGE_MS + 1);
  EXPECT_FALSE(stale.has_last_packet);
}

TEST(RadioActivityWindow, WarmupUsesObservedDurationNotAFixedTwentyMinutes) {
  RadioActivityWindow w;
  w.reset(0);
  recordTypical(w, 30000);

  // Five minutes in, rates are computed against five minutes, not twenty.
  RadioActivitySnapshot warm = snapshotAt(w, 5 * MINUTE);
  EXPECT_TRUE(warm.isWarmingUp());
  EXPECT_EQ(5u, warm.warmupMinutes());
  EXPECT_EQ(5 * MINUTE, warm.window_ms);
  // 1 packet over 5 minutes is 0.2/min. Against a fixed 1200 s denominator the
  // same data would round away to 0.0/min.
  EXPECT_EQ(2u, warm.packetsPerMinuteX10());
}

TEST(RadioActivityWindow, SteadyStateWindowNeverClaimsMoreCoverageThanTheRingHas) {
  RadioActivityWindow w;
  w.reset(0);
  for (int m = 0; m < 25; m++) recordTypical(w, m * MINUTE + 1000);

  // 19 whole minutes plus the elapsed part of the current one - never 20:00.
  RadioActivitySnapshot at_start = snapshotAt(w, 25 * MINUTE);
  EXPECT_FALSE(at_start.isWarmingUp());
  EXPECT_EQ(19 * MINUTE, at_start.window_ms);

  RadioActivitySnapshot mid = snapshotAt(w, 25 * MINUTE + 30000);
  EXPECT_EQ(19 * MINUTE + 30000, mid.window_ms);

  RadioActivitySnapshot late = snapshotAt(w, 25 * MINUTE + 59999);
  EXPECT_EQ(19 * MINUTE + 59999, late.window_ms);
  EXPECT_LT(late.window_ms, (uint32_t)N * MINUTE);
}

TEST(RadioActivityWindow, PeakIsTheBusiestVisibleMinute) {
  RadioActivityWindow w;
  w.reset(0);
  recordTypical(w, 1000);
  for (int i = 0; i < 7; i++) recordTypical(w, MINUTE + 1000 + i);
  recordTypical(w, 2 * MINUTE + 1000);

  EXPECT_EQ(7u, snapshotAt(w, 2 * MINUTE + 30000).peak_per_min);

  // Once the busy minute ages out of the ring, so does the peak.
  EXPECT_EQ(1u, snapshotAt(w, 21 * MINUTE).peak_per_min);
}

TEST(RadioActivityWindow, SurvivesMillisRollover) {
  const uint32_t base = 0xFFFFF000u;   // ~4 s before the 32-bit wrap
  RadioActivityWindow w;
  w.reset(base);

  recordTypical(w, base + 1000);

  // 65 s later, which is 60904 in wrapped millis().
  uint32_t after_wrap = (uint32_t)(base + 65000);
  ASSERT_LT(after_wrap, base) << "test setup must actually cross the wrap";
  recordTypical(w, after_wrap);

  RadioActivitySnapshot s = snapshotAt(w, after_wrap + 1000);

  EXPECT_EQ(2u, s.packets);
  EXPECT_EQ(1u, s.buckets[N - 1]);   // the post-wrap minute
  EXPECT_EQ(1u, s.buckets[N - 2]);   // the pre-wrap minute
  EXPECT_EQ(66000u, s.window_ms);
  EXPECT_EQ(1000u, s.last_packet_age_ms);
}

TEST(RadioActivityWindow, RolloverDoesNotCorruptTheMinuteBoundary) {
  // A boundary derived from now_ms / BUCKET_MS would misplace a minute here,
  // because 2^32 is not a whole number of 60000 ms buckets.
  const uint32_t base = 0xFFFFFFFFu - 30000u;
  RadioActivityWindow w;
  w.reset(base);

  for (int m = 0; m < 5; m++) recordTypical(w, (uint32_t)(base + m * MINUTE + 1000));

  RadioActivitySnapshot s = snapshotAt(w, (uint32_t)(base + 4 * MINUTE + 30000));

  EXPECT_EQ(5u, s.packets);
  for (int i = 0; i < 5; i++) {
    EXPECT_EQ(1u, s.buckets[N - 1 - i]) << "minute -" << i;
  }
  EXPECT_EQ(1u, s.peak_per_min);
}

TEST(RadioActivityWindow, SurvivesAFullMillisCycleOfContinuousUptime) {
  // The always-on dashboard services the tracker every few seconds forever. Past
  // 2^32 ms (~49.7 days) a 32-bit tracker age wraps back to a small value, which
  // would drop the window into warm-up and divide 20 minutes of traffic by
  // seconds - inflating every rate on screen.
  RadioActivityWindow w;
  w.reset(0);

  const uint32_t STEP = 30000;   // two packets per minute bucket
  uint32_t now = 0;
  for (uint64_t elapsed = 0; elapsed < 0x100000000ull + 10 * MINUTE; elapsed += STEP) {
    recordTypical(w, now);
    RadioActivitySnapshot tick;
    w.snapshot(now, &tick);
    now += STEP;
  }

  RadioActivitySnapshot s = snapshotAt(w, now);

  EXPECT_FALSE(s.isWarmingUp()) << "must not fall back into warm-up after the wrap";
  EXPECT_GE(s.window_ms, 19 * MINUTE);
  EXPECT_LE(s.window_ms, (uint32_t)N * MINUTE);

  // 19 whole minutes at two packets each, plus however much of the current
  // minute has elapsed.
  EXPECT_GE(s.packets, 38u);
  EXPECT_LE(s.packets, 41u);
  // Two packets a minute, and it must still read as two.
  EXPECT_GE(s.packetsPerMinuteX10(), 15u);
  EXPECT_LE(s.packetsPerMinuteX10(), 25u);

  EXPECT_TRUE(s.has_last_packet);
  EXPECT_EQ(STEP, s.last_packet_age_ms);
}

TEST(RadioActivityWindow, StaleLastPacketDoesNotComeBackAfterTheWrap) {
  RadioActivityWindow w;
  w.reset(0);
  recordTypical(w, 1000);

  // Serviced continuously, but silent, for more than one full 32-bit cycle.
  uint32_t now = 0;
  const uint32_t STEP = 60000;
  for (uint64_t elapsed = 0; elapsed < 0x100000000ull + 10 * MINUTE; elapsed += STEP) {
    RadioActivitySnapshot tick;
    w.snapshot(now, &tick);
    if (elapsed > RADIO_ACTIVITY_MAX_AGE_MS) {
      ASSERT_FALSE(tick.has_last_packet) << "a stale age must never look fresh again";
    }
    now += STEP;
  }

  RadioActivitySnapshot s = snapshotAt(w, now);
  EXPECT_TRUE(s.isEmpty());
  EXPECT_FALSE(s.has_last_packet);
}

TEST(RadioActivityWindow, SaturatedMinuteDropsFurtherEventsWhole) {
  RadioActivityWindow w;
  w.reset(0);

  for (uint32_t i = 0; i < 65535; i++) w.recordPacket(1000, 10, 1, 4, -100);

  RadioActivitySnapshot full = snapshotAt(w, 2000);
  EXPECT_EQ(65535u, full.packets);
  EXPECT_EQ(655350u, full.wire_bytes);

  // Past saturation nothing is counted, so bytes-per-packet stays truthful.
  w.recordPacket(1500, 10, 1, 4, -100);
  RadioActivitySnapshot after = snapshotAt(w, 2000);
  EXPECT_EQ(65535u, after.packets);
  EXPECT_EQ(655350u, after.wire_bytes);
  EXPECT_EQ(10u, after.avgBytesPerPacket());
}

TEST(RadioActivityWindow, AnOlderTimestampDoesNotExpireTheWindow) {
  // recordPacket() and snapshot() read millis() at slightly different moments;
  // a reading that arrives out of order must cost nothing.
  RadioActivityWindow w;
  w.reset(0);
  recordTypical(w, 5000);

  RadioActivitySnapshot ahead = snapshotAt(w, 10000);
  ASSERT_EQ(1u, ahead.packets);

  recordTypical(w, 9000);   // stale reading, 1 s behind the last snapshot
  RadioActivitySnapshot s = snapshotAt(w, 10000);

  EXPECT_EQ(2u, s.packets) << "the ring must not have been cleared";
  EXPECT_EQ(2u, s.buckets[N - 1]);
  EXPECT_EQ(10000u, s.window_ms);
}

TEST(RadioActivityWindow, AveragesHandleNegativeSnrAndMixedSigns) {
  RadioActivityWindow w;
  w.reset(0);
  w.recordPacket(1000, 40, 50, 28, -80);    // +7.0 dB
  w.recordPacket(1100, 40, 50, -28, -120);  // -7.0 dB

  RadioActivitySnapshot s = snapshotAt(w, 2000);
  EXPECT_EQ(0, s.avgSnrX10());
  EXPECT_EQ(-100, s.avgRssi());
}

TEST(RadioActivityWindow, StaysWithinItsMemoryBudget) {
  EXPECT_LE(sizeof(RadioActivityWindow), 1024u);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
