#include <gtest/gtest.h>

#include <stdint.h>

#include "helpers/poll/PollSchedule.h"

namespace {

constexpr uint32_t kHour = 3600;
constexpr uint32_t kDay = poll::kDay;

poll::TargetState blank() { return poll::TargetState{}; }

// The plugin records the attempt when it starts and the result when it ends.
void pollStarted(poll::TargetState& t, uint8_t code, uint32_t now, bool quick = false) {
  poll::recordSend(t, now);
  poll::recordResult(t, code, now, quick);
}

}  // namespace

// -- spreading --------------------------------------------------------------

TEST(PollSchedule, SpreadPhaseSpacesTargetsAcrossTheInterval) {
  EXPECT_EQ(0U, poll::spreadPhase(7200, 0, 4));
  EXPECT_EQ(1800U, poll::spreadPhase(7200, 1, 4));
  EXPECT_EQ(3600U, poll::spreadPhase(7200, 2, 4));
  EXPECT_EQ(5400U, poll::spreadPhase(7200, 3, 4));

  EXPECT_EQ(0U, poll::spreadPhase(7200, 0, 3));
  EXPECT_EQ(2400U, poll::spreadPhase(7200, 1, 3));
  EXPECT_EQ(4800U, poll::spreadPhase(7200, 2, 3));
}

TEST(PollSchedule, SpreadPhaseHandlesDegenerateCounts) {
  EXPECT_EQ(0U, poll::spreadPhase(7200, 0, 1));
  EXPECT_EQ(0U, poll::spreadPhase(7200, 0, 0));
  EXPECT_EQ(0U, poll::spreadPhase(7200, 3, 3));  // index past the group
  EXPECT_EQ(0U, poll::spreadPhase(0, 2, 4));
}

TEST(PollSchedule, SpreadPhaseDoesNotOverflow) {
  EXPECT_EQ(2000000000U, poll::spreadPhase(4000000000U, 1, 2));
  EXPECT_EQ(2666666666U, poll::spreadPhase(4000000000U, 2, 3));
}

// -- slotDue ----------------------------------------------------------------

TEST(PollSchedule, SlotDueWithoutPhaseUsesTheInterval) {
  const uint32_t every = 3600;
  EXPECT_FALSE(poll::slotDue(0, every - 1, every, 0, false));
  EXPECT_TRUE(poll::slotDue(0, every, every, 0, false));
  EXPECT_FALSE(poll::slotDue(1000, 1000 + every - 1, every, 0, false));
  EXPECT_TRUE(poll::slotDue(1000, 1000 + every, every, 0, false));
  // An unset stamp counts from the epoch, as the plugin's `or 0` does.
  EXPECT_FALSE(poll::slotDue(0, 1000, every, 0, false));
}

TEST(PollSchedule, SlotDueWithPhaseWaitsForTheSlot) {
  const uint32_t every = 3600;
  const uint32_t phase = 600;
  // Just before the slot, even long after the last poll: not due.
  EXPECT_FALSE(poll::slotDue(0, 600 - 1, every, phase, true));
  // At the first slot only 600 s have passed since the epoch, under half.
  EXPECT_FALSE(poll::slotDue(0, 600, every, phase, true));
  // At the next slot the half-period guard is satisfied.
  EXPECT_TRUE(poll::slotDue(0, 600 + every, every, phase, true));
  // Just polled in this period: not due.
  EXPECT_FALSE(poll::slotDue(2400, 3000, every, phase, true));
  // Same slot as last time: wait for the next one.
  EXPECT_FALSE(poll::slotDue(600, 3000, every, phase, true));
  EXPECT_TRUE(poll::slotDue(600, 4200, every, phase, true));
}

TEST(PollSchedule, SlotDueRequiresHalfAPeriodSinceTheLastPoll) {
  const uint32_t every = 3600;
  const uint32_t slot = 7200;
  const uint32_t last = slot - 1;  // a test one second before the slot
  EXPECT_FALSE(poll::slotDue(last, slot + 1798, every, 0, true));
  EXPECT_TRUE(poll::slotDue(last, slot + 1799, every, 0, true));
  // A poll exactly at the slot is not repeated immediately.
  EXPECT_FALSE(poll::slotDue(slot, slot, every, 0, true));
}

TEST(PollSchedule, SlotDueRoundsHalfAPeriodUpForOddIntervals) {
  const uint32_t every = 3601;
  const uint32_t slot = 7202;
  const uint32_t last = slot - 1;
  EXPECT_FALSE(poll::slotDue(last, slot + 1799, every, 0, true));  // 1800 s
  EXPECT_TRUE(poll::slotDue(last, slot + 1800, every, 0, true));   // 1801 s
}

TEST(PollSchedule, SlotDueIsQuietBeforeTheFirstPhase) {
  // now < phase: the latest slot is negative, so nothing is due yet.
  EXPECT_FALSE(poll::slotDue(0, 100, 3600, 5000, true));
  EXPECT_TRUE(poll::slotDue(0, 5000, 3600, 5000, true));
}

TEST(PollSchedule, SlotDueTreatsAZeroPeriodAsDue) {
  EXPECT_TRUE(poll::slotDue(0, 0, 0, 0, false));
  EXPECT_TRUE(poll::slotDue(500, 500, 0, 0, true));
}

TEST(PollSchedule, SlotDueIgnoresAClockThatSteppedBack) {
  EXPECT_FALSE(poll::slotDue(2000, 1000, 3600, 0, false));
  EXPECT_FALSE(poll::slotDue(2000, 1000, 3600, 0, true));
}

// -- isDue ------------------------------------------------------------------

TEST(PollSchedule, IsDueIsFalseForAnAdminTarget) {
  poll::TargetState t = blank();
  t.admin_seen = true;
  t.last_attempt = 0;
  EXPECT_FALSE(poll::isDue(t, kHour, 10 * kHour, 0));
}

TEST(PollSchedule, IsDueHonorsNextDueAt) {
  poll::TargetState t = blank();
  t.last_attempt = 0;
  t.next_due_at = 5000;
  EXPECT_FALSE(poll::isDue(t, kHour, 4999, 0));
  EXPECT_TRUE(poll::isDue(t, kHour, 5000, 0));  // held until, not past
  EXPECT_TRUE(poll::isDue(t, kHour, 5001, 0));
}

TEST(PollSchedule, IsDuePollsANeverPolledTarget) {
  poll::TargetState t = blank();
  EXPECT_TRUE(poll::isDue(t, kHour, 1000000, 0));
}

TEST(PollSchedule, IsDueDoesNotPollAfterAClockStepBack) {
  poll::TargetState t = blank();
  t.last_attempt = 2000;
  EXPECT_FALSE(poll::isDue(t, kHour, 1000, 0));

  t.code = poll::CODE_NO_REPLY;
  t.no_reply_count = 1;
  t.last_success = 500;
  EXPECT_FALSE(poll::isDue(t, kHour, 1000, 0));
}

TEST(PollSchedule, IsDueQuickRetriesOnlyForRecentlyHeardTargets) {
  const uint32_t now = 2000000;
  poll::TargetState heard = blank();
  heard.code = poll::CODE_NO_REPLY;
  heard.no_reply_count = 1;
  heard.last_attempt = now - 1500;
  heard.last_success = now - kHour;
  EXPECT_TRUE(poll::isDue(heard, kHour, now, 0));

  poll::TargetState silent = blank();
  silent.code = poll::CODE_NO_REPLY;
  silent.no_reply_count = 1;
  silent.last_attempt = now - 1500;
  EXPECT_FALSE(poll::isDue(silent, kHour, now, 0));
}

TEST(PollSchedule, IsDueQuickRetryHasAnExactBoundary) {
  const uint32_t now = 2000000;
  poll::TargetState t = blank();
  t.code = poll::CODE_NO_REPLY;
  t.no_reply_count = 1;
  t.last_success = now - kHour;

  t.last_attempt = now - (20 * 60) + 1;
  EXPECT_FALSE(poll::isDue(t, kHour, now, 0));
  t.last_attempt = now - (20 * 60);
  EXPECT_TRUE(poll::isDue(t, kHour, now, 0));
}

TEST(PollSchedule, IsDueQuickRetryStopsAtTheDayOldSuccess) {
  const uint32_t now = 2000000;
  poll::TargetState t = blank();
  t.code = poll::CODE_NO_REPLY;
  t.no_reply_count = 1;
  t.last_attempt = now - 1500;  // would be a quick retry if it were recent

  t.last_success = now - kDay + 1;
  EXPECT_TRUE(poll::isDue(t, kHour, now, 0));
  t.last_success = now - kDay;
  EXPECT_FALSE(poll::isDue(t, kHour, now, 0));
}

TEST(PollSchedule, IsDueQuickRetryStopsAfterTwoMisses) {
  const uint32_t now = 2000000;
  poll::TargetState t = blank();
  t.code = poll::CODE_NO_REPLY;
  t.no_reply_count = 2;
  t.last_attempt = now - 1500;
  t.last_success = now - kHour;
  EXPECT_FALSE(poll::isDue(t, kHour, now, 0));
}

TEST(PollSchedule, IsDueQuickRetryUsesTheShorterInterval) {
  const uint32_t now = 2000000;
  poll::TargetState t = blank();
  t.code = poll::CODE_NO_REPLY;
  t.no_reply_count = 1;
  t.last_success = now - kHour;
  t.last_attempt = now - 600;

  EXPECT_TRUE(poll::isDue(t, 600, now, 0));  // min(interval, retry)
  t.last_attempt = now - 599;
  EXPECT_FALSE(poll::isDue(t, 600, now, 0));
}

TEST(PollSchedule, IsDueRetriesThreeTimesAnHourThenHourly) {
  poll::TargetState t = blank();
  pollStarted(t, poll::CODE_NO_REPLY, 0);
  EXPECT_FALSE(poll::isDue(t, kHour, 19 * 60, 0));
  EXPECT_TRUE(poll::isDue(t, kHour, 20 * 60, 0));

  for (uint32_t i = 1; i <= 8; i++) pollStarted(t, poll::CODE_NO_REPLY, i * 1200);
  const uint32_t last = 8 * 1200;
  EXPECT_FALSE(poll::isDue(t, kHour, last + 1200, 0));
  EXPECT_TRUE(poll::isDue(t, kHour, last + kHour, 0));

  poll::recordResult(t, poll::CODE_OK, last + kHour, false);
  EXPECT_EQ(0, t.no_reply_count);
}

TEST(PollSchedule, IsDueUsesADailySlotAfterADayUnheard) {
  const uint32_t now0 = 1000000;
  poll::TargetState t = blank();
  t.code = poll::CODE_NO_REPLY;
  t.unheard_since = now0 - 25 * kHour;
  t.last_attempt = now0 - kHour;
  t.no_reply_count = 50;

  EXPECT_FALSE(poll::isDue(t, kHour, now0, 0));
  EXPECT_TRUE(poll::isDue(t, kHour, now0 + 24 * kHour, 0));

  poll::recordResult(t, poll::CODE_OK, now0 + 24 * kHour, false);
  EXPECT_EQ(0U, t.unheard_since);
  t.last_attempt = now0 + 24 * kHour;
  EXPECT_TRUE(poll::isDue(t, kHour, now0 + 25 * kHour + 1, 0));
}

TEST(PollSchedule, IsDueScalesThePhaseIntoTheDailyPeriod) {
  const uint32_t now = 1020000;
  poll::TargetState t = blank();
  t.code = poll::CODE_NO_REPLY;
  t.no_reply_count = 5;
  t.unheard_since = now - 25 * kHour;
  // 1800 s into a 7200 s interval is 21600 s into the day. This last attempt
  // is after the unscaled 1800 s slot but before the scaled 21600 s one.
  t.last_attempt = 960000;
  EXPECT_TRUE(poll::isDue(t, 7200, now, 1800));
}

TEST(PollSchedule, IsDueGoesDailyWhenRepliesFlood) {
  poll::TargetState t = blank();
  t.code = poll::CODE_OK;
  t.flood_replies = 2;
  t.last_attempt = 2000001;
  EXPECT_FALSE(poll::isDue(t, kHour, 2000000 + 2 * kHour, 0));
  EXPECT_TRUE(poll::isDue(t, kHour, 2000000 + 25 * kHour, 0));
}

TEST(PollSchedule, IsDueKeepsAnIntervalLongerThanADayWhenFlooding) {
  const uint32_t every = 3 * kDay;
  const uint32_t now = 1000000;
  poll::TargetState t = blank();
  t.code = poll::CODE_OK;
  t.flood_replies = 2;
  t.last_attempt = now - 100000;
  EXPECT_FALSE(poll::isDue(t, every, now, 0));
  t.last_attempt = now - every;
  EXPECT_TRUE(poll::isDue(t, every, now, 0));
}

TEST(PollSchedule, IsDueGoesDailyWithoutABatteryVoltage) {
  const uint32_t now = 1000000;
  poll::TargetState t = blank();
  t.code = poll::CODE_NO_BATTERY;
  t.last_attempt = now - kHour;
  EXPECT_FALSE(poll::isDue(t, kHour, now, 0));

  t.last_attempt = now - kDay;
  EXPECT_TRUE(poll::isDue(t, kHour, now, 0));
}

TEST(PollSchedule, IsDueRecentFailuresDoNotBackOff) {
  const uint32_t now0 = 1000000;
  poll::TargetState t = blank();
  t.code = poll::CODE_NO_REPLY;
  t.unheard_since = now0 - 2 * kHour;
  t.last_attempt = now0 - kHour;
  t.no_reply_count = 50;
  EXPECT_TRUE(poll::isDue(t, kHour, now0, 0));
}

// -- nextSlot ---------------------------------------------------------------

TEST(PollSchedule, NextSlotReportsNowWhenTheSlotIsDue) {
  poll::TargetState t = blank();
  t.last_attempt = 6000;  // before the 7200 slot
  EXPECT_EQ(8000U, poll::nextSlot(t, kHour, 8000, 0));
}

TEST(PollSchedule, NextSlotReportsTheFollowingSlotOtherwise) {
  poll::TargetState t = blank();
  t.last_attempt = 7000;  // half a period not reached
  EXPECT_EQ(10800U, poll::nextSlot(t, kHour, 8000, 0));

  t.last_attempt = 7200;  // already polled at this slot
  EXPECT_EQ(10800U, poll::nextSlot(t, kHour, 8000, 0));

  t.last_attempt = 9000;  // clock stepped back
  EXPECT_EQ(10800U, poll::nextSlot(t, kHour, 8000, 0));
}

TEST(PollSchedule, NextSlotForANeverPolledTargetIsNow) {
  poll::TargetState t = blank();
  EXPECT_EQ(8000U, poll::nextSlot(t, kHour, 8000, 0));
}

TEST(PollSchedule, NextSlotUsesThePhase) {
  poll::TargetState t = blank();
  EXPECT_EQ(4200U, poll::nextSlot(t, kHour, 1000, 600));
}

TEST(PollSchedule, NextSlotSurvivesAZeroInterval) {
  poll::TargetState t = blank();
  EXPECT_EQ(1000U, poll::nextSlot(t, 0, 1000, 0));
}

// -- request cap ------------------------------------------------------------

TEST(PollSchedule, CapReachedCountsSendsInsideTheWindow) {
  const uint32_t now = 1000000;
  poll::TargetState t = blank();
  t.sends[0] = now - 1200;
  t.sends[1] = now - 600;

  uint32_t until = 0;
  EXPECT_TRUE(poll::capReached(t, kHour, 2, now, &until));
  EXPECT_EQ(now - 1200 + kHour, until);
  EXPECT_FALSE(poll::capReached(t, kHour, 3, now, &until));
}

TEST(PollSchedule, CapReachedExcludesTheWindowEdge) {
  const uint32_t now = 1000000;
  poll::TargetState t = blank();
  t.sends[0] = now - kHour;  // exactly one window ago
  t.sends[1] = now - 600;

  uint32_t until = 0;
  EXPECT_FALSE(poll::capReached(t, kHour, 2, now, &until));

  t.sends[0] = now - kHour + 1;
  EXPECT_TRUE(poll::capReached(t, kHour, 2, now, &until));
  EXPECT_EQ(now - kHour + 1 + kHour, until);
}

TEST(PollSchedule, CapReachedIgnoresEmptySlots) {
  const uint32_t now = 60;
  poll::TargetState t = blank();
  uint32_t until = 0;
  EXPECT_FALSE(poll::capReached(t, kHour, 1, now, &until));
}

TEST(PollSchedule, CapReachedWithAZeroLimitBlocks) {
  const uint32_t now = 1000;
  poll::TargetState t = blank();
  uint32_t until = 0;
  EXPECT_TRUE(poll::capReached(t, kHour, 0, now, &until));
  EXPECT_EQ(now, until);
}

// -- recordSend -------------------------------------------------------------

TEST(PollSchedule, RecordSendAppendsAndSetsTheAttempt) {
  poll::TargetState t = blank();
  poll::recordSend(t, 100);
  EXPECT_EQ(100U, t.last_attempt);
  EXPECT_EQ(0U, t.sends[0]);
  EXPECT_EQ(0U, t.sends[1]);
  EXPECT_EQ(0U, t.sends[2]);
  EXPECT_EQ(100U, t.sends[3]);
}

TEST(PollSchedule, RecordSendDropsTheOldestWhenFull) {
  poll::TargetState t = blank();
  for (uint32_t i = 1; i <= 5; i++) poll::recordSend(t, i * 100);
  EXPECT_EQ(200U, t.sends[0]);
  EXPECT_EQ(300U, t.sends[1]);
  EXPECT_EQ(400U, t.sends[2]);
  EXPECT_EQ(500U, t.sends[3]);
  EXPECT_EQ(500U, t.last_attempt);
}

TEST(PollSchedule, AGuestQuickRetryCountsTowardTheCap) {
  const uint32_t now = 1000000;
  poll::TargetState t = blank();
  t.last_success = now - kHour;
  pollStarted(t, poll::CODE_NO_REPLY, now);
  EXPECT_TRUE(poll::isDue(t, kHour, now + 1201, 0));

  uint32_t until = 0;
  EXPECT_TRUE(poll::capReached(t, kHour, 1, now + 1201, &until));
  EXPECT_EQ(now + kHour, until);
}

// -- recordResult -----------------------------------------------------------

TEST(PollSchedule, RecordResultOkClearsTheMisses) {
  poll::TargetState t = blank();
  t.code = poll::CODE_NO_REPLY;
  t.no_reply_count = 3;
  t.unheard_since = 100;
  t.last_success = 50;
  t.learned_pw = poll::LEARNED_HELLO;

  poll::recordResult(t, poll::CODE_OK, 1000, false);
  EXPECT_EQ(poll::CODE_OK, t.code);
  EXPECT_EQ(0, t.no_reply_count);
  EXPECT_EQ(0U, t.unheard_since);
  EXPECT_EQ(1000U, t.last_success);
  EXPECT_EQ(poll::LEARNED_HELLO, t.learned_pw);  // a reading doesn't touch the password
}

TEST(PollSchedule, RecordResultNoBatteryCountsAsAReading) {
  poll::TargetState t = blank();
  t.code = poll::CODE_NO_BATTERY;
  t.no_reply_count = 2;
  t.unheard_since = 100;
  poll::recordResult(t, poll::CODE_NO_BATTERY, 1000, false);
  EXPECT_EQ(1000U, t.last_success);
  EXPECT_EQ(0, t.no_reply_count);
  EXPECT_EQ(0U, t.unheard_since);
}

TEST(PollSchedule, RecordResultNoReplyCountsAndKeepsTheFirstMiss) {
  poll::TargetState t = blank();
  poll::recordResult(t, poll::CODE_NO_REPLY, 100, false);
  EXPECT_EQ(1, t.no_reply_count);
  EXPECT_EQ(100U, t.unheard_since);
  EXPECT_EQ(0U, t.last_success);

  poll::recordResult(t, poll::CODE_NO_REPLY, 200, false);
  EXPECT_EQ(2, t.no_reply_count);
  EXPECT_EQ(100U, t.unheard_since);
}

TEST(PollSchedule, RecordResultNoReplyWrapsTheMissCountToKeepTheCadence) {
  poll::TargetState t = blank();
  t.no_reply_count = 255;
  t.unheard_since = 100;
  poll::recordResult(t, poll::CODE_NO_REPLY, 200, false);
  EXPECT_EQ(253, t.no_reply_count);
}

TEST(PollSchedule, RecordResultNoReplyKeepsTheEveryThirdMissCadencePastSaturation) {
  poll::TargetState t = blank();
  t.no_reply_count = 250;
  t.unheard_since = 100;
  uint32_t now = 200;
  uint32_t unbounded = 250;
  for (int i = 0; i < 12; i++) {  // two wraps past 255
    now += 60;
    unbounded++;
    poll::recordResult(t, poll::CODE_NO_REPLY, now, false);
    EXPECT_EQ((unbounded + 1) % 3, (t.no_reply_count + 1) % 3) << i;
    EXPECT_GE(t.no_reply_count, 2);
  }
}

TEST(PollSchedule, RecordResultForgetsHelloAfterSevenDaysUnheard) {
  poll::TargetState t = blank();
  t.learned_pw = poll::LEARNED_HELLO;
  t.unheard_since = 1000;

  poll::recordResult(t, poll::CODE_NO_REPLY, 1000 + 7 * kDay - 1, false);
  EXPECT_EQ(poll::LEARNED_HELLO, t.learned_pw);

  t.no_reply_count = 0;
  poll::recordResult(t, poll::CODE_NO_REPLY, 1000 + 7 * kDay, false);
  EXPECT_EQ(poll::LEARNED_NONE, t.learned_pw);
}

TEST(PollSchedule, RecordResultForgetsBlankAfterSevenDaysUnheard) {
  poll::TargetState t = blank();
  t.learned_pw = poll::LEARNED_BLANK;
  t.unheard_since = 1000;

  poll::recordResult(t, poll::CODE_NO_REPLY, 1000 + 7 * kDay - 1, false);
  EXPECT_EQ(poll::LEARNED_BLANK, t.learned_pw);

  t.no_reply_count = 0;
  poll::recordResult(t, poll::CODE_NO_REPLY, 1000 + 7 * kDay, false);
  EXPECT_EQ(poll::LEARNED_NONE, t.learned_pw);
}

TEST(PollSchedule, RecordResultKeepsTheMissClockForUnsentPolls) {
  poll::TargetState t = blank();
  t.no_reply_count = 2;
  t.unheard_since = 100;
  t.last_success = 50;
  t.learned_pw = poll::LEARNED_HELLO;

  poll::recordResult(t, poll::CODE_NOT_DIRECT, 1000 + 8 * kDay, false);
  EXPECT_EQ(poll::CODE_NOT_DIRECT, t.code);
  EXPECT_EQ(2, t.no_reply_count);
  EXPECT_EQ(100U, t.unheard_since);
  EXPECT_EQ(50U, t.last_success);
  EXPECT_EQ(poll::LEARNED_HELLO, t.learned_pw);

  poll::recordResult(t, poll::CODE_ERROR, 1000 + 8 * kDay, false);
  EXPECT_EQ(poll::CODE_ERROR, t.code);
  EXPECT_EQ(100U, t.unheard_since);
}

TEST(PollSchedule, RecordResultStopsTheMissClockForAnyAnswer) {
  poll::TargetState t = blank();
  t.no_reply_count = 1;
  t.unheard_since = 100;
  poll::recordResult(t, poll::CODE_ADMIN_STOP, 1000, false);
  EXPECT_EQ(0U, t.unheard_since);
  EXPECT_EQ(1, t.no_reply_count);  // not a reading, so the count stands
}

TEST(PollSchedule, RecordResultRemembersAQuickRetry) {
  poll::TargetState t = blank();
  poll::recordResult(t, poll::CODE_NO_REPLY, 100, true);
  EXPECT_TRUE(t.retried);

  poll::recordResult(t, poll::CODE_NO_REPLY, 200, false);
  EXPECT_FALSE(t.retried);
}

// -- pickDue ----------------------------------------------------------------

TEST(PollSchedule, PickDueChoosesTheOldestAttempt) {
  poll::TargetState t[3] = {blank(), blank(), blank()};
  t[0].last_attempt = 500;
  t[1].last_attempt = 100;
  t[2].last_attempt = 300;
  const bool due[3] = {true, true, true};
  EXPECT_EQ(1, poll::pickDue(t, due, 3, 1000, 0));
}

TEST(PollSchedule, PickDueSkipsTargetsThatAreNotDue) {
  poll::TargetState t[3] = {blank(), blank(), blank()};
  t[0].last_attempt = 100;
  t[1].last_attempt = 200;
  t[2].last_attempt = 300;
  const bool due[3] = {false, false, true};
  EXPECT_EQ(2, poll::pickDue(t, due, 3, 1000, 0));
}

TEST(PollSchedule, PickDueReturnsMinusOneWhenNothingIsDue) {
  poll::TargetState t[2] = {blank(), blank()};
  const bool due[2] = {false, false};
  EXPECT_EQ(-1, poll::pickDue(t, due, 2, 1000, 0));
  EXPECT_EQ(-1, poll::pickDue(t, due, 0, 1000, 0));
  EXPECT_EQ(-1, poll::pickDue(nullptr, due, 2, 1000, 0));
}

TEST(PollSchedule, PickDueKeepsTheScheduledGap) {
  poll::TargetState t[1] = {blank()};
  const bool due[1] = {true};

  EXPECT_EQ(-1, poll::pickDue(t, due, 1, 1000, 950));
  EXPECT_EQ(0, poll::pickDue(t, due, 1, 1000, 940));  // exactly the gap
  EXPECT_EQ(-1, poll::pickDue(t, due, 1, 1000, 1100));  // clock stepped back
}

TEST(PollSchedule, PickDueTreatsZeroAsNeverScheduled) {
  poll::TargetState t[1] = {blank()};
  const bool due[1] = {true};
  EXPECT_EQ(0, poll::pickDue(t, due, 1, 30, 0));
  EXPECT_EQ(0, poll::pickDue(t, due, 1, 30, 0, 60));
}

TEST(PollSchedule, PickDueTiesKeepTheEarlierIndex) {
  poll::TargetState t[3] = {blank(), blank(), blank()};
  t[0].last_attempt = 200;
  t[1].last_attempt = 100;
  t[2].last_attempt = 100;
  const bool due[3] = {true, true, true};
  EXPECT_EQ(1, poll::pickDue(t, due, 3, 1000, 0));
}

// -- ported plugin scenarios ------------------------------------------------

TEST(PollSchedule, PollsAreSpreadAcrossTheInterval) {
  const uint32_t interval = 7200;
  constexpr uint8_t kCount = 4;
  uint32_t phase[kCount];
  for (uint8_t i = 0; i < kCount; i++) phase[i] = poll::spreadPhase(interval, i, kCount);
  EXPECT_EQ(0U, phase[0]);
  EXPECT_EQ(1800U, phase[1]);
  EXPECT_EQ(3600U, phase[2]);
  EXPECT_EQ(5400U, phase[3]);

  // A period boundary that still fits the 32-bit epoch.
  const uint32_t base = 555000UL * interval;
  uint32_t due_at[kCount] = {};
  for (uint8_t i = 0; i < kCount; i++) {
    for (uint32_t t = 0; t < interval; t += 60) {
      poll::TargetState st = blank();
      st.code = poll::CODE_OK;
      st.last_attempt = base - interval + phase[i];
      if (poll::isDue(st, interval, base + t, phase[i])) {
        due_at[i] = t;
        break;
      }
    }
  }
  EXPECT_EQ(0U, due_at[0]);
  EXPECT_EQ(1800U, due_at[1]);
  EXPECT_EQ(3600U, due_at[2]);
  EXPECT_EQ(5400U, due_at[3]);
}

TEST(PollSchedule, ASlotIsSkippedJustAfterATest) {
  const uint32_t base = 1000000UL * kHour;  // a slot boundary
  poll::TargetState st = blank();
  st.code = poll::CODE_OK;
  st.last_attempt = base - 600;  // tested ten minutes before the slot
  EXPECT_FALSE(poll::isDue(st, kHour, base, 0));
  EXPECT_TRUE(poll::isDue(st, kHour, base + kHour, 0));
}

TEST(PollSchedule, CapFailureHoldsUntilTheOldestSendLeaves) {
  const uint32_t now = 2000000;
  poll::TargetState t = blank();
  t.sends[0] = now - 1200;
  t.sends[1] = now - 600;

  uint32_t until = 0;
  ASSERT_TRUE(poll::capReached(t, kHour, 2, now, &until));
  t.next_due_at = until;
  EXPECT_FALSE(poll::isDue(t, kHour, now, 0));
  EXPECT_TRUE(poll::isDue(t, kHour, until, 0));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
