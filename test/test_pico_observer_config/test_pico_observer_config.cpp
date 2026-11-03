#define WITH_MQTT_BRIDGE 1
#include <gtest/gtest.h>
#include "helpers/mqtt/PicoObserverConfigPolicy.h"

using namespace PicoObserverConfigPolicy;

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

TEST(PicoObserverConfig, StatusIntervalMatchesEsp32Range) {
  EXPECT_EQ(statusIntervalMs(0), kDefaultStatusIntervalMs);
  EXPECT_EQ(statusIntervalMs(999), kDefaultStatusIntervalMs);
  EXPECT_EQ(statusIntervalMs(1000), 1000u);
  EXPECT_EQ(statusIntervalMs(60000), 60000u);
  EXPECT_EQ(statusIntervalMs(3600000), 3600000u);
  EXPECT_EQ(statusIntervalMs(3600001), kDefaultStatusIntervalMs);
}

TEST(PicoObserverConfig, OwnerKeyClaimIsUppercaseOrAbsent) {
  char out[65];
  EXPECT_EQ(ownerKeyClaim("", out, sizeof(out)), nullptr);
  EXPECT_EQ(ownerKeyClaim(nullptr, out, sizeof(out)), nullptr);
  const char* lower = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  ASSERT_EQ(ownerKeyClaim(lower, out, sizeof(out)), out);
  EXPECT_STREQ(out, "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF");
  char tiny[4];
  ASSERT_EQ(ownerKeyClaim("abcdef", tiny, sizeof(tiny)), tiny);
  EXPECT_STREQ(tiny, "ABC");
}

TEST(PicoObserverConfig, OnlyConnectionInputsRequireReconnect) {
  MQTTPrefs a{}, b{};
  strcpy(a.wifi_ssid, "net"); strcpy(a.mqtt_slot_preset[0], "analyzer-us");
  b = a;
  EXPECT_FALSE(connectionChanged(a, b));
  b.mqtt_status_interval = 60000; b.mqtt_packets_enabled = 1; b.mqtt_slot_packet_filter[0] = 3;
  b.mqtt_advert_verify = 1; b.display_timeout_secs = 5; strcpy(b.mqtt_ntp_server, "ntp.local");
  strcpy(b.mqtt_slot_host[1], "other-slot"); strcpy(b.mqtt_origin, "renamed");
  EXPECT_FALSE(connectionChanged(a, b));
  // Bytes after the terminator are not configuration.
  b = a; b.wifi_ssid[10] = 'x';
  EXPECT_FALSE(connectionChanged(a, b));
  auto changed = [&](void (*edit)(MQTTPrefs&)) { MQTTPrefs c = a; edit(c); return connectionChanged(a, c); };
  EXPECT_TRUE(changed([](MQTTPrefs& p) { strcpy(p.wifi_ssid, "other"); }));
  EXPECT_TRUE(changed([](MQTTPrefs& p) { strcpy(p.wifi_password, "pw"); }));
  EXPECT_TRUE(changed([](MQTTPrefs& p) { strcpy(p.mqtt_iata, "SEA"); }));
  EXPECT_TRUE(changed([](MQTTPrefs& p) { strcpy(p.mqtt_slot_iata[0], "PDX"); }));
  EXPECT_TRUE(changed([](MQTTPrefs& p) { strcpy(p.mqtt_slot_preset[0], "custom"); }));
  EXPECT_TRUE(changed([](MQTTPrefs& p) { strcpy(p.mqtt_slot_host[0], "broker"); }));
  EXPECT_TRUE(changed([](MQTTPrefs& p) { p.mqtt_slot_port[0] = 1884; }));
  EXPECT_TRUE(changed([](MQTTPrefs& p) { strcpy(p.mqtt_slot_username[0], "u"); }));
  EXPECT_TRUE(changed([](MQTTPrefs& p) { strcpy(p.mqtt_slot_password[0], "p"); }));
  EXPECT_TRUE(changed([](MQTTPrefs& p) { strcpy(p.mqtt_slot_token[0], "t"); }));
  EXPECT_TRUE(changed([](MQTTPrefs& p) { strcpy(p.mqtt_slot_topic[0], "x/y"); }));
  EXPECT_TRUE(changed([](MQTTPrefs& p) { strcpy(p.mqtt_slot_audience[0], "aud"); }));
  EXPECT_TRUE(changed([](MQTTPrefs& p) { strcpy(p.mqtt_owner_public_key, "AB"); }));
  EXPECT_TRUE(changed([](MQTTPrefs& p) { strcpy(p.mqtt_email, "a@b"); }));
}
