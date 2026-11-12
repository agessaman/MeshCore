#include <gtest/gtest.h>

#include <stdlib.h>
#include <string.h>

#include "helpers/poll/PollReplyParsers.h"

namespace {

size_t FromHex(const char* hex, uint8_t* out, size_t cap) {
  size_t n = 0;
  for (const char* p = hex; p[0] && p[1]; p += 2) {
    if (n >= cap) return 0;
    char byte[3] = {p[0], p[1], 0};
    out[n++] = (uint8_t)strtoul(byte, nullptr, 16);
  }
  return n;
}

void PutBeI16(uint8_t* p, int16_t v) {
  uint16_t u = (uint16_t)v;
  p[0] = (uint8_t)(u >> 8);
  p[1] = (uint8_t)u;
}

}  // namespace

TEST(PollReplyParsers, RealStatusReply) {
  // West Lomond, 25 Oct 2026. Four trailing zeros follow the 56-byte struct.
  const char* hex =
      "c60f00008cff8eff523d0100344f00007d47000079060c000b4b000029040000"
      "a629010094130000020005002400eadb6b200100aa6d000000000000";
  uint8_t data[80];
  size_t n = FromHex(hex, data, sizeof(data));
  ASSERT_EQ(60u, n);  // 56-byte struct plus 4 trailing zeros
  poll::StatusReading s;
  memset(&s, 0x5A, sizeof(s));
  ASSERT_TRUE(poll::parseStatus(data, n, &s));
  EXPECT_EQ(4038, s.battery_mv);
  EXPECT_EQ(0, s.tx_queue);
  EXPECT_EQ(-116, s.noise_floor);
  EXPECT_TRUE(s.has_uptime);
  EXPECT_EQ(788089u, s.uptime_secs);
  EXPECT_EQ(76198u, s.recv_flood);
  EXPECT_EQ(56298, s.flood_dups);
  EXPECT_EQ(28074u, s.recv_errors);
}

TEST(PollReplyParsers, ShortStatusLeavesTheTailOut) {
  const uint8_t data[] = {0x3C, 0x0F, 0x00, 0x00, 0x92, 0xFF, 0xA6, 0xFF};
  poll::StatusReading s;
  ASSERT_TRUE(poll::parseStatus(data, sizeof(data), &s));
  EXPECT_EQ(3900, s.battery_mv);
  EXPECT_EQ(0, s.tx_queue);
  EXPECT_EQ(-110, s.noise_floor);
  EXPECT_EQ(-90, s.last_rssi);
  EXPECT_FALSE(s.has_uptime);
  EXPECT_EQ(0u, s.uptime_secs);
  EXPECT_EQ(0u, s.packets_recv);
  EXPECT_EQ(0u, s.recv_flood);
  EXPECT_EQ(0, s.flood_dups);
}

TEST(PollReplyParsers, TruncatedAndBlockPaddedStatus) {
  poll::StatusReading s;
  EXPECT_FALSE(poll::parseStatus(nullptr, 2, &s));
  const uint8_t one[] = {0x3C};
  EXPECT_FALSE(poll::parseStatus(one, 1, &s));
  EXPECT_FALSE(poll::parseStatus(one, 0, &s));

  // Two bytes is a battery and nothing else. A partial next field is not taken.
  const uint8_t two[] = {0xE8, 0x03, 0xFF};
  ASSERT_TRUE(poll::parseStatus(two, 2, &s));
  EXPECT_EQ(1000, s.battery_mv);
  EXPECT_EQ(0, s.tx_queue);
  EXPECT_FALSE(s.has_uptime);

  // Uptime starts at offset 20. 20 bytes fills tx air time and stops before it.
  uint8_t almost[20];
  memset(almost, 0xFF, sizeof(almost));
  ASSERT_TRUE(poll::parseStatus(almost, sizeof(almost), &s));
  EXPECT_FALSE(s.has_uptime);
  EXPECT_EQ(0u, s.uptime_secs);
  EXPECT_EQ(0xFFFFu, s.battery_mv);

  uint8_t with_up[24];
  memset(with_up, 0, sizeof(with_up));
  with_up[20] = 0x04;
  with_up[21] = 0x03;
  with_up[22] = 0x02;
  with_up[23] = 0x01;
  ASSERT_TRUE(poll::parseStatus(with_up, sizeof(with_up), &s));
  EXPECT_TRUE(s.has_uptime);
  EXPECT_EQ(0x01020304u, s.uptime_secs);

  // Short stats (8) inside a 16-byte plaintext after the 4-byte tag: 4 zero
  // pad bytes. Those complete packets_recv as 0 and do not reach uptime.
  uint8_t padded[12];
  memset(padded, 0, sizeof(padded));
  memcpy(padded, (const uint8_t[]){0x3C, 0x0F, 0x00, 0x00, 0x92, 0xFF, 0xA6, 0xFF}, 8);
  ASSERT_TRUE(poll::parseStatus(padded, sizeof(padded), &s));
  EXPECT_EQ(3900, s.battery_mv);
  EXPECT_EQ(0, s.tx_queue);
  EXPECT_EQ(-110, s.noise_floor);
  EXPECT_EQ(-90, s.last_rssi);
  EXPECT_EQ(0u, s.packets_recv);
  EXPECT_FALSE(s.has_uptime);
}

TEST(PollReplyParsers, VoltageAndTemperatureOnChannelOne) {
  const uint8_t both[] = {0x01, 0x74, 0x01, 0x97, 0x01, 0x67, 0x00, 0xA7};
  poll::TelemetryReading t;
  ASSERT_TRUE(poll::parseTelemetry(both, sizeof(both), &t));
  EXPECT_TRUE(t.has_voltage);
  EXPECT_EQ(4070, t.battery_mv);
  EXPECT_TRUE(t.has_temperature);
  EXPECT_EQ(167, t.temperature_c10);

  const uint8_t volts[] = {0x01, 0x74, 0x01, 0x97};
  ASSERT_TRUE(poll::parseTelemetry(volts, sizeof(volts), &t));
  EXPECT_TRUE(t.has_voltage);
  EXPECT_EQ(4070, t.battery_mv);
  EXPECT_FALSE(t.has_temperature);

  // Temperature alone is not a voltage.
  const uint8_t temp_only[] = {1, 103, 0, 1};
  ASSERT_TRUE(poll::parseTelemetry(temp_only, sizeof(temp_only), &t));
  EXPECT_FALSE(t.has_voltage);
  EXPECT_TRUE(t.has_temperature);
  EXPECT_EQ(1, t.temperature_c10);
}

TEST(PollReplyParsers, TemperatureRangeAndIgnoredChannels) {
  poll::TelemetryReading t;
  const uint8_t neg[] = {1, 116, 0x01, 0x8A, 1, 103, 0xFF, 0xF6, 0, 0};
  ASSERT_TRUE(poll::parseTelemetry(neg, sizeof(neg), &t));
  EXPECT_EQ(3940, t.battery_mv);
  EXPECT_TRUE(t.has_temperature);
  EXPECT_EQ(-10, t.temperature_c10);

  const uint8_t no_temp[] = {1, 116, 0x01, 0x8A};
  ASSERT_TRUE(poll::parseTelemetry(no_temp, sizeof(no_temp), &t));
  EXPECT_FALSE(t.has_temperature);

  const uint8_t other[] = {2, 103, 0x00, 0x64};
  ASSERT_TRUE(poll::parseTelemetry(other, sizeof(other), &t));
  EXPECT_FALSE(t.has_temperature);
  EXPECT_FALSE(t.has_voltage);

  // Unknown type stops the walk, so the voltage after it is not read.
  const uint8_t unknown[] = {1, 250, 0, 1, 103, 0, 1};
  ASSERT_TRUE(poll::parseTelemetry(unknown, sizeof(unknown), &t));
  EXPECT_FALSE(t.has_temperature);
  EXPECT_FALSE(t.has_voltage);

  // Another channel's record is skipped; channel 1 on either side is kept.
  const uint8_t mixed[] = {2, 103, 0x00, 0x64, 1, 116, 0x01, 0x8A, 3, 103, 0x01, 0x00, 1, 103,
                           0x00, 0xA5};
  ASSERT_TRUE(poll::parseTelemetry(mixed, sizeof(mixed), &t));
  EXPECT_EQ(3940, t.battery_mv);
  EXPECT_EQ(165, t.temperature_c10);

  uint8_t cold[4] = {1, 103, 0, 0};
  PutBeI16(cold + 2, -1280);  // -128.0 C, the "no sensor" sentinel
  ASSERT_TRUE(poll::parseTelemetry(cold, 4, &t));
  EXPECT_FALSE(t.has_temperature);

  uint8_t ok[4] = {1, 103, 0, 0};
  PutBeI16(ok + 2, 165);
  ASSERT_TRUE(poll::parseTelemetry(ok, 4, &t));
  EXPECT_TRUE(t.has_temperature);
  EXPECT_EQ(165, t.temperature_c10);

  PutBeI16(ok + 2, -600);
  ASSERT_TRUE(poll::parseTelemetry(ok, 4, &t));
  EXPECT_TRUE(t.has_temperature);
  EXPECT_EQ(-600, t.temperature_c10);
  PutBeI16(ok + 2, -601);
  ASSERT_TRUE(poll::parseTelemetry(ok, 4, &t));
  EXPECT_FALSE(t.has_temperature);

  PutBeI16(ok + 2, 1250);
  ASSERT_TRUE(poll::parseTelemetry(ok, 4, &t));
  EXPECT_TRUE(t.has_temperature);
  EXPECT_EQ(1250, t.temperature_c10);
  PutBeI16(ok + 2, 1251);
  ASSERT_TRUE(poll::parseTelemetry(ok, 4, &t));
  EXPECT_FALSE(t.has_temperature);
}

TEST(PollReplyParsers, FirstOutOfRangeTemperatureBlocksALaterOne) {
  uint8_t data[8] = {1, 103, 0, 0, 1, 103, 0, 0};
  PutBeI16(data + 2, -1280);
  PutBeI16(data + 6, 165);
  poll::TelemetryReading t;
  ASSERT_TRUE(poll::parseTelemetry(data, sizeof(data), &t));
  EXPECT_FALSE(t.has_temperature);

  // Voltage after that unusable temperature is still read.
  uint8_t then_v[] = {1, 103, 0xFB, 0x00, 1, 116, 0x01, 0x97};
  ASSERT_TRUE(poll::parseTelemetry(then_v, sizeof(then_v), &t));
  EXPECT_FALSE(t.has_temperature);
  EXPECT_TRUE(t.has_voltage);
  EXPECT_EQ(4070, t.battery_mv);
}

TEST(PollReplyParsers, ChannelZeroPaddingAndTruncationStopTheWalk) {
  poll::TelemetryReading t;
  const uint8_t pad[] = {1, 116, 0x01, 0x97, 0, 0, 0, 0, 0, 0, 0, 0};
  ASSERT_TRUE(poll::parseTelemetry(pad, sizeof(pad), &t));
  EXPECT_TRUE(t.has_voltage);
  EXPECT_EQ(4070, t.battery_mv);
  EXPECT_FALSE(t.has_temperature);

  // Zero-filled to a 16-byte block. Channel 0 stops before a false record.
  uint8_t block[16];
  memset(block, 0, sizeof(block));
  memcpy(block, (const uint8_t[]){1, 116, 0x01, 0x97}, 4);
  ASSERT_TRUE(poll::parseTelemetry(block, sizeof(block), &t));
  EXPECT_EQ(4070, t.battery_mv);
  EXPECT_FALSE(t.has_temperature);

  const uint8_t trunc_v[] = {1, 116, 0x01};
  ASSERT_TRUE(poll::parseTelemetry(trunc_v, sizeof(trunc_v), &t));
  EXPECT_FALSE(t.has_voltage);

  const uint8_t trunc_t[] = {1, 116, 0x01, 0x97, 1, 103, 0x00};
  ASSERT_TRUE(poll::parseTelemetry(trunc_t, sizeof(trunc_t), &t));
  EXPECT_TRUE(t.has_voltage);
  EXPECT_EQ(4070, t.battery_mv);
  EXPECT_FALSE(t.has_temperature);

  // Type 0 is a known 1-byte record, so parsing continues to the voltage.
  const uint8_t digital[] = {1, 0, 0x7F, 1, 116, 0x01, 0x97};
  ASSERT_TRUE(poll::parseTelemetry(digital, sizeof(digital), &t));
  EXPECT_EQ(4070, t.battery_mv);

  const uint8_t channel_zero[] = {0, 116, 0x01, 0x97};
  ASSERT_TRUE(poll::parseTelemetry(channel_zero, sizeof(channel_zero), &t));
  EXPECT_FALSE(t.has_voltage);

  EXPECT_TRUE(poll::parseTelemetry(nullptr, 0, &t));
  EXPECT_FALSE(t.has_voltage);
  EXPECT_FALSE(poll::parseTelemetry(nullptr, 4, &t));
  EXPECT_FALSE(poll::parseTelemetry(digital, 4, nullptr));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
