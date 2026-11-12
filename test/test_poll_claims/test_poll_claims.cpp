#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "helpers/poll/PollClaims.h"

namespace {

constexpr uint32_t kTs = 1700000000;
constexpr const char* kPlugin = "openhop.corescope_observer_plus/0.11.1";

poll::ClaimNode makeNode(uint8_t node_fill, uint8_t hops, bool has_via, uint8_t via_fill,
                         bool has_snr, int16_t snr_x4) {
  poll::ClaimNode n{};
  memset(n.node, node_fill, sizeof(n.node));
  n.reply_hops = hops;
  n.has_via = has_via;
  memset(n.via, via_fill, sizeof(n.via));
  n.has_snr = has_snr;
  n.snr_x4 = snr_x4;
  return n;
}

uint8_t g_observer[poll::kPubKeySize];

void initKeys() {
  for (uint8_t i = 0; i < poll::kPubKeySize; i++) g_observer[i] = i;
}

size_t build(char* out, size_t cap, const char* origin, uint32_t ts, const char* policy,
             bool community, const poll::ClaimNode* nodes, uint8_t count) {
  return poll::buildClaimJson(out, cap, origin, g_observer, ts, kPlugin, policy, community,
                              nodes, count);
}

// The plugin's own json.dumps output, then the same payload through the exact
// buffer and one byte short.
void expectClaim(const std::string& expected, const char* origin, uint32_t ts,
                 const char* policy, bool community, const poll::ClaimNode* nodes,
                 uint8_t count) {
  std::vector<char> roomy(expected.size() + 32, 'X');
  size_t len = build(roomy.data(), roomy.size(), origin, ts, policy, community, nodes, count);
  ASSERT_EQ(expected.size(), len);
  EXPECT_EQ(expected, std::string(roomy.data(), len));
  EXPECT_EQ('\0', roomy[len]);

  std::vector<char> exact(expected.size() + 1 + 8, 'X');
  len = build(exact.data(), exact.size() - 8, origin, ts, policy, community, nodes, count);
  ASSERT_EQ(expected.size(), len);
  EXPECT_EQ(expected, std::string(exact.data(), len));
  for (size_t i = expected.size() + 1; i < exact.size(); i++) EXPECT_EQ('X', exact[i]);

  std::vector<char> tight(expected.size() + 8, 'X');
  len = build(tight.data(), expected.size(), origin, ts, policy, community, nodes, count);
  EXPECT_EQ(0u, len);
  for (size_t i = expected.size(); i < tight.size(); i++) EXPECT_EQ('X', tight[i]);
}

std::string claimField(const std::string& json, const char* name) {
  const std::string key = std::string("\"") + name + "\": ";
  const size_t pos = json.find(key);
  if (pos == std::string::npos) return "<missing>";
  const size_t start = pos + key.size();
  if (json.compare(start, 4, "null") == 0) return "null";
  size_t end = json.find_first_of(",}", start);
  return json.substr(start, end - start);
}

TEST(PollClaims, EmptyNodeListMatchesPlugin) {
  initKeys();
  const std::string expected =
      R"json({"v": 2, "origin": "Cadham Village", "origin_id": "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", "timestamp": "2023-11-14T22:13:20Z", "plugin": "openhop.corescope_observer_plus/0.11.1", "policy_version": "", "community": true, "nodes": []})json";
  expectClaim(expected, "Cadham Village", kTs, "", true, nullptr, 0);
}

TEST(PollClaims, OneNodeMatchesPlugin) {
  initKeys();
  const poll::ClaimNode n = makeNode(0xab, 0, false, 0, true, 26);
  const std::string expected =
      R"json({"v": 2, "origin": "Cadham Village", "origin_id": "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", "timestamp": "2023-11-14T22:13:20Z", "plugin": "openhop.corescope_observer_plus/0.11.1", "policy_version": "a1b2c3d4", "community": true, "nodes": [{"node_id": "abababababababababababababababababababababababababababababababab", "reply_hops": 0, "via": null, "key": "shared", "snr": 6.5}]})json";
  expectClaim(expected, "Cadham Village", kTs, "a1b2c3d4", true, &n, 1);
}

TEST(PollClaims, SeveralNodesMatchPlugin) {
  initKeys();
  const poll::ClaimNode nodes[4] = {
      makeNode(0xab, 0, false, 0, true, 26),
      makeNode(0xcd, 1, true, 0xef, false, 0),
      makeNode(0x11, 1, false, 0, true, -13),
      makeNode(0x22, 0, true, 0x33, true, 40),
  };
  const std::string expected =
      R"json({"v": 2, "origin": "Cadham Village", "origin_id": "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", "timestamp": "2023-11-14T22:13:20Z", "plugin": "openhop.corescope_observer_plus/0.11.1", "policy_version": "a1b2c3d4", "community": true, "nodes": [{"node_id": "abababababababababababababababababababababababababababababababab", "reply_hops": 0, "via": null, "key": "shared", "snr": 6.5}, {"node_id": "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd", "reply_hops": 1, "via": "efefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefef", "key": "shared", "snr": null}, {"node_id": "1111111111111111111111111111111111111111111111111111111111111111", "reply_hops": 1, "via": null, "key": "shared", "snr": -3.2}, {"node_id": "2222222222222222222222222222222222222222222222222222222222222222", "reply_hops": 0, "via": "3333333333333333333333333333333333333333333333333333333333333333", "key": "shared", "snr": 10.0}]})json";
  expectClaim(expected, "Cadham Village", kTs, "a1b2c3d4", true, nodes, 4);
}

TEST(PollClaims, OriginEscapingMatchesEnsureAscii) {
  initKeys();
  const char* origin = "Cadham \"Village\" \\ Caf\xc3\xa9 \xf0\x9f\x98\x80\n\t\x01/<>";
  const poll::ClaimNode n = makeNode(0x11, 1, false, 0, true, -13);
  const std::string expected =
      R"json({"v": 2, "origin": "Cadham \"Village\" \\ Caf\u00e9 \ud83d\ude00\n\t\u0001/<>", "origin_id": "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", "timestamp": "2023-11-14T22:13:21Z", "plugin": "openhop.corescope_observer_plus/0.11.1", "policy_version": "x\"y", "community": false, "nodes": [{"node_id": "1111111111111111111111111111111111111111111111111111111111111111", "reply_hops": 1, "via": null, "key": "shared", "snr": -3.2}]})json";
  expectClaim(expected, origin, kTs + 1, "x\"y", false, &n, 1);
}

TEST(PollClaims, MalformedUtf8BecomesReplacementChars) {
  initKeys();
  std::string lone = "ab";
  lone.push_back((char)0x80);
  lone += "cd";
  struct Case {
    std::string in;
    std::string out;
  };
  const Case cases[] = {
      // Multi-byte characters cut by a byte-wise truncation.
      {std::string("abc\xc3"), "\"abc\\ufffd\""},
      {std::string("abc\xe2\x82"), "\"abc\\ufffd\\ufffd\""},
      {std::string("abc\xf0\x9f\x98"), "\"abc\\ufffd\\ufffd\\ufffd\""},
      // A stray continuation byte is a bad byte of its own.
      {lone, "\"ab\\ufffdcd\""},
      // Overlong and out-of-range sequences.
      {std::string("\xe0\x80\x80"), "\"\\ufffd\""},
      {std::string("\xf5\x80"), "\"\\ufffd\\ufffd\""},
  };
  for (const Case& c : cases) {
    char buf[1024];
    const size_t len = build(buf, sizeof(buf), c.in.c_str(), kTs, "", true, nullptr, 0);
    ASSERT_GT(len, 0u) << c.out;
    const std::string json(buf, len);
    const std::string key = "\"origin\": ";
    const size_t start = json.find(key) + key.size();
    const size_t end = json.find(", \"origin_id\"", start);
    EXPECT_EQ(c.out, json.substr(start, end - start)) << c.out;
  }
}

TEST(PollClaims, ReplacementCharsNeedRoomLikeAnyOtherText) {
  initKeys();
  const std::string expected =
      R"json({"v": 2, "origin": "abc\ufffd", "origin_id": "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", "timestamp": "2023-11-14T22:13:20Z", "plugin": "openhop.corescope_observer_plus/0.11.1", "policy_version": "", "community": true, "nodes": []})json";
  expectClaim(expected, "abc\xc3", kTs, "", true, nullptr, 0);
}

TEST(PollClaims, SnrRoundsLikePython) {
  initKeys();
  struct Case {
    int16_t x4;
    const char* text;
  };
  const Case cases[] = {
      {0, "0.0"},      {1, "0.2"},      {3, "0.8"},     {5, "1.2"},      {7, "1.8"},
      {9, "2.2"},      {26, "6.5"},     {30, "7.5"},    {39, "9.8"},     {40, "10.0"},
      {41, "10.2"},    {-1, "-0.2"},    {-3, "-0.8"},   {-5, "-1.2"},    {-7, "-1.8"},
      {-9, "-2.2"},    {-11, "-2.8"},   {-13, "-3.2"},  {32767, "8191.8"}, {-32768, "-8192.0"},
  };
  for (const Case& c : cases) {
    const poll::ClaimNode n = makeNode(0xab, 0, false, 0, true, c.x4);
    char buf[1024];
    const size_t len = build(buf, sizeof(buf), "Cadham Village", kTs, "", true, &n, 1);
    ASSERT_GT(len, 0u) << c.x4;
    EXPECT_EQ(c.text, claimField(std::string(buf, len), "snr")) << c.x4;
  }

  const poll::ClaimNode no_snr = makeNode(0xab, 0, false, 0, false, 0);
  char buf[1024];
  const size_t len = build(buf, sizeof(buf), "Cadham Village", kTs, "", true, &no_snr, 1);
  ASSERT_GT(len, 0u);
  EXPECT_EQ("null", claimField(std::string(buf, len), "snr"));
}

TEST(PollClaims, SnrRoundingIsUnaffectedByPersonalKey) {
  initKeys();
  poll::ClaimNode n = makeNode(0xab, 0, false, 0, true, 30);
  n.personal_key = true;
  char buf[1024];
  const size_t len = build(buf, sizeof(buf), "Cadham Village", kTs, "", true, &n, 1);
  ASSERT_GT(len, 0u);
  EXPECT_EQ("7.5", claimField(std::string(buf, len), "snr"));
  EXPECT_NE(std::string::npos, std::string(buf, len).find("\"key\": \"shared\""));
}

TEST(PollClaims, TimestampIsUtcCivilTime) {
  initKeys();
  struct Case {
    uint32_t epoch;
    const char* iso;
  };
  const Case cases[] = {
      {0, "1970-01-01T00:00:00Z"},
      {951782400, "2000-02-29T00:00:00Z"},
      {1700000000, "2023-11-14T22:13:20Z"},
      {4102444799, "2099-12-31T23:59:59Z"},
      {4294967295, "2106-02-07T06:28:15Z"},
  };
  for (const Case& c : cases) {
    char buf[1024];
    const size_t len = build(buf, sizeof(buf), "Cadham Village", c.epoch, "", true, nullptr, 0);
    ASSERT_GT(len, 0u);
    const std::string json(buf, len);
    const std::string key = "\"timestamp\": \"";
    const size_t pos = json.find(key) + key.size();
    EXPECT_EQ(c.iso, json.substr(pos, 20)) << c.epoch;
  }
}

TEST(PollClaims, RefusesUnusableInputs) {
  initKeys();
  const poll::ClaimNode n = makeNode(0xab, 0, false, 0, true, 0);
  char buf[1024];
  EXPECT_EQ(0u, build(buf, sizeof(buf), "x", kTs, "", true, nullptr, 1));
  EXPECT_EQ(0u, build(nullptr, sizeof(buf), "x", kTs, "", true, &n, 1));
  EXPECT_EQ(0u, build(buf, 0, "x", kTs, "", true, &n, 1));
  EXPECT_EQ(0u, build(buf, sizeof(buf), "x", kTs, "", true, &n, poll::kMaxTargets + 1));
}

TEST(PollClaims, HexHelpersAreLowerOrUpperAndTerminated) {
  const uint8_t bytes[2] = {0x0a, 0xf1};
  char buf[5];
  poll::toHexLower(buf, bytes, 2);
  EXPECT_STREQ("0af1", buf);
  poll::toHexUpper(buf, bytes, 2);
  EXPECT_STREQ("0AF1", buf);
}

TEST(PollClaims, SignatureIgnoresOrder) {
  initKeys();
  const poll::ClaimNode nodes[3] = {
      makeNode(0xab, 0, false, 0, true, 26),
      makeNode(0xcd, 1, true, 0xef, false, 0),
      makeNode(0x11, 1, false, 0, true, -13),
  };
  const poll::ClaimNode shuffled[3] = {nodes[2], nodes[0], nodes[1]};
  EXPECT_EQ(poll::claimSignature(nodes, 3, true), poll::claimSignature(shuffled, 3, true));
  EXPECT_NE(poll::claimSignature(nodes, 3, true), poll::claimSignature(nodes, 3, false));
}

TEST(PollClaims, SignatureTracksRouteAndCommunity) {
  initKeys();
  const poll::ClaimNode base = makeNode(0xab, 0, false, 0, true, 26);
  const uint32_t sig = poll::claimSignature(&base, 1, true);

  poll::ClaimNode hop = base;
  hop.reply_hops = 1;
  EXPECT_NE(sig, poll::claimSignature(&hop, 1, true));

  poll::ClaimNode via = base;
  via.has_via = true;
  memset(via.via, 0xef, sizeof(via.via));
  EXPECT_NE(sig, poll::claimSignature(&via, 1, true));

  poll::ClaimNode zero_via = base;
  zero_via.has_via = true;  // plugin keeps "" and the zero key apart
  EXPECT_NE(sig, poll::claimSignature(&zero_via, 1, true));

  poll::ClaimNode other_node = base;
  other_node.node[0] ^= 0x01;
  EXPECT_NE(sig, poll::claimSignature(&other_node, 1, true));

  poll::ClaimNode no_snr = base;
  no_snr.has_snr = false;
  no_snr.personal_key = true;
  EXPECT_EQ(sig, poll::claimSignature(&no_snr, 1, true));
}

TEST(PollClaims, SignatureHandlesDuplicateNodes) {
  initKeys();
  const poll::ClaimNode twice[2] = {makeNode(0xab, 0, false, 0, true, 26),
                                    makeNode(0xab, 0, false, 0, true, 26)};
  const poll::ClaimNode once[1] = {makeNode(0xab, 0, false, 0, true, 26)};
  EXPECT_NE(poll::claimSignature(once, 1, true), poll::claimSignature(twice, 2, true));
  EXPECT_EQ(0u, poll::claimSignature(twice, poll::kMaxTargets + 1, true));
}

TEST(PollClaims, ClaimTimingEveryThirtyMinutes) {
  const uint32_t base = kTs;
  poll::ClaimTiming t{0, 0, 0, false, false};
  EXPECT_TRUE(poll::claimDue(t, 0x12345678u, true, base));

  poll::claimSent(t, 0x12345678u, true, base);
  EXPECT_TRUE(t.sent_once);
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, true, base + 1799));
  EXPECT_TRUE(poll::claimDue(t, 0x12345678u, true, base + 1800));
  // A failed attempt does not move the 30 min clock.
  poll::claimFailed(t, base + 900);
  EXPECT_TRUE(poll::claimDue(t, 0x12345678u, true, base + 1800));
}

TEST(PollClaims, SignatureChangeWaitsForTheRetryGate) {
  const uint32_t base = kTs;
  poll::ClaimTiming t{0, 0, 0, false, false};
  poll::claimSent(t, 0x11111111u, true, base);
  EXPECT_FALSE(poll::claimDue(t, 0x22222222u, true, base + 59));
  EXPECT_TRUE(poll::claimDue(t, 0x22222222u, true, base + 60));
  // After a failed attempt too.
  poll::claimFailed(t, base + 100);
  EXPECT_FALSE(poll::claimDue(t, 0x22222222u, true, base + 159));
  EXPECT_TRUE(poll::claimDue(t, 0x22222222u, true, base + 160));
}

TEST(PollClaims, FailedAttemptRetriesAfterSixtySeconds) {
  const uint32_t base = kTs;
  poll::ClaimTiming t{0, 0, 0, false, false};
  poll::claimFailed(t, base);
  EXPECT_FALSE(poll::claimDue(t, 0xabcdef01u, true, base + 59));
  EXPECT_TRUE(poll::claimDue(t, 0xabcdef01u, true, base + 60));
}

TEST(PollClaims, CommunityOffSendsOneLastClaim) {
  const uint32_t base = kTs;
  poll::ClaimTiming t{0, 0, 0, false, false};
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, false, base));  // nothing to take back

  poll::claimSent(t, 0x12345678u, true, base);
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, false, base + 59));
  EXPECT_TRUE(poll::claimDue(t, 0x12345678u, false, base + 60));

  poll::claimSent(t, 0x12345678u, false, base + 60);
  EXPECT_FALSE(t.last_community);
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, false, base + 3600));
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, false, base + 999999));

  // Turning it back on is a change, so it goes at once (after the retry gate).
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, true, base + 119));
  EXPECT_TRUE(poll::claimDue(t, 0x12345678u, true, base + 120));
}

TEST(PollClaims, ClockJumpsNeverLookDue) {
  const uint32_t base = kTs;
  poll::ClaimTiming t{base + 100, base + 100, 0x12345678u, true, true};
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, true, base));
  t.sent_at = base;
  t.tried_at = base + 100;
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, true, base));
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
