#include <gtest/gtest.h>
#include <helpers/radiolib/RxBoostedGainDefaults.h>
#include "../../examples/companion_radio/NodePrefs.h"

using mesh::radio::selectRxBoostedGainDefault;

// JSON loading does not use the hardware CLI adapters.
bool CommonRadioPrefs::getByKey(const char*, char*, size_t) { return false; }
bool CommonRadioPrefs::setByKey(const char*, const char*) { return false; }

class RxGainInput : public Stream {
  const char* text;
  size_t position = 0;

public:
  explicit RxGainInput(const char* value) : text(value) { }
  int available() override { return strlen(text + position); }
  int read() override { return text[position] ? text[position++] : -1; }
  int peek() override { return text[position] ? text[position] : -1; }
};

TEST(RxBoostedGainDefaults, UsesTargetFlagsIncludingExplicitOff) {
  EXPECT_EQ(0, selectRxBoostedGainDefault(true, 0, false, 0, 1));
  EXPECT_EQ(1, selectRxBoostedGainDefault(true, 1, false, 0, 0));
  EXPECT_EQ(0, selectRxBoostedGainDefault(false, 0, true, 0, 1));
  EXPECT_EQ(1, selectRxBoostedGainDefault(false, 0, true, 1, 0));
}

TEST(RxBoostedGainDefaults, GivesSpecificFlagPrecedence) {
  EXPECT_EQ(0, selectRxBoostedGainDefault(true, 0, true, 1, 1));
  EXPECT_EQ(1, selectRxBoostedGainDefault(true, 1, true, 0, 0));
}

TEST(RxBoostedGainDefaults, PreservesFallbackForUnconfiguredTargets) {
  EXPECT_EQ(0, selectRxBoostedGainDefault(false, 1, false, 1, 0));
  EXPECT_EQ(1, selectRxBoostedGainDefault(false, 0, false, 0, 1));
}

TEST(RxBoostedGainDefaults, SavedOffOverridesEnabledDefault) {
  NodePrefs prefs;
  prefs.rx_boosted_gain = selectRxBoostedGainDefault(false, 0, true, 1, 0);
  ASSERT_EQ(1, prefs.rx_boosted_gain);
  RxGainInput saved("{radio:{rxgain:0}}");
  ASSERT_TRUE(prefs.loadSerial(saved));
  EXPECT_EQ(0, prefs.getRadioPrefs()->getRxGain());
}

TEST(RxBoostedGainDefaults, SavedOnOverridesDisabledDefault) {
  NodePrefs prefs;
  prefs.rx_boosted_gain = selectRxBoostedGainDefault(false, 0, true, 0, 1);
  ASSERT_EQ(0, prefs.rx_boosted_gain);
  RxGainInput saved("{radio:{rxgain:1}}");
  ASSERT_TRUE(prefs.loadSerial(saved));
  EXPECT_EQ(1, prefs.getRadioPrefs()->getRxGain());
}

TEST(RxBoostedGainDefaults, MissingSavedGainRetainsTargetDefault) {
  for (uint8_t gain : {0, 1}) {
    NodePrefs prefs;
    prefs.rx_boosted_gain = selectRxBoostedGainDefault(false, 0, true, gain, 0);
    RxGainInput saved("{radio:{freq:910.525}}");
    ASSERT_TRUE(prefs.loadSerial(saved));
    EXPECT_EQ(gain, prefs.getRadioPrefs()->getRxGain());
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
