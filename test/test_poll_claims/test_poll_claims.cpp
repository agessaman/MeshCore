#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "helpers/poll/PollClaims.h"
#include "helpers/poll/PollControlTypes.h"

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

size_t buildExtended(char* out, size_t cap, const char* origin, uint32_t ts, const char* policy,
                     bool community, const poll::ClaimNode* nodes, uint8_t count,
                     bool accepts_requests, uint8_t request_slots) {
  poll::ClaimOptions opts{};
  opts.extended = true;
  opts.accepts_requests = accepts_requests;
  opts.request_slots = request_slots;
  return poll::buildClaimJson(out, cap, origin, g_observer, ts, kPlugin, policy, community,
                              nodes, count, opts);
}

uint32_t signatureExtended(const poll::ClaimNode* nodes, uint8_t count, bool community,
                           bool accepts_requests) {
  poll::ClaimOptions opts{};
  opts.extended = true;
  opts.accepts_requests = accepts_requests;
  return poll::claimSignature(nodes, count, community, opts);
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
  poll::ClaimTiming t{0, 0, 0, false, false, false};
  EXPECT_TRUE(poll::claimDue(t, 0x12345678u, true, true, base));

  poll::claimSent(t, 0x12345678u, true, true, base);
  EXPECT_TRUE(t.sent_once);
  EXPECT_TRUE(t.last_listed);
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, true, true, base + 1799));
  EXPECT_TRUE(poll::claimDue(t, 0x12345678u, true, true, base + 1800));
  // A failed attempt does not move the 30 min clock.
  poll::claimFailed(t, base + 900);
  EXPECT_TRUE(poll::claimDue(t, 0x12345678u, true, true, base + 1800));
}

TEST(PollClaims, SignatureChangeWaitsForTheRetryGate) {
  const uint32_t base = kTs;
  poll::ClaimTiming t{0, 0, 0, false, false, false};
  poll::claimSent(t, 0x11111111u, true, true, base);
  EXPECT_FALSE(poll::claimDue(t, 0x22222222u, true, true, base + 59));
  EXPECT_TRUE(poll::claimDue(t, 0x22222222u, true, true, base + 60));
  // After a failed attempt too.
  poll::claimFailed(t, base + 100);
  EXPECT_FALSE(poll::claimDue(t, 0x22222222u, true, true, base + 159));
  EXPECT_TRUE(poll::claimDue(t, 0x22222222u, true, true, base + 160));
}

TEST(PollClaims, FailedAttemptRetriesAfterSixtySeconds) {
  const uint32_t base = kTs;
  poll::ClaimTiming t{0, 0, 0, false, false, false};
  poll::claimFailed(t, base);
  EXPECT_FALSE(poll::claimDue(t, 0xabcdef01u, true, true, base + 59));
  EXPECT_TRUE(poll::claimDue(t, 0xabcdef01u, true, true, base + 60));
}

TEST(PollClaims, CommunityOffSendsOneLastClaim) {
  const uint32_t base = kTs;
  poll::ClaimTiming t{0, 0, 0, false, false, false};
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, false, true, base));  // nothing to take back

  poll::claimSent(t, 0x12345678u, true, true, base);
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, false, true, base + 59));
  EXPECT_TRUE(poll::claimDue(t, 0x12345678u, false, true, base + 60));

  poll::claimSent(t, 0x12345678u, false, true, base + 60);
  EXPECT_FALSE(t.last_community);
  EXPECT_TRUE(t.last_listed);  // the notice still names the nodes
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, false, true, base + 3600));
  EXPECT_FALSE(poll::claimDue(t, 0x99999999u, false, true, base + 999999));  // a route change waits until it is on

  // Turning it back on is a change, so it goes at once (after the retry gate).
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, true, true, base + 119));
  EXPECT_TRUE(poll::claimDue(t, 0x12345678u, true, true, base + 120));
}

TEST(PollClaims, NoKeyNamesNoNodesUntilOneWasPublished) {
  EXPECT_FALSE(poll::claimMayList(0));
  EXPECT_TRUE(poll::claimMayList(1));
  EXPECT_TRUE(poll::claimMayList(2));

  const uint32_t base = kTs;
  poll::ClaimTiming t{0, 0, 0, false, false, false};
  EXPECT_FALSE(poll::emptyClaimOwed(t, false));
  // Boot: the key is none and nothing has been claimed, so nothing goes out.
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, true, false, base));
  EXPECT_FALSE(poll::claimDue(t, 0, true, false, base + 999999));
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, false, false, base));
}

TEST(PollClaims, LostKeySendsOneEmptyClaim) {
  const uint32_t base = kTs;
  poll::ClaimTiming t{0, 0, 0, false, false, false};
  poll::claimSent(t, 0x12345678u, true, true, base);
  EXPECT_TRUE(poll::emptyClaimOwed(t, false));
  EXPECT_FALSE(poll::emptyClaimOwed(t, true));
  EXPECT_FALSE(poll::claimDue(t, 0, true, false, base + 59));
  EXPECT_TRUE(poll::claimDue(t, 0, true, false, base + 60));

  // A failed retraction leaves the debt, and retries on the same clock.
  poll::claimFailed(t, base + 60);
  EXPECT_TRUE(t.last_listed);
  EXPECT_FALSE(poll::claimDue(t, 0, true, false, base + 119));
  EXPECT_TRUE(poll::claimDue(t, 0, true, false, base + 120));

  poll::claimSent(t, 0, true, false, base + 120);
  EXPECT_FALSE(t.last_listed);
  EXPECT_FALSE(poll::emptyClaimOwed(t, false));
  EXPECT_FALSE(poll::claimDue(t, 0, true, false, base + 180));
  EXPECT_FALSE(poll::claimDue(t, 0x99u, true, false, base + 999999));
  EXPECT_FALSE(poll::claimDue(t, 0, false, false, base + 999999));  // community flipping waits for a key

  // A key in use again is a change, so the normal claim follows the retry gate.
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, true, true, base + 179));
  EXPECT_TRUE(poll::claimDue(t, 0x12345678u, true, true, base + 180));
}

TEST(PollClaims, LostKeyAfterCommunityOffStillRetracts) {
  const uint32_t base = kTs;
  poll::ClaimTiming t{0, 0, 0, false, false, false};
  poll::claimSent(t, 0x12345678u, true, true, base);
  poll::claimSent(t, 0x12345678u, false, true, base + 60);
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, false, true, base + 3600));
  // The off notice named nodes, so losing the key still owes an empty claim.
  EXPECT_FALSE(poll::claimDue(t, 0, false, false, base + 119));
  EXPECT_TRUE(poll::claimDue(t, 0, false, false, base + 120));
  poll::claimSent(t, 0, false, false, base + 120);
  EXPECT_FALSE(t.last_community);
  EXPECT_FALSE(t.last_listed);
  EXPECT_FALSE(poll::claimDue(t, 0, false, false, base + 999999));
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, true, false, base + 999999));
}

TEST(PollClaims, KeylessCommunityOffIsOneEmptyClaim) {
  const uint32_t base = kTs;
  poll::ClaimTiming t{0, 0, 0, false, false, false};
  poll::claimSent(t, 0x12345678u, true, true, base);
  // Both debts at once: one empty claim says community is off and drops the nodes.
  EXPECT_TRUE(poll::claimDue(t, 0, false, false, base + 60));
  poll::claimSent(t, 0, false, false, base + 60);
  EXPECT_FALSE(t.last_community);
  EXPECT_FALSE(t.last_listed);
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, false, false, base + 999999));
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, true, false, base + 999999));
  // The off notice already went out, so a key does not resume the list until community is on.
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, false, true, base + 999999));
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, true, true, base + 119));
  EXPECT_TRUE(poll::claimDue(t, 0x12345678u, true, true, base + 120));
}

TEST(PollClaims, ClockJumpsNeverLookDue) {
  const uint32_t base = kTs;
  poll::ClaimTiming t{base + 100, base + 100, 0x12345678u, true, true, true};
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, true, true, base));
  t.sent_at = base;
  t.tried_at = base + 100;
  EXPECT_FALSE(poll::claimDue(t, 0x12345678u, true, true, base));
}

TEST(PollClaims, ExtendedClaimWritesEveryOptionalField) {
  initKeys();
  poll::ClaimNode n = makeNode(0xab, 0, false, 0, true, 26);
  n.key = poll::CLAIM_KEY_OWN;
  n.state = poll::CLAIM_STATE_OK;
  n.misses = 2;
  n.requested = true;
  n.last_attempt = kTs - 100;
  n.last_ok = kTs - 50;
  const std::string expected =
      R"json({"v": 2, "origin": "Cadham Village", "origin_id": "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", "timestamp": "2023-11-14T22:13:20Z", "plugin": "openhop.corescope_observer_plus/0.11.1", "policy_version": "a1b2c3d4", "community": true, "accepts_requests": true, "request_slots": 3, "nodes": [{"node_id": "abababababababababababababababababababababababababababababababab", "reply_hops": 0, "via": null, "key": "own", "snr": 6.5, "requested": true, "state": "ok", "misses": 2, "last_attempt": "2023-11-14T22:11:40Z", "last_ok": "2023-11-14T22:12:30Z"}]})json";
  char buf[1024];
  const size_t len =
      buildExtended(buf, sizeof(buf), "Cadham Village", kTs, "a1b2c3d4", true, &n, 1, true, 3);
  ASSERT_EQ(expected.size(), len);
  EXPECT_EQ(expected, std::string(buf, len));
}

TEST(PollClaims, ExtendedEmptyClaimStillCarriesTheEnvelope) {
  initKeys();
  const std::string expected =
      R"json({"v": 2, "origin": "Cadham Village", "origin_id": "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", "timestamp": "2023-11-14T22:13:20Z", "plugin": "openhop.corescope_observer_plus/0.11.1", "policy_version": "", "community": false, "accepts_requests": true, "request_slots": 8, "nodes": []})json";
  char buf[1024];
  const size_t len = buildExtended(buf, sizeof(buf), "Cadham Village", kTs, "", false, nullptr, 0,
                                   true, 8);
  ASSERT_EQ(expected.size(), len);
  EXPECT_EQ(expected, std::string(buf, len));
}

TEST(PollClaims, ExtendedRequestedIsWrittenOnlyWhenTrue) {
  initKeys();
  poll::ClaimNode off = makeNode(0xab, 0, false, 0, true, 26);
  char buf[1024];
  size_t len = buildExtended(buf, sizeof(buf), "x", kTs, "", true, &off, 1, false, 0);
  ASSERT_GT(len, 0u);
  EXPECT_EQ(std::string::npos, std::string(buf, len).find("\"requested\""));

  poll::ClaimNode on = off;
  on.requested = true;
  len = buildExtended(buf, sizeof(buf), "x", kTs, "", true, &on, 1, false, 0);
  ASSERT_GT(len, 0u);
  EXPECT_NE(std::string::npos, std::string(buf, len).find("\"requested\": true"));
}

TEST(PollClaims, ClaimKeyNamesAndKinds) {
  EXPECT_STREQ("shared", poll::claimKeyName(poll::CLAIM_KEY_SHARED));
  EXPECT_STREQ("own", poll::claimKeyName(poll::CLAIM_KEY_OWN));
  EXPECT_STREQ("password", poll::claimKeyName(poll::CLAIM_KEY_PASSWORD));

  char buf[1024];
  const uint8_t fills[] = {0xab, 0xcd, 0xef};
  const uint8_t kinds[] = {poll::CLAIM_KEY_SHARED, poll::CLAIM_KEY_OWN, poll::CLAIM_KEY_PASSWORD};
  const char* names[] = {"shared", "own", "password"};
  for (uint8_t i = 0; i < 3; i++) {
    poll::ClaimNode n = makeNode(fills[i], 0, false, 0, true, 26);
    n.key = kinds[i];
    const size_t len = buildExtended(buf, sizeof(buf), "x", kTs, "", true, &n, 1, false, 0);
    ASSERT_GT(len, 0u);
    EXPECT_EQ(std::string("\"") + names[i] + "\"",
              claimField(std::string(buf, len), "key"));
  }

  // The own registered polling key still names itself through personal_key.
  poll::ClaimNode own = makeNode(0xab, 0, false, 0, true, 26);
  own.personal_key = true;
  size_t len = buildExtended(buf, sizeof(buf), "x", kTs, "", true, &own, 1, false, 0);
  ASSERT_GT(len, 0u);
  EXPECT_EQ("\"own\"", claimField(std::string(buf, len), "key"));

  // A target password wins over the own key.
  own.key = poll::CLAIM_KEY_PASSWORD;
  len = buildExtended(buf, sizeof(buf), "x", kTs, "", true, &own, 1, false, 0);
  ASSERT_GT(len, 0u);
  EXPECT_EQ("\"password\"", claimField(std::string(buf, len), "key"));
}

TEST(PollClaims, ClaimStateNames) {
  EXPECT_STREQ("pending", poll::claimStateName(poll::CLAIM_STATE_PENDING));
  EXPECT_STREQ("ok", poll::claimStateName(poll::CLAIM_STATE_OK));
  EXPECT_STREQ("no_reply", poll::claimStateName(poll::CLAIM_STATE_NO_REPLY));
  EXPECT_STREQ("login_failed", poll::claimStateName(poll::CLAIM_STATE_LOGIN_FAILED));
  EXPECT_STREQ("admin_stop", poll::claimStateName(poll::CLAIM_STATE_ADMIN_STOP));
  EXPECT_STREQ("not_direct", poll::claimStateName(poll::CLAIM_STATE_NOT_DIRECT));
  EXPECT_STREQ("pending", poll::claimStateName(200));
}

TEST(PollClaims, ClaimStateForTarget) {
  poll::TargetState t{};
  EXPECT_EQ(poll::CLAIM_STATE_PENDING, poll::claimStateFor(t, false));

  t.code = poll::CODE_OK;
  EXPECT_EQ(poll::CLAIM_STATE_OK, poll::claimStateFor(t, false));
  t.code = poll::CODE_NO_BATTERY;
  EXPECT_EQ(poll::CLAIM_STATE_OK, poll::claimStateFor(t, false));
  t.code = poll::CODE_NOT_DIRECT;
  EXPECT_EQ(poll::CLAIM_STATE_NOT_DIRECT, poll::claimStateFor(t, false));
  t.code = poll::CODE_NO_REPLY;
  EXPECT_EQ(poll::CLAIM_STATE_NO_REPLY, poll::claimStateFor(t, false));
  t.code = poll::CODE_ERROR;
  EXPECT_EQ(poll::CLAIM_STATE_NO_REPLY, poll::claimStateFor(t, false));

  t.code = poll::CODE_NO_REPLY;
  t.key_login_fails = 1;
  EXPECT_EQ(poll::CLAIM_STATE_NO_REPLY, poll::claimStateFor(t, true));
  t.key_login_fails = 2;
  EXPECT_EQ(poll::CLAIM_STATE_NO_REPLY, poll::claimStateFor(t, false));
  EXPECT_EQ(poll::CLAIM_STATE_LOGIN_FAILED, poll::claimStateFor(t, true));

  // A stop wins over whatever the last poll did.
  t.code = poll::CODE_OK;
  t.admin_seen = true;
  EXPECT_EQ(poll::CLAIM_STATE_ADMIN_STOP, poll::claimStateFor(t, false));
}

TEST(PollClaims, ExtendedStateAndHistoryFields) {
  initKeys();
  poll::ClaimNode n = makeNode(0xab, 0, false, 0, true, 26);
  n.state = poll::CLAIM_STATE_LOGIN_FAILED;
  n.misses = 255;
  n.last_attempt = kTs;
  n.last_ok = 0;
  char buf[1024];
  size_t len = buildExtended(buf, sizeof(buf), "x", kTs, "", true, &n, 1, false, 0);
  ASSERT_GT(len, 0u);
  std::string json(buf, len);
  EXPECT_EQ("\"login_failed\"", claimField(json, "state"));
  EXPECT_EQ("255", claimField(json, "misses"));
  EXPECT_EQ("\"2023-11-14T22:13:20Z\"", claimField(json, "last_attempt"));
  EXPECT_EQ("null", claimField(json, "last_ok"));

  n.last_attempt = 0;
  n.last_ok = kTs;
  len = buildExtended(buf, sizeof(buf), "x", kTs, "", true, &n, 1, false, 0);
  ASSERT_GT(len, 0u);
  json.assign(buf, len);
  EXPECT_EQ("null", claimField(json, "last_attempt"));
  EXPECT_EQ("\"2023-11-14T22:13:20Z\"", claimField(json, "last_ok"));
}

TEST(PollClaims, ExtendedRequestSlotsStayInRange) {
  initKeys();
  char buf[1024];
  struct Case {
    bool accepts;
    uint8_t slots;
    const char* accepts_text;
    const char* slots_text;
  };
  const Case cases[] = {
      {false, 0, "false", "0"},
      {true, 3, "true", "3"},
      {true, 8, "true", "8"},
      {true, 9, "true", "8"},
      {true, 255, "true", "8"},
  };
  for (const Case& c : cases) {
    const size_t len =
        buildExtended(buf, sizeof(buf), "x", kTs, "", true, nullptr, 0, c.accepts, c.slots);
    ASSERT_GT(len, 0u);
    const std::string json(buf, len);
    EXPECT_EQ(c.accepts_text, claimField(json, "accepts_requests")) << (int)c.slots;
    EXPECT_EQ(c.slots_text, claimField(json, "request_slots")) << (int)c.slots;
  }
}

TEST(PollClaims, PlainClaimIgnoresEveryExtendedField) {
  initKeys();
  poll::ClaimNode plain = makeNode(0xab, 0, false, 0, true, 26);
  poll::ClaimNode loaded = plain;
  loaded.key = poll::CLAIM_KEY_PASSWORD;
  loaded.state = poll::CLAIM_STATE_ADMIN_STOP;
  loaded.misses = 9;
  loaded.requested = true;
  loaded.last_attempt = kTs;
  loaded.last_ok = kTs;
  char a[1024];
  char b[1024];
  const size_t la = build(a, sizeof(a), "x", kTs, "", true, &plain, 1);
  const size_t lb = build(b, sizeof(b), "x", kTs, "", true, &loaded, 1);
  ASSERT_GT(la, 0u);
  ASSERT_EQ(la, lb);
  EXPECT_EQ(0, memcmp(a, b, la));
  EXPECT_EQ(poll::claimSignature(&plain, 1, true), poll::claimSignature(&loaded, 1, true));
}

TEST(PollClaims, ExtendedSignatureTracksOnlyStoppingStates) {
  poll::ClaimNode base = makeNode(0xab, 0, false, 0, true, 26);
  base.state = poll::CLAIM_STATE_PENDING;
  const uint32_t pending = signatureExtended(&base, 1, true, false);

  poll::ClaimNode n = base;
  n.state = poll::CLAIM_STATE_OK;
  EXPECT_EQ(pending, signatureExtended(&n, 1, true, false));
  n.state = poll::CLAIM_STATE_NO_REPLY;
  EXPECT_EQ(pending, signatureExtended(&n, 1, true, false));
  n.state = poll::CLAIM_STATE_NOT_DIRECT;
  EXPECT_EQ(pending, signatureExtended(&n, 1, true, false));

  n.state = poll::CLAIM_STATE_LOGIN_FAILED;
  const uint32_t failed = signatureExtended(&n, 1, true, false);
  EXPECT_NE(pending, failed);
  n.state = poll::CLAIM_STATE_ADMIN_STOP;
  EXPECT_EQ(failed, signatureExtended(&n, 1, true, false));  // one bit, not the state value

  // Leaves the stopped pair, so the signature goes back.
  n.state = poll::CLAIM_STATE_OK;
  EXPECT_EQ(pending, signatureExtended(&n, 1, true, false));

  // A second node gets its own bit.
  poll::ClaimNode a = makeNode(0xab, 0, false, 0, true, 26);
  poll::ClaimNode b = makeNode(0xcd, 0, false, 0, true, 26);
  poll::ClaimNode pair[2] = {a, b};
  const uint32_t both_ok = signatureExtended(pair, 2, true, false);
  pair[0].state = poll::CLAIM_STATE_ADMIN_STOP;
  const uint32_t first_stopped = signatureExtended(pair, 2, true, false);
  EXPECT_NE(both_ok, first_stopped);
  pair[1].state = poll::CLAIM_STATE_ADMIN_STOP;
  const uint32_t both_stopped = signatureExtended(pair, 2, true, false);
  EXPECT_NE(first_stopped, both_stopped);
  pair[0].state = poll::CLAIM_STATE_PENDING;
  const uint32_t second_stopped = signatureExtended(pair, 2, true, false);
  EXPECT_NE(both_stopped, second_stopped);
  EXPECT_NE(both_ok, second_stopped);
}

TEST(PollClaims, ExtendedSignatureTracksKeyAndRequested) {
  poll::ClaimNode base = makeNode(0xab, 0, false, 0, true, 26);
  const uint32_t shared = signatureExtended(&base, 1, true, false);

  poll::ClaimNode n = base;
  n.key = poll::CLAIM_KEY_OWN;
  const uint32_t own = signatureExtended(&n, 1, true, false);
  EXPECT_NE(shared, own);
  n.key = poll::CLAIM_KEY_PASSWORD;
  EXPECT_NE(shared, signatureExtended(&n, 1, true, false));
  EXPECT_NE(own, signatureExtended(&n, 1, true, false));

  n = base;
  n.personal_key = true;  // the own registered key names itself
  EXPECT_NE(shared, signatureExtended(&n, 1, true, false));

  n = base;
  n.requested = true;
  const uint32_t requested = signatureExtended(&n, 1, true, false);
  EXPECT_NE(shared, requested);

  // Accepts requests is part of the envelope signature...
  EXPECT_NE(shared, signatureExtended(&base, 1, true, true));
  // ...but the free slot count, misses, and timestamps ride on the refresh.
  poll::ClaimOptions slots{};
  slots.extended = true;
  slots.request_slots = 7;
  EXPECT_EQ(shared, poll::claimSignature(&base, 1, true, slots));
  n = base;
  n.misses = 255;
  n.last_attempt = kTs;
  n.last_ok = kTs;
  EXPECT_EQ(shared, signatureExtended(&n, 1, true, false));
}

TEST(PollClaims, AcceptsRequestsPublishesAndRefreshesEmptyClaim) {
  const uint32_t base = kTs;
  EXPECT_FALSE(poll::claimMayList(0));
  EXPECT_FALSE(poll::claimMayList(0, false));
  EXPECT_TRUE(poll::claimMayList(0, true));
  EXPECT_TRUE(poll::claimMayList(2, false));
  EXPECT_TRUE(poll::claimMayList(2, true));

  poll::ClaimTiming t{0, 0, 0, false, false, false};
  const bool may = poll::claimMayList(0, true);
  EXPECT_FALSE(poll::emptyClaimOwed(t, may));
  EXPECT_TRUE(poll::claimDue(t, 0xabcdu, true, may, base));
  poll::claimSent(t, 0xabcdu, true, true, base);  // an empty snapshot, not a withdrawal
  EXPECT_TRUE(t.last_listed);
  EXPECT_FALSE(poll::claimDue(t, 0xabcdu, true, may, base + 1799));
  EXPECT_TRUE(poll::claimDue(t, 0xabcdu, true, may, base + 1800));
  poll::claimSent(t, 0xabcdu, true, true, base + 1800);

  // Switching the opt-in off retracts the list with one empty claim, then silence.
  const bool off = poll::claimMayList(0);
  EXPECT_TRUE(poll::emptyClaimOwed(t, off));
  EXPECT_FALSE(poll::claimDue(t, 0xabcdu, true, off, base + 1859));
  EXPECT_TRUE(poll::claimDue(t, 0xabcdu, true, off, base + 1860));
  poll::claimSent(t, 0xabcdu, true, false, base + 1860);
  EXPECT_FALSE(t.last_listed);
  EXPECT_FALSE(poll::claimDue(t, 0xabcdu, true, off, base + 999999));
}

TEST(PollClaims, ExtendedWorstCaseFitsClaimBuffer) {
  initKeys();
  const std::string origin(32, 'A');
  const std::string plugin = "meshcore.observer_fw/" + std::string(26, 'p');
  const std::string policy(15, 'v');
  ASSERT_EQ(47u, plugin.size());

  poll::ClaimNode nodes[poll::kMaxTargets];
  for (uint8_t i = 0; i < poll::kMaxTargets; i++) {
    nodes[i] = makeNode((uint8_t)(0xa0 + i), 1, false, 0, true, 26);
    nodes[i].key = poll::CLAIM_KEY_PASSWORD;
    nodes[i].state = poll::CLAIM_STATE_LOGIN_FAILED;
    nodes[i].misses = 255;
    nodes[i].requested = true;
    nodes[i].last_attempt = 4294967295u;
    nodes[i].last_ok = 4294967295u;
  }

  std::vector<char> buf(poll::kClaimJsonMax + 1, 'X');
  const size_t len = buildExtended(buf.data(), buf.size(), origin.c_str(), 4294967295u,
                                   policy.c_str(), true, nodes, poll::kMaxTargets, true, 8);
  ASSERT_GT(len, 0u);
  EXPECT_LE(len, (size_t)poll::kClaimJsonMax);
  EXPECT_EQ((size_t)poll::kClaimJsonMax + 1, buf.size());
  EXPECT_EQ('\0', buf[len]);
  EXPECT_EQ('}', buf[len - 1]);

  // The exact same claim in a one-byte-short buffer is refused, not truncated.
  std::vector<char> tight(len, 'X');
  EXPECT_EQ(0u, buildExtended(tight.data(), tight.size(), origin.c_str(), 4294967295u,
                              policy.c_str(), true, nodes, poll::kMaxTargets, true, 8));
  std::vector<char> tiny(64, 'X');
  EXPECT_EQ(0u, buildExtended(tiny.data(), tiny.size(), origin.c_str(), 4294967295u,
                              policy.c_str(), true, nodes, poll::kMaxTargets, true, 8));

  // One more byte is enough for the terminator.
  std::vector<char> exact(len + 1, 'X');
  EXPECT_EQ(len, buildExtended(exact.data(), exact.size(), origin.c_str(), 4294967295u,
                               policy.c_str(), true, nodes, poll::kMaxTargets, true, 8));
  EXPECT_EQ(0, memcmp(buf.data(), exact.data(), len));
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
