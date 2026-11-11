#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "helpers/bridges/ObserverSelfHeal.h"

namespace {

namespace Heal = MQTTSelfHealPolicy;
const uint32_t kMin = 60000UL;

struct FakePlatform {
  static const bool kSupported = true;
  static uint32_t now;
  static uint32_t jitter;
  static uint32_t random_bound;
  static unsigned random_calls;
  static Heal::RtcRecord rtc;
  static uint32_t nowMs() { return now; }
  static uint32_t randomBelow(uint32_t bound) {
    random_calls++;
    random_bound = bound;
    return jitter;
  }
  static Heal::RtcRecord rtcLoad() { return rtc; }
  static void rtcStore(const Heal::RtcRecord& rec) { rtc = rec; }
};

uint32_t FakePlatform::now = 0;
uint32_t FakePlatform::jitter = 0;
uint32_t FakePlatform::random_bound = 0;
unsigned FakePlatform::random_calls = 0;
Heal::RtcRecord FakePlatform::rtc{};

struct FakeLog {
  static void line(const char*, const char* = nullptr) {}
  static void flush() {}
};

struct FakeHost {
  struct ObserverPrefs {
    bool mqtt_selfheal_enabled = true;
    bool alert_enabled = true;
  } obs;
  struct NodePrefs { bool bridge_enabled = true; } node;
  std::vector<std::string> alerts;
  unsigned reboots = 0;
  ObserverPrefs* getObserverPrefs() { return &obs; }
  NodePrefs* getNodePrefs() { return &node; }
  bool sendAlertText(const char* msg) { alerts.emplace_back(msg); return true; }
  void rebootForSelfHeal() { reboots++; }
  bool isOtaInProgress() { return false; }
};

struct FakeBridge {
  bool connected = false;
  bool isRunning() const { return true; }
  bool isStopUnproven() const { return false; }
  bool anyClientQuarantined() const { return false; }
  bool isNetworkUp() const { return true; }
  uint32_t ntpEvidenceAgeMs() const { return 0; }
  uint8_t candidateSlotCount() const { return 1; }
  uint8_t connectedSlotCount() const { return connected ? 1 : 0; }
};

class ObserverSelfHealJitterTest : public ::testing::Test {
protected:
  void SetUp() override {
    FakePlatform::now = 0;
    FakePlatform::jitter = 0;
    FakePlatform::random_bound = 0;
    FakePlatform::random_calls = 0;
    FakePlatform::rtc = Heal::rtcCleared();
  }
  void pollAt(uint32_t now) {
    FakePlatform::now = now;
    driver.poll(host, &bridge);
  }
  ObserverSelfHeal<FakeLog, FakePlatform> driver;
  FakeHost host;
  FakeBridge bridge;
};

TEST_F(ObserverSelfHealJitterTest, JitterIsChosenAtFirstPollAndAddedToTheHold) {
  EXPECT_EQ(0u, FakePlatform::random_calls);
  FakePlatform::jitter = MQTT_SELFHEAL_ALL_DOWN_JITTER_MS;
  pollAt(0);
  EXPECT_EQ(1u, FakePlatform::random_calls);
  EXPECT_EQ(MQTT_SELFHEAL_ALL_DOWN_JITTER_MS + 1, FakePlatform::random_bound);
  const uint32_t start = 40 * kMin;
  pollAt(start);
  pollAt(start + 120 * kMin);
  EXPECT_TRUE(host.alerts.empty());
  const uint32_t hold = 120 * kMin + MQTT_SELFHEAL_ALL_DOWN_JITTER_MS;
  pollAt(start + hold - 10000);
  EXPECT_TRUE(host.alerts.empty());
  pollAt(start + hold);
  ASSERT_EQ(1u, host.alerts.size());
  EXPECT_EQ("MQTT self-heal: all slots down for 150m, rebooting in 1m", host.alerts[0]);
  pollAt(start + hold + kMin);
  EXPECT_EQ(1u, host.reboots);
  EXPECT_EQ(1, Heal::rtcConsecutive(FakePlatform::rtc));
  EXPECT_EQ(1u, FakePlatform::random_calls);
}

TEST_F(ObserverSelfHealJitterTest, ZeroJitterKeepsTheBaseHold) {
  const uint32_t start = 40 * kMin;
  pollAt(start);
  pollAt(start + 120 * kMin - 10000);
  EXPECT_TRUE(host.alerts.empty());
  pollAt(start + 120 * kMin);
  ASSERT_EQ(1u, host.alerts.size());
  EXPECT_EQ("MQTT self-heal: all slots down for 120m, rebooting in 1m", host.alerts[0]);
}

TEST_F(ObserverSelfHealJitterTest, SuppressedOutageUsesJitterAndNeverReboots) {
  FakePlatform::jitter = 15 * kMin;
  FakePlatform::rtc = Heal::rtcMake(1, Heal::Cause::StopUnproven);
  const uint32_t start = 40 * kMin;
  pollAt(start);
  pollAt(start + 240 * kMin);
  EXPECT_TRUE(host.alerts.empty());
  pollAt(start + 270 * kMin);
  ASSERT_EQ(1u, host.alerts.size());
  EXPECT_EQ("MQTT self-heal: all slots down for 270m, reboot limit reached", host.alerts[0]);
  for (uint32_t t = 271 * kMin; t < 7UL * 24 * 60 * kMin; t += kMin) pollAt(start + t);
  EXPECT_EQ(1u, host.alerts.size());
  EXPECT_EQ(0u, host.reboots);
  EXPECT_EQ(1, Heal::rtcConsecutive(FakePlatform::rtc));
}

TEST_F(ObserverSelfHealJitterTest, BridgeRestartKeepsTheChosenJitter) {
  FakePlatform::jitter = 15 * kMin;
  pollAt(40 * kMin);
  driver.onBridgeRestart();
  FakePlatform::jitter = 0;
  const uint32_t start = 50 * kMin;
  pollAt(start);
  pollAt(start + 120 * kMin);
  EXPECT_TRUE(host.alerts.empty());
  pollAt(start + 135 * kMin);
  ASSERT_EQ(1u, host.alerts.size());
  EXPECT_EQ(1u, FakePlatform::random_calls);
}

}  // namespace
