// Host tests for AlertReporter's fault edge detector, duration math, and
// message formatting (src/helpers/AlertFaultPolicy.h). Tick and formatWifiAlert
// share an OutageSnapshot (down, started_ms, initiating reason) — the same
// data MQTTBridge feeds production.
#include <gtest/gtest.h>
#include <atomic>
#include <limits>
#include <stdint.h>

#include "helpers/AlertFaultPolicy.h"

namespace Alert = AlertFaultPolicy;

namespace {

const uint32_t kWifiThresh = Alert::thresholdMs(30);   // default alert.wifi
const uint32_t kMinInterval = Alert::minIntervalMs(60);
const uint8_t kBeaconTimeout = 200;  // WIFI_REASON_BEACON_TIMEOUT
const uint8_t kAssocLeave = 8;       // WIFI_REASON_ASSOC_LEAVE (WiFi.disconnect())

Alert::Fault OkFault() {
  Alert::Fault f{};
  f.state = Alert::State::OK;
  return f;
}

Alert::OutageSnapshot Down(uint32_t started_ms, uint8_t reason = 0) {
  Alert::OutageSnapshot snap{};
  snap.down = true;
  snap.started_ms = started_ms;
  snap.reason = reason;
  return snap;
}

Alert::OutageSnapshot Up() { return {}; }

// Same sequence AlertReporter uses on the WiFi path: tick, format from the
// snapshot, commit on FireDown / FireRecovered.
bool ProductionWifiAlert(Alert::Fault& f, uint32_t now,
                         const Alert::OutageSnapshot& snap, char* text,
                         size_t text_size) {
  Alert::TickResult r = Alert::tick(f, now, snap, kWifiThresh, kMinInterval);
  if (!Alert::formatWifiAlert(text, text_size, r, snap)) return false;
  if (r.action == Alert::Action::FireDown) {
    Alert::commitDown(f, now, snap.started_ms);
  } else {
    Alert::commitRecovered(f);
  }
  return true;
}

}  // namespace

TEST(AlertFaultPolicy, DownDurationUsesCurrentOutageStartNotLastEvent) {
  Alert::Fault f = OkFault();
  const uint32_t boot_event = 1;
  const uint32_t outage_start = 10 * 60000;
  const uint32_t now = outage_start + kWifiThresh;
  const uint32_t last_reconnect_event = now - 5000;

  Alert::TickResult r =
      Alert::tick(f, now, Down(outage_start, kBeaconTimeout), kWifiThresh,
                  kMinInterval);
  EXPECT_EQ(Alert::Action::FireDown, r.action);
  EXPECT_EQ(kWifiThresh, r.duration_ms);
  EXPECT_NE(now - boot_event, r.duration_ms);
  EXPECT_NE(now - last_reconnect_event, r.duration_ms);
}

TEST(AlertFaultPolicy,
     ProductionFlowReason8ReconnectsPreserveDurationAndInitiatingReason) {
  Alert::OutageSnapshot snap{};
  snap = Alert::applyWifiStatus(1000, true, snap, false);
  EXPECT_FALSE(snap.down);
  EXPECT_EQ(0U, snap.started_ms);
  EXPECT_EQ(0, snap.reason);

  const uint32_t drop = 10 * 60000U;
  snap = Alert::applyWifiDisconnectEvent(drop, kBeaconTimeout, snap);
  snap = Alert::applyWifiStatus(drop + 100, false, snap, true);
  EXPECT_TRUE(snap.down);
  EXPECT_EQ(drop, snap.started_ms);
  EXPECT_EQ(kBeaconTimeout, snap.reason);

  for (int i = 1; i <= 24; ++i) {
    const uint32_t t = drop + (uint32_t)i * 300000U;
    snap = Alert::applyWifiDisconnectEvent(t, kAssocLeave, snap);
    snap = Alert::applyWifiStatus(t + 1, false, snap, true);
  }
  EXPECT_EQ(drop, snap.started_ms);
  EXPECT_EQ(kBeaconTimeout, snap.reason);

  const uint32_t two_h_two_m = (2U * 3600U + 2U * 60U) * 1000U;
  const uint32_t now = drop + two_h_two_m;
  Alert::Fault f = OkFault();
  char text[80];
  ASSERT_TRUE(ProductionWifiAlert(f, now, snap, text, sizeof(text)));
  EXPECT_STREQ("WiFi down 2h2m (reason 200)", text);
  EXPECT_EQ(Alert::State::FIRING, f.state);
  EXPECT_EQ(drop, f.last_outage_started_ms);
}

TEST(AlertFaultPolicy, PollFirstEventFillsReasonThenReason8DoesNotOverwrite) {
  Alert::OutageSnapshot snap{};
  snap = Alert::applyWifiStatus(1000, true, snap, false);
  const uint32_t drop = 5000;
  snap = Alert::applyWifiStatus(drop, false, snap, true);
  EXPECT_TRUE(snap.down);
  EXPECT_EQ(drop, snap.started_ms);
  EXPECT_EQ(0, snap.reason);

  snap = Alert::applyWifiDisconnectEvent(drop + 20, kBeaconTimeout, snap);
  EXPECT_EQ(kBeaconTimeout, snap.reason);
  EXPECT_EQ(drop, snap.started_ms);

  snap = Alert::applyWifiDisconnectEvent(drop + 15000, kAssocLeave, snap);
  EXPECT_EQ(kBeaconTimeout, snap.reason);
  EXPECT_EQ(drop, snap.started_ms);
}

TEST(AlertFaultPolicy, RecoverClearsSnapshotReasonAndStart) {
  Alert::OutageSnapshot snap = Down(5000, kBeaconTimeout);
  snap = Alert::applyWifiStatus(8000, true, snap, true);
  EXPECT_FALSE(snap.down);
  EXPECT_EQ(0U, snap.started_ms);
  EXPECT_EQ(0, snap.reason);
}

TEST(AlertFaultPolicy, DisconnectAndReconnectBetweenStatusPollsIsANewOutage) {
  Alert::OutageSnapshot snap{};
  snap = Alert::applyWifiStatus(1000, true, snap, false);

  snap = Alert::applyWifiDisconnectEvent(2000, kBeaconTimeout, snap);
  EXPECT_TRUE(snap.down);
  EXPECT_EQ(2000U, snap.started_ms);
  EXPECT_EQ(kBeaconTimeout, snap.reason);

  // GOT_IP with no status poll in between — the flap is fully between polls.
  snap = Alert::applyWifiGotIp(snap);
  EXPECT_FALSE(snap.down);
  EXPECT_EQ(0U, snap.started_ms);
  EXPECT_EQ(0, snap.reason);

  const uint8_t kAssocExpire = 4;
  snap = Alert::applyWifiDisconnectEvent(3500, kAssocExpire, snap);
  EXPECT_TRUE(snap.down);
  EXPECT_EQ(3500U, snap.started_ms);
  EXPECT_EQ(kAssocExpire, snap.reason);
  EXPECT_NE(kBeaconTimeout, snap.reason);
}

TEST(AlertFaultPolicy, StatusDetectedDownAtMillisZeroIsStillDown) {
  Alert::OutageSnapshot snap{};
  snap = Alert::applyWifiStatus(0, false, snap, false);
  EXPECT_TRUE(snap.down);
  EXPECT_EQ(0U, snap.started_ms);

  Alert::Fault f = OkFault();
  Alert::TickResult r =
      Alert::tick(f, kWifiThresh, snap, kWifiThresh, kMinInterval);
  EXPECT_EQ(Alert::Action::FireDown, r.action);
  EXPECT_EQ(kWifiThresh, r.duration_ms);
}

TEST(AlertFaultPolicy, RecoveryAfterDownCommittedAtMillisZeroQuotesElapsedTime) {
  Alert::Fault f = OkFault();
  Alert::OutageSnapshot snap = Down(0, kBeaconTimeout);
  const uint32_t down_at = kWifiThresh;
  Alert::TickResult down =
      Alert::tick(f, down_at, snap, kWifiThresh, kMinInterval);
  ASSERT_EQ(Alert::Action::FireDown, down.action);
  Alert::commitDown(f, down_at, snap.started_ms);
  EXPECT_EQ(0U, f.last_outage_started_ms);

  const uint32_t recovered_at = (2U * 3600U + 5U * 60U) * 1000U;
  Alert::TickResult rec =
      Alert::tick(f, recovered_at, Up(), kWifiThresh, kMinInterval);
  EXPECT_EQ(Alert::Action::FireRecovered, rec.action);
  EXPECT_EQ(recovered_at, rec.duration_ms);

  char text[80];
  ASSERT_TRUE(Alert::formatWifiAlert(text, sizeof(text), rec, Up()));
  EXPECT_STREQ("WiFi recovered after 2h5m", text);
}

TEST(AlertFaultPolicy, PackedSnapshotIsTheCoherentCrossTaskWord) {
  Alert::OutageSnapshot down_at_zero = Down(0, kBeaconTimeout);
  std::atomic<uint64_t> cell{0};
  cell.store(Alert::packOutageSnapshot(down_at_zero), std::memory_order_release);
  Alert::OutageSnapshot loaded = Alert::unpackOutageSnapshot(
      cell.load(std::memory_order_acquire));
  EXPECT_TRUE(loaded.down);
  EXPECT_EQ(0U, loaded.started_ms);
  EXPECT_EQ(kBeaconTimeout, loaded.reason);

  cell.store(Alert::packOutageSnapshot(Up()), std::memory_order_release);
  loaded = Alert::unpackOutageSnapshot(cell.load(std::memory_order_acquire));
  EXPECT_FALSE(loaded.down);
  EXPECT_EQ(0U, loaded.started_ms);
  EXPECT_EQ(0, loaded.reason);

  Alert::OutageSnapshot dirty_up{};
  dirty_up.down = false;
  dirty_up.started_ms = 12345;
  dirty_up.reason = kAssocLeave;
  loaded = Alert::unpackOutageSnapshot(Alert::packOutageSnapshot(dirty_up));
  EXPECT_FALSE(loaded.down);
  EXPECT_EQ(0U, loaded.started_ms);
  EXPECT_EQ(0, loaded.reason);
}

TEST(AlertFaultPolicy, DoesNotFireBelowThreshold) {
  Alert::Fault f = OkFault();
  Alert::TickResult r =
      Alert::tick(f, 1000 + kWifiThresh - 1, Down(1000), kWifiThresh,
                  kMinInterval);
  EXPECT_EQ(Alert::Action::None, r.action);
}

TEST(AlertFaultPolicy, FiresAtExactThreshold) {
  Alert::Fault f = OkFault();
  Alert::TickResult r =
      Alert::tick(f, 1000 + kWifiThresh, Down(1000), kWifiThresh, kMinInterval);
  EXPECT_EQ(Alert::Action::FireDown, r.action);
  EXPECT_EQ(kWifiThresh, r.duration_ms);
}

TEST(AlertFaultPolicy, FirstFireIgnoresMinIntervalUptime) {
  Alert::Fault f = OkFault();
  EXPECT_EQ(0U, f.fired_at_ms);
  const uint32_t start = 1000;
  const uint32_t now = start + kWifiThresh;
  EXPECT_LT(now, kMinInterval);
  Alert::TickResult r =
      Alert::tick(f, now, Down(start), kWifiThresh, kMinInterval);
  EXPECT_EQ(Alert::Action::FireDown, r.action);
}

TEST(AlertFaultPolicy, RateLimitBlocksRepeatUntilFloorElapses) {
  Alert::Fault f = OkFault();
  const uint32_t start = 1000;
  const uint32_t first = start + kWifiThresh;
  Alert::commitDown(f, first, start);

  Alert::commitRecovered(f);
  Alert::TickResult r = Alert::tick(f, first + kMinInterval - 1, Down(start),
                                    kWifiThresh, kMinInterval);
  EXPECT_EQ(Alert::Action::None, r.action);

  Alert::TickResult due = Alert::tick(f, first + kMinInterval, Down(start),
                                      kWifiThresh, kMinInterval);
  EXPECT_EQ(Alert::Action::FireDown, due.action);
}

TEST(AlertFaultPolicy, MinIntervalClampsBelowOneHour) {
  EXPECT_EQ(60U * 60000U, Alert::minIntervalMs(0));
  EXPECT_EQ(60U * 60000U, Alert::minIntervalMs(30));
  EXPECT_EQ(60U * 60000U, Alert::minIntervalMs(59));
  EXPECT_EQ(60U * 60000U, Alert::minIntervalMs(60));
  EXPECT_EQ(120U * 60000U, Alert::minIntervalMs(120));
}

TEST(AlertFaultPolicy, RecoveredDurationUsesRememberedOutageStart) {
  Alert::Fault f = OkFault();
  const uint32_t start = 10 * 60000;
  const uint32_t down_at = start + kWifiThresh;
  Alert::commitDown(f, down_at, start);

  const uint32_t recovered_at = start + (2U * 3600U + 5U * 60U) * 1000U;
  Alert::TickResult r =
      Alert::tick(f, recovered_at, Up(), kWifiThresh, kMinInterval);
  EXPECT_EQ(Alert::Action::FireRecovered, r.action);
  EXPECT_EQ(recovered_at - start, r.duration_ms);

  char text[80];
  ASSERT_TRUE(Alert::formatWifiAlert(text, sizeof(text), r, Up()));
  EXPECT_STREQ("WiFi recovered after 2h5m", text);
}

TEST(AlertFaultPolicy, SecondOutageUsesNewStartNotTheFirst) {
  Alert::Fault f = OkFault();
  const uint32_t first_start = 1000;
  Alert::commitDown(f, first_start + kWifiThresh, first_start);
  Alert::TickResult recovered = Alert::tick(
      f, first_start + kWifiThresh + 1000, Up(), kWifiThresh, kMinInterval);
  EXPECT_EQ(Alert::Action::FireRecovered, recovered.action);
  Alert::commitRecovered(f);

  const uint32_t second_start = first_start + kWifiThresh + 5000;
  const uint32_t second_now = second_start + kWifiThresh + kMinInterval;
  Alert::TickResult r = Alert::tick(f, second_now, Down(second_start),
                                    kWifiThresh, kMinInterval);
  EXPECT_EQ(Alert::Action::FireDown, r.action);
  EXPECT_EQ(second_now - second_start, r.duration_ms);
  EXPECT_NE(second_now - first_start, r.duration_ms);
}

TEST(AlertFaultPolicy, TickDoesNotMutateUntilCommit) {
  Alert::Fault f = OkFault();
  const uint32_t start = 1000;
  Alert::tick(f, start + kWifiThresh, Down(start), kWifiThresh, kMinInterval);
  EXPECT_EQ(Alert::State::OK, f.state);
  EXPECT_EQ(0U, f.fired_at_ms);

  Alert::commitDown(f, start + kWifiThresh, start);
  Alert::TickResult recovered =
      Alert::tick(f, start + kWifiThresh + 1, Up(), kWifiThresh, kMinInterval);
  EXPECT_EQ(Alert::Action::FireRecovered, recovered.action);
  EXPECT_EQ(Alert::State::FIRING, f.state);
  Alert::commitRecovered(f);
  EXPECT_EQ(Alert::State::OK, f.state);
}

TEST(AlertFaultPolicy, RearmClearsFiringWhenThresholdDisabled) {
  Alert::Fault f = OkFault();
  Alert::commitDown(f, 1000, 1);
  Alert::rearmIfDisabled(f);
  EXPECT_EQ(Alert::State::OK, f.state);
}

TEST(AlertFaultPolicy, ResetRearmsWithoutForcingASend) {
  Alert::Fault f = OkFault();
  Alert::commitDown(f, 1000, 1);
  Alert::reset(f);
  EXPECT_EQ(Alert::State::OK, f.state);
  EXPECT_EQ(0U, f.fired_at_ms);
}

TEST(AlertFaultPolicy, FormatAgeMinutesAndHours) {
  char buf[16];
  Alert::formatAge(0, buf, sizeof(buf));
  EXPECT_STREQ("0m", buf);
  Alert::formatAge(47U * 60000U, buf, sizeof(buf));
  EXPECT_STREQ("47m", buf);
  Alert::formatAge((1U * 3600U + 3U * 60U) * 1000U, buf, sizeof(buf));
  EXPECT_STREQ("1h3m", buf);
}

TEST(AlertFaultPolicy, FormatWifiAlertUsesSnapshotReason) {
  Alert::Fault f = OkFault();
  Alert::TickResult r = Alert::tick(f, 1000 + kWifiThresh,
                                    Down(1000, kBeaconTimeout), kWifiThresh,
                                    kMinInterval);
  char text[80];
  ASSERT_TRUE(Alert::formatWifiAlert(text, sizeof(text), r,
                                     Down(1000, kBeaconTimeout)));
  EXPECT_STREQ("WiFi down 30m (reason 200)", text);

  Alert::formatWifiDown(text, sizeof(text), 47U * 60000U, 0);
  EXPECT_STREQ("WiFi down 47m", text);
}

TEST(AlertFaultPolicy, FormatNetworkAlertUsesSelectedMediumLabel) {
  Alert::Fault f = OkFault();
  const Alert::OutageSnapshot snap = Down(1000, 0);
  const Alert::TickResult r = Alert::tick(
      f, 1000 + kWifiThresh, snap, kWifiThresh, kMinInterval);
  char text[80];
  ASSERT_TRUE(Alert::formatNetworkAlert(
      text, sizeof(text), "Ethernet", r, snap));
  EXPECT_STREQ("Ethernet down 30m", text);
}

TEST(AlertFaultPolicy, FormatMqttSlotMessages) {
  char text[100];
  Alert::formatMqttDown(text, sizeof(text), 1, "analyzer-us", 30U * 60000U);
  EXPECT_STREQ("MQTT slot 1 (analyzer-us) down 30m", text);
  Alert::formatMqttRecovered(text, sizeof(text), 1, "analyzer-us",
                             4U * 3600U * 1000U + 45U * 60000U);
  EXPECT_STREQ("MQTT slot 1 (analyzer-us) recovered after 4h45m", text);
}

TEST(AlertFaultPolicy, CheckDueMatchesFiveSecondCadenceAndWrap) {
  EXPECT_TRUE(Alert::checkDue(0, 0));
  EXPECT_TRUE(Alert::checkDue(1000, 0));
  const uint32_t next = Alert::nextCheckMs(1000);
  EXPECT_EQ(6000U, next);
  EXPECT_FALSE(Alert::checkDue(5999, next));
  EXPECT_TRUE(Alert::checkDue(6000, next));

  const uint32_t before_wrap = std::numeric_limits<uint32_t>::max() - 1000U;
  const uint32_t wrapped_next = Alert::nextCheckMs(before_wrap);
  EXPECT_FALSE(Alert::checkDue(before_wrap + 4999U, wrapped_next));
  EXPECT_TRUE(Alert::checkDue(before_wrap + 5000U, wrapped_next));
}

TEST(AlertFaultPolicy, DownDurationSurvivesMillisRollover) {
  Alert::Fault f = OkFault();
  const uint32_t start = std::numeric_limits<uint32_t>::max() - 1000U;
  const uint32_t now = start + kWifiThresh;
  Alert::TickResult r =
      Alert::tick(f, now, Down(start), kWifiThresh, kMinInterval);
  EXPECT_EQ(Alert::Action::FireDown, r.action);
  EXPECT_EQ(kWifiThresh, r.duration_ms);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
