// Host tests for the MQTT self-heal reboot policy (src/helpers/MQTTSelfHealPolicy.h).
#include <gtest/gtest.h>
#include <stdint.h>
#include <string.h>

#include "helpers/MQTTSelfHealPolicy.h"

namespace Heal = MQTTSelfHealPolicy;

namespace {

const uint32_t kMin = 60000UL;

Heal::Inputs Healthy(uint32_t now) {
  Heal::Inputs in{};
  in.enabled = true;
  in.now_ms = now;
  in.bridge_running = true;
  in.network_up = true;
  in.evidence_age_ms = 5 * kMin;
  in.candidate_slots = 2;
  in.connected_slots = 2;
  return in;
}

Heal::Inputs Unproven(uint32_t now) {
  Heal::Inputs in = Healthy(now);
  in.bridge_running = false;
  in.stop_unproven = true;
  in.connected_slots = 0;
  return in;
}

Heal::Inputs AllDown(uint32_t now) {
  Heal::Inputs in = Healthy(now);
  in.connected_slots = 0;
  return in;
}

struct Harness {
  Heal::State st{};
  Heal::Config cfg = Heal::defaultConfig();
  uint8_t consecutive = 0;
  Heal::Result tick(const Heal::Inputs& in) { return Heal::tick(st, in, cfg, consecutive); }
};

TEST(MQTTSelfHealPolicy, DefaultsMatchTheAgreedThresholds) {
  Heal::Config cfg = Heal::defaultConfig();
  EXPECT_EQ(10 * kMin, cfg.stop_unproven_ms);
  EXPECT_EQ(30 * kMin, cfg.quarantined_ms);
  EXPECT_EQ(120 * kMin, cfg.all_slots_down_ms);
  EXPECT_EQ(30 * kMin, cfg.min_uptime_ms);
  EXPECT_EQ(1 * kMin, cfg.alert_grace_ms);
  EXPECT_EQ(10 * kMin, cfg.healthy_reset_ms);
  EXPECT_EQ(60 * kMin, cfg.evidence_max_age_ms);
  EXPECT_EQ(3, cfg.max_backoff_shift);
  EXPECT_EQ(5, cfg.max_consecutive);
}

TEST(MQTTSelfHealPolicy, HealthyNodeNeverActs) {
  Harness h;
  for (uint32_t t = 0; t < 24UL * 60 * kMin; t += kMin) {
    EXPECT_EQ(Heal::Action::None, h.tick(Healthy(t)).action);
  }
}

TEST(MQTTSelfHealPolicy, StopUnprovenAlertsThenRebootsAfterGrace) {
  Harness h;
  const uint32_t start = 40 * kMin;
  EXPECT_EQ(Heal::Action::None, h.tick(Unproven(start)).action);
  EXPECT_EQ(Heal::Action::None, h.tick(Unproven(start + 10 * kMin - 1)).action);

  Heal::Result r = h.tick(Unproven(start + 10 * kMin));
  EXPECT_EQ(Heal::Action::Alert, r.action);
  EXPECT_EQ(Heal::Cause::StopUnproven, r.cause);
  EXPECT_EQ(10 * kMin, r.held_ms);
  EXPECT_FALSE(r.reboot_suppressed);

  EXPECT_EQ(Heal::Action::None, h.tick(Unproven(start + 11 * kMin - 1)).action);
  r = h.tick(Unproven(start + 11 * kMin));
  EXPECT_EQ(Heal::Action::Reboot, r.action);
  EXPECT_EQ(Heal::Cause::StopUnproven, r.cause);
  EXPECT_EQ(11 * kMin, r.held_ms);
}

TEST(MQTTSelfHealPolicy, LateRecoveryCancelsPendingReboot) {
  Harness h;
  const uint32_t start = 40 * kMin;
  h.tick(Unproven(start));
  EXPECT_EQ(Heal::Action::Alert, h.tick(Unproven(start + 10 * kMin)).action);
  EXPECT_EQ(Heal::Action::None, h.tick(Healthy(start + 10 * kMin + 30000)).action);
  EXPECT_EQ(Heal::Action::None, h.tick(Healthy(start + 20 * kMin)).action);

  // A new episode starts its hold from scratch.
  h.tick(Unproven(start + 30 * kMin));
  EXPECT_EQ(Heal::Action::None, h.tick(Unproven(start + 35 * kMin)).action);
  EXPECT_EQ(Heal::Action::Alert, h.tick(Unproven(start + 40 * kMin)).action);
}

TEST(MQTTSelfHealPolicy, DisabledPrefNeverActsButKeepsHoldTime) {
  Harness h;
  Heal::Inputs in = Unproven(40 * kMin);
  in.enabled = false;
  h.tick(in);
  in.now_ms = 120 * kMin;
  EXPECT_EQ(Heal::Action::None, h.tick(in).action);

  in.enabled = true;
  in.now_ms = 121 * kMin;
  Heal::Result r = h.tick(in);
  EXPECT_EQ(Heal::Action::Alert, r.action);
  EXPECT_EQ(81 * kMin, r.held_ms);
}

TEST(MQTTSelfHealPolicy, MinimumUptimeHoldsOffEarlyFaults) {
  Harness h;
  h.tick(Unproven(0));
  EXPECT_EQ(Heal::Action::None, h.tick(Unproven(20 * kMin)).action);
  EXPECT_EQ(Heal::Action::None, h.tick(Unproven(30 * kMin - 1)).action);
  EXPECT_EQ(Heal::Action::Alert, h.tick(Unproven(30 * kMin)).action);
}

TEST(MQTTSelfHealPolicy, UptimeLatchSurvivesMillisWrap) {
  Harness h;
  h.tick(Healthy(UINT32_MAX - 5 * kMin));
  const uint32_t start = UINT32_MAX - 2 * kMin;
  h.tick(Unproven(start));
  // now_ms has wrapped to a small value; the hold math and uptime latch still work.
  EXPECT_EQ(Heal::Action::None, h.tick(Unproven(start + 9 * kMin)).action);
  Heal::Result r = h.tick(Unproven(start + 10 * kMin));
  EXPECT_EQ(Heal::Action::Alert, r.action);
  EXPECT_EQ(10 * kMin, r.held_ms);
}

TEST(MQTTSelfHealPolicy, AllSlotsDownNeedsNetworkBridgeAndCandidateSlots) {
  const uint32_t start = 40 * kMin;
  const uint32_t late = start + 3UL * 60 * kMin;

  Harness no_net;
  Heal::Inputs in = AllDown(start);
  in.network_up = false;
  no_net.tick(in);
  in.now_ms = late;
  EXPECT_EQ(Heal::Action::None, no_net.tick(in).action);

  Harness stopped;
  in = AllDown(start);
  in.bridge_running = false;
  stopped.tick(in);
  in.now_ms = late;
  EXPECT_EQ(Heal::Action::None, stopped.tick(in).action);

  Harness no_slots;
  in = AllDown(start);
  in.candidate_slots = 0;
  no_slots.tick(in);
  in.now_ms = late;
  EXPECT_EQ(Heal::Action::None, no_slots.tick(in).action);

  Harness down;
  down.tick(AllDown(start));
  EXPECT_EQ(Heal::Action::None, down.tick(AllDown(start + 2UL * 60 * kMin - 1)).action);
  Heal::Result r = down.tick(AllDown(start + 2UL * 60 * kMin));
  EXPECT_EQ(Heal::Action::Alert, r.action);
  EXPECT_EQ(Heal::Cause::AllSlotsDown, r.cause);
}

TEST(MQTTSelfHealPolicy, NetworkDropRestartsAllSlotsDownHold) {
  Harness h;
  const uint32_t start = 40 * kMin;
  h.tick(AllDown(start));
  Heal::Inputs in = AllDown(start + 90 * kMin);
  in.network_up = false;
  h.tick(in);
  const uint32_t back = start + 91 * kMin;
  h.tick(AllDown(back));
  EXPECT_EQ(Heal::Action::None, h.tick(AllDown(back + 2UL * 60 * kMin - 1)).action);
  EXPECT_EQ(Heal::Action::Alert, h.tick(AllDown(back + 2UL * 60 * kMin)).action);
}

TEST(MQTTSelfHealPolicy, MissingEvidenceIsNotACause) {
  Harness h;
  const uint32_t start = 40 * kMin;
  Heal::Inputs in = AllDown(start);
  in.evidence_age_ms = Heal::kNoEvidence;
  h.tick(in);
  in.now_ms = start + 5UL * 60 * kMin;
  EXPECT_EQ(Heal::Action::None, h.tick(in).action);
}

TEST(MQTTSelfHealPolicy, StaleEvidenceRestartsAllSlotsDownHold) {
  Harness h;
  const uint32_t start = 40 * kMin;
  h.tick(AllDown(start));
  Heal::Inputs in = AllDown(start + 100 * kMin);
  in.evidence_age_ms = 60 * kMin;   // still fresh at the limit
  EXPECT_EQ(Heal::Action::None, h.tick(in).action);
  in.now_ms = start + 110 * kMin;
  in.evidence_age_ms = 60 * kMin + 1;   // stale: the cause lapses
  EXPECT_EQ(Heal::Action::None, h.tick(in).action);

  // Fresh evidence returns and the hold starts over from here.
  const uint32_t back = start + 115 * kMin;
  h.tick(AllDown(back));
  EXPECT_EQ(Heal::Action::None, h.tick(AllDown(start + 2UL * 60 * kMin)).action);
  EXPECT_EQ(Heal::Action::None, h.tick(AllDown(back + 2UL * 60 * kMin - 1)).action);
  Heal::Result r = h.tick(AllDown(back + 2UL * 60 * kMin));
  EXPECT_EQ(Heal::Action::Alert, r.action);
  EXPECT_EQ(2UL * 60 * kMin, r.held_ms);
}

TEST(MQTTSelfHealPolicy, AllRefusedSlotsAreNotACause) {
  // Two enabled slots, both refused by their brokers: no candidate remains.
  Harness h;
  const uint32_t start = 40 * kMin;
  Heal::Inputs in = AllDown(start);
  in.candidate_slots = 0;
  h.tick(in);
  in.now_ms = start + 5UL * 60 * kMin;
  EXPECT_EQ(Heal::Action::None, h.tick(in).action);
}

TEST(MQTTSelfHealPolicy, MixedRefusedAndDownSlotsAreACause) {
  // One slot refused, one down for a transport reason.
  Harness h;
  const uint32_t start = 40 * kMin;
  Heal::Inputs in = AllDown(start);
  in.candidate_slots = 1;
  h.tick(in);
  in.now_ms = start + 2UL * 60 * kMin;
  Heal::Result r = h.tick(in);
  EXPECT_EQ(Heal::Action::Alert, r.action);
  EXPECT_EQ(Heal::Cause::AllSlotsDown, r.cause);
}

TEST(MQTTSelfHealPolicy, ConnectedSlotCancelsAllSlotsDown) {
  Harness h;
  const uint32_t start = 40 * kMin;
  h.tick(AllDown(start));
  Heal::Inputs in = AllDown(start + 60 * kMin);
  in.connected_slots = 1;
  h.tick(in);
  const uint32_t again = start + 61 * kMin;
  h.tick(AllDown(again));
  EXPECT_EQ(Heal::Action::None, h.tick(AllDown(start + 2UL * 60 * kMin)).action);
  EXPECT_EQ(Heal::Action::Alert, h.tick(AllDown(again + 2UL * 60 * kMin)).action);
}

TEST(MQTTSelfHealPolicy, QuarantineUsesItsOwnThreshold) {
  Harness h;
  const uint32_t start = 40 * kMin;
  Heal::Inputs in = Healthy(start);
  in.client_quarantined = true;
  h.tick(in);
  in.now_ms = start + 30 * kMin - 1;
  EXPECT_EQ(Heal::Action::None, h.tick(in).action);
  in.now_ms = start + 30 * kMin;
  Heal::Result r = h.tick(in);
  EXPECT_EQ(Heal::Action::Alert, r.action);
  EXPECT_EQ(Heal::Cause::ClientQuarantined, r.cause);
}

TEST(MQTTSelfHealPolicy, HoldDoublesPerConsecutiveRebootUpToCap) {
  Heal::Config cfg = Heal::defaultConfig();
  EXPECT_EQ(10 * kMin, Heal::requiredHoldMs(cfg, Heal::Cause::StopUnproven, 0));
  EXPECT_EQ(20 * kMin, Heal::requiredHoldMs(cfg, Heal::Cause::StopUnproven, 1));
  EXPECT_EQ(80 * kMin, Heal::requiredHoldMs(cfg, Heal::Cause::StopUnproven, 3));
  EXPECT_EQ(80 * kMin, Heal::requiredHoldMs(cfg, Heal::Cause::StopUnproven, 4));

  cfg.all_slots_down_ms = UINT32_MAX / 2;
  EXPECT_EQ(UINT32_MAX, Heal::requiredHoldMs(cfg, Heal::Cause::AllSlotsDown, 3));

  Harness h;
  h.consecutive = 1;
  const uint32_t start = 40 * kMin;
  h.tick(Unproven(start));
  EXPECT_EQ(Heal::Action::None, h.tick(Unproven(start + 10 * kMin)).action);
  EXPECT_EQ(Heal::Action::Alert, h.tick(Unproven(start + 20 * kMin)).action);
}

TEST(MQTTSelfHealPolicy, RebootLimitAlertsOnceAndNeverReboots) {
  Harness h;
  h.consecutive = h.cfg.max_consecutive;
  const uint32_t start = 40 * kMin;
  h.tick(Unproven(start));
  Heal::Result r = h.tick(Unproven(start + 80 * kMin));
  EXPECT_EQ(Heal::Action::Alert, r.action);
  EXPECT_TRUE(r.reboot_suppressed);
  for (uint32_t t = start + 81 * kMin; t < start + 600 * kMin; t += kMin) {
    EXPECT_EQ(Heal::Action::None, h.tick(Unproven(t)).action);
  }
}

TEST(MQTTSelfHealPolicy, HealthyPeriodClearsBootCounterOnce) {
  Harness h;
  h.consecutive = 2;
  h.tick(Healthy(kMin));
  EXPECT_EQ(Heal::Action::None, h.tick(Healthy(10 * kMin)).action);
  EXPECT_EQ(Heal::Action::ClearBootCounter, h.tick(Healthy(11 * kMin)).action);
  h.consecutive = 0;
  EXPECT_EQ(Heal::Action::None, h.tick(Healthy(12 * kMin)).action);
  EXPECT_EQ(Heal::Action::None, h.tick(Healthy(60 * kMin)).action);
}

TEST(MQTTSelfHealPolicy, QuarantinedNodeIsNotHealthy) {
  Harness h;
  h.consecutive = 1;
  Heal::Inputs in = Healthy(kMin);
  in.client_quarantined = true;
  h.tick(in);
  in.now_ms = 20 * kMin;
  EXPECT_EQ(Heal::Action::None, h.tick(in).action);
}

TEST(MQTTSelfHealPolicy, RtcRecordValidatesAndCounts) {
  Heal::RtcRecord garbage;
  memset(&garbage, 0xA5, sizeof(garbage));
  EXPECT_FALSE(Heal::rtcValid(garbage));
  EXPECT_EQ(0, Heal::rtcConsecutive(garbage));

  Heal::RtcRecord rec = Heal::rtcBeforeReboot(garbage, Heal::Cause::StopUnproven);
  EXPECT_TRUE(Heal::rtcValid(rec));
  EXPECT_EQ(1, Heal::rtcConsecutive(rec));
  rec = Heal::rtcBeforeReboot(rec, Heal::Cause::AllSlotsDown);
  EXPECT_EQ(2, Heal::rtcConsecutive(rec));
  EXPECT_EQ((uint8_t)Heal::Cause::AllSlotsDown, rec.last_cause);

  rec.consecutive ^= 0x01;
  EXPECT_FALSE(Heal::rtcValid(rec));

  rec = Heal::rtcMake(UINT8_MAX, Heal::Cause::StopUnproven);
  EXPECT_EQ(UINT8_MAX, Heal::rtcConsecutive(Heal::rtcBeforeReboot(rec, Heal::Cause::StopUnproven)));
  EXPECT_EQ(0, Heal::rtcConsecutive(Heal::rtcCleared()));
}

TEST(MQTTSelfHealPolicy, AlertTextNamesCauseAndOutcome) {
  Heal::Config cfg = Heal::defaultConfig();
  Heal::Result r{};
  r.action = Heal::Action::Alert;
  r.cause = Heal::Cause::StopUnproven;
  r.held_ms = 10 * kMin;
  char buf[96];
  Heal::formatAlert(r, cfg, buf, sizeof(buf));
  EXPECT_STREQ("MQTT self-heal: stop unproven for 10m, rebooting in 1m", buf);

  cfg.alert_grace_ms = 15000;
  Heal::formatAlert(r, cfg, buf, sizeof(buf));
  EXPECT_STREQ("MQTT self-heal: stop unproven for 10m, rebooting in 15s", buf);
  r.held_ms = 45000;
  Heal::formatAlert(r, cfg, buf, sizeof(buf));
  EXPECT_STREQ("MQTT self-heal: stop unproven for 45s, rebooting in 15s", buf);
  r.held_ms = 10 * kMin;

  r.reboot_suppressed = true;
  Heal::formatAlert(r, cfg, buf, sizeof(buf));
  EXPECT_STREQ("MQTT self-heal: stop unproven for 10m, reboot limit reached", buf);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
