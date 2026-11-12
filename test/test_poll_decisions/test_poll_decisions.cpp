#include <gtest/gtest.h>

#include <stdint.h>
#include <string.h>

#include "helpers/poll/PollDecisions.h"

namespace {

poll::OwnerEntry entry(uint8_t role, bool mine, bool excluded = false) {
  poll::OwnerEntry e;
  memset(&e, 0, sizeof(e));
  e.present = true;
  e.role = role;
  e.mine = mine;
  e.excluded = excluded;
  e.standby_rank = 1;
  return e;
}

poll::FreshnessInput fresh(uint8_t role) {
  poll::FreshnessInput in;
  memset(&in, 0, sizeof(in));
  in.role = role;
  in.rank = 1;
  in.window_s = 3600;
  in.grace_s = 1800;
  in.corescope_reachable = true;
  in.hold_recheck_s = poll::kHoldRecheckS;
  return in;
}

}  // namespace

TEST(PollDecisions, RoleTextAndMappingFollowTheOwnersReply) {
  EXPECT_EQ(poll::OWNER_ROLE_ABSENT, poll::ownerRoleFromText(nullptr));
  EXPECT_EQ(poll::OWNER_ROLE_ABSENT, poll::ownerRoleFromText(""));
  EXPECT_EQ(poll::OWNER_ROLE_POLLING, poll::ownerRoleFromText("polling"));
  EXPECT_EQ(poll::OWNER_ROLE_INDEPENDENT, poll::ownerRoleFromText("independent"));
  EXPECT_EQ(poll::OWNER_ROLE_STANDBY, poll::ownerRoleFromText("standby"));
  EXPECT_EQ(poll::OWNER_ROLE_EXCLUDED, poll::ownerRoleFromText("excluded"));
  EXPECT_EQ(poll::OWNER_ROLE_PAUSED, poll::ownerRoleFromText("paused"));
  EXPECT_EQ(poll::OWNER_ROLE_DISABLED, poll::ownerRoleFromText("disabled"));
  EXPECT_EQ(poll::OWNER_ROLE_UNKNOWN, poll::ownerRoleFromText("poller"));
  EXPECT_EQ(poll::OWNER_ROLE_UNKNOWN, poll::ownerRoleFromText("Standby"));

  const poll::OwnerEntry standby = entry(poll::OWNER_ROLE_STANDBY, false);
  EXPECT_EQ(poll::ROLE_POLLER, poll::mapRole(false, standby));

  poll::OwnerEntry missing = standby;
  missing.present = false;
  EXPECT_EQ(poll::ROLE_POLLER, poll::mapRole(true, missing));

  EXPECT_EQ(poll::ROLE_EXCLUDED, poll::mapRole(true, entry(poll::OWNER_ROLE_EXCLUDED, true)));
  EXPECT_EQ(poll::ROLE_EXCLUDED,
            poll::mapRole(true, entry(poll::OWNER_ROLE_POLLING, true, true)));
  EXPECT_EQ(poll::ROLE_PAUSED, poll::mapRole(true, entry(poll::OWNER_ROLE_PAUSED, false)));
  EXPECT_EQ(poll::ROLE_DISABLED, poll::mapRole(true, entry(poll::OWNER_ROLE_DISABLED, true)));
  EXPECT_EQ(poll::ROLE_POLLER, poll::mapRole(true, entry(poll::OWNER_ROLE_POLLING, false)));
  EXPECT_EQ(poll::ROLE_POLLER, poll::mapRole(true, entry(poll::OWNER_ROLE_INDEPENDENT, false)));

  // Older CoreScope: no role. Not mine means standby; mine means poller.
  EXPECT_EQ(poll::ROLE_STANDBY, poll::mapRole(true, entry(poll::OWNER_ROLE_ABSENT, false)));
  EXPECT_EQ(poll::ROLE_POLLER, poll::mapRole(true, entry(poll::OWNER_ROLE_ABSENT, true)));
  EXPECT_EQ(poll::ROLE_STANDBY, poll::mapRole(true, entry(poll::OWNER_ROLE_UNKNOWN, false)));
  EXPECT_EQ(poll::ROLE_POLLER, poll::mapRole(true, entry(poll::OWNER_ROLE_UNKNOWN, true)));
  EXPECT_EQ(poll::ROLE_STANDBY, poll::mapRole(true, entry(poll::OWNER_ROLE_STANDBY, false)));
  // A standby flag on our own node is still ours to poll.
  EXPECT_EQ(poll::ROLE_POLLER, poll::mapRole(true, entry(poll::OWNER_ROLE_STANDBY, true)));

  EXPECT_STREQ("standby", poll::roleName(poll::ROLE_STANDBY));
  EXPECT_STREQ("", poll::skipReasonName(poll::SKIP_NONE));
  EXPECT_STREQ("gate", poll::skipReasonName(poll::SKIP_GATE));
  EXPECT_STREQ("excluded", poll::skipReasonName(poll::SKIP_EXCLUDED));
  EXPECT_STREQ("cap", poll::skipReasonName(poll::SKIP_CAP));
  EXPECT_STREQ("fresh", poll::skipReasonName(poll::SKIP_FRESH));
  EXPECT_STREQ("standby", poll::skipReasonName(poll::SKIP_STANDBY));
  EXPECT_STREQ("standby_hold", poll::skipReasonName(poll::SKIP_STANDBY_HOLD));
  EXPECT_STREQ("starting", poll::skipReasonName(poll::SKIP_STARTUP_HOLD));
  EXPECT_STREQ("role", poll::skipReasonName(poll::SKIP_ROLE));
}

TEST(PollDecisions, PollerSkipsAFreshReadingAndFailsOpenWhenUnreachable) {
  const uint32_t now = 1'000'000;
  poll::FreshnessInput in = fresh(poll::ROLE_POLLER);
  in.latest_ts = now - 600;
  poll::Decision d = poll::decideFreshness(in, now);
  EXPECT_FALSE(d.poll);
  EXPECT_EQ(poll::SKIP_FRESH, d.reason);
  EXPECT_EQ(in.latest_ts + 3600U, d.until);

  in.latest_ts = now - 3599;
  d = poll::decideFreshness(in, now);
  EXPECT_FALSE(d.poll);
  EXPECT_EQ(in.latest_ts + 3600U, d.until);

  in.latest_ts = now - 3600;
  d = poll::decideFreshness(in, now);
  EXPECT_TRUE(d.poll);
  EXPECT_EQ(poll::SKIP_NONE, d.reason);
  EXPECT_EQ(0U, d.until);

  in.latest_ts = 0;
  d = poll::decideFreshness(in, now);
  EXPECT_TRUE(d.poll);

  // Ahead of the clock: still inside the window, not a wrapped age.
  in.latest_ts = now + 5;
  d = poll::decideFreshness(in, now);
  EXPECT_FALSE(d.poll);
  EXPECT_EQ(poll::SKIP_FRESH, d.reason);

  in.corescope_reachable = false;
  in.latest_ts = now - 10;
  d = poll::decideFreshness(in, now);
  EXPECT_TRUE(d.poll);
  EXPECT_EQ(0U, d.until);
}

TEST(PollDecisions, StandbyWaitsWindowPlusGraceTimesRankThenStepsIn) {
  const uint32_t now = 1'000'000;
  poll::FreshnessInput in = fresh(poll::ROLE_STANDBY);
  in.window_s = 7200;
  in.rank = 1;
  // 2 h 10 min old: past the one-hop window, inside window + grace.
  in.latest_ts = now - 7800;
  poll::Decision d = poll::decideFreshness(in, now);
  EXPECT_FALSE(d.poll);
  EXPECT_EQ(poll::SKIP_STANDBY, d.reason);
  EXPECT_EQ(in.latest_ts + 7200U + 1800U, d.until);

  in.latest_ts = now - 9600;
  d = poll::decideFreshness(in, now);
  EXPECT_TRUE(d.poll);
  EXPECT_EQ(poll::SKIP_NONE, d.reason);

  in.window_s = 3600;
  in.rank = 2;
  in.latest_ts = now - 6000;
  d = poll::decideFreshness(in, now);
  EXPECT_FALSE(d.poll);
  EXPECT_EQ(in.latest_ts + 3600U + 2U * 1800U, d.until);

  in.rank = 1;
  d = poll::decideFreshness(in, now);
  EXPECT_TRUE(d.poll);

  in.rank = 0;  // missing rank still queues one grace behind the poller
  in.latest_ts = now - 3600;
  d = poll::decideFreshness(in, now);
  EXPECT_FALSE(d.poll);
  EXPECT_EQ(in.latest_ts + 3600U + 1800U, d.until);
}

TEST(PollDecisions, StandbyWithNoReadingGivesThePollerTheFullWait) {
  const uint32_t now = 1'000'000;
  poll::FreshnessInput in = fresh(poll::ROLE_STANDBY);
  in.latest_ts = 0;
  in.standby_since = now;
  in.hold_from = now - 10'000;
  poll::Decision d = poll::decideFreshness(in, now);
  EXPECT_FALSE(d.poll);
  EXPECT_EQ(poll::SKIP_STANDBY, d.reason);
  EXPECT_EQ(now + 3600U + 1800U, d.until);

  in.standby_since = now - (3600U + 1800U);
  d = poll::decideFreshness(in, now);
  EXPECT_TRUE(d.poll);

  in.standby_since = now - (3600U + 1800U) + 1;
  d = poll::decideFreshness(in, now);
  EXPECT_FALSE(d.poll);

  // Lost standby_since after a reboot: the start time is the anchor.
  in.standby_since = 0;
  in.hold_from = now;
  d = poll::decideFreshness(in, now);
  EXPECT_FALSE(d.poll);
  EXPECT_EQ(now + 3600U + 1800U, d.until);
}

TEST(PollDecisions, StandbyWaitRestartsWhenThePauseLifts) {
  const uint32_t now = 1'000'000;
  poll::FreshnessInput in = fresh(poll::ROLE_STANDBY);
  in.latest_ts = now - 20U * 3600U;
  in.hold_from = now - 60;
  in.standby_since = now - 24U * 3600U;
  const poll::Decision d = poll::decideFreshness(in, now);
  EXPECT_FALSE(d.poll);
  EXPECT_EQ(poll::SKIP_STANDBY, d.reason);
  EXPECT_EQ(in.hold_from + 3600U + 1800U, d.until);

  // An older hold does not pull a newer reading backward.
  in.hold_from = now - 100'000;
  in.latest_ts = now - 600;
  const poll::Decision kept = poll::decideFreshness(in, now);
  EXPECT_FALSE(kept.poll);
  EXPECT_EQ(in.latest_ts + 3600U + 1800U, kept.until);
}

TEST(PollDecisions, StandbyHoldsWhileCoreScopeIsUnreachable) {
  const uint32_t now = 1'000'000;
  poll::FreshnessInput in = fresh(poll::ROLE_STANDBY);
  in.corescope_reachable = false;
  in.latest_ts = now - 10;  // not trusted while the owners reply is down
  const poll::Decision d = poll::decideFreshness(in, now);
  EXPECT_FALSE(d.poll);
  EXPECT_EQ(poll::SKIP_STANDBY_HOLD, d.reason);
  EXPECT_EQ(now + poll::kHoldRecheckS, d.until);

  const poll::Decision again = poll::decideFreshness(in, d.until);
  EXPECT_FALSE(again.poll);
  EXPECT_EQ(d.until + poll::kHoldRecheckS, again.until);
}

TEST(PollDecisions, OwnersLapseAfterThirtyMinutesAndFailOpenIfNeverAnswered) {
  const uint32_t ok = 50'000;
  EXPECT_FALSE(poll::ownersLapsed(ok, ok));
  EXPECT_FALSE(poll::ownersLapsed(ok, ok + 1800U));
  EXPECT_TRUE(poll::ownersLapsed(ok, ok + 1801U));
  EXPECT_TRUE(poll::ownersLapsed(0, 100));
  EXPECT_TRUE(poll::ownersLapsed(0, 0));
  EXPECT_FALSE(poll::ownersLapsed(ok + 10, ok));
  EXPECT_FALSE(poll::ownersLapsed(ok, ok + 10, 30));
  EXPECT_TRUE(poll::ownersLapsed(ok, ok + 31, 30));
}

TEST(PollDecisions, StartupHoldWaitsForAClaimAndAnOwnersAnswer) {
  const uint32_t started = 10'000;
  EXPECT_FALSE(poll::startupHold(false, true, started, 0, 0, started));
  EXPECT_FALSE(poll::startupHold(true, false, started, 0, 0, started));
  EXPECT_TRUE(poll::startupHold(true, true, started, 0, 0, started));

  // Owners answered, but no claim has gone out yet.
  EXPECT_TRUE(poll::startupHold(true, true, started, 0, started + 10, started + 10));

  // Answered before the claim: still waiting.
  EXPECT_TRUE(poll::startupHold(true, true, started, started + 100, started + 50, started + 100));
  // The same second as the claim is not "after it".
  EXPECT_TRUE(poll::startupHold(true, true, started, started + 100, started + 100, started + 100));
  EXPECT_FALSE(poll::startupHold(true, true, started, started + 100, started + 101, started + 101));

  EXPECT_TRUE(poll::startupHold(true, true, started, 0, 0, started + poll::kStartupHoldS - 1));
  EXPECT_FALSE(poll::startupHold(true, true, started, 0, 0, started + poll::kStartupHoldS));
  // A claim that never gets an answer still runs out.
  EXPECT_FALSE(poll::startupHold(true, true, started, started + 1, 0, started + poll::kStartupHoldS));

  // Clock behind the start stamp: the hold has not expired.
  EXPECT_TRUE(poll::startupHold(true, true, started, 0, 0, started - 1));
  EXPECT_FALSE(poll::startupHold(true, true, 0, 0, 0, poll::kStartupHoldS));
  EXPECT_TRUE(poll::startupHold(true, true, 0, 0, 0, poll::kStartupHoldS - 1));
}

TEST(PollDecisions, FreshnessNearTheTopOfTheEpochDoesNotWrap) {
  const uint32_t latest = UINT32_MAX - 10U;
  const uint32_t now = UINT32_MAX - 5U;
  poll::FreshnessInput in = fresh(poll::ROLE_POLLER);
  in.latest_ts = latest;
  in.window_s = 100;
  const poll::Decision d = poll::decideFreshness(in, now);
  EXPECT_FALSE(d.poll);
  EXPECT_EQ(poll::SKIP_FRESH, d.reason);
  EXPECT_EQ(UINT32_MAX, d.until);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
