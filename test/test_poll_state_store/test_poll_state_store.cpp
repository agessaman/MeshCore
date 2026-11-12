#include <gtest/gtest.h>

#include <stdint.h>
#include <string.h>

#include "helpers/poll/PollStateStore.h"

namespace {

poll::StateRecord sample() {
  poll::StateRecord r;
  poll::stateClear(r);
  r.seq = 7;
  r.has_key = true;
  for (uint8_t i = 0; i < poll::kPubKeySize; i++) r.pub[i] = (uint8_t)(0x40 + i);
  for (uint8_t i = 0; i < poll::kPrvKeySize; i++) r.prv[i] = (uint8_t)(0x90 + i);
  r.key_created_at = 1762900000UL;
  r.tag_high_water = 1762986400UL;
  r.gate.blocked = false;
  r.gate.version_disabled = true;
  r.gate.paused = true;
  r.gate.paused_until = 1763000000UL;
  for (uint8_t i = 0; i < 3; i++) {
    memset(r.targets[i].pub, 0xA0 + i, poll::kPubKeySize);
  }
  r.targets[0].admin_seen = true;
  r.targets[0].admin_removed = true;
  r.targets[1].learned_pw = poll::LEARNED_HELLO;
  r.targets[2].learned_pw = poll::LEARNED_BLANK;
  return r;
}

size_t encode(const poll::StateRecord& r, uint8_t* buf) {
  return poll::encodeState(r, buf, poll::kStateRecordSize);
}

// Re-seal a hand-edited image so only the edited field is wrong.
void reseal(uint8_t* buf, size_t len) {
  poll::statePut32(buf + len - 4, poll::stateCrc32(buf, len - 4));
}

}  // namespace

TEST(PollStateStore, Crc32MatchesZlib) {
  const uint8_t text[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(0xCBF43926UL, poll::stateCrc32(text, sizeof(text)));
  EXPECT_EQ(0UL, poll::stateCrc32(text, 0));
}

TEST(PollStateStore, RoundTripKeepsEveryField) {
  uint8_t buf[poll::kStateRecordSize];
  const poll::StateRecord in = sample();
  ASSERT_EQ(poll::kStateRecordSize, encode(in, buf));

  poll::StateRecord out;
  ASSERT_EQ(poll::STATE_OK, poll::decodeState(buf, sizeof(buf), &out));
  EXPECT_EQ(in.seq, out.seq);
  EXPECT_TRUE(out.has_key);
  EXPECT_EQ(0, memcmp(in.pub, out.pub, sizeof(in.pub)));
  EXPECT_EQ(0, memcmp(in.prv, out.prv, sizeof(in.prv)));
  EXPECT_EQ(in.key_created_at, out.key_created_at);
  EXPECT_EQ(in.tag_high_water, out.tag_high_water);
  EXPECT_FALSE(out.gate.blocked);
  EXPECT_TRUE(out.gate.version_disabled);
  EXPECT_TRUE(out.gate.paused);
  EXPECT_EQ(in.gate.paused_until, out.gate.paused_until);
  for (uint8_t i = 0; i < poll::kMaxTargets; i++) {
    EXPECT_EQ(0, memcmp(in.targets[i].pub, out.targets[i].pub, poll::kPubKeySize)) << (int)i;
    EXPECT_EQ(in.targets[i].admin_seen, out.targets[i].admin_seen) << (int)i;
    EXPECT_EQ(in.targets[i].admin_removed, out.targets[i].admin_removed) << (int)i;
    EXPECT_EQ(in.targets[i].learned_pw, out.targets[i].learned_pw) << (int)i;
  }
}

TEST(PollStateStore, EmptyRecordRoundTrips) {
  uint8_t buf[poll::kStateRecordSize];
  poll::StateRecord in;
  poll::stateClear(in);
  ASSERT_EQ(poll::kStateRecordSize, encode(in, buf));
  poll::StateRecord out;
  ASSERT_EQ(poll::STATE_OK, poll::decodeState(buf, sizeof(buf), &out));
  EXPECT_FALSE(out.has_key);
  EXPECT_EQ(0U, out.tag_high_water);
}

TEST(PollStateStore, LayoutIsFixed) {
  // A size change needs a version bump; this catches one made by accident.
  EXPECT_EQ(398U, poll::kStateRecordSize);
  uint8_t buf[poll::kStateRecordSize];
  encode(sample(), buf);
  EXPECT_EQ(0x52, buf[0]);   // "RPST", little-endian magic
  EXPECT_EQ(0x50, buf[1]);
  EXPECT_EQ(0x53, buf[2]);
  EXPECT_EQ(0x54, buf[3]);
  EXPECT_EQ(1, buf[4]);
  EXPECT_EQ(0, buf[5]);
  EXPECT_EQ(poll::kStateBodySizeV1, (size_t)poll::stateGet16(buf + 6));
}

TEST(PollStateStore, EncodeRefusesASmallBuffer) {
  uint8_t buf[poll::kStateRecordSize];
  EXPECT_EQ(0U, poll::encodeState(sample(), buf, sizeof(buf) - 1));
  EXPECT_EQ(0U, poll::encodeState(sample(), nullptr, sizeof(buf)));
}

TEST(PollStateStore, NoBytesIsEmpty) {
  poll::StateRecord out;
  EXPECT_EQ(poll::STATE_EMPTY, poll::decodeState(nullptr, 0, &out));
  uint8_t one = 0;
  EXPECT_EQ(poll::STATE_EMPTY, poll::decodeState(&one, 0, &out));
}

TEST(PollStateStore, EveryTruncationIsCorrupt) {
  uint8_t buf[poll::kStateRecordSize];
  encode(sample(), buf);
  poll::StateRecord out;
  for (size_t n = 1; n < sizeof(buf); n++) {
    EXPECT_EQ(poll::STATE_CORRUPT, poll::decodeState(buf, n, &out)) << n;
  }
}

TEST(PollStateStore, TrailingBytesAreCorrupt) {
  uint8_t buf[poll::kStateRecordSize + 1];
  encode(sample(), buf);
  buf[poll::kStateRecordSize] = 0;
  poll::StateRecord out;
  EXPECT_EQ(poll::STATE_CORRUPT, poll::decodeState(buf, sizeof(buf), &out));
}

TEST(PollStateStore, EverySingleBitFlipIsCorrupt) {
  uint8_t buf[poll::kStateRecordSize];
  encode(sample(), buf);
  poll::StateRecord out;
  for (size_t i = 0; i < sizeof(buf); i++) {
    for (uint8_t b = 0; b < 8; b++) {
      buf[i] ^= (uint8_t)(1u << b);
      uint8_t r = poll::decodeState(buf, sizeof(buf), &out);
      // A flip in the version field can still be a valid-looking newer version
      // only if the CRC also matches, which a single flip cannot achieve.
      EXPECT_EQ(poll::STATE_CORRUPT, r) << "byte " << i << " bit " << (int)b;
      buf[i] ^= (uint8_t)(1u << b);
    }
  }
  EXPECT_EQ(poll::STATE_OK, poll::decodeState(buf, sizeof(buf), &out));
}

TEST(PollStateStore, BadMagicIsCorruptEvenWithAGoodCrc) {
  uint8_t buf[poll::kStateRecordSize];
  encode(sample(), buf);
  buf[0] = 'X';
  reseal(buf, sizeof(buf));
  poll::StateRecord out;
  EXPECT_EQ(poll::STATE_CORRUPT, poll::decodeState(buf, sizeof(buf), &out));
}

TEST(PollStateStore, NewerVersionIsReportedNotParsed) {
  uint8_t buf[poll::kStateRecordSize];
  encode(sample(), buf);
  poll::statePut16(buf + 4, poll::kStateVersion + 1);
  reseal(buf, sizeof(buf));
  poll::StateRecord out;
  EXPECT_EQ(poll::STATE_NEWER, poll::decodeState(buf, sizeof(buf), &out));
  EXPECT_FALSE(out.has_key);
}

TEST(PollStateStore, NewerVersionWithADifferentBodySizeIsStillNewer) {
  // A later layout may grow; the CRC still covers header + body.
  uint8_t buf[poll::kStateRecordSize + 10];
  size_t n = encode(sample(), buf);
  ASSERT_EQ(poll::kStateRecordSize, n);
  memset(buf + n - 4, 0x5A, 10);
  const size_t body = poll::kStateBodySizeV1 + 10;
  poll::statePut16(buf + 4, 2);
  poll::statePut16(buf + 6, (uint16_t)body);
  reseal(buf, sizeof(buf));
  poll::StateRecord out;
  EXPECT_EQ(poll::STATE_NEWER, poll::decodeState(buf, sizeof(buf), &out));
}

TEST(PollStateStore, NewerVersionWithABadCrcIsCorrupt) {
  uint8_t buf[poll::kStateRecordSize];
  encode(sample(), buf);
  poll::statePut16(buf + 4, poll::kStateVersion + 1);
  poll::StateRecord out;
  EXPECT_EQ(poll::STATE_CORRUPT, poll::decodeState(buf, sizeof(buf), &out));
}

TEST(PollStateStore, VersionZeroOrWrongBodySizeIsCorrupt) {
  uint8_t buf[poll::kStateRecordSize];
  poll::StateRecord out;

  encode(sample(), buf);
  poll::statePut16(buf + 4, 0);
  reseal(buf, sizeof(buf));
  EXPECT_EQ(poll::STATE_CORRUPT, poll::decodeState(buf, sizeof(buf), &out));

  // Same version, shorter body that still seals: not a layout this firmware wrote.
  uint8_t small[poll::kStateRecordSize];
  encode(sample(), small);
  const size_t body = poll::kStateBodySizeV1 - 34;
  poll::statePut16(small + 6, (uint16_t)body);
  const size_t len = poll::kStateHeaderSize + body + 4;
  reseal(small, len);
  EXPECT_EQ(poll::STATE_CORRUPT, poll::decodeState(small, len, &out));
}

TEST(PollStateStore, ImpossibleFieldValuesAreCorrupt) {
  uint8_t buf[poll::kStateRecordSize];
  poll::StateRecord out;
  const size_t key_flag_at = poll::kStateHeaderSize + 4;
  const size_t gate_at = key_flag_at + 1 + poll::kPubKeySize + poll::kPrvKeySize + 8;
  const size_t first_target_flags = gate_at + 5 + poll::kPubKeySize;

  encode(sample(), buf);
  buf[key_flag_at] = 2;
  reseal(buf, sizeof(buf));
  EXPECT_EQ(poll::STATE_CORRUPT, poll::decodeState(buf, sizeof(buf), &out));

  encode(sample(), buf);
  buf[gate_at] = 8;
  reseal(buf, sizeof(buf));
  EXPECT_EQ(poll::STATE_CORRUPT, poll::decodeState(buf, sizeof(buf), &out));

  encode(sample(), buf);
  buf[first_target_flags] = 4;
  reseal(buf, sizeof(buf));
  EXPECT_EQ(poll::STATE_CORRUPT, poll::decodeState(buf, sizeof(buf), &out));

  encode(sample(), buf);
  buf[first_target_flags + 1] = poll::LEARNED_HELLO + 1;
  reseal(buf, sizeof(buf));
  EXPECT_EQ(poll::STATE_CORRUPT, poll::decodeState(buf, sizeof(buf), &out));
}

TEST(PollStateStore, EncodeNeverWritesAnOutOfRangeLearnedPassword) {
  poll::StateRecord in = sample();
  in.targets[3].learned_pw = 9;
  uint8_t buf[poll::kStateRecordSize];
  encode(in, buf);
  poll::StateRecord out;
  ASSERT_EQ(poll::STATE_OK, poll::decodeState(buf, sizeof(buf), &out));
  EXPECT_EQ(poll::LEARNED_NONE, out.targets[3].learned_pw);
}

TEST(PollStateStore, PickPrefersTheHighestValidSeq) {
  poll::StateRecord recs[3];
  for (auto& r : recs) poll::stateClear(r);
  recs[0].seq = 4;
  recs[1].seq = 5;
  recs[2].seq = 3;
  uint8_t results[3] = {poll::STATE_OK, poll::STATE_OK, poll::STATE_OK};
  uint8_t why = 0xFF;
  EXPECT_EQ(1, poll::pickState(results, recs, 3, &why));
  EXPECT_EQ(poll::STATE_OK, why);

  results[1] = poll::STATE_CORRUPT;   // an interrupted temp write
  EXPECT_EQ(0, poll::pickState(results, recs, 3, &why));
  EXPECT_EQ(poll::STATE_OK, why);
}

TEST(PollStateStore, PickReportsNewerAnywhere) {
  poll::StateRecord recs[2];
  for (auto& r : recs) poll::stateClear(r);
  uint8_t results[2] = {poll::STATE_OK, poll::STATE_NEWER};
  uint8_t why = 0xFF;
  EXPECT_EQ(-1, poll::pickState(results, recs, 2, &why));
  EXPECT_EQ(poll::STATE_NEWER, why);
}

TEST(PollStateStore, PickWithNothingUsable) {
  poll::StateRecord recs[3];
  for (auto& r : recs) poll::stateClear(r);
  uint8_t empty[3] = {poll::STATE_EMPTY, poll::STATE_EMPTY, poll::STATE_EMPTY};
  uint8_t why = 0xFF;
  EXPECT_EQ(-1, poll::pickState(empty, recs, 3, &why));
  EXPECT_EQ(poll::STATE_EMPTY, why);

  uint8_t bad[3] = {poll::STATE_EMPTY, poll::STATE_CORRUPT, poll::STATE_EMPTY};
  EXPECT_EQ(-1, poll::pickState(bad, recs, 3, &why));
  EXPECT_EQ(poll::STATE_CORRUPT, why);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
