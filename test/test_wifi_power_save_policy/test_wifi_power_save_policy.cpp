#include "helpers/WifiPowerSavePolicy.h"

#include <gtest/gtest.h>

using namespace WifiPowerSavePolicy;

// F11: the CLI applied stored 0 as MIN_MODEM while the reconnect path applied it
// as NONE, so a node configured for `min` changed behaviour after every
// reconnect and `get wifi.powersave` still reported min. One table, one answer.
TEST(WifiPowerSavePolicy, StoredValuesMapToOneModeEach) {
  EXPECT_EQ(kModeMinModem, modeFor(kMin));
  EXPECT_EQ(kModeNone, modeFor(kNone));
  EXPECT_EQ(kModeMaxModem, modeFor(kMax));
}

TEST(WifiPowerSavePolicy, NamesRoundTripWithStoredValues) {
  for (uint8_t stored = 0; stored <= 2; stored++) {
    uint8_t parsed = 0xFF;
    ASSERT_TRUE(parseName(nameFor(stored), &parsed)) << "stored " << (int)stored;
    EXPECT_EQ(stored, parsed);
    EXPECT_EQ(modeFor(stored), modeFor(parsed));
  }
}

// A byte outside the stored range must read as the product default, not as
// whatever mode happens to sit at that index.
TEST(WifiPowerSavePolicy, OutOfRangeStoredValueReadsAsDefault) {
  for (int stored = 3; stored <= 255; stored++) {
    EXPECT_EQ(kModeNone, modeFor((uint8_t)stored)) << "stored " << stored;
    EXPECT_STREQ("none", nameFor((uint8_t)stored));
  }
}

// The setters take the remainder of the command line, so a trailing argument
// must still parse — and a longer word starting with a valid name must not.
TEST(WifiPowerSavePolicy, ParsesCliArgumentsExactly) {
  uint8_t stored = 0xFF;

  EXPECT_TRUE(parseName("min", &stored));
  EXPECT_EQ(kMin, stored);
  EXPECT_TRUE(parseName("none extra", &stored));
  EXPECT_EQ(kNone, stored);
  EXPECT_TRUE(parseName("max ", &stored));
  EXPECT_EQ(kMax, stored);

  stored = 0xFF;
  EXPECT_FALSE(parseName("minimum", &stored));
  EXPECT_FALSE(parseName("", &stored));
  EXPECT_FALSE(parseName("off", &stored));
  EXPECT_FALSE(parseName("MIN", &stored));
  EXPECT_FALSE(parseName(nullptr, &stored));
  EXPECT_EQ(0xFF, stored);   // rejected input leaves the caller's value alone
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
