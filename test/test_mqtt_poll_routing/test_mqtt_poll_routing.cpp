// Host contract tests for polling publication routing: reading kinds and slot choice by broker host.
#define WITH_MQTT_BRIDGE 1
#define PROGMEM

#include <gtest/gtest.h>
#include <cstring>
#include <string>

#include "helpers/MQTTPresets.h"
#include "helpers/MQTTPollRouting.h"

namespace {

std::string span(const char* broker) {
  const char* start = nullptr;
  size_t len = 0;
  if (!mqttBrokerHostSpan(broker, &start, &len)) return "<none>";
  return std::string(start, len);
}

}  // namespace

TEST(MQTTPollRouting, ReadingKindsMapToBatteryAndTelemetryTopics) {
  EXPECT_EQ(MQTT_PUBLICATION_BATTERY, mqttPollReadingPublication(MQTT_POLL_READING_BATTERY));
  EXPECT_EQ(MQTT_PUBLICATION_TELEMETRY, mqttPollReadingPublication(MQTT_POLL_READING_TELEMETRY));
  EXPECT_EQ(-1, mqttPollReadingPublication(2));
  EXPECT_EQ(-1, mqttPollReadingPublication(0xFF));
  EXPECT_EQ(0, MQTT_POLL_READING_BATTERY);
  EXPECT_EQ(1, MQTT_POLL_READING_TELEMETRY);
  EXPECT_GE(kMQTTPollReadingJsonMax, 1400u);
}

TEST(MQTTPollRouting, HostSpanStripsSchemePortPathUserInfoAndTrailingDot) {
  EXPECT_EQ("mqtt-us-v1.letsmesh.net", span("wss://mqtt-us-v1.letsmesh.net:443/mqtt"));
  EXPECT_EQ("meshrank.net", span("mqtts://meshrank.net:8883"));
  EXPECT_EQ("broker.example.com", span("broker.example.com"));
  EXPECT_EQ("broker.example.com", span("broker.example.com:1883"));
  EXPECT_EQ("broker.example.com", span("broker.example.com."));
  EXPECT_EQ("broker.example.com", span("mqtt://user:pw@broker.example.com.:1883/x?y#z"));
  EXPECT_EQ("2001:db8::1", span("mqtts://[2001:db8::1]:8883"));
  EXPECT_EQ("2001:db8::1", span("2001:db8::1"));
  EXPECT_EQ("<none>", span(""));
  EXPECT_EQ("<none>", span(nullptr));
  EXPECT_EQ("<none>", span("wss://:443/mqtt"));
  EXPECT_EQ("<none>", span("mqtts://[2001:db8::1"));
  EXPECT_EQ("<none>", span("."));
}

TEST(MQTTPollRouting, PicksLowestEnabledSlotWhoseHostMatches) {
  const char hosts[2][64] = {"mqtt.example.org", "mqtt-us-v1.letsmesh.net"};
  const MQTTPollSlotHost slots[] = {
    {true, "mqtts://meshrank.net:8883"},
    {false, "mqtt.example.org"},                       // disabled: skipped
    {true, "wss://mqtt-us-v1.letsmesh.net:443/mqtt"},  // matches the second entry
    {true, "mqtt.example.org"},                        // matches the first, but higher
  };
  EXPECT_EQ(3, mqttPollSlotForHosts(hosts, 2, slots, 4));
  EXPECT_EQ(0, mqttPollSlotForHosts(hosts, 0, slots, 4));
  EXPECT_EQ(0, mqttPollSlotForHosts(hosts, 2, slots, 2));
  EXPECT_EQ(0, mqttPollSlotForHosts(nullptr, 2, slots, 4));
  EXPECT_EQ(0, mqttPollSlotForHosts(hosts, 2, nullptr, 4));
}

TEST(MQTTPollRouting, ComparesCaseInsensitivelyAndExactlyOnTheHost) {
  const MQTTPollSlotHost slots[] = {{true, "wss://Broker.Example.COM:443/mqtt"}};
  const char exact[1][64] = {"broker.example.com"};
  const char dotted[1][64] = {"broker.example.com."};
  const char upper[1][64] = {"BROKER.EXAMPLE.COM"};
  const char suffix[1][64] = {"example.com"};
  const char longer[1][64] = {"broker.example.com.au"};
  const char with_port[1][64] = {"broker.example.com:443"};
  const char empty[1][64] = {""};
  EXPECT_EQ(1, mqttPollSlotForHosts(exact, 1, slots, 1));
  EXPECT_EQ(1, mqttPollSlotForHosts(dotted, 1, slots, 1));
  EXPECT_EQ(1, mqttPollSlotForHosts(upper, 1, slots, 1));
  EXPECT_EQ(0, mqttPollSlotForHosts(suffix, 1, slots, 1));
  EXPECT_EQ(0, mqttPollSlotForHosts(longer, 1, slots, 1));
  EXPECT_EQ(0, mqttPollSlotForHosts(with_port, 1, slots, 1));
  EXPECT_EQ(0, mqttPollSlotForHosts(empty, 1, slots, 1));
}

TEST(MQTTPollRouting, ReadsAtMostFourEntriesAndToleratesUnterminatedOnes) {
  char hosts[5][64];
  memset(hosts, 'a', sizeof(hosts));  // no NUL anywhere in the first four
  strcpy(hosts[4], "fifth.example.com");
  const MQTTPollSlotHost slots[] = {{true, "fifth.example.com"}};
  EXPECT_EQ(0, mqttPollSlotForHosts(hosts, 5, slots, 1));

  std::string sixty_four(64, 'a');
  const MQTTPollSlotHost long_slot[] = {{true, sixty_four.c_str()}};
  EXPECT_EQ(1, mqttPollSlotForHosts(hosts, 1, long_slot, 1));
}

TEST(MQTTPollRouting, PresetUrlsResolveToTheirHost) {
  const MQTTPresetDef* us = findMQTTPreset("analyzer-us");
  ASSERT_NE(nullptr, us);
  const MQTTPollSlotHost slots[] = {{true, nullptr}, {true, us->server_url}};
  const char hosts[1][64] = {"mqtt-us-v1.letsmesh.net"};
  EXPECT_EQ(2, mqttPollSlotForHosts(hosts, 1, slots, 2));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
