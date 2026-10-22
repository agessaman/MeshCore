#include "helpers/WifiTxPowerPolicy.h"

#include <gtest/gtest.h>

using namespace WifiTxPowerPolicy;

// Stored 0 must keep following the build flag: every save writes this field,
// so a stored 11 would pin today's default on every node in the fleet.
TEST(WifiTxPowerPolicy, BoardDefaultFollowsBuildValue) {
  EXPECT_EQ(44, qdbmFor(kBoardDefault, 44));
  EXPECT_EQ(34, qdbmFor(kBoardDefault, 34));  // WIFI_POWER_8_5dBm, not 8 dBm
  EXPECT_EQ(78, qdbmFor(kBoardDefault, 78));
}

TEST(WifiTxPowerPolicy, ExplicitValuesAreWholeDbm) {
  EXPECT_EQ(8, qdbmFor(2, 44));
  EXPECT_EQ(60, qdbmFor(15, 44));
  EXPECT_EQ(80, qdbmFor(20, 44));
}

// WIFI_POWER_MINUS_1dBm (-4) is rejected by the SDK, which would leave the
// radio at its full-power default instead of the cap the build asked for.
TEST(WifiTxPowerPolicy, BuildValueIsClampedToSdkRange) {
  EXPECT_EQ(kMinQdbm, qdbmFor(kBoardDefault, -4));
  EXPECT_EQ(kMaxQdbm, qdbmFor(kBoardDefault, 120));
}

TEST(WifiTxPowerPolicy, CorruptStoredValueReadsAsBoardDefault) {
  EXPECT_FALSE(isValidStored(1));
  EXPECT_FALSE(isValidStored(21));
  EXPECT_FALSE(isValidStored(-1));
  EXPECT_EQ(44, qdbmFor(1, 44));
  EXPECT_EQ(44, qdbmFor(200, 44));
}

TEST(WifiTxPowerPolicy, ParsesCliArgumentsExactly) {
  uint8_t out = 0xFF;
  ASSERT_TRUE(parse("default", &out));
  EXPECT_EQ(kBoardDefault, out);
  ASSERT_TRUE(parse("0", &out));
  EXPECT_EQ(kBoardDefault, out);
  ASSERT_TRUE(parse("2", &out));
  EXPECT_EQ(2, out);
  ASSERT_TRUE(parse("20 trailing", &out));
  EXPECT_EQ(20, out);

  out = 0xFF;
  EXPECT_FALSE(parse("1", &out));
  EXPECT_FALSE(parse("21", &out));
  EXPECT_FALSE(parse("-5", &out));
  EXPECT_FALSE(parse("11.5", &out));
  EXPECT_FALSE(parse("11dBm", &out));
  EXPECT_FALSE(parse("defaults", &out));
  EXPECT_FALSE(parse("", &out));
  EXPECT_FALSE(parse(nullptr, &out));
  EXPECT_EQ(0xFF, out);
}

TEST(WifiTxPowerPolicy, FormatsQuarterDbmExactly) {
  char buf[8];
  formatQdbm(buf, sizeof(buf), 44);
  EXPECT_STREQ("11", buf);
  formatQdbm(buf, sizeof(buf), 66);
  EXPECT_STREQ("16.5", buf);
  formatQdbm(buf, sizeof(buf), 33);
  EXPECT_STREQ("8.25", buf);
  formatQdbm(buf, sizeof(buf), 79);
  EXPECT_STREQ("19.75", buf);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
