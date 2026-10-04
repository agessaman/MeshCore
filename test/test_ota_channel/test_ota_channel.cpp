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

TEST(OtaCompat, OwnTagParses) {
  OtaCompat own;
  ASSERT_TRUE(ota_compat_parse(ota_compat_tag + sizeof(OTA_COMPAT_TAG) - 1, &own));
  EXPECT_EQ(own.gen, OTA_STATE_GEN);
  EXPECT_EQ(own.caps, 0);
}
TEST(OtaCompat, ParsesCapsAndRejectsJunk) {
  OtaCompat c;
  ASSERT_TRUE(ota_compat_parse("12+eth+future", &c));
  EXPECT_EQ(c.gen, 12);
  EXPECT_EQ(c.caps, OTA_CAP_ETH);
  EXPECT_FALSE(ota_compat_parse("", &c));
  EXPECT_FALSE(ota_compat_parse("x1", &c));
  EXPECT_FALSE(ota_compat_parse("2eth", &c));
}
TEST(OtaCompat, FindsCompleteTagOnly) {
  const char img[] = "\xe9junk\0ota-compat:2+eth\0tail";
  const char* v = ota_compat_find((const uint8_t*)img, sizeof(img));
  ASSERT_NE(v, nullptr);
  EXPECT_STREQ(v, "2+eth");
  const char cut[] = "junk ota-compat:2+e";  // value runs past the chunk end
  EXPECT_EQ(ota_compat_find((const uint8_t*)cut, sizeof(cut) - 1), nullptr);
  const char literal_first[] = "ota-compat:\0code\0ota-compat:1\0";  // search literal precedes the tag
  v = ota_compat_find((const uint8_t*)literal_first, sizeof(literal_first));
  ASSERT_NE(v, nullptr);
  EXPECT_STREQ(v, "1");
  const char none[] = "ota-compat";
  EXPECT_EQ(ota_compat_find((const uint8_t*)none, sizeof(none)), nullptr);
}
TEST(OtaCompat, TargetMustKeepStateAndTransports) {
  EXPECT_TRUE(ota_compat_ok({2, 0}, {2, 0}));
  EXPECT_TRUE(ota_compat_ok({1, 0}, {2, OTA_CAP_ETH}));
  EXPECT_FALSE(ota_compat_ok({2, 0}, {1, 0}));                       // cannot read /mqtt.json
  EXPECT_FALSE(ota_compat_ok({2, OTA_CAP_ETH}, {2, 0}));             // drops Ethernet
  EXPECT_TRUE(ota_compat_ok({2, OTA_CAP_ETH}, {3, OTA_CAP_ETH}));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
