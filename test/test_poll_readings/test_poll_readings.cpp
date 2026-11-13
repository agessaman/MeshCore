#include <gtest/gtest.h>

#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "helpers/poll/PollReadings.h"

namespace {

constexpr uint32_t kSampleTs = 1700000000;
constexpr uint32_t kNowTs = 1700000003;
constexpr const char* kPlugin = "meshcore.observer_fw/example";

uint8_t g_origin_pub[poll::kPubKeySize];

void initKeys() {
  for (uint8_t i = 0; i < poll::kPubKeySize; i++) g_origin_pub[i] = i;
}

size_t FromHex(const char* hex, uint8_t* out, size_t cap) {
  size_t n = 0;
  for (const char* p = hex; p[0] && p[1]; p += 2) {
    if (n >= cap) return 0;
    char byte[3] = {p[0], p[1], 0};
    out[n++] = (uint8_t)strtoul(byte, nullptr, 16);
  }
  return n;
}

// The real 56-byte status prefix plus four zero pad bytes.
poll::Reading fullStatusReading(uint8_t node_fill) {
  const char* hex =
      "c60f00008cff8eff523d0100344f00007d47000079060c000b4b000029040000"
      "a629010094130000020005002400eadb6b200100aa6d000000000000";
  uint8_t data[80];
  const size_t n = FromHex(hex, data, sizeof(data));
  poll::Reading r;
  memset(&r, 0, sizeof(r));
  memset(r.node, node_fill, sizeof(r.node));
  r.method = poll::REQ_STATUS;
  r.sampled_at = kSampleTs;
  EXPECT_TRUE(poll::parseStatus(data, n, &r.status));
  r.status_len = r.status.data_len;
  r.has_battery = true;
  r.battery_mv = r.status.battery_mv;
  r.has_snr = true;
  r.snr_x4 = 26;  // 6.5 dB
  r.has_tag = true;
  r.tag = 1794484800u;
  r.has_hash = true;
  for (uint8_t i = 0; i < poll::kReadingHashSize; i++) r.hash[i] = (uint8_t)(0x01 + i * 0x22);
  return r;
}

poll::Reading telemetryReading(const uint8_t* data, size_t len, uint8_t node_fill) {
  poll::Reading r;
  memset(&r, 0, sizeof(r));
  memset(r.node, node_fill, sizeof(r.node));
  r.method = poll::REQ_TELEMETRY;
  r.sampled_at = kSampleTs;
  poll::TelemetryReading t;
  EXPECT_TRUE(poll::parseTelemetry(data, len, &t));
  r.has_battery = t.has_voltage;
  r.battery_mv = t.battery_mv;
  r.has_temperature = t.has_temperature;
  r.temperature_c10 = t.temperature_c10;
  r.other_records = t.other_records;
  r.lpp_len = t.lpp_len;
  memcpy(r.lpp, data, t.lpp_len);
  return r;
}

size_t build(char* out, size_t cap, uint8_t format, const char* origin, const poll::Reading& r) {
  return poll::buildReadingJson(out, cap, format, origin, g_origin_pub, kNowTs, kPlugin, r);
}

// Byte-for-byte, then the same payload with only just enough room, then one
// byte short with the buffer left untouched.
void expectReading(const std::string& expected, uint8_t format, const char* origin,
                   const poll::Reading& r) {
  std::vector<char> roomy(expected.size() + 32, 'X');
  size_t len = build(roomy.data(), roomy.size(), format, origin, r);
  ASSERT_EQ(expected.size(), len);
  EXPECT_EQ(expected, std::string(roomy.data(), len));
  EXPECT_EQ('\0', roomy[len]);

  std::vector<char> exact(expected.size() + 1 + 8, 'X');
  len = build(exact.data(), exact.size() - 8, format, origin, r);
  ASSERT_EQ(expected.size(), len);
  EXPECT_EQ(expected, std::string(exact.data(), len));
  for (size_t i = expected.size() + 1; i < exact.size(); i++) EXPECT_EQ('X', exact[i]);

  std::vector<char> tight(expected.size() + 8, 'X');
  len = build(tight.data(), expected.size(), format, origin, r);
  EXPECT_EQ(0u, len);
  for (size_t i = expected.size(); i < tight.size(); i++) EXPECT_EQ('X', tight[i]);
}

TEST(PollReadings, BatteryV1StatusMatchesTheExample) {
  initKeys();
  const poll::Reading r = fullStatusReading(0x22);
  const std::string expected =
      R"json({"v":1,"origin":"Cadham Village","origin_id":"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F","timestamp":"2023-11-14T22:13:23Z","plugin":"meshcore.observer_fw/example","readings":[{"node_id":"2222222222222222222222222222222222222222222222222222222222222222","battery_mv":4038,"source":"poll","reply_hops":0,"method":"status","sampled_at":"2023-11-14T22:13:20Z","snr":6.5,"stats":{"uptime_secs":788089,"noise_floor":-116,"last_rssi":-114,"last_snr":1.25,"tx_queue":0,"packets_recv":81234,"packets_sent":20276,"recv_flood":76198,"recv_direct":5012,"sent_flood":19211,"sent_direct":1065,"tx_air_secs":18301,"rx_air_secs":73835,"flood_dups":56298,"direct_dups":36,"recv_errors":28074,"err_events":2},"tag":1794484800,"packet_hash":"0123456789ABCDEF"}]})json";
  expectReading(expected, poll::kReadingV1, "Cadham Village", r);
}

TEST(PollReadings, ShortStatusWritesOnlyTheStatsItHad) {
  initKeys();
  const uint8_t data[8] = {0x3C, 0x0F, 0x00, 0x00, 0x92, 0xFF, 0xA6, 0xFF};
  poll::Reading r;
  memset(&r, 0, sizeof(r));
  memset(r.node, 0x22, sizeof(r.node));
  r.method = poll::REQ_STATUS;
  r.sampled_at = kSampleTs;
  ASSERT_TRUE(poll::parseStatus(data, sizeof(data), &r.status));
  r.status_len = r.status.data_len;
  r.has_battery = true;
  r.battery_mv = r.status.battery_mv;

  const std::string expected =
      R"json({"v":1,"origin":"Cadham Village","origin_id":"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F","timestamp":"2023-11-14T22:13:23Z","plugin":"meshcore.observer_fw/example","readings":[{"node_id":"2222222222222222222222222222222222222222222222222222222222222222","battery_mv":3900,"source":"poll","reply_hops":0,"method":"status","sampled_at":"2023-11-14T22:13:20Z","stats":{"noise_floor":-110,"last_rssi":-90,"tx_queue":0}}]})json";
  expectReading(expected, poll::kReadingV1, "Cadham Village", r);
}

TEST(PollReadings, BatteryV1TelemetryCarriesNoLpp) {
  initKeys();
  const uint8_t data[8] = {1, 116, 0x01, 0x8A, 1, 103, 0x00, 0xA5};
  const poll::Reading r = telemetryReading(data, sizeof(data), 0x33);
  EXPECT_TRUE(r.has_battery);
  EXPECT_EQ(3940, r.battery_mv);
  const std::string expected =
      R"json({"v":1,"origin":"Cadham Village","origin_id":"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F","timestamp":"2023-11-14T22:13:23Z","plugin":"meshcore.observer_fw/example","readings":[{"node_id":"3333333333333333333333333333333333333333333333333333333333333333","battery_mv":3940,"source":"poll","reply_hops":0,"method":"telemetry","sampled_at":"2023-11-14T22:13:20Z","temperature_c":16.5}]})json";
  expectReading(expected, poll::kReadingV1, "Cadham Village", r);

  // Without a voltage the version 1 field is still written, as a zero.
  const uint8_t temp_only[4] = {1, 103, 0x00, 0xA5};
  const poll::Reading no_volts = telemetryReading(temp_only, sizeof(temp_only), 0x33);
  EXPECT_FALSE(no_volts.has_battery);
  EXPECT_FALSE(poll::readingPublishable(poll::kReadingV1, no_volts));
  const std::string zero =
      R"json({"v":1,"origin":"Cadham Village","origin_id":"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F","timestamp":"2023-11-14T22:13:23Z","plugin":"meshcore.observer_fw/example","readings":[{"node_id":"3333333333333333333333333333333333333333333333333333333333333333","battery_mv":0,"source":"poll","reply_hops":0,"method":"telemetry","sampled_at":"2023-11-14T22:13:20Z","temperature_c":16.5}]})json";
  expectReading(zero, poll::kReadingV1, "Cadham Village", no_volts);
}

TEST(PollReadings, TelemetryV2KeepsTheLppRecords) {
  initKeys();
  const uint8_t data[8] = {1, 116, 0x01, 0x8A, 1, 103, 0x00, 0xA5};
  const poll::Reading r = telemetryReading(data, sizeof(data), 0x33);
  const std::string expected =
      R"json({"v":2,"origin":"Cadham Village","origin_id":"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F","timestamp":"2023-11-14T22:13:23Z","plugin":"meshcore.observer_fw/example","readings":[{"node_id":"3333333333333333333333333333333333333333333333333333333333333333","battery_mv":3940,"source":"poll","reply_hops":0,"method":"telemetry","sampled_at":"2023-11-14T22:13:20Z","temperature_c":16.5,"lpp":"0174018a016700a5"}]})json";
  expectReading(expected, poll::kReadingV2, "Cadham Village", r);
}

TEST(PollReadings, TelemetryV2WithoutVoltageAndWithAnUnknownRecord) {
  initKeys();
  // A channel-2 temperature record, then an unknown type, then zero padding.
  const uint8_t data[9] = {2, 0x67, 0x00, 0x64, 1, 250, 0, 0, 0};
  const poll::Reading r = telemetryReading(data, sizeof(data), 0x44);
  EXPECT_FALSE(r.has_battery);
  EXPECT_TRUE(r.other_records);
  EXPECT_EQ(6, r.lpp_len);
  EXPECT_TRUE(poll::readingPublishable(poll::kReadingV2, r));
  const std::string expected =
      R"json({"v":2,"origin":"Cadham Village","origin_id":"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F","timestamp":"2023-11-14T22:13:23Z","plugin":"meshcore.observer_fw/example","readings":[{"node_id":"4444444444444444444444444444444444444444444444444444444444444444","source":"poll","reply_hops":0,"method":"telemetry","sampled_at":"2023-11-14T22:13:20Z","lpp":"0267006401fa"}]})json";
  expectReading(expected, poll::kReadingV2, "Cadham Village", r);
}

TEST(PollReadings, TelemetryV2WithZeroVoltageAndPadding) {
  initKeys();
  uint8_t block[16];
  memset(block, 0, sizeof(block));
  memcpy(block, (const uint8_t[]){1, 116, 0x00, 0x00}, 4);
  const poll::Reading r = telemetryReading(block, sizeof(block), 0x55);
  EXPECT_TRUE(r.has_battery);
  EXPECT_EQ(0, r.battery_mv);
  EXPECT_EQ(4, r.lpp_len);
  const std::string expected =
      R"json({"v":2,"origin":"Cadham Village","origin_id":"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F","timestamp":"2023-11-14T22:13:23Z","plugin":"meshcore.observer_fw/example","readings":[{"node_id":"5555555555555555555555555555555555555555555555555555555555555555","battery_mv":0,"source":"poll","reply_hops":0,"method":"telemetry","sampled_at":"2023-11-14T22:13:20Z","lpp":"01740000"}]})json";
  expectReading(expected, poll::kReadingV2, "Cadham Village", r);
}

TEST(PollReadings, TelemetryV2StatusReadsNoLpp) {
  initKeys();
  const poll::Reading r = fullStatusReading(0x22);
  const std::string expected =
      R"json({"v":2,"origin":"Cadham Village","origin_id":"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F","timestamp":"2023-11-14T22:13:23Z","plugin":"meshcore.observer_fw/example","readings":[{"node_id":"2222222222222222222222222222222222222222222222222222222222222222","battery_mv":4038,"source":"poll","reply_hops":0,"method":"status","sampled_at":"2023-11-14T22:13:20Z","snr":6.5,"stats":{"uptime_secs":788089,"noise_floor":-116,"last_rssi":-114,"last_snr":1.25,"tx_queue":0,"packets_recv":81234,"packets_sent":20276,"recv_flood":76198,"recv_direct":5012,"sent_flood":19211,"sent_direct":1065,"tx_air_secs":18301,"rx_air_secs":73835,"flood_dups":56298,"direct_dups":36,"recv_errors":28074,"err_events":2},"tag":1794484800,"packet_hash":"0123456789ABCDEF"}]})json";
  expectReading(expected, poll::kReadingV2, "Cadham Village", r);
  EXPECT_EQ(std::string::npos, expected.find("\"lpp\""));
}

TEST(PollReadings, OriginIsEscapedLikeTheClaim) {
  initKeys();
  const char* origin = "Cadham \"Village\" \\ Caf\xc3\xa9 \xf0\x9f\x98\x80\n\t\x01/<>";
  const uint8_t data[2] = {0xF4, 0x01};
  poll::Reading r;
  memset(&r, 0, sizeof(r));
  memset(r.node, 0x66, sizeof(r.node));
  r.method = poll::REQ_STATUS;
  r.sampled_at = kSampleTs;
  ASSERT_TRUE(poll::parseStatus(data, sizeof(data), &r.status));
  r.status_len = r.status.data_len;
  r.has_battery = true;
  r.battery_mv = r.status.battery_mv;

  const std::string expected =
      R"json({"v":1,"origin":"Cadham \"Village\" \\ Caf\u00e9 \ud83d\ude00\n\t\u0001/<>","origin_id":"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F","timestamp":"2023-11-14T22:13:23Z","plugin":"meshcore.observer_fw/example","readings":[{"node_id":"6666666666666666666666666666666666666666666666666666666666666666","battery_mv":500,"source":"poll","reply_hops":0,"method":"status","sampled_at":"2023-11-14T22:13:20Z","stats":{}}]})json";
  expectReading(expected, poll::kReadingV1, origin, r);
}

TEST(PollReadings, RefusesAShortBufferOrBadInput) {
  initKeys();
  const poll::Reading r = fullStatusReading(0x22);
  char buf[64];
  memset(buf, 'X', sizeof(buf));
  EXPECT_EQ(0u, build(buf, 0, poll::kReadingV1, "x", r));
  EXPECT_EQ(0u, build(nullptr, sizeof(buf), poll::kReadingV1, "x", r));
  EXPECT_EQ(0u, poll::buildReadingJson(buf, sizeof(buf), poll::kReadingV1, "x", nullptr, kNowTs,
                                      kPlugin, r));
  EXPECT_EQ(0u, build(buf, sizeof(buf), 0, "x", r));
  for (size_t i = 0; i < sizeof(buf); i++) EXPECT_EQ('X', buf[i]);
}

TEST(PollReadings, WorstCaseFitsKReadingJsonMax) {
  initKeys();
  // Every optional field, a full LPP copy, and the longest origin and plugin
  // label the firmware can hold, each byte escaping to its widest form.
  poll::Reading r = fullStatusReading(0x22);
  r.has_temperature = true;
  r.temperature_c10 = -32768;
  r.lpp_len = poll::kReadingLppMax;
  memset(r.lpp, 0xAB, sizeof(r.lpp));
  char origin[32];
  memset(origin, 0xFF, 31);
  origin[31] = '\0';
  char plugin[48];
  memset(plugin, 0xFF, 47);
  plugin[47] = '\0';

  std::vector<char> buf(poll::kReadingJsonMax + 8, 'X');
  const size_t len = poll::buildReadingJson(buf.data(), poll::kReadingJsonMax, poll::kReadingV2,
                                            origin, g_origin_pub, kNowTs, plugin, r);
  ASSERT_GT(len, 0u);
  EXPECT_LT(len + 1, (size_t)poll::kReadingJsonMax);
  EXPECT_EQ('\0', buf[len]);
  // One byte short of room for the payload and its terminator is refused whole.
  EXPECT_EQ(0u, poll::buildReadingJson(buf.data(), len, poll::kReadingV2, origin, g_origin_pub,
                                       kNowTs, plugin, r));
}

TEST(PollReadings, PublishablePerFormat) {
  poll::Reading r;
  memset(&r, 0, sizeof(r));

  r.method = poll::REQ_STATUS;
  EXPECT_TRUE(poll::readingPublishable(poll::kReadingV1, r));
  EXPECT_TRUE(poll::readingPublishable(poll::kReadingV2, r));

  r.method = poll::REQ_TELEMETRY;
  r.has_battery = true;
  r.battery_mv = 0;  // a zero voltage is still a report
  EXPECT_TRUE(poll::readingPublishable(poll::kReadingV1, r));
  EXPECT_TRUE(poll::readingPublishable(poll::kReadingV2, r));

  r.has_battery = false;
  EXPECT_FALSE(poll::readingPublishable(poll::kReadingV1, r));
  EXPECT_TRUE(poll::readingPublishable(poll::kReadingV2, r));

  // Zero is not a delivery format, so nothing is published.
  EXPECT_FALSE(poll::readingPublishable(0, r));
}

TEST(PollReadings, PacingSkipsOnlyEmptyReplies) {
  poll::Reading r;
  memset(&r, 0, sizeof(r));
  r.method = poll::REQ_STATUS;

  EXPECT_TRUE(poll::replyEmptyForPacing(r));
  r.has_battery = true;
  r.battery_mv = 0;
  EXPECT_TRUE(poll::replyEmptyForPacing(r));
  r.battery_mv = 4010;
  EXPECT_FALSE(poll::replyEmptyForPacing(r));

  r.method = poll::REQ_TELEMETRY;
  r.has_battery = false;
  EXPECT_TRUE(poll::replyEmptyForPacing(r));
  r.other_records = true;
  EXPECT_FALSE(poll::replyEmptyForPacing(r));
  r.battery_mv = 0;
  r.has_battery = true;
  EXPECT_FALSE(poll::replyEmptyForPacing(r));
}

TEST(PollReadings, HoldKeepsTheNewestPerTargetOldestFirst) {
  poll::ReadingHold h;
  memset(&h, 0, sizeof(h));
  EXPECT_EQ(nullptr, poll::readingHoldPeek(h));
  poll::Reading out;
  EXPECT_FALSE(poll::readingHoldTake(h, out));

  poll::Reading a;
  memset(&a, 0, sizeof(a));
  memset(a.node, 0x11, sizeof(a.node));
  a.battery_mv = 1;
  poll::Reading b;
  memset(&b, 0, sizeof(b));
  memset(b.node, 0x22, sizeof(b.node));
  b.battery_mv = 2;

  poll::readingHoldPut(h, a);
  poll::readingHoldPut(h, b);
  ASSERT_EQ(2, h.count);
  ASSERT_NE(nullptr, poll::readingHoldPeek(h));
  EXPECT_EQ(0x11, poll::readingHoldPeek(h)->node[0]);

  // A newer reading for a target replaces the held one and sorts last.
  poll::Reading a2 = a;
  a2.battery_mv = 11;
  poll::readingHoldPut(h, a2);
  ASSERT_EQ(2, h.count);
  EXPECT_EQ(0x22, poll::readingHoldPeek(h)->node[0]);
  EXPECT_EQ(11, h.entries[1].battery_mv);

  ASSERT_TRUE(poll::readingHoldTake(h, out));
  EXPECT_EQ(0x22, out.node[0]);
  EXPECT_EQ(2, out.battery_mv);
  ASSERT_EQ(1, h.count);
  EXPECT_EQ(11, poll::readingHoldPeek(h)->battery_mv);

  // A ninth target drops the oldest one.
  for (uint8_t i = 0; i < poll::kReadingHoldMax; i++) {
    poll::Reading r;
    memset(&r, 0, sizeof(r));
    memset(r.node, (uint8_t)(0xC0 + i), sizeof(r.node));
    poll::readingHoldPut(h, r);
  }
  ASSERT_EQ(poll::kReadingHoldMax, h.count);
  EXPECT_EQ(0xC0, poll::readingHoldPeek(h)->node[0]);
  EXPECT_EQ(0xC7, h.entries[h.count - 1].node[0]);

  uint8_t oldest[poll::kPubKeySize];
  memset(oldest, 0xC0, sizeof(oldest));
  EXPECT_TRUE(poll::readingHoldDrop(h, oldest));
  EXPECT_EQ(poll::kReadingHoldMax - 1, h.count);
  EXPECT_EQ(0xC1, poll::readingHoldPeek(h)->node[0]);
  EXPECT_FALSE(poll::readingHoldDrop(h, oldest));
  EXPECT_FALSE(poll::readingHoldDrop(h, nullptr));

  poll::readingHoldClear(h);
  EXPECT_EQ(0, h.count);
  EXPECT_EQ(nullptr, poll::readingHoldPeek(h));
}

}  // namespace

TEST(PollReadings, DataReplyCodeKeepsTodaysRuleOutsideTelemetry) {
  for (uint8_t f : {(uint8_t)0, poll::kReadingV1}) {
    EXPECT_EQ(poll::CODE_OK, poll::dataReplyCode(f, true, 3900, false));
    EXPECT_EQ(poll::CODE_NO_BATTERY, poll::dataReplyCode(f, true, 0, false));
    EXPECT_EQ(poll::CODE_NO_REPLY, poll::dataReplyCode(f, false, 0, true));
    EXPECT_EQ(poll::CODE_NO_REPLY, poll::dataReplyCode(f, false, 0, false));
  }
}

TEST(PollReadings, DataReplyCodeInTelemetryPacesOnlyEmptyReplies) {
  EXPECT_EQ(poll::CODE_OK, poll::dataReplyCode(poll::kReadingV2, true, 3900, false));
  EXPECT_EQ(poll::CODE_OK, poll::dataReplyCode(poll::kReadingV2, false, 0, true));   // no voltage, other records
  EXPECT_EQ(poll::CODE_OK, poll::dataReplyCode(poll::kReadingV2, true, 0, true));
  EXPECT_EQ(poll::CODE_NO_BATTERY, poll::dataReplyCode(poll::kReadingV2, true, 0, false));
  EXPECT_EQ(poll::CODE_NO_BATTERY, poll::dataReplyCode(poll::kReadingV2, false, 0, false));
  // Same verdict as replyEmptyForPacing on the reading.
  poll::Reading r;
  memset(&r, 0, sizeof(r));
  r.other_records = true;
  EXPECT_EQ(poll::replyEmptyForPacing(r), poll::dataReplyCode(poll::kReadingV2, r.has_battery, r.battery_mv, r.other_records) == poll::CODE_NO_BATTERY);
  r.other_records = false;
  EXPECT_EQ(poll::replyEmptyForPacing(r), poll::dataReplyCode(poll::kReadingV2, r.has_battery, r.battery_mv, r.other_records) == poll::CODE_NO_BATTERY);
}

TEST(PollReadings, SnrIsClampedToThirtyDb) {
  EXPECT_EQ(-120, poll::clampSnrX4(-500));
  EXPECT_EQ(120, poll::clampSnrX4(127));
  EXPECT_EQ(-41, poll::clampSnrX4(-41));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
