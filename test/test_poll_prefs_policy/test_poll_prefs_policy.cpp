#define WITH_MQTT_BRIDGE 1

#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <string>

#include "helpers/poll/PollPrefsPolicy.h"

namespace {

const char* kKeyA = "c8c8c8580123456789abcdef0123456789abcdef0123456789abcdef01234567";
const char* kKeyB = "010101c00123456789abcdef0123456789abcdef0123456789abcdef01234567";
const char* kKeyC = "aaaaaaaa0123456789abcdef0123456789abcdef0123456789abcdef01234567";

void put(uint8_t targets[][MQTT_POLL_PUBKEY_SIZE], uint8_t* pw, int slot, const char* hex,
         uint8_t mode) {
  ASSERT_EQ(poll::TargetHex::Ok, poll::parseTargetHex(hex, targets[slot], nullptr));
  pw[slot] = mode;
}

}  // namespace

TEST(PollPrefsPolicy, ParsesAndFormatsTargetHex) {
  uint8_t key[MQTT_POLL_PUBKEY_SIZE];
  bool folded = true;
  EXPECT_EQ(poll::TargetHex::Empty, poll::parseTargetHex("", key, &folded));
  EXPECT_FALSE(folded);
  EXPECT_TRUE(poll::targetIsEmpty(key));

  folded = false;
  EXPECT_EQ(poll::TargetHex::Ok, poll::parseTargetHex(kKeyA, key, &folded));
  EXPECT_FALSE(folded);
  char formatted[65];
  poll::formatTargetHex(key, formatted);
  EXPECT_STREQ(kKeyA, formatted);

  std::string upper = kKeyA;
  for (char& c : upper) {
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
  }
  EXPECT_EQ(poll::TargetHex::Ok, poll::parseTargetHex(upper.c_str(), key, &folded));
  EXPECT_TRUE(folded);
  poll::formatTargetHex(key, formatted);
  EXPECT_STREQ(kKeyA, formatted);

  EXPECT_EQ(poll::TargetHex::Bad, poll::parseTargetHex("abcd", key, nullptr));
  EXPECT_EQ(poll::TargetHex::Bad, poll::parseTargetHex(std::string(64, 'g').c_str(), key, nullptr));
  EXPECT_EQ(poll::TargetHex::Bad, poll::parseTargetHex(std::string(63, 'a').c_str(), key, nullptr));
  EXPECT_EQ(poll::TargetHex::Empty,
            poll::parseTargetHex(std::string(64, '0').c_str(), key, nullptr));
  EXPECT_TRUE(poll::targetIsEmpty(key));
}

TEST(PollPrefsPolicy, IntervalAndStatusHours) {
  EXPECT_TRUE(poll::pollIntervalValid(30));
  EXPECT_TRUE(poll::pollIntervalValid(60));
  EXPECT_TRUE(poll::pollIntervalValid(1440));
  EXPECT_FALSE(poll::pollIntervalValid(29));
  EXPECT_FALSE(poll::pollIntervalValid(1441));
  EXPECT_FALSE(poll::pollIntervalValid(-1));

  for (int hours : {1, 3, 6, 12, 24}) EXPECT_TRUE(poll::pollStatusHoursValid(hours));
  for (int hours : {0, 2, 4, 5, 7, 23, 25, -1}) EXPECT_FALSE(poll::pollStatusHoursValid(hours));
}

TEST(PollPrefsPolicy, NormalizesUrlsAndGatesEnable) {
  char url[MQTT_POLL_URL_LEN];
  bool changed = true;
  strcpy(url, "");
  EXPECT_TRUE(poll::normalizePollUrl(url, &changed));
  EXPECT_FALSE(changed);

  strcpy(url, "https://corescope.example/");
  EXPECT_TRUE(poll::normalizePollUrl(url, &changed));
  EXPECT_TRUE(changed);
  EXPECT_STREQ("https://corescope.example", url);

  strcpy(url, "https://corescope.example/api/");
  EXPECT_TRUE(poll::normalizePollUrl(url, &changed));
  EXPECT_STREQ("https://corescope.example/api", url);

  strcpy(url, "http://corescope.example");
  EXPECT_TRUE(poll::normalizePollUrl(url, &changed));
  EXPECT_FALSE(changed);

  strcpy(url, "ftp://corescope.example");
  EXPECT_FALSE(poll::normalizePollUrl(url, &changed));
  EXPECT_TRUE(changed);
  EXPECT_STREQ("", url);

  strcpy(url, "HTTPS://corescope.example");
  EXPECT_FALSE(poll::normalizePollUrl(url, &changed));
  EXPECT_STREQ("", url);

  EXPECT_FALSE(poll::pollCanEnable("", 1, 1, MQTT_PREFS_SLOT_COUNT));
  EXPECT_FALSE(poll::pollCanEnable("https://corescope.example", 0, 1, MQTT_PREFS_SLOT_COUNT));
  EXPECT_FALSE(poll::pollCanEnable("https://corescope.example", 1, 0, MQTT_PREFS_SLOT_COUNT));
  EXPECT_FALSE(poll::pollCanEnable("https://corescope.example", 7, 1, MQTT_PREFS_SLOT_COUNT));
  EXPECT_TRUE(poll::pollCanEnable("https://corescope.example", 1, 1, MQTT_PREFS_SLOT_COUNT));
  EXPECT_TRUE(poll::pollCanEnable("http://corescope.example", 6, 1, 6));
  EXPECT_FALSE(poll::pollCanEnable("http://corescope.example", 4, 1, 3));
}

TEST(PollPrefsPolicy, AddRemoveAndPwKeepTargetsPacked) {
  uint8_t targets[MQTT_POLL_MAX_TARGETS][MQTT_POLL_PUBKEY_SIZE] = {};
  uint8_t pw[MQTT_POLL_MAX_TARGETS] = {};

  EXPECT_EQ(poll::TargetEdit::BadKey, poll::addTarget(targets, pw, "abcd"));
  EXPECT_EQ(poll::TargetEdit::BadKey, poll::addTarget(targets, pw, std::string(64, '0').c_str()));
  EXPECT_EQ(poll::TargetEdit::Ok, poll::addTarget(targets, pw, kKeyA));
  EXPECT_EQ(MQTT_POLL_PW_AUTO, pw[0]);
  EXPECT_EQ(poll::TargetEdit::Duplicate, poll::addTarget(targets, pw, kKeyA));
  std::string upper = kKeyA;
  for (char& c : upper) {
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
  }
  EXPECT_EQ(poll::TargetEdit::Duplicate, poll::addTarget(targets, pw, upper.c_str()));
  EXPECT_EQ(poll::TargetEdit::Ok, poll::addTarget(targets, pw, kKeyB));
  EXPECT_EQ(poll::TargetEdit::Ok, poll::addTarget(targets, pw, kKeyC));

  int index = -1;
  EXPECT_EQ(poll::TargetEdit::Ok, poll::findTargetPrefix(targets, "C8C8C858", &index));
  EXPECT_EQ(0, index);
  EXPECT_EQ(poll::TargetEdit::Ok, poll::findTargetPrefix(targets, kKeyB, &index));
  EXPECT_EQ(1, index);
  EXPECT_EQ(poll::TargetEdit::NotFound, poll::findTargetPrefix(targets, "ff", &index));
  EXPECT_EQ(poll::TargetEdit::BadKey, poll::findTargetPrefix(targets, "zz", &index));

  pw[0] = MQTT_POLL_PW_HELLO;
  pw[1] = MQTT_POLL_PW_BLANK;
  EXPECT_EQ(poll::TargetEdit::Ok, poll::removeTarget(targets, pw, "010101c0"));
  EXPECT_TRUE(poll::targetIsEmpty(targets[2]));
  EXPECT_EQ(MQTT_POLL_PW_AUTO, pw[2]);
  char left[65];
  poll::formatTargetHex(targets[0], left);
  EXPECT_STREQ(kKeyA, left);
  poll::formatTargetHex(targets[1], left);
  EXPECT_STREQ(kKeyC, left);
  EXPECT_EQ(MQTT_POLL_PW_HELLO, pw[0]);
  EXPECT_EQ(MQTT_POLL_PW_AUTO, pw[1]);

  EXPECT_EQ(poll::TargetEdit::Ok, poll::setTargetPwMode(targets, pw, "aaaa", MQTT_POLL_PW_BLANK));
  EXPECT_EQ(MQTT_POLL_PW_BLANK, pw[1]);
  EXPECT_EQ(poll::TargetEdit::BadMode,
            poll::setTargetPwMode(targets, pw, "aaaa", 9));
  EXPECT_EQ(MQTT_POLL_PW_BLANK, pw[1]);

  uint8_t ambiguous[MQTT_POLL_MAX_TARGETS][MQTT_POLL_PUBKEY_SIZE] = {};
  uint8_t ambiguous_pw[MQTT_POLL_MAX_TARGETS] = {};
  put(ambiguous, ambiguous_pw, 0, kKeyA, MQTT_POLL_PW_AUTO);
  std::string shared = kKeyA;
  shared[10] = '0';
  put(ambiguous, ambiguous_pw, 1, shared.c_str(), MQTT_POLL_PW_AUTO);
  EXPECT_EQ(poll::TargetEdit::Ambiguous, poll::findTargetPrefix(ambiguous, "c8c8c85801", &index));

  uint8_t full[MQTT_POLL_MAX_TARGETS][MQTT_POLL_PUBKEY_SIZE] = {};
  uint8_t full_pw[MQTT_POLL_MAX_TARGETS] = {};
  for (int i = 0; i < MQTT_POLL_MAX_TARGETS; i++) {
    std::string hex(64, 'b');
    hex[63] = static_cast<char>('0' + i);
    EXPECT_EQ(poll::TargetEdit::Ok, poll::addTarget(full, full_pw, hex.c_str()));
  }
  EXPECT_EQ(poll::TargetEdit::Full, poll::addTarget(full, full_pw, kKeyA));

  poll::clearTargets(full, full_pw);
  EXPECT_TRUE(poll::targetIsEmpty(full[0]));
  EXPECT_EQ(0, full_pw[0]);

  EXPECT_TRUE(poll::parsePwMode("auto", &full_pw[0]));
  EXPECT_EQ(MQTT_POLL_PW_AUTO, full_pw[0]);
  EXPECT_TRUE(poll::parsePwMode("blank", &full_pw[0]));
  EXPECT_EQ(MQTT_POLL_PW_BLANK, full_pw[0]);
  EXPECT_TRUE(poll::parsePwMode("hello", &full_pw[0]));
  EXPECT_EQ(MQTT_POLL_PW_HELLO, full_pw[0]);
  EXPECT_FALSE(poll::parsePwMode("on", &full_pw[0]));
  EXPECT_STREQ("auto", poll::pwModeName(MQTT_POLL_PW_AUTO));
  EXPECT_STREQ("blank", poll::pwModeName(MQTT_POLL_PW_BLANK));
  EXPECT_STREQ("hello", poll::pwModeName(MQTT_POLL_PW_HELLO));
}

TEST(PollPrefsPolicy, ListUsesPrefixesAndFitsTheCliReply) {
  uint8_t targets[MQTT_POLL_MAX_TARGETS][MQTT_POLL_PUBKEY_SIZE] = {};
  uint8_t pw[MQTT_POLL_MAX_TARGETS] = {};
  char line[192];
  ASSERT_TRUE(poll::formatTargetList(targets, pw, line, sizeof(line)));
  EXPECT_STREQ("> none", line);

  put(targets, pw, 0, kKeyA, MQTT_POLL_PW_AUTO);
  put(targets, pw, 1, kKeyB, MQTT_POLL_PW_HELLO);
  ASSERT_TRUE(poll::formatTargetList(targets, pw, line, 160));
  EXPECT_STREQ("> 1: C8C8C858 (auto), 2: 010101C0 (hello)", line);

  uint8_t many[MQTT_POLL_MAX_TARGETS][MQTT_POLL_PUBKEY_SIZE] = {};
  uint8_t many_pw[MQTT_POLL_MAX_TARGETS] = {};
  for (int i = 0; i < MQTT_POLL_MAX_TARGETS; i++) {
    std::string hex(64, 'c');
    hex[63] = static_cast<char>('0' + i);
    put(many, many_pw, i, hex.c_str(), MQTT_POLL_PW_HELLO);
  }
  ASSERT_TRUE(poll::formatTargetList(many, many_pw, line, 160));
  EXPECT_LT(strlen(line), 160u);
  EXPECT_EQ(8, static_cast<int>(std::count(line, line + strlen(line), '(')));
  ASSERT_TRUE(poll::formatTargetList(many, many_pw, line, sizeof(line)));
  EXPECT_NE(std::string::npos, std::string(line).find(", "));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
