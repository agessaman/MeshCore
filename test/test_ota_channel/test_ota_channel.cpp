#include <gtest/gtest.h>
#include "helpers/OtaChannel.h"

// The three base URLs are provided as -D macros by the test env (see platformio.ini).
TEST(OtaChannel, ResolvesNativeToBaseMacro) {
  EXPECT_STREQ(ota_resolve_base(OTA_CH_NATIVE), "https://stable.example/mqtt/v");
}
TEST(OtaChannel, ResolvesStable) {
  EXPECT_STREQ(ota_resolve_base(OTA_CH_STABLE), "https://stable.example/mqtt/v");
}
TEST(OtaChannel, ResolvesDev) {
  EXPECT_STREQ(ota_resolve_base(OTA_CH_DEV), "https://dev.example/mqtt/dev/v");
}
TEST(OtaChannel, BasesAreStoredBehindCiTags) {
  EXPECT_STREQ(ota_tagged_native, "ota-base-native:https://stable.example/mqtt/v");
  EXPECT_STREQ(ota_tagged_stable, "ota-base-stable:https://stable.example/mqtt/v");
  EXPECT_STREQ(ota_tagged_dev, "ota-base-dev:https://dev.example/mqtt/dev/v");
}
TEST(OtaChannel, ParseKnownKeywords) {
  uint8_t ch = 99;
  EXPECT_TRUE(ota_parse_channel("prod", &ch));    EXPECT_EQ(ch, OTA_CH_STABLE);
  EXPECT_TRUE(ota_parse_channel("stable", &ch));  EXPECT_EQ(ch, OTA_CH_STABLE);
  EXPECT_TRUE(ota_parse_channel("beta", &ch));    EXPECT_EQ(ch, OTA_CH_DEV);
  EXPECT_TRUE(ota_parse_channel("default", &ch)); EXPECT_EQ(ch, OTA_CH_NATIVE);
  EXPECT_TRUE(ota_parse_channel("dev", &ch));     EXPECT_EQ(ch, OTA_CH_DEV);
}
TEST(OtaChannel, ParseRejectsUnknownAndLeavesOutputUntouched) {
  uint8_t ch = 7;
  EXPECT_FALSE(ota_parse_channel("production", &ch));
  EXPECT_FALSE(ota_parse_channel("Beta", &ch));
  EXPECT_FALSE(ota_parse_channel("", &ch));
  EXPECT_EQ(ch, 7);
}
TEST(OtaChannel, NameLabels) {
  EXPECT_STREQ(ota_channel_name(OTA_CH_NATIVE), "default");
  EXPECT_STREQ(ota_channel_name(OTA_CH_STABLE), "prod");
  EXPECT_STREQ(ota_channel_name(OTA_CH_DEV), "beta");
}
TEST(OtaChannel, NativeChannelNameMatchesBase) {
  EXPECT_STREQ(ota_native_channel_name(), "prod");
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
