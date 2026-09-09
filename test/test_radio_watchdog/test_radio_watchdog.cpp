#include "helpers/RadioWatchdog.h"

#include <gtest/gtest.h>

namespace {

const uint32_t WATCHDOG_MS = 300000UL;   // RADIO_WATCHDOG_MS default: 5 minutes

// Mirrors Dispatcher::loop(): the radio's three timestamps are sampled every
// pass, and a recovery is acknowledged as soon as it is decided.
struct Radio {
  uint32_t last_recv = 0;
  uint32_t last_irq = 0;
  uint32_t last_tx = 0;
};

RadioWatchdogDecision run(RadioWatchdog& w, const Radio& r, uint32_t now,
                          bool in_recv_mode = true, uint32_t watchdog_ms = WATCHDOG_MS) {
  RadioWatchdogDecision d = w.update(now, in_recv_mode, watchdog_ms,
                                     r.last_recv, r.last_irq, r.last_tx);
  if (d.recover) w.noteRecovery();
  return d;
}

} // namespace

TEST(RadioWatchdog, NoActivitySinceBootNeverTrips) {
  RadioWatchdog w;
  w.reset(0);
  Radio r;   // radio has never received, interrupted or transmitted

  for (uint32_t t = 0; t <= 4 * WATCHDOG_MS; t += 10000) {
    RadioWatchdogDecision d = run(w, r, t);
    EXPECT_FALSE(d.recover) << "tripped at t=" << t;
    EXPECT_FALSE(d.measurable);
  }
}

TEST(RadioWatchdog, ContinuousReceiveNeverTrips) {
  RadioWatchdog w;
  w.reset(0);
  Radio r;

  for (uint32_t t = 1000; t <= 3 * WATCHDOG_MS; t += 1000) {
    r.last_recv = t;
    r.last_irq = t;
    RadioWatchdogDecision d = run(w, r, t);
    EXPECT_FALSE(d.recover) << "tripped at t=" << t;
    EXPECT_LE(d.silent_ms, 1000u);
  }
}

TEST(RadioWatchdog, SilentRadioTripsOncePerInterval) {
  RadioWatchdog w;
  w.reset(0);
  Radio r;
  r.last_recv = 1000;
  run(w, r, 1000);

  int trips = 0;
  uint32_t first_trip = 0;
  for (uint32_t t = 2000; t <= 1000000; t += 1000) {
    if (run(w, r, t).recover) {
      if (trips == 0) first_trip = t;
      trips++;
    }
  }

  EXPECT_EQ(302000u, first_trip);           // strictly past the interval, from the last receive
  EXPECT_EQ(3, trips);                      // ~1 M ms / 300 s, never per-loop
}

// F13: a transmit timestamp from just before the millis() wrap is numerically
// larger than every fresh post-wrap receive. Selecting the largest timestamp
// reported ~24 days of silence and reset a radio that was receiving normally.
TEST(RadioWatchdog, PreWrapTransmitDoesNotSilenceFreshPostWrapReceives) {
  const uint32_t WRAP = 0xFFFFFFFFu;
  const uint32_t TX_AT = 0x80000000u;       // upper half: beats any post-wrap value

  RadioWatchdog w;
  w.reset(TX_AT - 60000);
  Radio r;
  r.last_recv = TX_AT - 60000;
  r.last_irq = TX_AT - 60000;
  run(w, r, TX_AT - 60000);

  r.last_tx = TX_AT;
  run(w, r, TX_AT);

  // Receive steadily right up to the wrap...
  for (uint32_t t = TX_AT + 1000; t < WRAP - 1000; t += 1000) {
    r.last_recv = t;
    r.last_irq = t;
    ASSERT_FALSE(run(w, r, t).recover) << "tripped before wrap at t=" << t;
  }

  // ...and straight through it. The stale transmit is still the numerically
  // largest of the three timestamps for the next ~24 days.
  for (uint32_t t = 1000; t < 2 * WATCHDOG_MS; t += 1000) {
    r.last_recv = t;
    r.last_irq = t;
    RadioWatchdogDecision d = run(w, r, t);
    ASSERT_FALSE(d.recover) << "tripped after wrap at t=" << t;
    ASSERT_LE(d.silent_ms, 1000u);
  }
}

// The wrap must not hide a genuinely silent radio either.
TEST(RadioWatchdog, SilenceAcrossTheWrapIsStillDetected) {
  const uint32_t LAST_RX = 0xFFFF0000u;     // ~65 s before the wrap

  RadioWatchdog w;
  w.reset(LAST_RX - 1000);
  Radio r;
  r.last_recv = LAST_RX;
  r.last_irq = LAST_RX;
  run(w, r, LAST_RX);

  bool tripped = false;
  uint64_t silent_at_trip = 0;
  for (uint32_t step = 1000; step <= WATCHDOG_MS + 5000; step += 1000) {
    uint32_t t = LAST_RX + step;            // wraps on its own
    RadioWatchdogDecision d = run(w, r, t);
    if (d.recover) {
      tripped = true;
      silent_at_trip = d.silent_ms;
      break;
    }
  }

  EXPECT_TRUE(tripped);
  EXPECT_GT(silent_at_trip, (uint64_t)WATCHDOG_MS);
}

// Uptime past 2^32 ms must not alias a long silence back to "recent".
TEST(RadioWatchdog, SilenceBeyondOneMillisCycleKeepsGrowing) {
  RadioWatchdog w;
  w.reset(0);
  Radio r;
  r.last_recv = 1000;
  run(w, r, 1000);

  // Walk a full cycle in 1-minute steps; the raw timestamp aliases back to its
  // starting value, the tracked age does not.
  uint64_t last_silent = 0;
  for (uint64_t step = 60000; step <= 0x100000000ull + 600000ull; step += 60000) {
    uint32_t t = (uint32_t)((1000ull + step) & 0xFFFFFFFFull);
    RadioWatchdogDecision d = w.update(t, true, WATCHDOG_MS, r.last_recv, r.last_irq, r.last_tx);
    if (d.recover) w.noteRecovery();
    ASSERT_GT(d.silent_ms, last_silent);
    last_silent = d.silent_ms;
  }

  EXPECT_GT(last_silent, 0x100000000ull);
}

TEST(RadioWatchdog, DisabledWatchdogAndNonRecvModeNeverTrip) {
  RadioWatchdog w;
  w.reset(0);
  Radio r;
  r.last_recv = 1000;
  run(w, r, 1000);

  for (uint32_t t = 2000; t <= 1000000; t += 10000) {
    EXPECT_FALSE(run(w, r, t, /*in_recv_mode=*/true, /*watchdog_ms=*/0).recover);
    EXPECT_FALSE(run(w, r, t, /*in_recv_mode=*/false).recover);
  }
}

// Transmit-only traffic still counts as a live radio.
TEST(RadioWatchdog, TransmitActivityRefreshesSilence) {
  RadioWatchdog w;
  w.reset(0);
  Radio r;
  r.last_recv = 1000;
  run(w, r, 1000);

  for (uint32_t t = 100000; t <= 900000; t += 100000) {
    r.last_tx = t;
    EXPECT_FALSE(run(w, r, t).recover) << "tripped at t=" << t;
  }
}

// A blocked mesh loop (e.g. a 30 s NTP probe) collapses several events into one
// observation. That may only shorten measured silence, never lengthen it.
TEST(RadioWatchdog, LoopStallDoesNotManufactureSilence) {
  RadioWatchdog w;
  w.reset(0);
  Radio r;
  r.last_recv = 1000;
  run(w, r, 1000);

  r.last_recv = 20000;   // arrived during the stall
  r.last_irq = 20000;
  RadioWatchdogDecision d = run(w, r, 31000);

  EXPECT_FALSE(d.recover);
  EXPECT_EQ(0u, d.silent_ms);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
