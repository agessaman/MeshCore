#include "helpers/NtpValidation.h"

#include <gtest/gtest.h>

#include <string.h>

using namespace NtpValidation;

namespace {

const uint32_t MIN_EPOCH = 1767225600UL;   // 2026-01-01, the bridge's floor
const uint32_t MAX_EPOCH = 4102444800UL;   // 2100-01-01
const Nonce NONCE = { 0xA1B2C3D4UL, 0x0F1E2D3CUL };

// A well-formed reply to buildRequest(): stratum 2, mode 4, our nonce echoed.
void makeReply(uint8_t out[48], uint32_t unix_epoch = 1789000000UL) {
  memset(out, 0, 48);
  out[0] = (0 << 6) | (4 << 3) | 4;   // LI=0, VN=4, mode=4 (server)
  out[1] = 2;                         // stratum
  writeU32(out + 24, NONCE.seconds);      // originate = our transmit timestamp
  writeU32(out + 28, NONCE.fraction);
  writeU32(out + 32, unix_epoch + kUnixEpochOffset);   // receive
  writeU32(out + 40, unix_epoch + kUnixEpochOffset);   // transmit
}

Result check(const uint8_t* data, size_t len, bool source_matches = true) {
  return validate(data, len, source_matches, NONCE, MIN_EPOCH, MAX_EPOCH);
}

} // namespace

TEST(NtpValidation, RequestIsAClientPacketCarryingTheNonce) {
  uint8_t req[48];
  memset(req, 0xFF, sizeof(req));
  buildRequest(req, NONCE);

  EXPECT_EQ(0u, (req[0] >> 6) & 0x03);   // LI = 0, not the library's bogus alarm
  EXPECT_EQ(4u, (req[0] >> 3) & 0x07);   // version 4
  EXPECT_EQ(3u, req[0] & 0x07);          // mode 3 = client
  EXPECT_EQ(NONCE.seconds, readU32(req + 40));
  EXPECT_EQ(NONCE.fraction, readU32(req + 44));
}

TEST(NtpValidation, ValidReplyIsAccepted) {
  uint8_t pkt[48];
  makeReply(pkt, 1789000000UL);

  Result r = check(pkt, sizeof(pkt));

  EXPECT_EQ(kAccepted, r.reject);
  EXPECT_EQ(1789000000UL, r.epoch);
}

// F07's reproduction: the installed NTPClient accepted a one-byte non-NTP
// datagram and derived epoch 2085978496 from the uninitialised buffer.
TEST(NtpValidation, OneByteNonNtpDatagramIsRejected) {
  const uint8_t junk[1] = { 0x00 };

  Result r = check(junk, sizeof(junk));

  EXPECT_EQ(kShortPacket, r.reject);
  EXPECT_EQ(0u, r.epoch);
}

TEST(NtpValidation, ShortAndEmptyDatagramsAreRejected) {
  uint8_t pkt[48];
  makeReply(pkt);

  for (size_t len = 0; len < 48; len++) {
    EXPECT_EQ(kShortPacket, check(pkt, len).reject) << "len " << len;
  }
  EXPECT_EQ(kShortPacket, check(nullptr, 48).reject);
}

TEST(NtpValidation, ReplyFromAnotherSourceIsRejected) {
  uint8_t pkt[48];
  makeReply(pkt);

  EXPECT_EQ(kWrongSource, check(pkt, sizeof(pkt), /*source_matches=*/false).reject);
}

// An off-path sender that never saw the query cannot echo the nonce.
TEST(NtpValidation, UnsolicitedReplyIsRejected) {
  uint8_t pkt[48];
  makeReply(pkt);
  writeU32(pkt + 24, 0);
  writeU32(pkt + 28, 0);

  EXPECT_EQ(kOriginMismatch, check(pkt, sizeof(pkt)).reject);

  makeReply(pkt);
  writeU32(pkt + 28, NONCE.fraction ^ 1u);   // one bit off is still not ours
  EXPECT_EQ(kOriginMismatch, check(pkt, sizeof(pkt)).reject);
}

TEST(NtpValidation, ClientModeAndWrongVersionAreRejected) {
  uint8_t pkt[48];

  makeReply(pkt);
  pkt[0] = (0 << 6) | (4 << 3) | 3;          // our own request echoed back
  EXPECT_EQ(kBadMode, check(pkt, sizeof(pkt)).reject);

  makeReply(pkt);
  pkt[0] = (0 << 6) | (5 << 3) | 4;          // version 5 does not exist
  EXPECT_EQ(kBadVersion, check(pkt, sizeof(pkt)).reject);

  makeReply(pkt);
  pkt[0] = (0 << 6) | (3 << 3) | 4;          // NTPv3 servers are fine
  EXPECT_EQ(kAccepted, check(pkt, sizeof(pkt)).reject);
}

TEST(NtpValidation, UnsynchronisedServersAreRejected) {
  uint8_t pkt[48];

  makeReply(pkt);
  pkt[0] |= (3 << 6);                        // LI = 3: alarm, clock not set
  EXPECT_EQ(kLeapAlarm, check(pkt, sizeof(pkt)).reject);

  makeReply(pkt);
  pkt[1] = 0;                                // stratum 0: kiss-of-death
  EXPECT_EQ(kBadStratum, check(pkt, sizeof(pkt)).reject);

  makeReply(pkt);
  pkt[1] = 16;                               // unsynchronised
  EXPECT_EQ(kBadStratum, check(pkt, sizeof(pkt)).reject);

  makeReply(pkt);
  pkt[1] = 15;                               // last usable stratum
  EXPECT_EQ(kAccepted, check(pkt, sizeof(pkt)).reject);
}

TEST(NtpValidation, MissingTransmitTimestampIsRejected) {
  uint8_t pkt[48];
  makeReply(pkt);
  writeU32(pkt + 40, 0);

  EXPECT_EQ(kZeroTransmit, check(pkt, sizeof(pkt)).reject);
}

// The bridge's floor is the only defence against a stale or rolled-back clock,
// and nothing previously stopped an absurd forward jump.
TEST(NtpValidation, ImplausibleTimesAreRejected) {
  uint8_t pkt[48];

  makeReply(pkt, MIN_EPOCH - 1);
  EXPECT_EQ(kEpochTooEarly, check(pkt, sizeof(pkt)).reject);

  makeReply(pkt, MIN_EPOCH);
  EXPECT_EQ(kAccepted, check(pkt, sizeof(pkt)).reject);

  makeReply(pkt, MAX_EPOCH + 1);
  EXPECT_EQ(kEpochTooLate, check(pkt, sizeof(pkt)).reject);

  makeReply(pkt, MAX_EPOCH);
  EXPECT_EQ(kAccepted, check(pkt, sizeof(pkt)).reject);
}

// Era 1 begins 2036-02-07; a naive subtraction turns those seconds into 1900.
TEST(NtpValidation, Era1TimestampsConvertForward) {
  EXPECT_EQ(0u, unixFromNtpSeconds(kUnixEpochOffset));
  EXPECT_EQ(2085978496UL, unixFromNtpSeconds(0));          // 2036-02-07
  EXPECT_GT(unixFromNtpSeconds(1000), unixFromNtpSeconds(0xFFFFFFFFu));

  uint8_t pkt[48];
  makeReply(pkt);
  writeU32(pkt + 40, 1000);          // 1000 s into era 1
  Result r = check(pkt, sizeof(pkt));
  EXPECT_EQ(kAccepted, r.reject);
  EXPECT_EQ(2085979496UL, r.epoch);
}

TEST(NtpValidation, EveryRejectionHasAShortReason) {
  for (int i = 0; i <= (int)kEpochTooLate; i++) {
    const char* reason = rejectReason((Reject)i);
    ASSERT_NE(nullptr, reason);
    EXPECT_GT(strlen(reason), 0u);
    EXPECT_LE(strlen(reason), 20u) << "reject " << i;
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
