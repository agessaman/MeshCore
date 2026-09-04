#include <gtest/gtest.h>

#include "helpers/NetworkPolicy.h"

TEST(NetworkPolicy, MqttDownDisconnectsSlotsWithoutRequestingImmediateRetry) {
  const auto actions = NetworkPolicy::mqttActions(NetworkTransition::Down);
  EXPECT_TRUE(actions.stop_started_slots);
  EXPECT_FALSE(actions.retry_disconnected_slots_now);
  EXPECT_FALSE(actions.reset_reconnect_backoff);
}

TEST(NetworkPolicy, MqttUpRetriesWithoutClearingBrokerCircuitBreaker) {
  const auto actions = NetworkPolicy::mqttActions(NetworkTransition::Up);
  EXPECT_FALSE(actions.stop_started_slots);
  EXPECT_TRUE(actions.retry_disconnected_slots_now);
  EXPECT_FALSE(actions.reset_reconnect_backoff);
}

TEST(NetworkPolicy, NoTransitionHasNoMqttSideEffects) {
  const auto actions = NetworkPolicy::mqttActions(NetworkTransition::None);
  EXPECT_FALSE(actions.stop_started_slots);
  EXPECT_FALSE(actions.retry_disconnected_slots_now);
  EXPECT_FALSE(actions.reset_reconnect_backoff);
}

TEST(NetworkPolicy, LinkSwitchReconnectsMqttSlotsImmediately) {
  const auto actions = NetworkPolicy::mqttActions(NetworkTransition::Switched);
  EXPECT_TRUE(actions.stop_started_slots);
  EXPECT_TRUE(actions.retry_disconnected_slots_now);
  EXPECT_TRUE(actions.reset_reconnect_backoff);
}

TEST(NetworkPolicy, MediumNamesAreStableForTransitionLogs) {
  EXPECT_STREQ("none", NetworkPolicy::mediumName(NetworkMedium::None));
  EXPECT_STREQ("ethernet", NetworkPolicy::mediumName(NetworkMedium::Ethernet));
  EXPECT_STREQ("wifi", NetworkPolicy::mediumName(NetworkMedium::WiFi));
}

TEST(NetworkPolicy, BootPrefersEthernetAndOtherwiseUsesConfiguredWifi) {
  EXPECT_EQ(NetworkMedium::Ethernet, NetworkPolicy::bootSelection(true, true));
  EXPECT_EQ(NetworkMedium::WiFi, NetworkPolicy::bootSelection(false, true));
  EXPECT_EQ(NetworkMedium::None, NetworkPolicy::bootSelection(false, false));
}

TEST(NetworkPolicy, EthernetBootProbeHonorsFullDeadlineUntilConnected) {
  EXPECT_TRUE(NetworkPolicy::ethernetBootProbePending(true, false, 0, 8000));
  EXPECT_TRUE(NetworkPolicy::ethernetBootProbePending(true, false, 1500, 8000));
  EXPECT_TRUE(NetworkPolicy::ethernetBootProbePending(true, false, 7999, 8000));
  EXPECT_FALSE(NetworkPolicy::ethernetBootProbePending(true, false, 8000, 8000));
  EXPECT_FALSE(NetworkPolicy::ethernetBootProbePending(true, true, 100, 8000));
  EXPECT_FALSE(NetworkPolicy::ethernetBootProbePending(false, false, 100, 8000));
}

TEST(NetworkPolicy, EthernetFailureWaitsForGraceAndConnectedWifi) {
  NetworkPolicy::AutomaticSelectionInput input = {
      NetworkMedium::Ethernet, false, true, true, false, 0,
      NetworkPolicy::kEthernetDownGraceMs - 1};
  EXPECT_EQ(NetworkMedium::Ethernet, NetworkPolicy::automaticSelection(input));
  input.selected_down_ms = NetworkPolicy::kEthernetDownGraceMs;
  EXPECT_EQ(NetworkMedium::WiFi, NetworkPolicy::automaticSelection(input));
}

TEST(NetworkPolicy, FailbackRequiresStableEthernet) {
  NetworkPolicy::AutomaticSelectionInput input = {
      NetworkMedium::WiFi, true, true, true, false,
      NetworkPolicy::kEthernetFailbackStableMs - 1, 0};
  EXPECT_EQ(NetworkMedium::WiFi, NetworkPolicy::automaticSelection(input));
  input.ethernet_stable_ms = NetworkPolicy::kEthernetFailbackStableMs;
  EXPECT_EQ(NetworkMedium::Ethernet, NetworkPolicy::automaticSelection(input));
}

TEST(NetworkPolicy, DeadWifiFailsBackToReadyEthernetImmediately) {
  const NetworkPolicy::AutomaticSelectionInput input = {
      NetworkMedium::WiFi, true, false, true, false, 0, 0};
  EXPECT_EQ(NetworkMedium::Ethernet, NetworkPolicy::automaticSelection(input));
}

TEST(NetworkPolicy, SwitchingLockPinsTheCurrentMedium) {
  const NetworkPolicy::AutomaticSelectionInput input = {
      NetworkMedium::WiFi, true, true, true, true,
      NetworkPolicy::kEthernetFailbackStableMs, 0};
  EXPECT_EQ(NetworkMedium::WiFi, NetworkPolicy::automaticSelection(input));
}

TEST(NetworkPolicy, DiagnosticsIdentifyEthernetFallbackCause) {
  using Reason = NetworkDiagnosticReason;
  EXPECT_EQ(Reason::EthernetInitFailed,
            NetworkPolicy::automaticDiagnosticReason(
                false, false, false, false, NetworkMedium::WiFi, false, 0));
  EXPECT_EQ(Reason::EthernetLinkUnknown,
            NetworkPolicy::automaticDiagnosticReason(
                true, false, false, false, NetworkMedium::WiFi, false, 0));
  EXPECT_EQ(Reason::EthernetLinkDown,
            NetworkPolicy::automaticDiagnosticReason(
                true, true, false, false, NetworkMedium::WiFi, false, 0));
  EXPECT_EQ(Reason::EthernetAwaitingIp,
            NetworkPolicy::automaticDiagnosticReason(
                true, true, true, false, NetworkMedium::WiFi, false, 0));
}

TEST(NetworkPolicy, DiagnosticsExplainWhyReadyEthernetHasNotBeenSelected) {
  using Reason = NetworkDiagnosticReason;
  EXPECT_EQ(Reason::SwitchingLocked,
            NetworkPolicy::automaticDiagnosticReason(
                true, true, true, true, NetworkMedium::WiFi, true,
                NetworkPolicy::kEthernetFailbackStableMs));
  EXPECT_EQ(Reason::EthernetStabilizing,
            NetworkPolicy::automaticDiagnosticReason(
                true, true, true, true, NetworkMedium::WiFi, false,
                NetworkPolicy::kEthernetFailbackStableMs - 1));
  EXPECT_EQ(Reason::EthernetReady,
            NetworkPolicy::automaticDiagnosticReason(
                true, true, true, true, NetworkMedium::WiFi, false,
                NetworkPolicy::kEthernetFailbackStableMs));
  EXPECT_EQ(Reason::EthernetActive,
            NetworkPolicy::automaticDiagnosticReason(
                true, true, true, true, NetworkMedium::Ethernet, false, 0));
}

TEST(NetworkPolicy, NtpIsScheduledOncePerConnectivityEdge) {
  EXPECT_TRUE(NetworkPolicy::ntpPendingAfterConnectivitySample(
      false, false, false, true));
  EXPECT_FALSE(NetworkPolicy::ntpPendingAfterConnectivitySample(
      false, false, true, true));
  EXPECT_TRUE(NetworkPolicy::ntpPendingAfterConnectivitySample(
      false, true, true, true));
  EXPECT_FALSE(NetworkPolicy::ntpPendingAfterConnectivitySample(
      true, false, false, true));
}

TEST(NetworkPolicy, FailedNtpSyncWaitsForThirtySecondRetryBoundary) {
  EXPECT_FALSE(NetworkPolicy::ntpRetryDue(false, true, 29999, 0));
  EXPECT_TRUE(NetworkPolicy::ntpRetryDue(false, true, 30000, 0));
  EXPECT_FALSE(NetworkPolicy::ntpRetryDue(true, true, 60000, 0));
  EXPECT_FALSE(NetworkPolicy::ntpRetryDue(false, false, 60000, 0));
  EXPECT_TRUE(NetworkPolicy::ntpRetryDue(false, true, 10, 0xffff8000u));
}

TEST(NetworkPolicy, StartOtaUsesReachableSelectedNetworkByDefault) {
  EXPECT_TRUE(NetworkPolicy::startOtaUsesSelectedNetwork(false, true));
  EXPECT_FALSE(NetworkPolicy::startOtaUsesSelectedNetwork(false, false));
}

TEST(NetworkPolicy, StartOtaForceApOverridesAReachableSelectedNetwork) {
  EXPECT_FALSE(NetworkPolicy::startOtaUsesSelectedNetwork(true, true));
  EXPECT_FALSE(NetworkPolicy::startOtaUsesSelectedNetwork(true, false));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
