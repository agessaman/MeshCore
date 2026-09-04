#include <gtest/gtest.h>

#include <cstring>

#include "helpers/NetworkHostname.h"

namespace {

const uint8_t kStableId[] = {0xab, 0xcd, 0x01};

void expectDhcpSafe(const char* hostname) {
  ASSERT_NE(nullptr, hostname);
  const size_t length = strlen(hostname);
  EXPECT_GT(length, 0u);
  EXPECT_LE(length, NetworkHostname::kMaxLength);
  EXPECT_NE('-', hostname[0]);
  EXPECT_NE('-', hostname[length - 1]);
  for (size_t i = 0; i < length; ++i) {
    const char ch = hostname[i];
    EXPECT_TRUE((ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9') || ch == '-')
        << "invalid byte at " << i;
  }
}

}  // namespace

TEST(NetworkHostname, PrefixesAndLowercasesReadableNodeName) {
  char hostname[NetworkHostname::kBufferSize];
  ASSERT_TRUE(NetworkHostname::build(hostname, sizeof(hostname),
                                     "Hill Repeater 2", kStableId,
                                     sizeof(kStableId)));
  EXPECT_STREQ("meshcore-hill-repeater-2", hostname);
  expectDhcpSafe(hostname);
}

TEST(NetworkHostname, CollapsesInvalidRunsAndTrimsEdges) {
  char hostname[NetworkHostname::kBufferSize];
  ASSERT_TRUE(NetworkHostname::build(hostname, sizeof(hostname),
                                     "  Adam's___Hill / Repeater!  ",
                                     kStableId, sizeof(kStableId)));
  EXPECT_STREQ("meshcore-adam-s-hill-repeater", hostname);
  expectDhcpSafe(hostname);
}

TEST(NetworkHostname, UsesDocumentedFallbackForEmptySanitizedName) {
  char hostname[NetworkHostname::kBufferSize];
  ASSERT_TRUE(NetworkHostname::build(hostname, sizeof(hostname), "___  !!!",
                                     kStableId, sizeof(kStableId)));
  EXPECT_STREQ("meshcore-node", hostname);
  expectDhcpSafe(hostname);
}

TEST(NetworkHostname, KeepsExactBoundaryWithoutIdentitySuffix) {
  char hostname[NetworkHostname::kBufferSize];
  ASSERT_TRUE(NetworkHostname::build(hostname, sizeof(hostname),
                                     "abcdefghijklmnopqrstuv", kStableId,
                                     sizeof(kStableId)));
  EXPECT_STREQ("meshcore-abcdefghijklmnopqrstuv", hostname);
  EXPECT_EQ(NetworkHostname::kMaxLength, strlen(hostname));
}

TEST(NetworkHostname, TruncatesReadablePartAndAddsStableIdentitySuffix) {
  char hostname[NetworkHostname::kBufferSize];
  ASSERT_TRUE(NetworkHostname::build(hostname, sizeof(hostname),
                                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345",
                                     kStableId, sizeof(kStableId)));
  EXPECT_STREQ("meshcore-abcdefghijklmno-abcd01", hostname);
  EXPECT_EQ(NetworkHostname::kMaxLength, strlen(hostname));
  expectDhcpSafe(hostname);
}

TEST(NetworkHostname, IdentitySuffixSeparatesOtherwiseCollidingNames) {
  const uint8_t other_id[] = {0x12, 0x34, 0x56};
  char first[NetworkHostname::kBufferSize];
  char second[NetworkHostname::kBufferSize];
  ASSERT_TRUE(NetworkHostname::build(first, sizeof(first),
                                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345",
                                     kStableId, sizeof(kStableId)));
  ASSERT_TRUE(NetworkHostname::build(second, sizeof(second),
                                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345",
                                     other_id, sizeof(other_id)));
  EXPECT_STRNE(first, second);
  EXPECT_STREQ("meshcore-abcdefghijklmno-123456", second);
}

TEST(NetworkHostname, RejectsMissingOutputBuffer) {
  EXPECT_FALSE(NetworkHostname::build(nullptr, 0, "node", kStableId,
                                      sizeof(kStableId)));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
