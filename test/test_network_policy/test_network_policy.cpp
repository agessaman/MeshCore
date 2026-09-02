#include <gtest/gtest.h>

#include "helpers/NetworkPolicy.h"

TEST(NetworkPolicy, MqttDownDisconnectsSlotsWithoutRequestingImmediateRetry) {
  const auto actions = NetworkPolicy::mqttActions(NetworkTransition::Down);
  EXPECT_TRUE(actions.disconnect_slots);
  EXPECT_FALSE(actions.retry_disconnected_slots_now);
}

TEST(NetworkPolicy, MqttUpRequestsImmediateRetryWithoutDisconnectingSlots) {
  const auto actions = NetworkPolicy::mqttActions(NetworkTransition::Up);
  EXPECT_FALSE(actions.disconnect_slots);
  EXPECT_TRUE(actions.retry_disconnected_slots_now);
}

TEST(NetworkPolicy, NoTransitionHasNoMqttSideEffects) {
  const auto actions = NetworkPolicy::mqttActions(NetworkTransition::None);
  EXPECT_FALSE(actions.disconnect_slots);
  EXPECT_FALSE(actions.retry_disconnected_slots_now);
}

TEST(NetworkPolicy, LinkSwitchReconnectsMqttSlotsImmediately) {
  const auto actions = NetworkPolicy::mqttActions(NetworkTransition::Switched);
  EXPECT_TRUE(actions.disconnect_slots);
  EXPECT_TRUE(actions.retry_disconnected_slots_now);
}

TEST(NetworkPolicy, BootPrefersEthernetAndOtherwiseUsesConfiguredWifi) {
  EXPECT_EQ(NetworkMedium::Ethernet, NetworkPolicy::bootSelection(true, true));
  EXPECT_EQ(NetworkMedium::WiFi, NetworkPolicy::bootSelection(false, true));
  EXPECT_EQ(NetworkMedium::None, NetworkPolicy::bootSelection(false, false));
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

TEST(NetworkPolicy, SwitchingLockPinsTheCurrentMedium) {
  const NetworkPolicy::AutomaticSelectionInput input = {
      NetworkMedium::WiFi, true, true, true, true,
      NetworkPolicy::kEthernetFailbackStableMs, 0};
  EXPECT_EQ(NetworkMedium::WiFi, NetworkPolicy::automaticSelection(input));
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
