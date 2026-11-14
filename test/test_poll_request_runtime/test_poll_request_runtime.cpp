#include <gtest/gtest.h>
#include <helpers/poll/PollRequestRuntime.h>

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

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
