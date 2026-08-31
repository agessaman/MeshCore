#include <gtest/gtest.h>

#include <limits>

#include "helpers/ManualOtaSession.h"
#include "helpers/esp32/HttpPort80Lease.h"

TEST(ManualOtaSession, ScheduleIsDeferredAndIdempotent) {
  ManualOtaSession session;
  EXPECT_TRUE(session.schedule(1000, true));
  EXPECT_TRUE(session.isPending());
  EXPECT_TRUE(session.forceAp());
  EXPECT_FALSE(session.schedule(1001, false));
  EXPECT_FALSE(session.startDue(1000 + ManualOtaSession::kStartDelayMs - 1));
  EXPECT_TRUE(session.startDue(1000 + ManualOtaSession::kStartDelayMs));
}

TEST(ManualOtaSession, StartDelaySurvivesMillisRollover) {
  ManualOtaSession session;
  const uint32_t now = std::numeric_limits<uint32_t>::max() - 1000;
  ASSERT_TRUE(session.schedule(now, false));
  EXPECT_FALSE(session.startDue(now + ManualOtaSession::kStartDelayMs - 1));
  EXPECT_TRUE(session.startDue(now + ManualOtaSession::kStartDelayMs));
}

TEST(ManualOtaSession, ActiveSessionRemembersBridgeAndTimesOutOnlyWhenIdle) {
  ManualOtaSession session;
  ASSERT_TRUE(session.schedule(0, false));
  session.markActive(5000, true);
  EXPECT_TRUE(session.isActive());
  EXPECT_TRUE(session.bridgeWasRunning());
  EXPECT_FALSE(session.timeoutDue(5000 + ManualOtaSession::kSessionTimeoutMs - 1, false));
  EXPECT_FALSE(session.timeoutDue(5000 + ManualOtaSession::kSessionTimeoutMs, true));
  EXPECT_TRUE(session.timeoutDue(5000 + ManualOtaSession::kSessionTimeoutMs, false));
}

TEST(ManualOtaSession, SessionTimeoutSurvivesMillisRollover) {
  ManualOtaSession session;
  const uint32_t now = std::numeric_limits<uint32_t>::max() - 1000;
  ASSERT_TRUE(session.schedule(0, false));
  session.markActive(now, false);
  EXPECT_FALSE(session.timeoutDue(now + ManualOtaSession::kSessionTimeoutMs - 1, false));
  EXPECT_TRUE(session.timeoutDue(now + ManualOtaSession::kSessionTimeoutMs, false));
}

TEST(ManualOtaSession, ResetClearsAllSessionState) {
  ManualOtaSession session;
  ASSERT_TRUE(session.schedule(100, true));
  session.markActive(200, true);
  session.reset();
  EXPECT_TRUE(session.isIdle());
  EXPECT_FALSE(session.forceAp());
  EXPECT_FALSE(session.bridgeWasRunning());
  EXPECT_FALSE(session.timeoutDue(std::numeric_limits<uint32_t>::max(), false));
}

TEST(HttpPort80Lease, SerializesWebConfigAndOtaOwnership) {
  using namespace HttpPort80Lease;
  release(Owner::WebConfig);
  release(Owner::Ota);

  EXPECT_TRUE(acquire(Owner::WebConfig));
  EXPECT_STREQ("webconfig", ownerName());
  EXPECT_FALSE(acquire(Owner::Ota));
  release(Owner::Ota);  // wrong owner cannot release another service's lease
  EXPECT_STREQ("webconfig", ownerName());
  release(Owner::WebConfig);

  EXPECT_TRUE(acquire(Owner::Ota));
  EXPECT_STREQ("ota", ownerName());
  release(Owner::Ota);
  EXPECT_STREQ("none", ownerName());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
