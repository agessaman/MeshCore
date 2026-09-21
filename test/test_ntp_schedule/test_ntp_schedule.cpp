#include "helpers/NtpSchedule.h"
#include <gtest/gtest.h>

TEST(NtpSchedule, CompletionDoesNotRefreshAgainstThePreProbeSample) {
  NtpSchedule s;
  ASSERT_TRUE(s.begin(1000));
  s.complete(2200, NtpSchedule::Result::NetworkTime);
  EXPECT_FALSE(s.due(1000));
  EXPECT_FALSE(s.due(2200));
  EXPECT_FALSE(s.due(3602199));
  EXPECT_TRUE(s.due(3602200));
}

TEST(NtpSchedule, NoClockFailureAfterAnHourStillBacksOff) {
  NtpSchedule s;
  uint32_t now = 3600001;
  const uint32_t delays[] = {30000, 60000, 120000, 240000, 300000, 300000};
  for (uint32_t delay : delays) {
    ASSERT_TRUE(s.begin(now));
    now += 6000;
    s.complete(now, NtpSchedule::Result::Failed);
    EXPECT_FALSE(s.due(now + delay - 1));
    now += delay;
    EXPECT_TRUE(s.due(now));
  }
  EXPECT_FALSE(s.hasNetworkTime());
}

TEST(NtpSchedule, ForcedRequestSharesDeadlineAndCannotOverlap) {
  NtpSchedule s;
  ASSERT_TRUE(s.begin(0));
  EXPECT_FALSE(s.begin(1, true));
  s.complete(100, NtpSchedule::Result::NetworkTime);
  EXPECT_FALSE(s.begin(200));
  ASSERT_TRUE(s.begin(200, true));
  s.complete(500, NtpSchedule::Result::Failed);
  EXPECT_FALSE(s.due(501));
  EXPECT_TRUE(s.due(30500));
  EXPECT_EQ(100u, s.lastSuccessMs());
}

TEST(NtpSchedule, HoldoverDoesNotClaimANetworkSuccess) {
  NtpSchedule s;
  ASSERT_TRUE(s.begin(0));
  s.complete(0, NtpSchedule::Result::Holdover);
  EXPECT_FALSE(s.hasNetworkTime());
  EXPECT_EQ(NtpSchedule::Result::Holdover, s.result());
  EXPECT_FALSE(s.due(299999));
  EXPECT_TRUE(s.due(300000));
}

TEST(NtpSchedule, DueAttemptRemainsDueDuringALongNetworkOutage) {
  NtpSchedule s;
  ASSERT_TRUE(s.begin(0));
  s.complete(0, NtpSchedule::Result::Failed);
  EXPECT_TRUE(s.due(30000));
  EXPECT_TRUE(s.due(0x80010000u));
  EXPECT_TRUE(s.begin(0x80010000u));
}

TEST(NtpSchedule, NetworkSuccessAtZeroAndWrappedDeadlineAreValid) {
  NtpSchedule s;
  ASSERT_TRUE(s.begin(0xfffffff0u));
  s.complete(0, NtpSchedule::Result::NetworkTime);
  EXPECT_TRUE(s.hasNetworkTime());
  EXPECT_EQ(0u, s.lastSuccessMs());
  EXPECT_FALSE(s.due(1));
  ASSERT_TRUE(s.begin(0xfffffff0u, true));
  s.complete(0xfffffff0u, NtpSchedule::Result::Failed);
  EXPECT_FALSE(s.due(0));
  EXPECT_FALSE(s.due(29983));
  EXPECT_TRUE(s.due(29984));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
