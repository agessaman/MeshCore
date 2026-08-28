#include "helpers/ui/TouchTapDetector.h"

#include <gtest/gtest.h>

namespace {

// Drives the detector at the firmware's 50 ms poll cadence.
const uint32_t POLL = 50;

// Holds `pressed` for `ms`, returning how many taps were accepted.
int hold(TouchTapDetector& d, uint32_t& now, bool pressed, uint32_t ms) {
  int taps = 0;
  for (uint32_t t = 0; t < ms; t += POLL) {
    if (d.update(now, pressed)) taps++;
    now += POLL;
  }
  return taps;
}

} // namespace

TEST(TouchTapDetector, IdlePanelNeverTaps) {
  TouchTapDetector d;
  uint32_t now = 1000;
  d.reset(now);
  EXPECT_EQ(0, hold(d, now, false, 5000));
  EXPECT_FALSE(d.isTouched());
}

TEST(TouchTapDetector, OneTouchProducesExactlyOneTap) {
  TouchTapDetector d;
  uint32_t now = 1000;
  d.reset(now);

  EXPECT_EQ(1, hold(d, now, true, 300));
  EXPECT_TRUE(d.isTouched());
  EXPECT_EQ(0, hold(d, now, false, 300));
  EXPECT_FALSE(d.isTouched());
}

TEST(TouchTapDetector, HoldingAFingerDownDoesNotRepeat) {
  TouchTapDetector d;
  uint32_t now = 1000;
  d.reset(now);

  EXPECT_EQ(1, hold(d, now, true, 100));
  EXPECT_EQ(0, hold(d, now, true, 10000)) << "a long press must not toggle repeatedly";
}

TEST(TouchTapDetector, ContactShorterThanTheDebounceWindowIsIgnored) {
  TouchTapDetector d;
  uint32_t now = 1000;
  d.reset(now);

  // A single 30 ms blip, below TOUCH_TAP_DEBOUNCE_MS.
  EXPECT_FALSE(d.update(now, true));
  now += 30;
  EXPECT_FALSE(d.update(now, false));
  now += 30;
  EXPECT_EQ(0, hold(d, now, false, 500));
}

TEST(TouchTapDetector, BounceOnContactStillCountsAsOneTap) {
  TouchTapDetector d;
  uint32_t now = 1000;
  d.reset(now);

  int taps = 0;
  for (int i = 0; i < 6; i++) {          // chattering edge
    if (d.update(now, i % 2 == 0)) taps++;
    now += 10;
  }
  taps += hold(d, now, true, 200);       // then settles down
  EXPECT_EQ(1, taps);
}

TEST(TouchTapDetector, SecondTapTooSoonIsSuppressed) {
  TouchTapDetector d;
  uint32_t now = 1000;
  d.reset(now);

  EXPECT_EQ(1, hold(d, now, true, 100));
  EXPECT_EQ(0, hold(d, now, false, 100));
  EXPECT_EQ(0, hold(d, now, true, 100)) << "inside TOUCH_TAP_MIN_GAP_MS";
}

TEST(TouchTapDetector, DeliberateSecondTapIsAccepted) {
  TouchTapDetector d;
  uint32_t now = 1000;
  d.reset(now);

  EXPECT_EQ(1, hold(d, now, true, 100));
  EXPECT_EQ(0, hold(d, now, false, 600));   // past the min gap
  EXPECT_EQ(1, hold(d, now, true, 100));
}

TEST(TouchTapDetector, SurvivesMillisRollover) {
  TouchTapDetector d;
  const uint32_t start = 0xFFFFFF9Bu;   // ~100 ms short of the 32-bit wrap
  uint32_t now = start;
  d.reset(now);

  EXPECT_EQ(1, hold(d, now, true, 200));   // the touch itself crosses the wrap
  ASSERT_LT(now, start) << "test setup must actually wrap";
  EXPECT_EQ(0, hold(d, now, false, 600));
  EXPECT_EQ(1, hold(d, now, true, 200));
}

TEST(TouchTapDetector, ResetClearsPendingState) {
  TouchTapDetector d;
  uint32_t now = 1000;
  d.reset(now);
  EXPECT_EQ(1, hold(d, now, true, 200));

  d.reset(now);
  EXPECT_FALSE(d.isTouched());
  EXPECT_EQ(1, hold(d, now, true, 200)) << "a fresh probe starts clean";
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
