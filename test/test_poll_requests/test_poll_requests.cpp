#include <gtest/gtest.h>

#include <stdint.h>
#include <string.h>

#include "helpers/poll/PollRequests.h"

namespace {

void setKey(uint8_t key[poll::kPubKeySize], uint8_t id) {
  memset(key, 0, poll::kPubKeySize);
  key[0] = id;
}

poll::PwEnvelope envClear() {
  poll::PwEnvelope e;
  memset(&e, 0, sizeof(e));
  return e;
}

poll::PwEnvelope envGood(uint32_t ver, uint8_t mark) {
  poll::PwEnvelope e = envClear();
  e.present = true;
  e.epk[0] = mark;
  e.ct[0] = (uint8_t)(mark + 1);
  e.tag[0] = (uint8_t)(mark + 2);
  e.ver = ver;
  return e;
}

poll::PwEnvelope envBad() {
  poll::PwEnvelope e = envClear();
  e.bad = true;
  return e;
}

poll::RequestedEntry offerEntry(uint8_t id, poll::PwEnvelope env, bool requested = true) {
  poll::RequestedEntry e;
  memset(&e, 0, sizeof(e));
  setKey(e.pub, id);
  e.entry.present = true;
  e.entry.requested = requested;
  e.entry.standby_rank = 1;
  e.entry.pw = env;
  return e;
}

void hold(poll::RequestTable& table, uint8_t id, poll::PwEnvelope env, char fill) {
  if (table.count >= poll::kMaxTargets) return;
  poll::RequestedTarget& t = table.targets[table.count];
  memset(&t, 0, sizeof(t));
  t.held = true;
  setKey(t.pub, id);
  t.envelope = env;
  t.pw_bad = env.bad;
  memset(t.pw, fill, sizeof(t.pw));
  table.count++;
}

bool pwIs(const char pw[16], char fill) {
  for (int i = 0; i < 16; i++) if (pw[i] != fill) return false;
  return true;
}

bool tablePwClear(const poll::RequestTable& table) {
  for (uint8_t i = 0; i < poll::kMaxTargets; i++) {
    if (!pwIs(table.targets[i].pw, 0)) return false;
  }
  return true;
}

poll::RequestOffer offerOf(bool ok, const uint8_t (*local)[poll::kPubKeySize], uint8_t local_count,
                           const poll::RequestedEntry* entries, uint8_t count) {
  poll::RequestOffer o;
  memset(&o, 0, sizeof(o));
  o.owners_ok = ok;
  o.local = local;
  o.local_count = local_count;
  o.entries = entries;
  o.count = count;
  return o;
}

}  // namespace

TEST(PollRequests, AllowedAndFreeSlots) {
  EXPECT_TRUE(poll::requestsAllowed(true, true));
  EXPECT_FALSE(poll::requestsAllowed(false, true));
  EXPECT_FALSE(poll::requestsAllowed(true, false));
  EXPECT_FALSE(poll::requestsAllowed(false, false));

  EXPECT_EQ(poll::kMaxTargets, poll::requestFreeSlots(0, 0));
  EXPECT_EQ(3, poll::requestFreeSlots(3, 2));
  EXPECT_EQ(0, poll::requestFreeSlots(7, 1));
  EXPECT_EQ(0, poll::requestFreeSlots(8, 0));
  EXPECT_EQ(0, poll::requestFreeSlots(5, 4));
  EXPECT_EQ(0, poll::requestFreeSlots(9, 0));
  EXPECT_EQ(0, poll::requestFreeSlots(0, 9));
  EXPECT_EQ(0, poll::requestFreeSlots(200, 100));
}

TEST(PollRequests, MustDropWhenOptInUrlOrRolesFail) {
  const uint32_t now = 100000;
  EXPECT_FALSE(poll::requestsMustDrop(true, true, false, now, now));
  EXPECT_TRUE(poll::requestsMustDrop(false, true, false, now, now));
  EXPECT_TRUE(poll::requestsMustDrop(true, false, false, now, now));
  EXPECT_TRUE(poll::requestsMustDrop(true, true, true, now, now));
  EXPECT_TRUE(poll::requestsMustDrop(true, true, false, 0, now));
  EXPECT_FALSE(poll::requestsMustDrop(true, true, false, now - 1800, now));
  EXPECT_TRUE(poll::requestsMustDrop(true, true, false, now - 1801, now));
  EXPECT_FALSE(poll::requestsMustDrop(true, true, false, now + 10, now));
}

TEST(PollRequests, ReconcileAdoptsInOfferOrderUntilFull) {
  poll::RequestTable table;
  memset(&table, 0, sizeof(table));
  uint8_t local[7][poll::kPubKeySize];
  setKey(local[0], 0x10);
  for (uint8_t i = 1; i < 7; i++) setKey(local[i], (uint8_t)(0xA0 + i));
  poll::RequestedEntry entries[4];
  entries[0] = offerEntry(0x10, envGood(1, 1));    // already configured
  entries[1] = offerEntry(0x21, envGood(3, 2));
  entries[2] = offerEntry(0x22, envBad(), false);  // not requested
  entries[3] = offerEntry(0x23, envGood(4, 4));

  poll::RequestDelta delta;
  // Seven configured targets leave one seat, taken by the first new request.
  poll::RequestOffer offer = offerOf(true, local, 7, entries, 4);
  poll::reconcile(table, offer, 1, delta);
  ASSERT_EQ(1, table.count);
  EXPECT_EQ(1, delta.added_count);
  EXPECT_EQ(0, delta.dropped_count);
  EXPECT_EQ(0x21, table.targets[0].pub[0]);
  EXPECT_EQ(0x21, delta.added[0][0]);
  EXPECT_TRUE(table.targets[0].held);
  EXPECT_TRUE(table.targets[0].envelope.present);
  EXPECT_EQ(3u, table.targets[0].envelope.ver);
  EXPECT_EQ(2, table.targets[0].envelope.epk[0]);
  EXPECT_FALSE(table.targets[0].pw_ready);
  EXPECT_FALSE(table.targets[0].login_failed);
  EXPECT_TRUE(pwIs(table.targets[0].pw, 0));
  EXPECT_EQ(0, poll::requestFreeSlots(7, table.count));

  memset(table.targets[0].pw, 'Q', sizeof(table.targets[0].pw));
  table.targets[0].pw_ready = true;
  table.targets[0].pw_ver = 3;
  poll::reconcile(table, offer, 999999, delta);
  EXPECT_EQ(0, delta.added_count);
  EXPECT_EQ(0, delta.dropped_count);
  EXPECT_EQ(1, table.count);
  EXPECT_TRUE(table.targets[0].pw_ready);
  EXPECT_TRUE(pwIs(table.targets[0].pw, 'Q'));
  EXPECT_EQ(3u, table.targets[0].pw_ver);
}

TEST(PollRequests, EnvelopeChangeClearsPasswordAndRefusal) {
  poll::RequestTable table;
  memset(&table, 0, sizeof(table));
  hold(table, 0x31, envGood(1, 9), 'S');
  table.targets[0].pw_ready = true;
  table.targets[0].pw_ver = 1;
  table.targets[0].login_failed = true;
  table.targets[0].failed_ver = 1;

  poll::RequestedEntry same = offerEntry(0x31, envGood(1, 9));
  poll::RequestDelta delta;
  poll::reconcile(table, offerOf(true, nullptr, 0, &same, 1), 50, delta);
  EXPECT_TRUE(table.targets[0].pw_ready);
  EXPECT_TRUE(table.targets[0].login_failed);
  EXPECT_TRUE(pwIs(table.targets[0].pw, 'S'));
  EXPECT_EQ(0, delta.added_count);

  poll::PwEnvelope bytes = envGood(1, 9);
  bytes.ct[0] = 0xee;
  poll::RequestedEntry changed = offerEntry(0x31, bytes);
  poll::reconcile(table, offerOf(true, nullptr, 0, &changed, 1), 50, delta);
  EXPECT_FALSE(table.targets[0].pw_ready);
  EXPECT_FALSE(table.targets[0].login_failed);
  EXPECT_EQ(0u, table.targets[0].pw_ver);
  EXPECT_EQ(0u, table.targets[0].failed_ver);
  EXPECT_TRUE(pwIs(table.targets[0].pw, 0));
  EXPECT_EQ(0xee, table.targets[0].envelope.ct[0]);
  EXPECT_EQ(1u, table.targets[0].envelope.ver);
  EXPECT_FALSE(poll::requestMayPoll(table.targets[0]));

  memset(table.targets[0].pw, 'S', sizeof(table.targets[0].pw));
  table.targets[0].pw_ready = true;
  table.targets[0].login_failed = true;
  table.targets[0].failed_ver = 1;
  poll::RequestedEntry next = offerEntry(0x31, envGood(2, 9));
  poll::reconcile(table, offerOf(true, nullptr, 0, &next, 1), 50, delta);
  EXPECT_EQ(2u, table.targets[0].envelope.ver);
  EXPECT_FALSE(table.targets[0].login_failed);
  EXPECT_FALSE(table.targets[0].pw_ready);
  EXPECT_TRUE(pwIs(table.targets[0].pw, 0));

  poll::RequestedEntry blank = offerEntry(0x31, envClear());
  table.targets[0].pw_ready = true;
  table.targets[0].login_failed = true;
  table.targets[0].failed_ver = 2;
  memset(table.targets[0].pw, 'S', sizeof(table.targets[0].pw));
  poll::reconcile(table, offerOf(true, nullptr, 0, &blank, 1), 50, delta);
  EXPECT_FALSE(table.targets[0].envelope.present);
  EXPECT_FALSE(table.targets[0].login_failed);
  EXPECT_TRUE(pwIs(table.targets[0].pw, 0));
  EXPECT_TRUE(poll::requestMayPoll(table.targets[0]));

  // The same blank envelope does not clear a refusal: ver did not change.
  table.targets[0].login_failed = true;
  table.targets[0].failed_ver = 0;
  poll::reconcile(table, offerOf(true, nullptr, 0, &blank, 1), 50, delta);
  EXPECT_TRUE(table.targets[0].login_failed);
  EXPECT_FALSE(poll::requestMayPoll(table.targets[0]));

  poll::RequestedEntry bad = offerEntry(0x31, envBad());
  poll::reconcile(table, offerOf(true, nullptr, 0, &bad, 1), 50, delta);
  EXPECT_TRUE(table.targets[0].pw_bad);
  EXPECT_TRUE(table.targets[0].envelope.bad);
  EXPECT_FALSE(poll::requestMayPoll(table.targets[0]));
}

TEST(PollRequests, ValidAnswerDropsTargetsItNoLongerRequests) {
  poll::RequestTable table;
  memset(&table, 0, sizeof(table));
  hold(table, 0x41, envGood(1, 1), 'A');
  hold(table, 0x42, envGood(1, 2), 'B');
  poll::RequestedEntry keep = offerEntry(0x42, envGood(1, 2));
  poll::RequestDelta delta;
  poll::reconcile(table, offerOf(true, nullptr, 0, &keep, 1), 10, delta);
  ASSERT_EQ(1, table.count);
  EXPECT_EQ(0x42, table.targets[0].pub[0]);
  EXPECT_TRUE(pwIs(table.targets[0].pw, 'B'));
  EXPECT_EQ(1, delta.dropped_count);
  EXPECT_EQ(0x41, delta.dropped[0][0]);
  EXPECT_EQ(0, delta.added_count);
  EXPECT_TRUE(pwIs(table.targets[1].pw, 0));

  // The omitted target's seat is free for whoever the answer lists instead.
  poll::RequestedEntry neu = offerEntry(0x43, envGood(5, 3));
  poll::reconcile(table, offerOf(true, nullptr, 7, &neu, 1), 11, delta);
  ASSERT_EQ(1, table.count);
  EXPECT_EQ(0x43, table.targets[0].pub[0]);
  EXPECT_EQ(1, delta.dropped_count);
  EXPECT_EQ(0x42, delta.dropped[0][0]);
  EXPECT_EQ(1, delta.added_count);
  EXPECT_EQ(0x43, delta.added[0][0]);
  EXPECT_TRUE(pwIs(table.targets[1].pw, 0));
  EXPECT_EQ(0, poll::requestFreeSlots(7, table.count));

  poll::reconcile(table, offerOf(true, nullptr, 0, nullptr, 0), 12, delta);
  EXPECT_EQ(0, table.count);
  EXPECT_EQ(1, delta.dropped_count);
  EXPECT_TRUE(tablePwClear(table));

  hold(table, 0x44, envGood(1, 4), 'C');
  poll::RequestedEntry quiet = offerEntry(0x44, envGood(1, 4), false);
  poll::reconcile(table, offerOf(true, nullptr, 0, &quiet, 1), 13, delta);
  EXPECT_EQ(0, table.count);
  EXPECT_EQ(1, delta.dropped_count);
  EXPECT_EQ(0x44, delta.dropped[0][0]);
  EXPECT_TRUE(tablePwClear(table));
}

TEST(PollRequests, InvalidOwnersAnswerKeepsHeldTargets) {
  poll::RequestTable table;
  memset(&table, 0, sizeof(table));
  hold(table, 0x51, envGood(1, 1), 'K');
  poll::RequestDelta delta;
  poll::reconcile(table, offerOf(false, nullptr, 0, nullptr, 0), 0, delta);
  EXPECT_EQ(1, table.count);
  EXPECT_TRUE(pwIs(table.targets[0].pw, 'K'));
  EXPECT_EQ(0, delta.dropped_count);
  EXPECT_EQ(0, delta.added_count);

  poll::reconcile(table, offerOf(false, nullptr, 0, nullptr, 0), 0xffffffffu, delta);
  EXPECT_EQ(1, table.count);
  EXPECT_TRUE(pwIs(table.targets[0].pw, 'K'));
  EXPECT_TRUE(poll::requestsMustDrop(true, true, false, 1, 1 + 1801));
}

TEST(PollRequests, LocalTargetTakesTheRequestedSlot) {
  poll::RequestTable table;
  memset(&table, 0, sizeof(table));
  hold(table, 0x61, envGood(1, 1), 'P');
  hold(table, 0x62, envClear(), 'Q');
  uint8_t local[8][poll::kPubKeySize];
  for (uint8_t i = 0; i < 7; i++) setKey(local[i], (uint8_t)(0x70 + i));
  setKey(local[7], 0x61);
  poll::RequestedEntry neu = offerEntry(0x63, envGood(1, 3));
  poll::RequestDelta delta;
  // Roles are stale, but the operator's target still takes the slot.
  poll::reconcile(table, offerOf(false, local, 8, &neu, 1), 1, delta);
  ASSERT_EQ(1, table.count);
  EXPECT_EQ(0x62, table.targets[0].pub[0]);
  EXPECT_TRUE(pwIs(table.targets[0].pw, 'Q'));
  EXPECT_EQ(1, delta.dropped_count);
  EXPECT_EQ(0x61, delta.dropped[0][0]);
  EXPECT_EQ(0, delta.added_count);
  EXPECT_EQ(0, poll::requestFreeSlots(8, table.count));
  EXPECT_TRUE(pwIs(table.targets[1].pw, 0));

  uint8_t one[1][poll::kPubKeySize];
  setKey(one[0], 0x62);
  poll::reconcile(table, offerOf(true, one, 1, &neu, 1), 2, delta);
  ASSERT_EQ(1, table.count);
  EXPECT_EQ(0x63, table.targets[0].pub[0]);
  EXPECT_EQ(0x62, delta.dropped[0][0]);
  EXPECT_EQ(0x63, delta.added[0][0]);
  EXPECT_EQ(6, poll::requestFreeSlots(1, table.count));
  EXPECT_TRUE(pwIs(table.targets[1].pw, 0));
}

TEST(PollRequests, DropAllWipesEveryPassword) {
  poll::RequestTable table;
  memset(&table, 0, sizeof(table));
  hold(table, 0x71, envGood(1, 1), 'Z');
  hold(table, 0x72, envBad(), 'Y');
  poll::RequestDelta delta;
  poll::dropAll(table, delta);
  EXPECT_EQ(0, table.count);
  EXPECT_FALSE(table.targets[0].held);
  EXPECT_FALSE(table.targets[1].held);
  EXPECT_EQ(2, delta.dropped_count);
  EXPECT_EQ(0x71, delta.dropped[0][0]);
  EXPECT_EQ(0x72, delta.dropped[1][0]);
  EXPECT_TRUE(tablePwClear(table));

  poll::dropAll(table, delta);
  EXPECT_EQ(0, delta.dropped_count);
  EXPECT_TRUE(tablePwClear(table));

  hold(table, 0x73, envGood(4, 4), 'M');
  EXPECT_TRUE(poll::requestsMustDrop(false, true, false, 10, 10));
  poll::dropAll(table, delta);
  EXPECT_EQ(0, table.count);
  EXPECT_EQ(0x73, delta.dropped[0][0]);
  EXPECT_TRUE(tablePwClear(table));
}

TEST(PollRequests, MayPollRequiresAUsablePassword) {
  poll::RequestedTarget t;
  memset(&t, 0, sizeof(t));
  EXPECT_FALSE(poll::requestMayPoll(t));

  t.held = true;
  EXPECT_TRUE(poll::requestMayPoll(t));  // no envelope: blank, then hello

  t.login_failed = true;
  t.failed_ver = 0;
  EXPECT_FALSE(poll::requestMayPoll(t));
  t.failed_ver = 3;
  EXPECT_TRUE(poll::requestMayPoll(t));
  t.login_failed = false;
  t.failed_ver = 0;

  t.envelope = envGood(7, 1);
  t.pw_bad = false;
  EXPECT_FALSE(poll::requestMayPoll(t));
  t.pw_ready = true;
  t.pw_ver = 7;
  EXPECT_TRUE(poll::requestMayPoll(t));

  t.login_failed = true;
  t.failed_ver = 7;
  EXPECT_FALSE(poll::requestMayPoll(t));
  t.failed_ver = 6;
  EXPECT_TRUE(poll::requestMayPoll(t));

  t.pw_bad = true;
  EXPECT_FALSE(poll::requestMayPoll(t));
  t.pw_bad = false;
  t.envelope = envBad();
  t.pw_ready = true;
  EXPECT_FALSE(poll::requestMayPoll(t));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
