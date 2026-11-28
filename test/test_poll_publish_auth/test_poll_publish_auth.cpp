// Authorization of a poll publish after the slot's broker may have changed.
#include <gtest/gtest.h>

#include "helpers/poll/PollPublishAuth.h"

TEST(PollPublishAuth, MatchingHostSends) {
  EXPECT_EQ(poll::PUBLISH_SEND,
            poll::authorizePublish(false, 3, 3, "broker.example.com", "broker.example.com"));
  EXPECT_EQ(poll::PUBLISH_SEND,
            poll::authorizePublish(false, 3, 3, "Broker.Example.COM", "broker.example.com"));
}

TEST(PollPublishAuth, ReconfiguredHostIsRefused) {
  EXPECT_EQ(poll::PUBLISH_WRONG_HOST,
            poll::authorizePublish(false, 4, 5, "broker-a.example", "broker-b.example"));
  EXPECT_EQ(poll::PUBLISH_WRONG_HOST,
            poll::authorizePublish(false, 4, 4, "broker-a.example", "broker-a.example.net"));
  EXPECT_EQ(poll::PUBLISH_WRONG_HOST, poll::authorizePublish(false, 1, 2, "", "broker-a.example"));
  EXPECT_EQ(poll::PUBLISH_WRONG_HOST, poll::authorizePublish(false, 1, 2, nullptr, "broker-a.example"));
}

TEST(PollPublishAuth, ExplicitSlotIsNeverHostChecked) {
  EXPECT_EQ(poll::PUBLISH_SEND,
            poll::authorizePublish(true, 1, 9, "broker-a.example", "broker-b.example"));
  EXPECT_EQ(poll::PUBLISH_SEND, poll::authorizePublish(true, 0, 0, nullptr, nullptr));
  EXPECT_EQ(poll::PUBLISH_SEND, poll::authorizePublish(true, 2, 2, "", ""));
}

// Documented choice: the authorized broker is the host, so a rewrite that keeps it still sends.
TEST(PollPublishAuth, GenerationChangeWithTheSameHostStillSends) {
  EXPECT_EQ(poll::PUBLISH_SEND,
            poll::authorizePublish(false, 1, 2, "mqtt-us-v1.letsmesh.net", "mqtt-us-v1.letsmesh.net"));
  EXPECT_EQ(poll::PUBLISH_SEND,
            poll::authorizePublish(false, 0, 1, "MQTT.Example.NET", "mqtt.example.net"));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
