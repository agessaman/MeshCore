// The order of checks before a poll, ported from the plugin's
// tests/test_community.py (poll_decisions.skip_reason). Freshness itself is
// covered in test_poll_decisions; these check what reaches it and what each
// skip leaves in the target state.

#include <gtest/gtest.h>

#include <string.h>

#include "helpers/poll/PollCycle.h"

namespace {

const uint32_t kNow = 1800000000;

poll::TargetState target() {
  poll::TargetState t;
  memset(&t, 0, sizeof(t));
  return t;
}

poll::CycleInput input() {
  poll::CycleInput in;
  memset(&in, 0, sizeof(in));
  in.gate = poll::GATE_OPEN;
  in.role = poll::ROLE_POLLER;
  in.rank = 1;
  in.has_url = true;
  in.window_s = 3600;
  in.max_requests = 2;
  in.grace_s = 1800;
  in.corescope_reachable = true;
  in.hold_from = kNow - 86400;
  in.hold_recheck_s = poll::kHoldRecheckS;
  return in;
}

}  // namespace

TEST(PollCycle, GateStopsScheduledAndPollNow) {
  for (uint8_t gate : {poll::GATE_BLOCKED, poll::GATE_VERSION, poll::GATE_PAUSED}) {
    for (bool test : {false, true}) {
      poll::TargetState t = target();
      poll::CycleInput in = input();
      in.gate = gate;
      in.test = test;
      const poll::Decision d = poll::decideCycle(t, in, kNow);
      EXPECT_FALSE(d.poll);
      EXPECT_EQ(poll::SKIP_GATE, d.reason);
      EXPECT_EQ(0U, t.next_due_at);
    }
  }
}

TEST(PollCycle, ExcludedRepeaterIsNeverSentTo) {
  for (bool test : {false, true}) {
    poll::TargetState t = target();
    poll::CycleInput in = input();
    in.excluded = true;
    in.test = test;
    const poll::Decision d = poll::decideCycle(t, in, kNow);
    EXPECT_FALSE(d.poll);
    EXPECT_EQ(poll::SKIP_EXCLUDED, d.reason);
  }
  // The owner's "excluded" stops a scheduled poll too.
  poll::TargetState t = target();
  poll::CycleInput in = input();
  in.role = poll::ROLE_EXCLUDED;
  EXPECT_EQ(poll::SKIP_EXCLUDED, poll::decideCycle(t, in, kNow).reason);
}

TEST(PollCycle, PollNowIgnoresFreshnessStandbyCapAndStartupHold) {
  poll::TargetState t = target();
  t.sends[1] = kNow - 300;
  t.sends[2] = kNow - 200;
  t.sends[3] = kNow - 100;
  poll::CycleInput in = input();
  in.test = true;
  in.role = poll::ROLE_STANDBY;
  in.starting = true;
  in.latest_ts = kNow - 60;
  const poll::Decision d = poll::decideCycle(t, in, kNow);
  EXPECT_TRUE(d.poll);
  EXPECT_EQ(0U, t.next_due_at);
  EXPECT_EQ(0U, t.standby_since);
  // The plugin's Test does not look at the owner's role either.
  in.role = poll::ROLE_PAUSED;
  EXPECT_TRUE(poll::decideCycle(t, in, kNow).poll);
}

TEST(PollCycle, RequestCapPerWindowSetsNextDue) {
  poll::TargetState t = target();
  t.sends[2] = kNow - 1000;
  t.sends[3] = kNow - 500;
  poll::CycleInput in = input();
  const poll::Decision d = poll::decideCycle(t, in, kNow);
  EXPECT_FALSE(d.poll);
  EXPECT_EQ(poll::SKIP_CAP, d.reason);
  EXPECT_EQ(kNow - 1000 + 3600, t.next_due_at);
  EXPECT_EQ(t.next_due_at, d.until);
  // A larger cap from the policy lets it through.
  poll::TargetState u = target();
  u.sends[2] = kNow - 1000;
  u.sends[3] = kNow - 500;
  in.max_requests = 3;
  EXPECT_TRUE(poll::decideCycle(u, in, kNow).poll);
}

TEST(PollCycle, CapComesBeforeTheOwnersRole) {
  poll::TargetState t = target();
  t.sends[2] = kNow - 1000;
  t.sends[3] = kNow - 500;
  poll::CycleInput in = input();
  in.role = poll::ROLE_PAUSED;
  EXPECT_EQ(poll::SKIP_CAP, poll::decideCycle(t, in, kNow).reason);
}

TEST(PollCycle, PausedOrDisabledRoleSkipsWithoutRescheduling) {
  for (uint8_t role : {poll::ROLE_PAUSED, poll::ROLE_DISABLED}) {
    poll::TargetState t = target();
    t.standby_since = kNow - 50;
    poll::CycleInput in = input();
    in.role = role;
    const poll::Decision d = poll::decideCycle(t, in, kNow);
    EXPECT_FALSE(d.poll);
    EXPECT_EQ(poll::SKIP_ROLE, d.reason);
    EXPECT_EQ(0U, t.next_due_at);
    EXPECT_EQ(kNow - 50, t.standby_since);   // the role check comes first
  }
}

TEST(PollCycle, StandbySinceFollowsTheRole) {
  poll::TargetState t = target();
  poll::CycleInput in = input();
  in.role = poll::ROLE_STANDBY;
  in.latest_ts = kNow - 60;
  poll::decideCycle(t, in, kNow);
  EXPECT_EQ(kNow, t.standby_since);
  poll::decideCycle(t, in, kNow + 100);
  EXPECT_EQ(kNow, t.standby_since);   // set once
  in.role = poll::ROLE_POLLER;
  poll::decideCycle(t, in, kNow + 200);
  EXPECT_EQ(0U, t.standby_since);
}

TEST(PollCycle, StartupHoldWaitsWithoutANextDue) {
  poll::TargetState t = target();
  poll::CycleInput in = input();
  in.starting = true;
  const poll::Decision d = poll::decideCycle(t, in, kNow);
  EXPECT_FALSE(d.poll);
  EXPECT_EQ(poll::SKIP_STARTUP_HOLD, d.reason);
  EXPECT_EQ(0U, t.next_due_at);
}

TEST(PollCycle, PollerSkipsAFreshReadingAndReschedules) {
  poll::TargetState t = target();
  poll::CycleInput in = input();
  in.latest_ts = kNow - 600;
  const poll::Decision d = poll::decideCycle(t, in, kNow);
  EXPECT_FALSE(d.poll);
  EXPECT_EQ(poll::SKIP_FRESH, d.reason);
  EXPECT_EQ(kNow - 600 + 3600, t.next_due_at);
}

TEST(PollCycle, PollerFailsOpenWhenCoreScopeIsUnreachable) {
  poll::TargetState t = target();
  poll::CycleInput in = input();
  in.latest_ts = kNow - 600;
  in.corescope_reachable = false;
  EXPECT_TRUE(poll::decideCycle(t, in, kNow).poll);
}

TEST(PollCycle, StandbyHoldsWhenCoreScopeIsUnreachable) {
  poll::TargetState t = target();
  poll::CycleInput in = input();
  in.role = poll::ROLE_STANDBY;
  in.corescope_reachable = false;
  const poll::Decision d = poll::decideCycle(t, in, kNow);
  EXPECT_FALSE(d.poll);
  EXPECT_EQ(poll::SKIP_STANDBY_HOLD, d.reason);
  EXPECT_EQ(kNow + 600, t.next_due_at);
}

TEST(PollCycle, StandbyWithNoReadingGivesThePollerAWindowFirst) {
  poll::TargetState t = target();
  poll::CycleInput in = input();
  in.role = poll::ROLE_STANDBY;
  const poll::Decision d = poll::decideCycle(t, in, kNow);
  EXPECT_FALSE(d.poll);
  EXPECT_EQ(poll::SKIP_STANDBY, d.reason);
  EXPECT_EQ(kNow + 3600 + 1800, t.next_due_at);
  // Window plus grace on standby and still nothing: step in.
  t.next_due_at = 0;
  EXPECT_TRUE(poll::decideCycle(t, in, kNow + 3600 + 1800).poll);
}

TEST(PollCycle, OwnersLatestReadingCountsForAStandby) {
  poll::TargetState t = target();
  t.standby_since = kNow - 10 * 3600;
  poll::CycleInput in = input();
  in.role = poll::ROLE_STANDBY;
  in.latest_ts = kNow - 600;
  EXPECT_FALSE(poll::decideCycle(t, in, kNow).poll);
  EXPECT_EQ(kNow - 600 + 3600 + 1800, t.next_due_at);
}

TEST(PollCycle, StandbysQueueSoOnlyTheFirstStepsIn) {
  poll::CycleInput in = input();
  in.role = poll::ROLE_STANDBY;
  in.latest_ts = kNow - 6000;   // past window + one grace, inside window + two
  poll::TargetState second = target();
  in.rank = 2;
  EXPECT_FALSE(poll::decideCycle(second, in, kNow).poll);
  EXPECT_EQ(kNow - 6000 + 3600 + 2 * 1800, second.next_due_at);
  poll::TargetState first = target();
  in.rank = 1;
  EXPECT_TRUE(poll::decideCycle(first, in, kNow).poll);
}

TEST(PollCycle, AStandbysWaitRestartsWhenAPauseLifts) {
  poll::TargetState t = target();
  poll::CycleInput in = input();
  in.role = poll::ROLE_STANDBY;
  in.latest_ts = kNow - 20 * 3600;
  in.hold_from = kNow - 60;
  EXPECT_FALSE(poll::decideCycle(t, in, kNow).poll);
  EXPECT_EQ(kNow - 60 + 3600 + 1800, t.next_due_at);
}

TEST(PollCycle, CommunityOffKeepsTheCapAndFreshness) {
  // Community off maps every owner entry to poller (test_poll_decisions); the
  // limits and the freshness check still apply.
  poll::TargetState t = target();
  poll::CycleInput in = input();
  in.latest_ts = kNow - 60;
  EXPECT_EQ(poll::SKIP_FRESH, poll::decideCycle(t, in, kNow).reason);
  poll::TargetState u = target();
  u.sends[2] = kNow - 10;
  u.sends[3] = kNow - 5;
  in.latest_ts = 0;
  EXPECT_EQ(poll::SKIP_CAP, poll::decideCycle(u, in, kNow).reason);
}

TEST(PollCycle, NoCoreScopeMeansPoll) {
  poll::TargetState t = target();
  poll::CycleInput in = input();
  in.has_url = false;
  in.role = poll::ROLE_STANDBY;
  in.latest_ts = kNow - 60;
  EXPECT_TRUE(poll::decideCycle(t, in, kNow).poll);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
