#include <gtest/gtest.h>
#include <helpers/poll/PollExchange.h>
#include <helpers/poll/PollRequestRuntime.h>

#include <vector>

namespace {
poll::RequestedTarget target() {
  poll::RequestedTarget t = {};
  t.held = true;
  t.pub[0] = 0x21;
  t.envelope.present = true;
  t.envelope.ver = 7;
  t.envelope.epk[0] = 0x31;
  return t;
}
void seal(poll::RequestedTarget& t, const uint8_t secret[32], const uint8_t observer[32]) {
  ASSERT_TRUE(poll::sealPwEnvelope(secret, observer, t.pub, t.envelope.epk, t.envelope.ver,
                                 (const uint8_t*)"guest", 5, t.envelope.ct, t.envelope.tag));
}
bool zero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) if (p[i]) return false;
  return true;
}

const uint32_t kNow = 200000;

poll::TargetState session() {
  poll::TargetState state = {};
  state.logged_in_at = 1000;
  state.login_checked_at = kNow - 60;
  state.learned_pw = poll::LEARNED_HELLO;
  state.key_login_fails = 4;
  state.code = poll::CODE_OK;
  state.route_sent_at = kNow - 60;
  state.status_checked_at = kNow;
  return state;
}

void expectSessionKept(const poll::TargetState& state) {
  EXPECT_EQ(1000u, state.logged_in_at);
  EXPECT_EQ(kNow - 60, state.login_checked_at);
  EXPECT_EQ(poll::LEARNED_HELLO, state.learned_pw);
}

poll::RequestedEnvelopeEffect applyEnvelope(poll::TargetState& state, const poll::PwEnvelope& before,
                                            const poll::PwEnvelope& after) {
  const poll::RequestedEnvelopeEffect effect = poll::requestedEnvelopeEffect(before, after);
  if (effect.reset_login_fails) state.key_login_fails = 0;
  return effect;
}

struct ExchangeTrace {
  std::vector<uint8_t> acts;
  std::vector<uint8_t> pws;
};

void driveExchange(poll::PollExchange& exchange, poll::TargetState& state, const poll::ExchangeConfig& cfg,
                   uint32_t now, bool answer_data, bool answer_login, ExchangeTrace& trace) {
  poll::Step step = exchange.start(state, cfg, now);
  for (int guard = 0; guard < 40 && step.act != poll::ACT_FINISH; guard++) {
    if (poll::isNoChange(step)) break;
    if (step.act != poll::ACT_WAIT) {
      trace.acts.push_back(step.act);
      if (step.act == poll::ACT_SEND_LOGIN) trace.pws.push_back(step.pw);
      step = exchange.onEvent(state, poll::EV_TX_DONE, false, now);
      continue;
    }
    if (answer_login && exchange.expectsLogin() && !exchange.result().login_answered) {
      step = exchange.onEvent(state, poll::EV_LOGIN_GUEST, false, now);
      continue;
    }
    if (answer_data && exchange.expectsData() && !exchange.result().got_reply) {
      step = exchange.onEvent(state, poll::EV_DATA_REPLY, false, now);
      continue;
    }
    step = exchange.onEvent(state, poll::EV_TIMER, false, now);
  }
  EXPECT_EQ(poll::ACT_FINISH, step.act);
}

poll::ExchangeConfig exchangeCfg(bool custom_pw) {
  poll::ExchangeConfig cfg = poll::defaultExchangeConfig();
  cfg.custom_pw = custom_pw;
  return cfg;
}
}

TEST(PollRequestRuntime, OpensWithNodeSecretAndConsumesIt) {
  poll::RequestedTarget t = target();
  uint8_t secret[32] = {5}, observer[32] = {9};
  seal(t, secret, observer);
  poll::openRequestedEnvelope(t, secret, observer);
  EXPECT_TRUE(zero(secret, sizeof(secret)));
  ASSERT_TRUE(poll::requestMayPoll(t));
  EXPECT_STREQ("guest", poll::requestedPassword(t));
}

TEST(PollRequestRuntime, RejectsZeroSecretEvenWithAValidTag) {
  poll::RequestedTarget t = target();
  uint8_t secret[32] = {}, observer[32] = {9};
  seal(t, secret, observer);
  poll::openRequestedEnvelope(t, secret, observer);
  EXPECT_FALSE(poll::requestMayPoll(t));
  EXPECT_TRUE(t.pw_bad);
  EXPECT_TRUE(zero((const uint8_t*)t.pw, sizeof(t.pw)));
}

TEST(PollRequestRuntime, WrongIdentityFailsClosedAndWipesSecret) {
  poll::RequestedTarget t = target();
  uint8_t secret[32] = {5}, observer[32] = {9};
  seal(t, secret, observer);
  observer[0]++;
  poll::openRequestedEnvelope(t, secret, observer);
  EXPECT_TRUE(zero(secret, sizeof(secret)));
  EXPECT_FALSE(poll::requestMayPoll(t));
  EXPECT_EQ(nullptr, poll::requestedPassword(t));
  EXPECT_TRUE(zero((const uint8_t*)t.pw, sizeof(t.pw)));
}

TEST(PollRequestRuntime, AbsentParkedOrRefusedRequestsCannotStart) {
  poll::RequestedTarget t = {};
  t.held = true;
  poll::OwnerBrief b = {};
  EXPECT_FALSE(poll::requestedRunAllowed(t, b));
  b.present = b.requested = true;
  b.role = poll::OWNER_ROLE_POLLING;
  EXPECT_TRUE(poll::requestedRunAllowed(t, b));
  b.role = poll::OWNER_ROLE_STANDBY;
  EXPECT_TRUE(poll::requestedRunAllowed(t, b));
  for (uint8_t role : {poll::OWNER_ROLE_DISABLED, poll::OWNER_ROLE_PAUSED, poll::OWNER_ROLE_EXCLUDED}) {
    b.role = role;
    EXPECT_FALSE(poll::requestedRunAllowed(t, b));
  }
  b.role = poll::OWNER_ROLE_POLLING;
  t.login_failed = true;
  EXPECT_FALSE(poll::requestedRunAllowed(t, b));
}

TEST(PollRequestRuntime, AdminTombstoneWinsOverEnvelopeReplacementAndRefusal) {
  poll::RequestedTarget t = target();
  poll::TargetState state = {};
  state.admin_seen = true;
  EXPECT_EQ(poll::CLAIM_STATE_ADMIN_STOP, poll::requestedClaimState(state, t));
  poll::PwEnvelope next = t.envelope;
  next.ver++;
  poll::replaceEnvelope(t, next);
  t.login_failed = true;
  EXPECT_EQ(poll::CLAIM_STATE_ADMIN_STOP, poll::requestedClaimState(state, t));
  state.admin_seen = false;
  state.code = poll::CODE_NOT_DIRECT;
  EXPECT_EQ(poll::CLAIM_STATE_LOGIN_FAILED, poll::requestedClaimState(state, t));
}

TEST(PollRequestRuntime, LocalAddEvictsStandbyWithoutAnOwnersAnswer) {
  poll::RequestTable table = {};
  for (uint8_t i = 0; i < 3; i++) {
    table.targets[i] = target();
    table.targets[i].pub[0] = i + 1;
    strcpy(table.targets[i].pw, "guest");
  }
  table.count = 3;
  uint8_t local[7][poll::kPubKeySize] = {};
  for (uint8_t i = 0; i < 7; i++) local[i][0] = 10 + i;
  uint8_t roles[3] = {poll::ROLE_POLLER, poll::ROLE_STANDBY, poll::ROLE_POLLER};
  poll::RequestDelta delta;
  poll::reserveLocalRequests(table, local, 7, roles, delta);
  ASSERT_EQ(1, table.count);
  EXPECT_EQ(1, table.targets[0].pub[0]);
  ASSERT_EQ(2, delta.dropped_count);
  EXPECT_EQ(2, delta.dropped[0][0]);
  EXPECT_EQ(3, delta.dropped[1][0]);
  EXPECT_TRUE(zero((const uint8_t*)table.targets[1].pw, sizeof(table.targets[1].pw)));
  EXPECT_TRUE(zero((const uint8_t*)table.targets[2].pw, sizeof(table.targets[2].pw)));
}

TEST(PollRequestRuntime, LocalTakeoverRemovesMatchingKeyAndKeepsRoleIndicesAligned) {
  poll::RequestTable table = {};
  for (uint8_t i = 0; i < 3; i++) {
    table.targets[i] = target();
    table.targets[i].pub[0] = i + 1;
  }
  table.count = 3;
  uint8_t local[7][poll::kPubKeySize] = {};
  local[0][0] = 1;
  for (uint8_t i = 1; i < 7; i++) local[i][0] = 10 + i;
  uint8_t roles[3] = {poll::ROLE_POLLER, poll::ROLE_STANDBY, poll::ROLE_POLLER};
  poll::RequestDelta delta;
  poll::reserveLocalRequests(table, local, 7, roles, delta);
  ASSERT_EQ(1, table.count);
  EXPECT_EQ(3, table.targets[0].pub[0]);
  EXPECT_EQ(2, delta.dropped_count);
  EXPECT_EQ(1, delta.dropped[0][0]);
  EXPECT_EQ(2, delta.dropped[1][0]);
}

TEST(PollRequestRuntime, EnvelopeLoginCountsResetOnlyForNewPasswordVersionsOrPresence) {
  poll::PwEnvelope first = target().envelope;
  poll::PwEnvelope reencrypted = first;
  reencrypted.epk[0]++;
  reencrypted.ct[0]++;
  EXPECT_FALSE(poll::requestedLoginMustReset(first, reencrypted));
  reencrypted.ver++;
  EXPECT_TRUE(poll::requestedLoginMustReset(first, reencrypted));
  poll::PwEnvelope absent = {};
  first.ver = 0;
  EXPECT_TRUE(poll::requestedLoginMustReset(absent, first));
  EXPECT_TRUE(poll::requestedLoginMustReset(first, absent));
}

TEST(PollRequestRuntime, RefusedPasswordStillNamesTheCredentialUntilItIsDiscarded) {
  poll::RequestedTarget t = target();
  uint8_t secret[32] = {5}, observer[32] = {9};
  seal(t, secret, observer);
  poll::openRequestedEnvelope(t, secret, observer);
  ASSERT_TRUE(poll::noteEnvelopeLoginFails(t, 2));
  EXPECT_EQ(nullptr, poll::requestedPassword(t));
  EXPECT_EQ(poll::CLAIM_KEY_PASSWORD, poll::requestedClaimKey(t));
  poll::discardRequestedPassword(t);
  EXPECT_EQ(poll::CLAIM_KEY_OWN, poll::requestedClaimKey(t));
}

TEST(PollRequestRuntime, EnvelopeBytesAreSentWithoutTheOperatorAsciiRestriction) {
  poll::RequestedTarget t = target();
  uint8_t secret[32] = {5}, observer[32] = {9};
  const uint8_t pw[15] = {'a', ' ', 0x80, 0xff, 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l'};
  ASSERT_TRUE(poll::sealPwEnvelope(secret, observer, t.pub, t.envelope.epk, t.envelope.ver,
                                 pw, sizeof(pw), t.envelope.ct, t.envelope.tag));
  poll::openRequestedEnvelope(t, secret, observer);
  uint8_t out[19] = {};
  ASSERT_EQ(sizeof(out), poll::buildEnvelopeLoginPlain(out, sizeof(out), 0x01020304, poll::requestedPassword(t)));
  const uint8_t stamp[4] = {4, 3, 2, 1};
  EXPECT_EQ(0, memcmp(out, stamp, 4));
  EXPECT_EQ(0, memcmp(out + 4, pw, sizeof(pw)));
  EXPECT_EQ(0, poll::buildLoginPlainCustom(out, sizeof(out), 1, poll::requestedPassword(t)));
  EXPECT_EQ(0, poll::buildEnvelopeLoginPlain(out, sizeof(out) - 1, 1, poll::requestedPassword(t)));
}

TEST(PollRequestRuntime, InvalidEnvelopePlaintextCannotBuildALogin) {
  uint8_t out[32];
  EXPECT_EQ(0, poll::buildEnvelopeLoginPlain(out, sizeof(out), 1, nullptr));
  EXPECT_EQ(0, poll::buildEnvelopeLoginPlain(out, sizeof(out), 1, ""));
  EXPECT_EQ(0, poll::buildEnvelopeLoginPlain(out, sizeof(out), 1, "1234567890123456"));
}

TEST(PollRequestRuntime, SameVerReencryptionKeepsTheSessionAndDoesNotAbort) {
  poll::RequestedTarget t = target();
  uint8_t secret[32] = {5}, keep[32], observer[32] = {9};
  memcpy(keep, secret, sizeof(keep));
  seal(t, secret, observer);
  poll::openRequestedEnvelope(t, secret, observer);
  poll::PwEnvelope again = t.envelope;
  again.epk[0]++;
  ASSERT_TRUE(poll::sealPwEnvelope(keep, observer, t.pub, again.epk, again.ver, (const uint8_t*)"guest", 5,
                                   again.ct, again.tag));
  poll::TargetState state = session();
  const poll::RequestedEnvelopeEffect effect = applyEnvelope(state, t.envelope, again);
  EXPECT_FALSE(effect.abort_run);
  EXPECT_FALSE(effect.reset_login_fails);
  EXPECT_EQ(4, state.key_login_fails);
  expectSessionKept(state);
  poll::replaceEnvelope(t, again);
  uint8_t opened[32];
  memcpy(opened, keep, sizeof(opened));
  poll::openRequestedEnvelope(t, opened, observer);
  ASSERT_STREQ("guest", poll::requestedPassword(t));

  poll::PollExchange exchange;
  ExchangeTrace trace;
  driveExchange(exchange, state, exchangeCfg(poll::requestedPassword(t) != nullptr), kNow, true, false, trace);
  EXPECT_EQ(std::vector<uint8_t>{poll::ACT_SEND_REQUEST}, trace.acts);
  EXPECT_TRUE(trace.pws.empty());
  EXPECT_FALSE(exchange.result().login_tried);
  EXPECT_EQ(1000u, state.logged_in_at);
}

TEST(PollRequestRuntime, VersionChangeKeepsTheSessionAndDoesNotAbort) {
  poll::TargetState state = session();
  poll::PwEnvelope before = target().envelope;
  poll::PwEnvelope after = before;
  after.ver++;
  const poll::RequestedEnvelopeEffect effect = applyEnvelope(state, before, after);
  EXPECT_FALSE(effect.abort_run);
  EXPECT_TRUE(effect.reset_login_fails);
  EXPECT_EQ(0, state.key_login_fails);
  expectSessionKept(state);

  poll::PollExchange exchange;
  ExchangeTrace trace;
  driveExchange(exchange, state, exchangeCfg(true), kNow, true, false, trace);
  EXPECT_EQ(std::vector<uint8_t>{poll::ACT_SEND_REQUEST}, trace.acts);
  EXPECT_TRUE(trace.pws.empty());
  EXPECT_EQ(1000u, state.logged_in_at);
}

TEST(PollRequestRuntime, AddedOrRemovedEnvelopeKeepsTheSessionAndDoesNotAbort) {
  poll::PwEnvelope present = target().envelope;
  poll::PwEnvelope absent = {};
  poll::TargetState added = session();
  const poll::RequestedEnvelopeEffect add = applyEnvelope(added, absent, present);
  EXPECT_FALSE(add.abort_run);
  EXPECT_TRUE(add.reset_login_fails);
  EXPECT_EQ(0, added.key_login_fails);
  expectSessionKept(added);

  poll::TargetState removed = session();
  const poll::RequestedEnvelopeEffect drop = applyEnvelope(removed, present, absent);
  EXPECT_FALSE(drop.abort_run);
  EXPECT_TRUE(drop.reset_login_fails);
  EXPECT_EQ(0, removed.key_login_fails);
  expectSessionKept(removed);
}

TEST(PollRequestRuntime, NeverLoggedInTargetLogsInWithTheEnvelopePassword) {
  poll::RequestedTarget t = target();
  uint8_t secret[32] = {5}, observer[32] = {9};
  seal(t, secret, observer);
  poll::openRequestedEnvelope(t, secret, observer);
  ASSERT_STREQ("guest", poll::requestedPassword(t));
  poll::TargetState state = {};
  poll::PollExchange exchange;
  const poll::Step step = exchange.start(state, exchangeCfg(poll::requestedPassword(t) != nullptr), kNow);
  EXPECT_EQ(poll::ACT_SEND_LOGIN, step.act);
  EXPECT_EQ(poll::PW_CUSTOM, step.pw);

  poll::RequestedTarget plain = {};
  plain.held = true;
  EXPECT_EQ(nullptr, poll::requestedPassword(plain));
  poll::TargetState blank = {};
  poll::PollExchange blank_exchange;
  ExchangeTrace trace;
  driveExchange(blank_exchange, blank, exchangeCfg(false), kNow, false, false, trace);
  ASSERT_EQ((std::vector<uint8_t>{poll::PW_BLANK, poll::PW_HELLO}), trace.pws);
}

TEST(PollRequestRuntime, FastPathMissFallsBackToTheEnvelopePassword) {
  for (uint8_t misses = 0; misses < poll::kForgetLoginAfter; misses++) {
    poll::TargetState state = session();
    state.no_reply_count = misses;
    poll::PollExchange exchange;
    ExchangeTrace trace;
    driveExchange(exchange, state, exchangeCfg(true), kNow, false, false, trace);
    const bool fallback = (misses + 1) % poll::kForgetLoginAfter == 0;
    if (fallback) {
      EXPECT_EQ(std::vector<uint8_t>{poll::PW_CUSTOM}, trace.pws) << (int)misses;
      EXPECT_TRUE(exchange.result().login_tried);
      EXPECT_EQ(poll::XR_LOGIN_UNANSWERED, exchange.result().reason);
    } else {
      EXPECT_TRUE(trace.pws.empty()) << (int)misses;
      EXPECT_FALSE(exchange.result().login_tried);
      EXPECT_EQ(poll::XR_ENTRY_KEPT, exchange.result().reason);
      EXPECT_EQ(1000u, state.logged_in_at);
    }
  }
}

TEST(PollRequestRuntime, AdminRecheckUsesTheCurrentEnvelopePassword) {
  poll::RequestedTarget t = target();
  uint8_t secret[32] = {5}, keep[32], observer[32] = {9};
  memcpy(keep, secret, sizeof(keep));
  seal(t, secret, observer);
  poll::openRequestedEnvelope(t, secret, observer);
  poll::PwEnvelope next = t.envelope;
  next.ver++;
  next.epk[0]++;
  ASSERT_TRUE(poll::sealPwEnvelope(keep, observer, t.pub, next.epk, next.ver, (const uint8_t*)"next", 4,
                                   next.ct, next.tag));
  poll::TargetState state = session();
  state.login_checked_at = kNow - poll::kAdminRecheckS;
  const poll::RequestedEnvelopeEffect effect = applyEnvelope(state, t.envelope, next);
  EXPECT_FALSE(effect.abort_run);
  EXPECT_EQ(kNow - poll::kAdminRecheckS, state.login_checked_at);
  EXPECT_EQ(1000u, state.logged_in_at);
  poll::replaceEnvelope(t, next);
  uint8_t opened[32];
  memcpy(opened, keep, sizeof(opened));
  poll::openRequestedEnvelope(t, opened, observer);
  ASSERT_STREQ("next", poll::requestedPassword(t));

  poll::PollExchange exchange;
  ExchangeTrace trace;
  driveExchange(exchange, state, exchangeCfg(poll::requestedPassword(t) != nullptr), kNow, true, true, trace);
  EXPECT_EQ(std::vector<uint8_t>{poll::PW_CUSTOM}, trace.pws);
  EXPECT_TRUE(exchange.result().login_answered);
  EXPECT_EQ(kNow, state.login_checked_at);
  EXPECT_EQ(1000u, state.logged_in_at);
}

TEST(PollRequestRuntime, RefusedVerIsNotPolledEvenWhenTheKeyIsStillLoggedIn) {
  poll::RequestedTarget t = target();
  uint8_t secret[32] = {5}, observer[32] = {9};
  seal(t, secret, observer);
  poll::openRequestedEnvelope(t, secret, observer);
  ASSERT_TRUE(poll::noteEnvelopeLoginFails(t, 2));
  poll::TargetState state = session();
  poll::PwEnvelope again = t.envelope;
  again.ct[0]++;
  const poll::RequestedEnvelopeEffect effect = applyEnvelope(state, t.envelope, again);
  EXPECT_FALSE(effect.abort_run);
  EXPECT_FALSE(effect.reset_login_fails);
  expectSessionKept(state);
  EXPECT_EQ(4, state.key_login_fails);
  poll::replaceEnvelope(t, again);
  EXPECT_TRUE(t.login_failed);
  EXPECT_FALSE(poll::requestMayPoll(t));
  poll::OwnerBrief owner = {};
  owner.present = owner.requested = true;
  owner.role = poll::OWNER_ROLE_POLLING;
  EXPECT_FALSE(poll::requestedRunAllowed(t, owner));

  poll::PwEnvelope newer = again;
  newer.ver++;
  poll::TargetState lifted = state;
  const poll::RequestedEnvelopeEffect lift = applyEnvelope(lifted, again, newer);
  EXPECT_FALSE(lift.abort_run);
  EXPECT_EQ(1000u, lifted.logged_in_at);
  poll::replaceEnvelope(t, newer);
  EXPECT_FALSE(t.login_failed);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
