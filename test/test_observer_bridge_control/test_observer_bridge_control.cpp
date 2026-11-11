#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "helpers/bridges/ObserverBridgeControl.h"

namespace {

std::vector<std::string> g_log;

struct FakeLog {
  static void line(const char* msg, const char* detail = nullptr) {
    g_log.push_back(std::string(msg) + (detail ? detail : ""));
  }
  static void flush() {}
};

// Shared ordered record of host/RTC side effects for the self-heal tests.
std::vector<std::string>* g_heal_calls = nullptr;

struct FakeHealPlatform {
  static const bool kSupported = true;
  static uint32_t now;
  static MQTTSelfHealPolicy::RtcRecord rtc;
  static uint32_t nowMs() { return now; }
  static uint32_t randomBelow(uint32_t) { return 0; }
  static MQTTSelfHealPolicy::RtcRecord rtcLoad() { return rtc; }
  static void rtcStore(const MQTTSelfHealPolicy::RtcRecord& rec) {
    rtc = rec;
    if (g_heal_calls) {
      g_heal_calls->push_back("rtc:" + std::to_string(MQTTSelfHealPolicy::rtcConsecutive(rec)) + ":" +
                              MQTTSelfHealPolicy::causeLabel((MQTTSelfHealPolicy::Cause)rec.last_cause));
    }
  }
};
uint32_t FakeHealPlatform::now = 0;
MQTTSelfHealPolicy::RtcRecord FakeHealPlatform::rtc{};

// Mirrors the MQTTBridge lifecycle surface the control class touches.
struct FakeBridge {
  std::vector<std::string>* calls;
  bool running = false;
  bool unproven = false;       // stop timed out and has not been acknowledged
  bool late_acked = false;     // the task has since acknowledged
  bool stop_timed_out = false; // dirty-stop latch, cleared by a fresh start
  bool next_stop_times_out = false;

  explicit FakeBridge(std::vector<std::string>* c) : calls(c) {}

  bool isRunning() const { return running; }
  bool isStopUnproven() const { return unproven; }
  bool stopAcknowledgedLate() const { return unproven && late_acked; }
  void pollLateStopAck() {
    calls->push_back("pollLateStopAck");
    if (unproven && late_acked) {
      unproven = false;
      late_acked = false;
    }
  }
  bool canFlashAfterStop() const { return !running && !unproven && !stop_timed_out; }
  void begin() {
    calls->push_back("begin");
    if (running || unproven) return;   // refused while a stop is unproven
    running = true;
    stop_timed_out = false;
  }
  void end() {
    calls->push_back("end");
    if (!running) return;
    running = false;
    if (next_stop_times_out) {
      unproven = true;
      stop_timed_out = true;
      next_stop_times_out = false;
    }
  }
  void setSlotPreset(int slot, const char* preset) {
    calls->push_back("slot" + std::to_string(slot) + ":" + preset);
  }

  // Self-heal sampling surface.
  bool quarantined = false;
  bool network_up = true;
  uint32_t evidence_age_ms = MQTTSelfHealPolicy::kNoEvidence;
  uint8_t candidate_slots = 0;
  uint8_t connected_slots = 0;
  bool anyClientQuarantined() const { return quarantined; }
  bool isNetworkUp() const { return network_up; }
  uint32_t ntpEvidenceAgeMs() const { return evidence_age_ms; }
  uint8_t candidateSlotCount() const { return candidate_slots; }
  // Reports one connected slot from read number connect_from_read on (-1 = never).
  mutable int connected_reads = 0;
  int connect_from_read = -1;
  uint8_t connectedSlotCount() const {
    connected_reads++;
    if (connect_from_read >= 0 && connected_reads >= connect_from_read) return 1;
    return connected_slots;
  }
};

struct FakeNodePrefs { uint8_t bridge_enabled = 1; };
struct FakeObserverPrefs {
  char mqtt_slot_preset[6][24] = {"a", "b", "c", "d", "e", "f"};
  uint8_t mqtt_selfheal_enabled = 1;
  uint8_t alert_enabled = 1;
};

struct FakeHost {
  std::vector<std::string> calls;
  FakeNodePrefs node;
  FakeObserverPrefs obs;
  FakeBridge* alert_bridge = nullptr;
  int created = 0;
  bool flash_result = true;
  const char* flash_reply = "";
  std::vector<std::string> alerts;
  std::function<void()> on_drain;

  FakeBridge* newBridge() {
    created++;
    calls.push_back("new");
    return new FakeBridge(&calls);
  }
  void prepareBridgeStart(FakeBridge*, bool restart) { calls.push_back(restart ? "prepareRestart" : "prepare"); }
  void setAlertBridge(FakeBridge* b) { alert_bridge = b; }
  FakeNodePrefs* getNodePrefs() { return &node; }
  FakeObserverPrefs* getObserverPrefs() { return &obs; }
  void drainOutbound(uint32_t) {
    calls.push_back("drain");
    if (on_drain) on_drain();
  }
  bool flashFromManifest(char* reply) {
    calls.push_back("flash");
    strcpy(reply, flash_reply);
    return flash_result;
  }
  void otaAlert(const char* msg) { alerts.push_back(msg); }
  bool sendAlertText(const char* msg) {
    calls.push_back(std::string("alert:") + msg);
    return true;
  }
  void rebootForSelfHeal() { calls.push_back("reboot"); }
  bool ota = false;
  uint32_t ota_update_at = 0;
  bool portal_open = false;
  bool isOtaInProgress() { return ota_update_at != 0 || portal_open || ota; }
};

using Ctl = ObserverBridgeControl<FakeBridge, FakeLog, FakeHealPlatform>;

class ObserverBridgeControlTest : public ::testing::Test {
protected:
  void SetUp() override { g_log.clear(); }
  void TearDown() override { delete bridge; }

  void startRunning() {
    ctl.setState(host, bridge, true);
    ASSERT_TRUE(bridge && bridge->running);
    host.calls.clear();
  }

  FakeHost host;
  FakeBridge* bridge = nullptr;
  Ctl ctl;
};

}  // namespace

TEST(ObserverBridgeControlSize, FlagsAndBatchClockFitInEightBytesBesideSelfHealState) {
  EXPECT_LE(sizeof(Ctl), 8u + sizeof(Ctl::SelfHeal));
  EXPECT_LE(sizeof(Ctl::SelfHeal), 48u);
}

// --- enable / disable -------------------------------------------------------

TEST_F(ObserverBridgeControlTest, EnableCreatesPreparesAndAttachesAlerts) {
  ctl.setState(host, bridge, true);
  ASSERT_NE(nullptr, bridge);
  EXPECT_TRUE(bridge->running);
  EXPECT_EQ((std::vector<std::string>{"new", "prepare", "begin"}), host.calls);
  EXPECT_EQ(bridge, host.alert_bridge);
  EXPECT_FALSE(ctl.resumePending());
}

TEST_F(ObserverBridgeControlTest, EnableWhenRunningIsNoOp) {
  startRunning();
  ctl.setState(host, bridge, true);
  EXPECT_TRUE(host.calls.empty());
  EXPECT_EQ(1, host.created);
}

TEST_F(ObserverBridgeControlTest, DisableStopsAndDetachesAlerts) {
  startRunning();
  ctl.setState(host, bridge, false);
  EXPECT_FALSE(bridge->running);
  EXPECT_EQ((std::vector<std::string>{"end"}), host.calls);
  EXPECT_EQ(nullptr, host.alert_bridge);
}

// --- restart ----------------------------------------------------------------

TEST_F(ObserverBridgeControlTest, RestartWithCleanStop) {
  startRunning();
  ctl.restart(host, bridge);
  EXPECT_TRUE(bridge->running);
  EXPECT_EQ((std::vector<std::string>{"end", "prepareRestart", "begin"}), host.calls);
  EXPECT_FALSE(ctl.resumePending());
}

TEST_F(ObserverBridgeControlTest, RestartOfStoppedBridgeIsNoOp) {
  ctl.restart(host, bridge);
  EXPECT_EQ(nullptr, bridge);
  startRunning();
  ctl.setState(host, bridge, false);
  host.calls.clear();
  ctl.restart(host, bridge);
  EXPECT_TRUE(host.calls.empty());
}

TEST_F(ObserverBridgeControlTest, RestartWithUnprovenStopResumesAfterLateAck) {
  startRunning();
  bridge->next_stop_times_out = true;
  ctl.restart(host, bridge);
  EXPECT_FALSE(bridge->running);
  EXPECT_TRUE(ctl.resumePending());

  ctl.poll(host, bridge);   // not acknowledged yet
  EXPECT_FALSE(bridge->running);
  EXPECT_TRUE(ctl.resumePending());

  bridge->late_acked = true;
  host.calls.clear();
  ctl.poll(host, bridge);
  EXPECT_TRUE(bridge->running);
  EXPECT_FALSE(ctl.resumePending());
  EXPECT_EQ((std::vector<std::string>{"pollLateStopAck", "prepare", "begin"}), host.calls);
  EXPECT_EQ((std::vector<std::string>{"MQTT: stop acknowledged late - resuming bridge"}), g_log);
}

TEST_F(ObserverBridgeControlTest, LateAckDoesNotResumeWhenBridgeDisabledInPrefs) {
  startRunning();
  bridge->next_stop_times_out = true;
  ctl.restart(host, bridge);
  host.node.bridge_enabled = 0;
  bridge->late_acked = true;
  ctl.poll(host, bridge);
  EXPECT_FALSE(bridge->running);
  EXPECT_FALSE(bridge->unproven);   // resources still released
}

TEST_F(ObserverBridgeControlTest, ExplicitStopCancelsPendingResume) {
  startRunning();
  bridge->next_stop_times_out = true;
  ctl.restart(host, bridge);
  ASSERT_TRUE(ctl.resumePending());

  ctl.setState(host, bridge, false);
  EXPECT_FALSE(ctl.resumePending());

  bridge->late_acked = true;
  ctl.poll(host, bridge);
  EXPECT_FALSE(bridge->running);
}

// --- slot restart and config batches ---------------------------------------

TEST_F(ObserverBridgeControlTest, SlotRestartAppliesPreset) {
  startRunning();
  ctl.restartSlot(host, bridge, 2);
  EXPECT_EQ((std::vector<std::string>{"slot2:c"}), host.calls);
}

TEST_F(ObserverBridgeControlTest, SlotRestartIgnoredWhenStopped) {
  ctl.restartSlot(host, bridge, 1);
  EXPECT_TRUE(host.calls.empty());
}

TEST_F(ObserverBridgeControlTest, BatchCoalescesSlotRestarts) {
  startRunning();
  ctl.beginConfigBatch();
  ctl.restartSlot(host, bridge, 0);
  ctl.restartSlot(host, bridge, 2);
  ctl.restartSlot(host, bridge, 2);
  EXPECT_TRUE(host.calls.empty());
  ctl.endConfigBatch(host, bridge, 3);
  EXPECT_EQ((std::vector<std::string>{"slot0:a", "slot2:c"}), host.calls);
}

TEST_F(ObserverBridgeControlTest, BatchFullRestartSupersedesSlotRestarts) {
  startRunning();
  ctl.beginConfigBatch();
  ctl.restartSlot(host, bridge, 1);
  ctl.restart(host, bridge);
  EXPECT_TRUE(host.calls.empty());
  ctl.endConfigBatch(host, bridge, 3);
  EXPECT_EQ((std::vector<std::string>{"end", "prepareRestart", "begin"}), host.calls);
}

TEST_F(ObserverBridgeControlTest, BatchDropsSlotsBeyondRuntimeCount) {
  startRunning();
  ctl.beginConfigBatch();
  ctl.restartSlot(host, bridge, 4);
  ctl.endConfigBatch(host, bridge, 3);
  EXPECT_TRUE(host.calls.empty());
}

// --- deferred OTA -----------------------------------------------------------

TEST_F(ObserverBridgeControlTest, ChecksAreExcludedDuringTheDelayAndOutboundDrain) {
  EXPECT_FALSE(ctl.otaPending(0));
  EXPECT_TRUE(ctl.otaPending(1));
  host.on_drain = [&] { EXPECT_TRUE(ctl.otaPending(0)); };
  ctl.runOta(host, bridge, false);
  EXPECT_FALSE(ctl.otaPending(0));
}

TEST_F(ObserverBridgeControlTest, OtaWithRunningBridgeStopsThenFlashes) {
  startRunning();
  ctl.runOta(host, bridge, false);
  EXPECT_EQ((std::vector<std::string>{"drain", "pollLateStopAck", "end", "flash"}), host.calls);
  EXPECT_EQ(Ctl::OTA_IDLE, ctl.otaStep());
  EXPECT_EQ((std::vector<std::string>{"OTA: starting update"}), g_log);
}

TEST_F(ObserverBridgeControlTest, OtaWithStoppedBridgeLeavesItStoppedOnFailure) {
  startRunning();
  ctl.setState(host, bridge, false);
  host.calls.clear();
  host.flash_result = false;
  host.flash_reply = "already up to date";
  ctl.runOta(host, bridge, false);
  EXPECT_FALSE(bridge->running);
  EXPECT_EQ((std::vector<std::string>{"drain", "pollLateStopAck", "flash"}), host.calls);
  EXPECT_EQ((std::vector<std::string>{"OTA: starting update", "OTA: aborted - already up to date"}), g_log);
  EXPECT_EQ((std::vector<std::string>{"OTA aborted: already up to date"}), host.alerts);
}

TEST_F(ObserverBridgeControlTest, OtaWithNoBridgeDoesNotConstructOne) {
  host.flash_result = false;
  ctl.runOta(host, bridge, true);
  EXPECT_EQ(nullptr, bridge);
  EXPECT_EQ(0, host.created);
  EXPECT_EQ((std::vector<std::string>{"drain", "flash"}), host.calls);
}

TEST_F(ObserverBridgeControlTest, OtaFlashFailureResumesRunningBridge) {
  startRunning();
  host.flash_result = false;
  host.flash_reply = "download error";
  ctl.runOta(host, bridge, false);
  EXPECT_TRUE(bridge->running);
  EXPECT_EQ((std::vector<std::string>{"drain", "pollLateStopAck", "end", "flash", "prepare", "begin"}), host.calls);
  EXPECT_EQ((std::vector<std::string>{"OTA: starting update", "OTA: aborted - download error", "OTA: resuming bridge"}),
            g_log);
  EXPECT_EQ((std::vector<std::string>{"OTA aborted: download error"}), host.alerts);
}

TEST_F(ObserverBridgeControlTest, OtaRefusedAfterUncleanStopResumesWhenAcked) {
  startRunning();
  bridge->next_stop_times_out = true;
  ctl.runOta(host, bridge, false);
  EXPECT_EQ((std::vector<std::string>{"drain", "pollLateStopAck", "end", "prepare", "begin"}), host.calls);
  EXPECT_FALSE(bridge->running);
  EXPECT_TRUE(ctl.resumePending());
  EXPECT_EQ((std::vector<std::string>{"OTA: starting update", "OTA: aborted, MQTT stop did not complete cleanly",
                                      "OTA: resuming bridge"}),
            g_log);
  EXPECT_EQ((std::vector<std::string>{"OTA aborted: MQTT stop unproven, bridge resumes when it completes"}),
            host.alerts);

  bridge->late_acked = true;
  ctl.poll(host, bridge);
  EXPECT_TRUE(bridge->running);
}

TEST_F(ObserverBridgeControlTest, OtaRefusedForUnprovenIdleBridgeLeavesItStopped) {
  startRunning();
  bridge->next_stop_times_out = true;
  ctl.setState(host, bridge, false);   // operator stop that timed out
  host.calls.clear();
  ctl.runOta(host, bridge, false);
  EXPECT_FALSE(bridge->running);
  EXPECT_EQ((std::vector<std::string>{"drain", "pollLateStopAck"}), host.calls);
  EXPECT_EQ((std::vector<std::string>{"OTA aborted: MQTT stop unclean, bridge left stopped - reboot to clear"}),
            host.alerts);
}

TEST_F(ObserverBridgeControlTest, OtaCapturesBridgeStartedDuringDrain) {
  startRunning();
  ctl.setState(host, bridge, false);
  host.calls.clear();
  host.on_drain = [&] { ctl.setState(host, bridge, true); };   // operator start mid-drain
  host.flash_result = false;
  ctl.runOta(host, bridge, false);
  EXPECT_EQ((std::vector<std::string>{"drain", "prepare", "begin", "pollLateStopAck", "end", "flash", "prepare",
                                      "begin"}),
            host.calls);
  EXPECT_TRUE(bridge->running);
}

TEST_F(ObserverBridgeControlTest, OtaIdleLatchOnlyBlocksInStrictMode) {
  startRunning();
  bridge->next_stop_times_out = true;
  ctl.setState(host, bridge, false);
  bridge->late_acked = true;   // acknowledged, but the dirty-stop latch remains
  host.flash_result = false;

  host.calls.clear();
  ctl.runOta(host, bridge, false);
  EXPECT_EQ((std::vector<std::string>{"drain", "pollLateStopAck", "flash"}), host.calls);

  host.calls.clear();
  ctl.runOta(host, bridge, true);
  EXPECT_EQ((std::vector<std::string>{"drain", "pollLateStopAck"}), host.calls);
  EXPECT_FALSE(bridge->running);
}

TEST_F(ObserverBridgeControlTest, StrictOtaAfterExplicitLateAckPreservesTimeoutBarrier) {
  startRunning();
  bridge->next_stop_times_out = true;
  ctl.setState(host, bridge, false);
  ASSERT_FALSE(bridge->canFlashAfterStop());
  bridge->late_acked = true;
  ctl.poll(host, bridge);
  ASSERT_FALSE(bridge->unproven);
  ASSERT_TRUE(bridge->stop_timed_out);
  host.calls.clear();

  ctl.runOta(host, bridge, true);
  EXPECT_EQ((std::vector<std::string>{"drain", "pollLateStopAck"}), host.calls);
  EXPECT_FALSE(bridge->running);
  EXPECT_FALSE(ctl.resumePending());
}

TEST_F(ObserverBridgeControlTest, OtaRestoresPendingResumeConsumedByItsLateAckPoll) {
  startRunning();
  bridge->next_stop_times_out = true;
  ctl.restart(host, bridge);
  ASSERT_TRUE(ctl.resumePending());
  bridge->late_acked = true;   // the OTA's own pollLateStopAck consumes this ack
  host.flash_result = false;
  ctl.runOta(host, bridge, false);
  EXPECT_TRUE(bridge->running);
  EXPECT_FALSE(ctl.resumePending());
}

// --- bounded self-heal --------------------------------------------------------

namespace {

const uint32_t kMin = 60000UL;

class ObserverSelfHealTest : public ObserverBridgeControlTest {
protected:
  void SetUp() override {
    ObserverBridgeControlTest::SetUp();
    FakeHealPlatform::now = 0;
    FakeHealPlatform::rtc = MQTTSelfHealPolicy::rtcCleared();
    g_heal_calls = &host.calls;
  }
  void TearDown() override {
    g_heal_calls = nullptr;
    ObserverBridgeControlTest::TearDown();
  }
  void pollAt(uint32_t t) {
    FakeHealPlatform::now = t;
    ctl.poll(host, bridge);
  }
  // A running bridge whose stop then timed out: down, unproven, never acknowledged.
  void makeStopUnproven() {
    startRunning();
    bridge->next_stop_times_out = true;
    ctl.setState(host, bridge, false);
    ASSERT_TRUE(bridge->unproven);
    host.calls.clear();
  }
};

}  // namespace

TEST_F(ObserverSelfHealTest, StopUnprovenAlertsThenRebootsAfterGrace) {
  makeStopUnproven();
  const uint32_t start = 40 * kMin;
  pollAt(start);
  pollAt(start + 10 * kMin - 10000);
  EXPECT_TRUE(host.calls.empty());

  pollAt(start + 10 * kMin);
  EXPECT_EQ((std::vector<std::string>{"alert:MQTT self-heal: stop unproven for 10m, rebooting in 1m"}),
            host.calls);
  EXPECT_EQ((std::vector<std::string>{"MQTT self-heal: stop unproven for 10m, rebooting in 1m"}), g_log);

  host.calls.clear();
  pollAt(start + 11 * kMin - 10000);
  EXPECT_TRUE(host.calls.empty());
  pollAt(start + 11 * kMin);
  // The RTC record is written before the reboot call.
  EXPECT_EQ((std::vector<std::string>{"rtc:1:stop unproven", "reboot"}), host.calls);
  EXPECT_EQ("MQTT self-heal: rebooting for stop unproven", g_log.back());
}

TEST_F(ObserverSelfHealTest, AlertIsLoggedButNotSentWhenAlertsAreOff) {
  makeStopUnproven();
  host.obs.alert_enabled = 0;
  pollAt(40 * kMin);
  pollAt(50 * kMin);
  EXPECT_TRUE(host.calls.empty());
  EXPECT_EQ(1u, g_log.size());
}

TEST_F(ObserverSelfHealTest, TicksAboutEveryTenSeconds) {
  makeStopUnproven();
  const uint32_t start = 40 * kMin;
  pollAt(start);
  pollAt(start + 10 * kMin - 5000);   // the hold starts at the first tick
  pollAt(start + 10 * kMin);          // under 10 s since the last tick: skipped
  EXPECT_TRUE(host.calls.empty());
  pollAt(start + 10 * kMin + 5000);
  EXPECT_EQ(1u, host.calls.size());
}

TEST_F(ObserverSelfHealTest, HealthyPeriodClearsTheBootCounter) {
  FakeHealPlatform::rtc = MQTTSelfHealPolicy::rtcMake(2, MQTTSelfHealPolicy::Cause::AllSlotsDown);
  startRunning();
  bridge->connected_slots = 1;
  bridge->candidate_slots = 1;
  pollAt(kMin);
  pollAt(11 * kMin - 10000);
  EXPECT_TRUE(host.calls.empty());
  pollAt(11 * kMin);
  EXPECT_EQ((std::vector<std::string>{"rtc:0:none"}), host.calls);
  EXPECT_EQ(0, MQTTSelfHealPolicy::rtcConsecutive(FakeHealPlatform::rtc));
  host.calls.clear();
  pollAt(60 * kMin);
  EXPECT_TRUE(host.calls.empty());
}

TEST_F(ObserverSelfHealTest, PrefOffNeverAlertsOrReboots) {
  makeStopUnproven();
  host.obs.mqtt_selfheal_enabled = 0;
  for (uint32_t t = 40 * kMin; t < 600 * kMin; t += kMin) pollAt(t);
  EXPECT_TRUE(host.calls.empty());
  EXPECT_TRUE(g_log.empty());
}

TEST_F(ObserverSelfHealTest, NullBridgeNeverActs) {
  FakeHealPlatform::rtc = MQTTSelfHealPolicy::rtcMake(1, MQTTSelfHealPolicy::Cause::StopUnproven);
  for (uint32_t t = 0; t < 600 * kMin; t += kMin) pollAt(t);
  EXPECT_EQ(nullptr, bridge);
  EXPECT_TRUE(host.calls.empty());
  EXPECT_EQ(1, MQTTSelfHealPolicy::rtcConsecutive(FakeHealPlatform::rtc));
}

// The companion observer's configuration: lifecycle sequencing without the self-heal driver.
TEST(ObserverBridgeControlDisabledHeal, SavedPrefOnNeverActsAndLateAckStillResumes) {
  g_log.clear();
  FakeHost host;
  FakeBridge* bridge = nullptr;
  ObserverBridgeControl<FakeBridge, FakeLog, ObserverSelfHealDisabledPlatform> ctl;
  ctl.setState(host, bridge, true);
  ASSERT_TRUE(bridge && bridge->running);
  bridge->next_stop_times_out = true;
  ctl.restart(host, bridge);
  ASSERT_TRUE(bridge->unproven);
  ASSERT_EQ(1, host.obs.mqtt_selfheal_enabled);
  host.calls.clear();

  for (int i = 0; i < 1000; i++) ctl.poll(host, bridge);
  EXPECT_TRUE(host.calls.empty());
  EXPECT_TRUE(g_log.empty());

  bridge->late_acked = true;
  ctl.poll(host, bridge);
  EXPECT_TRUE(bridge->running);
  EXPECT_FALSE(ctl.resumePending());
  delete bridge;
}

TEST_F(ObserverSelfHealTest, AllSlotsDownNeedsFreshEvidence) {
  startRunning();
  bridge->candidate_slots = 2;
  bridge->connected_slots = 0;
  const uint32_t start = 40 * kMin;
  for (uint32_t t = start; t <= start + 3 * 60 * kMin; t += kMin) pollAt(t);
  EXPECT_TRUE(host.calls.empty());   // no NTP evidence: not a cause

  bridge->evidence_age_ms = 10 * kMin;
  const uint32_t fresh = start + 3 * 60 * kMin + kMin;
  pollAt(fresh);
  pollAt(fresh + 2 * 60 * kMin - kMin);
  EXPECT_TRUE(host.calls.empty());
  pollAt(fresh + 2 * 60 * kMin);
  EXPECT_EQ((std::vector<std::string>{"alert:MQTT self-heal: all slots down for 120m, rebooting in 1m"}),
            host.calls);
}

TEST_F(ObserverSelfHealTest, RebootLimitStopsRebooting) {
  FakeHealPlatform::rtc = MQTTSelfHealPolicy::rtcMake(5, MQTTSelfHealPolicy::Cause::StopUnproven);
  makeStopUnproven();
  const uint32_t start = 40 * kMin;
  pollAt(start);
  for (uint32_t t = start + kMin; t < start + 600 * kMin; t += kMin) pollAt(t);
  EXPECT_EQ((std::vector<std::string>{"alert:MQTT self-heal: stop unproven for 80m, reboot limit reached"}),
            host.calls);
}

TEST_F(ObserverSelfHealTest, OtaInProgressBlocksTheRebootAndNeedsAFreshAlert) {
  makeStopUnproven();
  const uint32_t start = 40 * kMin;
  pollAt(start);
  pollAt(start + 10 * kMin);
  ASSERT_EQ(1u, host.calls.size());   // alert
  host.calls.clear();

  host.ota = true;
  for (uint32_t t = start + 11 * kMin; t <= start + 30 * kMin; t += kMin) pollAt(t);
  EXPECT_TRUE(host.calls.empty());

  host.ota = false;
  pollAt(start + 31 * kMin);
  EXPECT_EQ((std::vector<std::string>{"alert:MQTT self-heal: stop unproven for 31m, rebooting in 1m"}),
            host.calls);
  host.calls.clear();
  pollAt(start + 32 * kMin - 10000);
  EXPECT_TRUE(host.calls.empty());
  pollAt(start + 32 * kMin);
  EXPECT_EQ((std::vector<std::string>{"rtc:1:stop unproven", "reboot"}), host.calls);
}

TEST_F(ObserverSelfHealTest, ScheduledOtaBlocksDueRebootUntilFreshAlertAndGrace) {
  makeStopUnproven();
  pollAt(40 * kMin);
  pollAt(50 * kMin);
  ASSERT_EQ(1u, host.calls.size());
  host.calls.clear();

  host.ota_update_at = 51 * kMin + 2500;
  pollAt(51 * kMin);
  EXPECT_TRUE(host.calls.empty());
  host.ota_update_at = 0;
  pollAt(51 * kMin + 10000);
  ASSERT_EQ(1u, host.calls.size());
  EXPECT_EQ(0u, host.calls[0].find("alert:"));
  host.calls.clear();
  pollAt(52 * kMin);
  EXPECT_TRUE(host.calls.empty());
  pollAt(52 * kMin + 10000);
  EXPECT_EQ((std::vector<std::string>{"rtc:1:stop unproven", "reboot"}), host.calls);
}

TEST_F(ObserverSelfHealTest, IdlePortalBlocksDueReboot) {
  makeStopUnproven();
  pollAt(40 * kMin);
  pollAt(50 * kMin);
  host.calls.clear();
  host.portal_open = true;
  pollAt(51 * kMin);
  pollAt(60 * kMin);
  EXPECT_TRUE(host.calls.empty());
  host.portal_open = false;
  pollAt(61 * kMin);
  ASSERT_EQ(1u, host.calls.size());
  EXPECT_EQ(0u, host.calls[0].find("alert:"));
}

TEST_F(ObserverSelfHealTest, ConfigBatchDefersRebootUntilEndAndFreshGrace) {
  makeStopUnproven();
  pollAt(40 * kMin);
  pollAt(50 * kMin);
  host.calls.clear();
  ctl.beginConfigBatch();
  pollAt(51 * kMin);
  EXPECT_TRUE(host.calls.empty());
  ctl.endConfigBatch(host, bridge, 3);
  pollAt(51 * kMin + 10000);
  ASSERT_EQ(1u, host.calls.size());
  EXPECT_EQ(0u, host.calls[0].find("alert:"));
  host.calls.clear();
  pollAt(52 * kMin);
  EXPECT_TRUE(host.calls.empty());
  pollAt(52 * kMin + 10000);
  EXPECT_EQ((std::vector<std::string>{"rtc:1:stop unproven", "reboot"}), host.calls);
}

TEST_F(ObserverSelfHealTest, AbandonedConfigBatchOnlyBlocksForTwoMinutes) {
  makeStopUnproven();
  pollAt(40 * kMin);
  pollAt(50 * kMin);
  host.calls.clear();
  ctl.beginConfigBatch();
  pollAt(52 * kMin - 10000);
  EXPECT_TRUE(host.calls.empty());
  pollAt(52 * kMin);
  ASSERT_EQ(1u, host.calls.size());
  EXPECT_EQ(0u, host.calls[0].find("alert:"));
  host.calls.clear();
  pollAt(53 * kMin);
  EXPECT_EQ((std::vector<std::string>{"rtc:1:stop unproven", "reboot"}), host.calls);
}

TEST_F(ObserverSelfHealTest, ConfigBatchBusyBoundSurvivesClockRollover) {
  makeStopUnproven();
  const uint32_t start = UINT32_MAX - 11 * kMin;
  pollAt(start);
  pollAt(start + 10 * kMin);
  ASSERT_EQ(1u, host.calls.size());
  host.calls.clear();
  ctl.beginConfigBatch();
  pollAt(start + 12 * kMin - 10000);
  EXPECT_TRUE(host.calls.empty());
  pollAt(start + 12 * kMin);
  ASSERT_EQ(1u, host.calls.size());
  EXPECT_EQ(0u, host.calls[0].find("alert:"));
}

TEST_F(ObserverSelfHealTest, BridgeRestartDropsAllSlotsDownHoldAndPendingReboot) {
  startRunning();
  bridge->candidate_slots = 1;
  bridge->evidence_age_ms = 5 * kMin;
  const uint32_t start = 40 * kMin;
  pollAt(start);
  pollAt(start + 120 * kMin);
  ASSERT_EQ(1u, host.calls.size());   // alert
  host.calls.clear();

  ctl.restart(host, bridge);   // operator config save between two ticks
  host.calls.clear();
  pollAt(start + 121 * kMin);
  pollAt(start + 241 * kMin - 10000);
  EXPECT_TRUE(host.calls.empty());
  pollAt(start + 241 * kMin);
  EXPECT_EQ(1u, host.calls.size());
  EXPECT_EQ(0u, host.calls[0].find("alert:MQTT self-heal: all slots down"));
}

TEST_F(ObserverSelfHealTest, SlotRestartAlsoResetsAllSlotsDownHold) {
  startRunning();
  bridge->candidate_slots = 1;
  bridge->evidence_age_ms = 5 * kMin;
  const uint32_t start = 40 * kMin;
  pollAt(start);
  pollAt(start + 100 * kMin);
  ctl.restartSlot(host, bridge, 0);
  host.calls.clear();
  pollAt(start + 120 * kMin);
  EXPECT_TRUE(host.calls.empty());
}

TEST_F(ObserverSelfHealTest, RestartAttemptKeepsTheStopUnprovenHold) {
  makeStopUnproven();
  const uint32_t start = 40 * kMin;
  pollAt(start);
  ctl.setState(host, bridge, true);   // refused while the stop is unproven
  ASSERT_FALSE(bridge->running);
  host.calls.clear();
  pollAt(start + 10 * kMin);
  EXPECT_EQ((std::vector<std::string>{"alert:MQTT self-heal: stop unproven for 10m, rebooting in 1m"}),
            host.calls);
}

TEST_F(ObserverSelfHealTest, BridgeRestartResetsTheHealthyTimer) {
  FakeHealPlatform::rtc = MQTTSelfHealPolicy::rtcMake(1, MQTTSelfHealPolicy::Cause::AllSlotsDown);
  startRunning();
  bridge->connected_slots = 1;
  pollAt(kMin);
  pollAt(9 * kMin);
  ctl.restart(host, bridge);
  host.calls.clear();
  pollAt(11 * kMin);
  EXPECT_TRUE(host.calls.empty());
  pollAt(21 * kMin);
  EXPECT_EQ((std::vector<std::string>{"rtc:0:none"}), host.calls);
}

TEST_F(ObserverSelfHealTest, RebootIsSkippedWhenAFreshSampleNoLongerShowsTheCause) {
  startRunning();
  bridge->candidate_slots = 1;
  bridge->evidence_age_ms = 5 * kMin;
  const uint32_t start = 40 * kMin;
  pollAt(start);
  pollAt(start + 120 * kMin);
  ASSERT_EQ(1u, host.calls.size());   // alert
  host.calls.clear();

  // The tick's own sample reads 0 connected; the confirming sample sees a slot up.
  bridge->connect_from_read = bridge->connected_reads + 2;
  pollAt(start + 121 * kMin);
  EXPECT_TRUE(host.calls.empty());
  pollAt(start + 122 * kMin);   // the slot stays up: the cause lapses
  EXPECT_TRUE(host.calls.empty());
  EXPECT_EQ(0, MQTTSelfHealPolicy::rtcConsecutive(FakeHealPlatform::rtc));
}

TEST_F(ObserverSelfHealTest, BridgeLeftOffDeliberatelyClearsTheCounter) {
  FakeHealPlatform::rtc = MQTTSelfHealPolicy::rtcMake(1, MQTTSelfHealPolicy::Cause::StopUnproven);
  host.node.bridge_enabled = 0;
  startRunning();
  ctl.setState(host, bridge, false);   // clean stop
  host.calls.clear();
  pollAt(kMin);
  pollAt(11 * kMin - 10000);
  EXPECT_TRUE(host.calls.empty());
  pollAt(11 * kMin);
  EXPECT_EQ((std::vector<std::string>{"rtc:0:none"}), host.calls);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
