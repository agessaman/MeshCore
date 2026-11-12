// When a control session is filed, what it asks for, and how a policy answer
// folds into the copy in use. The policy rules follow the plugin's
// tests/test_community.py (gates persist through an outage, a restart and a
// 404; a 404 means defaults and is not stale; only a fetched policy clears a
// switch).

#include <gtest/gtest.h>

#include <string.h>

#include "helpers/poll/PollControlPlan.h"
#include "helpers/poll/PollDecisions.h"

namespace {

const uint32_t kNow = 1800000000;

poll::ControlNeed need() {
  poll::ControlNeed n;
  memset(&n, 0, sizeof(n));
  n.enabled = true;
  n.has_url = true;
  n.clock_ok = true;
  n.policy_answered = true;
  return n;
}

poll::RegistrationState reg() {
  poll::RegistrationState r;
  memset(&r, 0, sizeof(r));
  return r;
}

poll::Policy defaults() {
  poll::Policy p;
  poll::policyDefaults(p);
  return p;
}

}  // namespace

TEST(PollControlPlan, NothingIsNeededWhenAllIsKnown) {
  EXPECT_FALSE(poll::controlNeeded(need()));
}

TEST(PollControlPlan, EachTriggerAloneIsEnough) {
  poll::ControlNeed n = need();
  n.registration_due = true;
  EXPECT_TRUE(poll::controlNeeded(n));
  n = need();
  n.claim_unanswered = true;
  EXPECT_TRUE(poll::controlNeeded(n));
  n = need();
  n.gate_closed = true;
  EXPECT_TRUE(poll::controlNeeded(n));
  n = need();
  n.starting = true;
  EXPECT_TRUE(poll::controlNeeded(n));
  n = need();
  n.policy_answered = false;
  EXPECT_TRUE(poll::controlNeeded(n));
  n = need();
  n.target_due = true;
  EXPECT_TRUE(poll::controlNeeded(n));
  n = need();
  n.manual_due = true;
  EXPECT_TRUE(poll::controlNeeded(n));
  n.enabled = false;
  EXPECT_FALSE(poll::controlNeeded(n));
}

TEST(PollControlPlan, NeverWithoutSwitchUrlOrClock) {
  poll::ControlNeed n = need();
  n.target_due = n.registration_due = true;
  n.policy_answered = false;
  poll::ControlNeed off = n;
  off.enabled = false;
  EXPECT_FALSE(poll::controlNeeded(off));
  off = n;
  off.has_url = false;
  EXPECT_FALSE(poll::controlNeeded(off));
  off = n;
  off.clock_ok = false;
  EXPECT_FALSE(poll::controlNeeded(off));
}

TEST(PollControlPlan, FirstSessionFilesWhileNoClaimCanBeSent) {
  // Boot, key still none: the hold is on because no claim has gone out, and
  // that asks for the session that registers the key. A claim published later
  // is what sets claim_unanswered, so its absence cannot block this one.
  const uint32_t started = kNow;
  poll::ControlNeed n = need();
  n.policy_answered = false;
  n.registration_due = true;
  n.starting = poll::startupHold(true, true, started, 0, 0, started);
  n.claim_unanswered = false;
  EXPECT_TRUE(n.starting);
  EXPECT_TRUE(poll::controlNeeded(n));
}

TEST(PollControlPlan, OneSessionPerPeriod) {
  EXPECT_TRUE(poll::controlAllowed(false, false, 0, poll::kControlEveryS));
  EXPECT_FALSE(poll::controlAllowed(true, false, 0, poll::kControlEveryS));
  EXPECT_FALSE(poll::controlAllowed(false, true, 599, poll::kControlEveryS));
  EXPECT_TRUE(poll::controlAllowed(false, true, 600, poll::kControlEveryS));
  EXPECT_FALSE(poll::controlAllowed(true, true, 6000, poll::kControlEveryS));
  EXPECT_TRUE(poll::controlAllowed(false, true, 60, 60));
}

TEST(PollControlPlan, WantsFollowCommunityRegistrationAndSharedKey) {
  EXPECT_EQ(poll::WANT_POLICY, poll::controlWants(false, false, false));
  EXPECT_EQ(poll::WANT_POLICY | poll::WANT_OWNERS, poll::controlWants(true, false, false));
  EXPECT_EQ(poll::WANT_POLICY | poll::WANT_OWNERS | poll::WANT_REGISTER | poll::WANT_SHARED_KEY,
            poll::controlWants(true, true, true));
}

TEST(PollControlPlan, SharedKeyOnlyWhileTheOwnKeyIsNotUsable) {
  poll::RegistrationState r = reg();
  // First registration in this session: its 404 brings the shared key along.
  EXPECT_FALSE(poll::sharedKeyWanted(false, 0, r, true, kNow));
  // Never registered and not registering (no own key): ask for it.
  EXPECT_TRUE(poll::sharedKeyWanted(false, 0, r, false, kNow));
  // A failed or unsupported registration: ask for it, also alongside a retry.
  poll::registrationResult(r, 503, false, kNow - 400);
  EXPECT_TRUE(poll::sharedKeyWanted(false, 0, r, true, kNow));
  poll::registrationResult(r, 404, false, kNow - 400);
  EXPECT_TRUE(poll::sharedKeyWanted(false, 0, r, false, kNow));
  // One held and fresh: no.
  EXPECT_FALSE(poll::sharedKeyWanted(true, kNow - 3600, r, false, kNow));
  // Held for a day: fetched again.
  EXPECT_TRUE(poll::sharedKeyWanted(true, kNow - poll::kDay, r, false, kNow));
  // Own key accepted and within its lifetime: no.
  poll::registrationResult(r, 200, true, kNow - 100);
  EXPECT_FALSE(poll::sharedKeyWanted(false, 0, r, false, kNow));
  // Its registration lapsed (a day without a renewal): yes.
  EXPECT_TRUE(poll::sharedKeyWanted(false, 0, r, false, kNow + poll::kDay));
}

TEST(PollControlPlan, A200ReplacesAndClampsThePolicy) {
  poll::Policy p = defaults();
  poll::Policy server = defaults();
  server.paused = true;
  server.direct_s = 60;
  strcpy(server.version, "p1");
  EXPECT_EQ(poll::POLICY_APPLIED, poll::applyPolicyAnswer(p, true, 200, server, kNow));
  EXPECT_TRUE(p.paused);
  EXPECT_EQ(poll::kDirectMinS, p.direct_s);
  EXPECT_STREQ("p1", p.version);
  EXPECT_TRUE(p.ever_fetched);
  EXPECT_EQ(kNow, p.fetched_at);
  EXPECT_EQ(200, p.last_status);
  // Fast mode keeps a one-minute window.
  poll::Policy q = defaults();
  poll::applyPolicyAnswer(q, true, 200, server, kNow, true);
  EXPECT_EQ(60U, q.direct_s);
}

TEST(PollControlPlan, GatesPersistThroughAnOutageAndA404) {
  poll::Policy p = defaults();
  poll::Policy server = defaults();
  server.paused = true;
  server.observer_blocked = true;
  server.has_version_ok = true;
  server.version_ok = false;
  poll::applyPolicyAnswer(p, true, 200, server, kNow);
  EXPECT_EQ(poll::GATE_BLOCKED, poll::policyGate(p, poll::kPluginVersion, kNow));
  // CoreScope goes away.
  EXPECT_EQ(poll::POLICY_KEPT, poll::applyPolicyAnswer(p, false, 0, defaults(), kNow + 3600));
  EXPECT_EQ(poll::GATE_BLOCKED, poll::policyGate(p, poll::kPluginVersion, kNow + 3600));
  EXPECT_TRUE(p.paused);
  // A 404 doesn't clear them either.
  EXPECT_EQ(poll::POLICY_NONE, poll::applyPolicyAnswer(p, false, 404, defaults(), kNow + 7200));
  EXPECT_EQ(poll::GATE_BLOCKED, poll::policyGate(p, poll::kPluginVersion, kNow + 7200));
  // Only a fetched policy clears them.
  poll::Policy open = defaults();
  open.has_version_ok = true;
  open.version_ok = true;
  poll::applyPolicyAnswer(p, true, 200, open, kNow + 9000);
  EXPECT_EQ(poll::GATE_OPEN, poll::policyGate(p, poll::kPluginVersion, kNow + 9000));
}

TEST(PollControlPlan, AGateSavedBeforeARestartHoldsUntilAFetch) {
  poll::Policy server = defaults();
  server.observer_blocked = true;
  poll::Policy before = defaults();
  poll::applyPolicyAnswer(before, true, 200, server, kNow);
  const poll::SavedGate saved = poll::savedGateFrom(before, poll::kPluginVersion);
  // After a reboot nothing is fetched yet: the flash copy rules.
  poll::Policy after = defaults();
  EXPECT_EQ(poll::GATE_BLOCKED, poll::gateInForce(after, saved, poll::kPluginVersion, kNow));
  poll::applyPolicyAnswer(after, false, 404, defaults(), kNow);
  EXPECT_EQ(poll::GATE_BLOCKED, poll::gateInForce(after, saved, poll::kPluginVersion, kNow));
  poll::applyPolicyAnswer(after, true, 200, defaults(), kNow);
  EXPECT_EQ(poll::GATE_OPEN, poll::gateInForce(after, saved, poll::kPluginVersion, kNow));
  const poll::SavedGate now_saved = poll::savedGateFrom(after, poll::kPluginVersion);
  EXPECT_FALSE(poll::savedGateEqual(saved, now_saved));
  EXPECT_TRUE(poll::savedGateEqual(now_saved, poll::savedGateFrom(after, poll::kPluginVersion)));
}

TEST(PollControlPlan, PauseLiftsItselfWhenPausedUntilPasses) {
  poll::Policy server = defaults();
  server.paused = true;
  server.paused_until = kNow + 600;
  poll::Policy p = defaults();
  poll::applyPolicyAnswer(p, true, 200, server, kNow);
  const poll::SavedGate saved = poll::savedGateFrom(p, poll::kPluginVersion);
  poll::applyPolicyAnswer(p, false, 0, defaults(), kNow + 1);   // and CoreScope is down when it ends
  EXPECT_EQ(poll::GATE_PAUSED, poll::gateInForce(p, saved, poll::kPluginVersion, kNow + 599));
  EXPECT_EQ(poll::GATE_OPEN, poll::gateInForce(p, saved, poll::kPluginVersion, kNow + 600));
  poll::Policy rebooted = defaults();
  EXPECT_EQ(poll::GATE_OPEN, poll::gateInForce(rebooted, saved, poll::kPluginVersion, kNow + 601));
}

TEST(PollControlPlan, Policy404UsesDefaultsAndIsNotStale) {
  poll::Policy p = defaults();
  EXPECT_EQ(poll::POLICY_NONE, poll::applyPolicyAnswer(p, false, 404, defaults(), kNow));
  EXPECT_FALSE(p.ever_fetched);
  EXPECT_FALSE(poll::policyStale(p, kNow + poll::kDay));
  EXPECT_EQ(3600U, poll::policyWindow(p, nullptr, 0, kNow));
  EXPECT_EQ(poll::GATE_OPEN, poll::policyGate(p, poll::kPluginVersion, kNow));
}

TEST(PollControlPlan, StalePolicyDoublesTheWindowOnlyWhenCoreScopeIsUnreachable) {
  poll::Policy p = defaults();
  poll::applyPolicyAnswer(p, true, 200, defaults(), kNow);
  poll::applyPolicyAnswer(p, false, -1, defaults(), kNow + 7 * 3600);
  EXPECT_EQ(7200U, poll::policyWindow(p, nullptr, 0, kNow + 7 * 3600));
  poll::applyPolicyAnswer(p, false, 404, defaults(), kNow + 7 * 3600);
  EXPECT_EQ(3600U, poll::policyWindow(p, nullptr, 0, kNow + 7 * 3600));
}

TEST(PollControlPlan, AnExpiredSharedKeyGoesOnlyWhenItsRenewalFails) {
  EXPECT_FALSE(poll::sharedKeyUsable(false, kNow, false, kNow));
  EXPECT_TRUE(poll::sharedKeyUsable(true, kNow - 3600, false, kNow));
  EXPECT_TRUE(poll::sharedKeyUsable(true, kNow - 3600, true, kNow));   // a failure inside the day changes nothing
  EXPECT_TRUE(poll::sharedKeyUsable(true, kNow - poll::kDay, false, kNow));   // renewal not answered yet
  EXPECT_FALSE(poll::sharedKeyUsable(true, kNow - poll::kDay, true, kNow));
  EXPECT_EQ(poll::KEY_NONE, poll::chooseKey(reg(), poll::sharedKeyUsable(true, kNow - poll::kDay, true, kNow), kNow));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
