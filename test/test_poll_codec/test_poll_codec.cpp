#include <gtest/gtest.h>

#include <string.h>

#include "MeshCore.h"
#include "Packet.h"
#include "helpers/TxtDataHelpers.h"
#include "helpers/poll/PollPacketCodec.h"

static_assert(poll::kPayloadTypeResponse == PAYLOAD_TYPE_RESPONSE, "PAYLOAD_TYPE_RESPONSE");
static_assert(poll::kPayloadTypePath == PAYLOAD_TYPE_PATH, "PAYLOAD_TYPE_PATH");
static_assert(poll::kCipherMacSize == CIPHER_MAC_SIZE, "CIPHER_MAC_SIZE");
static_assert(poll::kCipherBlockSize == CIPHER_BLOCK_SIZE, "CIPHER_BLOCK_SIZE");
static_assert(poll::kMinCandidatePayload == 2u + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE,
              "candidate minimum");
static_assert(poll::kMaxPathBytes == MAX_PATH_SIZE, "MAX_PATH_SIZE");
static_assert(poll::kSetPermFlags == (uint8_t)((0 & 3) | (TXT_TYPE_CLI_DATA << 2)),
              "CLI command flags");

namespace {

void ExpectBytes(const uint8_t* got, size_t n, const uint8_t* exp, size_t m) {
  ASSERT_EQ(m, n);
  for (size_t i = 0; i < n; i++) {
    EXPECT_EQ(exp[i], got[i]) << "byte " << i;
  }
}

poll::TargetState BlankTarget() {
  poll::TargetState t;
  memset(&t, 0, sizeof(t));
  return t;
}

}  // namespace

TEST(PollPacketCodec, LoginPlainIsTimestampThenPasswordBytes) {
  uint8_t buf[16];
  memset(buf, 0xEE, sizeof(buf));
  EXPECT_EQ(4u, poll::buildLoginPlain(buf, sizeof(buf), 0x11223344, poll::PW_BLANK));
  const uint8_t blank[] = {0x44, 0x33, 0x22, 0x11};
  ExpectBytes(buf, 4, blank, sizeof(blank));

  memset(buf, 0xEE, sizeof(buf));
  EXPECT_EQ(9u, poll::buildLoginPlain(buf, sizeof(buf), 0x11223344, poll::PW_HELLO));
  const uint8_t hello[] = {0x44, 0x33, 0x22, 0x11, 'h', 'e', 'l', 'l', 'o'};
  ExpectBytes(buf, 9, hello, sizeof(hello));
  EXPECT_EQ(0xEE, buf[9]);  // no trailing NUL in the plaintext length
}

TEST(PollPacketCodec, LoginRefusesAnyOtherPassword) {
  uint8_t buf[16];
  memset(buf, 0xEE, sizeof(buf));
  EXPECT_EQ(0u, poll::buildLoginPlain(buf, sizeof(buf), 1, 2));
  EXPECT_EQ(0u, poll::buildLoginPlain(buf, sizeof(buf), 1, 255));
  EXPECT_EQ(0u, poll::buildLoginPlain(buf, sizeof(buf), 1, (uint8_t)(poll::PW_HELLO + 1)));
  EXPECT_EQ(0xEE, buf[0]);
  EXPECT_EQ(0u, poll::buildLoginPlain(nullptr, 16, 1, poll::PW_BLANK));
  EXPECT_EQ(0u, poll::buildLoginPlain(buf, 3, 1, poll::PW_BLANK));
  EXPECT_EQ(0u, poll::buildLoginPlain(buf, 8, 1, poll::PW_HELLO));
  EXPECT_EQ(0xEE, buf[0]);
}

TEST(PollPacketCodec, RequestPlainMatchesSendRequest) {
  const uint8_t rnd[] = {0xDE, 0xAD, 0xBE, 0xEF};
  uint8_t buf[16];
  memset(buf, 0xEE, sizeof(buf));
  EXPECT_EQ(poll::kRequestPlainLen,
            poll::buildRequestPlain(buf, sizeof(buf), 0xA1B2C3D4, poll::kReqStatus, rnd));
  const uint8_t status[] = {0xD4, 0xC3, 0xB2, 0xA1, poll::kReqStatus, 0, 0, 0, 0,
                            0xDE, 0xAD, 0xBE, 0xEF};
  ExpectBytes(buf, 13, status, sizeof(status));

  EXPECT_EQ(poll::kRequestPlainLen,
            poll::buildRequestPlain(buf, sizeof(buf), 0xA1B2C3D4, poll::kReqTelemetry, rnd));
  const uint8_t telem[] = {0xD4, 0xC3, 0xB2, 0xA1, poll::kReqTelemetry, 0, 0, 0, 0,
                           0xDE, 0xAD, 0xBE, 0xEF};
  ExpectBytes(buf, 13, telem, sizeof(telem));
}

TEST(PollPacketCodec, RequestRefusesTheReqKindEnumAndAShortBuffer) {
  const uint8_t rnd[] = {1, 2, 3, 4};
  uint8_t buf[16];
  memset(buf, 0xEE, sizeof(buf));
  // ReqKind::REQ_TELEMETRY is 0, which is not the wire type 0x03. REQ_STATUS
  // happens to equal kReqStatus (both 1), so only the telemetry enum is refused.
  EXPECT_EQ(0u, poll::buildRequestPlain(buf, sizeof(buf), 1, poll::REQ_TELEMETRY, rnd));
  EXPECT_EQ(0u, poll::buildRequestPlain(buf, sizeof(buf), 1, 2, rnd));
  EXPECT_EQ(0u, poll::buildRequestPlain(buf, sizeof(buf), 1, 0x00, rnd));
  EXPECT_EQ(0u, poll::buildRequestPlain(buf, sizeof(buf), 1, 0x02, rnd));
  EXPECT_EQ(0u, poll::buildRequestPlain(buf, 12, 1, poll::kReqStatus, rnd));
  EXPECT_EQ(0u, poll::buildRequestPlain(nullptr, 13, 1, poll::kReqStatus, rnd));
  EXPECT_EQ(0u, poll::buildRequestPlain(buf, sizeof(buf), 1, poll::kReqStatus, nullptr));
  EXPECT_EQ(0xEE, buf[0]);
}

TEST(PollPacketCodec, TeachPlainMatchesCreatePathReturnWithNoExtra) {
  const uint8_t rnd[] = {0x11, 0x22, 0x33, 0x44};
  uint8_t buf[16];

  EXPECT_EQ(6u, poll::buildTeachPlain(buf, sizeof(buf), nullptr, 0, rnd));
  const uint8_t direct[] = {0x00, 0xFF, 0x11, 0x22, 0x33, 0x44};
  ExpectBytes(buf, 6, direct, sizeof(direct));

  const uint8_t one[] = {0x41};
  EXPECT_EQ(7u, poll::buildTeachPlain(buf, sizeof(buf), one, 0x01, rnd));
  const uint8_t one_hop[] = {0x01, 0x41, 0xFF, 0x11, 0x22, 0x33, 0x44};
  ExpectBytes(buf, 7, one_hop, sizeof(one_hop));

  const uint8_t relay[] = {0x41, 0x28, 0xA6};
  EXPECT_EQ(9u, poll::buildTeachPlain(buf, sizeof(buf), relay, 0x81, rnd));
  const uint8_t three[] = {0x81, 0x41, 0x28, 0xA6, 0xFF, 0x11, 0x22, 0x33, 0x44};
  ExpectBytes(buf, 9, three, sizeof(three));
}

TEST(PollPacketCodec, TeachRefusesABadPathLen) {
  const uint8_t rnd[] = {1, 2, 3, 4};
  const uint8_t path[8] = {0};
  uint8_t buf[80];
  memset(buf, 0xEE, sizeof(buf));
  EXPECT_EQ(0u, poll::buildTeachPlain(buf, sizeof(buf), path, 0xC0, rnd));  // hash size 4
  EXPECT_EQ(0u, poll::buildTeachPlain(buf, sizeof(buf), path, 0xC1, rnd));
  EXPECT_EQ(0u, poll::buildTeachPlain(buf, sizeof(buf), path, 0x61, rnd));  // 33 * 2 > 64
  EXPECT_EQ(0u, poll::buildTeachPlain(buf, sizeof(buf), nullptr, 0x01, rnd));
  EXPECT_EQ(0u, poll::buildTeachPlain(buf, 5, nullptr, 0, rnd));
  EXPECT_EQ(0u, poll::buildTeachPlain(nullptr, 16, nullptr, 0, rnd));
  EXPECT_EQ(0u, poll::buildTeachPlain(buf, sizeof(buf), nullptr, 0, nullptr));
  EXPECT_EQ(0xEE, buf[0]);
  EXPECT_TRUE(poll::pathBodyLen(0, nullptr));
  uint8_t n = 99;
  EXPECT_TRUE(poll::pathBodyLen(0x81, &n));
  EXPECT_EQ(3, n);
}

TEST(PollPacketCodec, SetPermPlainIsLowercaseCliCommand) {
  uint8_t pub[32];
  for (int i = 0; i < 32; i++) pub[i] = (uint8_t)i;
  pub[0] = 0xAB;
  pub[1] = 0x0C;
  uint8_t buf[96];
  memset(buf, 0xEE, sizeof(buf));
  EXPECT_EQ(79u, poll::buildSetPermPlain(buf, sizeof(buf), 0x01020304, pub));
  EXPECT_EQ(0x04, buf[0]);
  EXPECT_EQ(0x03, buf[1]);
  EXPECT_EQ(0x02, buf[2]);
  EXPECT_EQ(0x01, buf[3]);
  EXPECT_EQ(poll::kSetPermFlags, buf[4]);
  EXPECT_EQ(0, memcmp(buf + 5, "setperm ab0c", 12));
  // 64 hex chars, all lowercase, then " 0", and the length does not include a NUL.
  EXPECT_EQ(' ', buf[77]);
  EXPECT_EQ('0', buf[78]);
  EXPECT_EQ(0xEE, buf[79]);
  for (int i = 0; i < 64; i++) {
    char c = (char)buf[13 + i];
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << c;
  }

  char expect[80];
  memcpy(expect, "setperm ", 8);
  static const char* kHex = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    expect[8 + i * 2] = kHex[pub[i] >> 4];
    expect[9 + i * 2] = kHex[pub[i] & 0x0F];
  }
  expect[72] = ' ';
  expect[73] = '0';
  EXPECT_EQ(0, memcmp(buf + 5, expect, 74));

  memset(buf, 0xEE, sizeof(buf));
  EXPECT_EQ(0u, poll::buildSetPermPlain(buf, 78, 1, pub));
  EXPECT_EQ(0u, poll::buildSetPermPlain(nullptr, 79, 1, pub));
  EXPECT_EQ(0u, poll::buildSetPermPlain(buf, 79, 1, nullptr));
  EXPECT_EQ(0xEE, buf[0]);
  EXPECT_EQ(79u, poll::buildSetPermPlain(buf, 79, 1, pub));
}

TEST(PollPacketCodec, CandidateFilterIsTypeHashesAndMinimumLength) {
  uint8_t payload[20];
  memset(payload, 0, sizeof(payload));
  payload[0] = 0x11;
  payload[1] = 0x22;
  EXPECT_TRUE(poll::isCandidateReply(poll::kPayloadTypeResponse, payload, 20, 0x11, 0x22));
  EXPECT_TRUE(poll::isCandidateReply(poll::kPayloadTypePath, payload, 20, 0x11, 0x22));
  EXPECT_TRUE(poll::isCandidateReply(PAYLOAD_TYPE_RESPONSE, payload, 64, 0x11, 0x22));
  EXPECT_FALSE(poll::isCandidateReply(PAYLOAD_TYPE_REQ, payload, 20, 0x11, 0x22));
  EXPECT_FALSE(poll::isCandidateReply(PAYLOAD_TYPE_TXT_MSG, payload, 20, 0x11, 0x22));
  EXPECT_FALSE(poll::isCandidateReply(PAYLOAD_TYPE_ANON_REQ, payload, 20, 0x11, 0x22));
  EXPECT_FALSE(poll::isCandidateReply(poll::kPayloadTypeResponse, payload, 19, 0x11, 0x22));
  EXPECT_FALSE(poll::isCandidateReply(poll::kPayloadTypeResponse, payload, 20, 0x12, 0x22));
  EXPECT_FALSE(poll::isCandidateReply(poll::kPayloadTypeResponse, payload, 20, 0x11, 0x23));
  EXPECT_FALSE(poll::isCandidateReply(poll::kPayloadTypeResponse, payload, 20, 0x22, 0x11));
  EXPECT_FALSE(poll::isCandidateReply(poll::kPayloadTypeResponse, nullptr, 20, 0x11, 0x22));
  EXPECT_FALSE(poll::isCandidateReply(poll::kPayloadTypePath, payload, 0, 0x11, 0x22));
}

TEST(PollPacketCodec, UnwrapPathResponseFindsTheInnerReply) {
  const uint8_t direct[] = {0x00, poll::kPayloadTypeResponse, 0x78, 0x56, 0x34, 0x12, 0xAA};
  const uint8_t* inner = nullptr;
  size_t inner_len = 0;
  ASSERT_TRUE(poll::unwrapPathResponse(direct, sizeof(direct), &inner, &inner_len));
  EXPECT_EQ(5u, inner_len);
  EXPECT_EQ(0x78, inner[0]);
  EXPECT_EQ(0xAA, inner[4]);

  const uint8_t hop[] = {0x01, 0x41, 0x01, 0x10, 0x20};
  ASSERT_TRUE(poll::unwrapPathResponse(hop, sizeof(hop), &inner, &inner_len));
  EXPECT_EQ(2u, inner_len);
  EXPECT_EQ(0x10, inner[0]);

  const uint8_t relay[] = {0x81, 0x41, 0x28, 0xA6, 0x21, 0x99};
  ASSERT_TRUE(poll::unwrapPathResponse(relay, sizeof(relay), &inner, &inner_len));
  EXPECT_EQ(1u, inner_len);
  EXPECT_EQ(0x99, inner[0]);
}

TEST(PollPacketCodec, UnwrapRejectsATeachABadPathAndAMissingInner) {
  const uint8_t rnd[] = {1, 2, 3, 4};
  uint8_t teach[8];
  ASSERT_EQ(6u, poll::buildTeachPlain(teach, sizeof(teach), nullptr, 0, rnd));
  const uint8_t* inner = reinterpret_cast<const uint8_t*>(1);
  size_t inner_len = 7;
  EXPECT_FALSE(poll::unwrapPathResponse(teach, 6, &inner, &inner_len));
  EXPECT_EQ(7u, inner_len);

  const uint8_t bad[] = {0xC1, 0x01, 0x02};
  EXPECT_FALSE(poll::unwrapPathResponse(bad, sizeof(bad), &inner, &inner_len));
  const uint8_t only_type[] = {0x00, 0x01};
  EXPECT_FALSE(poll::unwrapPathResponse(only_type, sizeof(only_type), &inner, &inner_len));
  const uint8_t ack[] = {0x00, PAYLOAD_TYPE_ACK, 0x01};
  EXPECT_FALSE(poll::unwrapPathResponse(ack, sizeof(ack), &inner, &inner_len));
  EXPECT_FALSE(poll::unwrapPathResponse(nullptr, 3, &inner, &inner_len));
  EXPECT_FALSE(poll::unwrapPathResponse(only_type, 2, nullptr, &inner_len));
  EXPECT_FALSE(poll::unwrapPathResponse(only_type, 2, &inner, nullptr));
}

TEST(PollPacketCodec, SplitReplyIsLittleEndianTagThenData) {
  const uint8_t plain[] = {0x78, 0x56, 0x34, 0x12, 0xAA, 0xBB};
  poll::Reply r;
  ASSERT_TRUE(poll::splitReply(plain, sizeof(plain), &r));
  EXPECT_EQ(0x12345678u, r.tag);
  EXPECT_EQ(2u, r.len);
  EXPECT_EQ(0xAA, r.data[0]);
  EXPECT_EQ(0xBB, r.data[1]);

  const uint8_t tag_only[] = {1, 0, 0, 0};
  ASSERT_TRUE(poll::splitReply(tag_only, 4, &r));
  EXPECT_EQ(1u, r.tag);
  EXPECT_EQ(0u, r.len);

  EXPECT_FALSE(poll::splitReply(tag_only, 3, &r));
  EXPECT_FALSE(poll::splitReply(nullptr, 4, &r));
  EXPECT_FALSE(poll::splitReply(tag_only, 4, nullptr));
}

TEST(PollPacketCodec, TelemetryThatLooksLikeAdminIsNotALogin) {
  // Channel 1, voltage, high byte 1 (2.56-5.11 V). data[2] == 1 is the admin
  // flag, but data[0] is the channel, so the shape check must reject it.
  const uint8_t lpp[] = {1, 0x74, 0x01, 0x9A, 1, 103, 0, 50};
  poll::Reply r;
  r.tag = 1;
  r.data = lpp;
  r.len = sizeof(lpp);
  poll::TargetState t = BlankTarget();
  EXPECT_EQ(poll::REPLY_IGNORE, poll::classifyReply(r, t, nullptr, 0, true, false));

  uint8_t padded[12];
  memset(padded, 0, sizeof(padded));
  memcpy(padded, lpp, 4);
  r.data = padded;
  r.len = sizeof(padded);
  EXPECT_EQ(poll::REPLY_IGNORE, poll::classifyReply(r, t, nullptr, 0, true, true));

  uint32_t tags[] = {1};
  EXPECT_EQ(poll::REPLY_DATA, poll::classifyReply(r, t, tags, 1, true, true));
  EXPECT_EQ(poll::REPLY_IGNORE, poll::classifyReply(r, t, tags, 1, true, false));
}

TEST(PollPacketCodec, RecentTagIsNeverALoginAnswer) {
  poll::TargetState t = BlankTarget();
  poll::rememberTag(t, 0x11111111u);
  const uint8_t admin[] = {0x00, 0x00, 0x01, 0x03, 0, 0, 0, 0};
  poll::Reply r;
  r.tag = 0x11111111u;
  r.data = admin;
  r.len = sizeof(admin);
  EXPECT_EQ(poll::REPLY_IGNORE, poll::classifyReply(r, t, nullptr, 0, true, false));
  EXPECT_TRUE(poll::isRecentTag(t, r.tag));

  // Same shape, tag we did not send: admin.
  r.tag = 0x22222222u;
  EXPECT_EQ(poll::REPLY_LOGIN_ADMIN, poll::classifyReply(r, t, nullptr, 0, true, false));

  // A poll tag wins as data even when the body is an admin login.
  uint32_t tags[] = {0x22222222u};
  EXPECT_EQ(poll::REPLY_DATA, poll::classifyReply(r, t, tags, 1, true, true));
  EXPECT_EQ(poll::REPLY_IGNORE, poll::classifyReply(r, t, tags, 1, true, false));
}

TEST(PollPacketCodec, LoginShapeAdminGuestAndNeither) {
  poll::TargetState t = BlankTarget();
  poll::Reply r;
  r.tag = 50;
  const uint8_t guest[] = {0x00, 60, 0x00, 0x00};
  r.data = guest;
  r.len = sizeof(guest);
  EXPECT_EQ(poll::REPLY_LOGIN_GUEST, poll::classifyReply(r, t, nullptr, 0, true, false));

  const uint8_t room[] = {0x00, 0x00, 0x02, 0x02};  // room server guest code
  r.data = room;
  r.len = 4;
  EXPECT_EQ(poll::REPLY_LOGIN_GUEST, poll::classifyReply(r, t, nullptr, 0, true, false));

  const uint8_t code3[] = {0x00, 0x00, 0x03, 0x03};
  r.data = code3;
  EXPECT_EQ(poll::REPLY_LOGIN_GUEST, poll::classifyReply(r, t, nullptr, 0, true, false));

  const uint8_t too_big[] = {0x00, 0x00, 0x04, 0x00};
  r.data = too_big;
  EXPECT_EQ(poll::REPLY_IGNORE, poll::classifyReply(r, t, nullptr, 0, true, false));

  const uint8_t not_ok[] = {0x01, 0x00, 0x01, 0x00};
  r.data = not_ok;
  EXPECT_EQ(poll::REPLY_IGNORE, poll::classifyReply(r, t, nullptr, 0, true, false));

  const uint8_t admin[] = {0x00, 0x00, 0x01, 0x03};
  r.data = admin;
  r.len = 4;
  EXPECT_EQ(poll::REPLY_IGNORE, poll::classifyReply(r, t, nullptr, 0, false, false));
  EXPECT_EQ(poll::REPLY_LOGIN_ADMIN, poll::classifyReply(r, t, nullptr, 0, true, true));

  // Truncated, and zero-filled out to a cipher block (plaintext 16, data 12).
  r.len = 3;
  EXPECT_EQ(poll::REPLY_IGNORE, poll::classifyReply(r, t, nullptr, 0, true, false));
  uint8_t padded[12];
  memset(padded, 0, sizeof(padded));
  memcpy(padded, admin, 4);
  r.data = padded;
  r.len = sizeof(padded);
  EXPECT_EQ(poll::REPLY_LOGIN_ADMIN, poll::classifyReply(r, t, nullptr, 0, true, false));
}

TEST(PollPacketCodec, WrappedResponseSplitsToTheEchoedTag) {
  const uint8_t plain[] = {
      0x00, poll::kPayloadTypeResponse, 0x78, 0x56, 0x34, 0x12, 0x01, 0x74, 0x01, 0x9A};
  const uint8_t* inner = nullptr;
  size_t inner_len = 0;
  ASSERT_TRUE(poll::unwrapPathResponse(plain, sizeof(plain), &inner, &inner_len));
  poll::Reply r;
  ASSERT_TRUE(poll::splitReply(inner, inner_len, &r));
  EXPECT_EQ(0x12345678u, r.tag);
  EXPECT_EQ(4u, r.len);
  EXPECT_EQ(0x01, r.data[0]);
  EXPECT_EQ(0x74, r.data[1]);
  uint32_t tags[] = {0x12345678u};
  poll::TargetState t = BlankTarget();
  EXPECT_EQ(poll::REPLY_DATA, poll::classifyReply(r, t, tags, 1, true, true));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
