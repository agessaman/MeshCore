#include "helpers/MQTTClientState.h"

#include <gtest/gtest.h>

#include <string.h>
#include <vector>

namespace {

const MqttClientState kAllStates[] = {
    MqttClientState::Absent,       MqttClientState::Configured,
    MqttClientState::Starting,     MqttClientState::Connected,
    MqttClientState::Disconnected, MqttClientState::Stopped,
    MqttClientState::Quarantined,
};

bool ack(std::vector<MqttClientState> states) {
  return mqttStopMayBeAcknowledged(states.data(), (int)states.size());
}

} // namespace

TEST(MqttClientState, LiveMeansStartedAndNotProvenStopped) {
  EXPECT_TRUE(mqttClientStateIsLive(MqttClientState::Starting));
  EXPECT_TRUE(mqttClientStateIsLive(MqttClientState::Connected));
  EXPECT_TRUE(mqttClientStateIsLive(MqttClientState::Disconnected));

  EXPECT_FALSE(mqttClientStateIsLive(MqttClientState::Absent));
  EXPECT_FALSE(mqttClientStateIsLive(MqttClientState::Configured));
  EXPECT_FALSE(mqttClientStateIsLive(MqttClientState::Stopped));
  // Quarantined is not "live": it is worse. It may own a task and we can never
  // find out, which is why it is not simply lumped in with the live states.
  EXPECT_FALSE(mqttClientStateIsLive(MqttClientState::Quarantined));
}

TEST(MqttClientState, OnlyStartingHasAnAttemptInFlight) {
  for (MqttClientState s : kAllStates) {
    EXPECT_EQ(s == MqttClientState::Starting, mqttClientStateHasAttemptInFlight(s))
        << mqttClientStateName(s);
  }
}

TEST(MqttClientState, ProvenStoppedIsTheComplementOfLiveAndQuarantined) {
  for (MqttClientState s : kAllStates) {
    const bool proven = mqttClientStateIsProvenStopped(s);
    EXPECT_EQ(!mqttClientStateIsLive(s) && s != MqttClientState::Quarantined, proven)
        << mqttClientStateName(s);
  }
}

// The invariant the shutdown contract rests on: the acknowledgement means the
// task destroyed its clients, so it may not be published while any client's
// stop is unproven.
TEST(MqttClientState, StopIsAcknowledgedOnlyWhenEveryClientIsProvenStopped) {
  EXPECT_TRUE(ack({}));   // no slots configured at all
  EXPECT_TRUE(ack({MqttClientState::Absent, MqttClientState::Stopped,
                   MqttClientState::Configured}));

  // One quarantined client withholds the acknowledgement, whatever the others did.
  EXPECT_FALSE(ack({MqttClientState::Quarantined}));
  EXPECT_FALSE(ack({MqttClientState::Stopped, MqttClientState::Stopped,
                    MqttClientState::Quarantined, MqttClientState::Absent}));
  EXPECT_FALSE(ack({MqttClientState::Quarantined, MqttClientState::Absent,
                    MqttClientState::Absent, MqttClientState::Absent,
                    MqttClientState::Absent, MqttClientState::Absent}));

  // So does a client that is merely still live: teardown did not finish it.
  EXPECT_FALSE(ack({MqttClientState::Stopped, MqttClientState::Connected}));
  EXPECT_FALSE(ack({MqttClientState::Starting}));
  EXPECT_FALSE(ack({MqttClientState::Disconnected}));

  // An empty set is trivially proven; a null array with a nonzero count is a
  // caller bug and must never read as proof.
  EXPECT_TRUE(mqttStopMayBeAcknowledged(nullptr, 0));
  EXPECT_FALSE(mqttStopMayBeAcknowledged(nullptr, 3));
}

TEST(MqttClientState, EveryStateHasAName) {
  for (MqttClientState s : kAllStates) {
    ASSERT_NE(nullptr, mqttClientStateName(s));
    EXPECT_GT(strlen(mqttClientStateName(s)), 0u);
  }
  EXPECT_STREQ("quarantined", mqttClientStateName(MqttClientState::Quarantined));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
