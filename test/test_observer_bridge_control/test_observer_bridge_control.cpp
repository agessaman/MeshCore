#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "helpers/bridges/ObserverBridgeControl.h"

namespace {

std::vector<std::string> g_log;

struct FakeLog {
  static void line(const char* msg, const char* detail = nullptr) {
    g_log.push_back(std::string(msg) + (detail ? detail : ""));
  }
};

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
};

struct FakeNodePrefs { uint8_t bridge_enabled = 1; };
struct FakeObserverPrefs { char mqtt_slot_preset[6][24] = {"a", "b", "c", "d", "e", "f"}; };

struct FakeHost {
  std::vector<std::string> calls;
  FakeNodePrefs node;
  FakeObserverPrefs obs;
  FakeBridge* alert_bridge = nullptr;
  int created = 0;
  bool flash_result = true;
  const char* flash_reply = "";
  std::vector<std::string> alerts;

  FakeBridge* newBridge() {
    created++;
    calls.push_back("new");
    return new FakeBridge(&calls);
  }
  void prepareBridgeStart(FakeBridge*, bool restart) { calls.push_back(restart ? "prepareRestart" : "prepare"); }
  void setAlertBridge(FakeBridge* b) { alert_bridge = b; }
  FakeNodePrefs* getNodePrefs() { return &node; }
  FakeObserverPrefs* getObserverPrefs() { return &obs; }
  void drainOutbound(uint32_t) { calls.push_back("drain"); }
  bool flashFromManifest(char* reply) {
    calls.push_back("flash");
    strcpy(reply, flash_reply);
    return flash_result;
  }
  void otaAlert(const char* msg) { alerts.push_back(msg); }
};

using Ctl = ObserverBridgeControl<FakeBridge, FakeLog>;

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

TEST(ObserverBridgeControlSize, FitsInThreeBytes) {
  EXPECT_LE(sizeof(Ctl), 3u);
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
  EXPECT_EQ((std::vector<std::string>{"OTA aborted: MQTT stop unclean, bridge left stopped"}), host.alerts);
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

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
