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
