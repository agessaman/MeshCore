#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "helpers/poll/PollControlJson.h"

namespace {

// Replies captured from corescope_stub.py.
const char kOwnersEmbedded[] =
    "{\"observer\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"nodes\":{"
    "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\":{\"role\":\"polling\",\"mine\":true,"
    "\"owner\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"owner_hops\":0,"
    "\"latest_ts\":\"2026-11-12T10:00:00Z\"},"
    "\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\":{\"role\":\"standby\",\"mine\":false,"
    "\"owner\":\"2222222222222222222222222222222222222222222222222222222222222222\",\"owner_name\":\"other\","
    "\"owner_hops\":1,\"standby_rank\":2,\"latest_ts\":\"2026-11-12T09:30:00.123+00:00\"}},"
    "\"policy\":{\"direct_s\":3600,\"one_hop_s\":7200,\"grace_s\":1800,\"max_requests\":2,\"version\":\"stub-7\","
    "\"paused\":true,\"paused_reason\":\"\",\"paused_until\":\"2026-11-12T18:00:00Z\",\"excluded_nodes\":"
    "[\"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\"],\"node_windows\":"
    "{\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\":5400},\"min_plugin_version\":\"0.11.0\","
    "\"min_version_reason\":\"\",\"observer_blocked\":false,\"version_ok\":true}}";

const char kOwnersPlain[] =
    "{\"observer\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"nodes\":{"
    "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\":{\"role\":\"polling\",\"mine\":true,"
    "\"owner\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"owner_hops\":0,"
    "\"latest_ts\":\"2026-11-12T10:00:00Z\"}}}";

const char kPolicyPlain[] =
    "{\"direct_s\":3600,\"one_hop_s\":7200,\"grace_s\":1800,\"max_requests\":2,\"version\":\"stub-7\","
    "\"paused\":false,\"paused_reason\":\"\",\"paused_until\":\"\",\"excluded_nodes\":"
    "[\"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\"],\"node_windows\":"
    "{\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\":5400},\"min_plugin_version\":\"\","
    "\"min_version_reason\":\"\",\"observer_blocked\":false}";

const char kSharedKey[] =
    "{\"name\":\"shared guest\",\"version\":1,"
    "\"public_key\":\"03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8\","
    "\"private_key\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\"}";

void fill(uint8_t key[poll::kPubKeySize], uint8_t b) { memset(key, b, poll::kPubKeySize); }

std::string hexKey(char c) { return std::string(64, c); }

struct Owners {
  uint8_t targets[poll::kMaxTargets][poll::kPubKeySize];
  poll::OwnerEntry out[poll::kMaxTargets];
  poll::Policy policy;
  bool nodes_ok = false;
  bool has_policy = false;
  Owners() { memset(targets, 0, sizeof(targets)); }
  bool parse(const std::string& body, uint8_t count) {
    return poll::parseOwnersReply(body.data(), body.size(), targets, count, out, &nodes_ok,
                                  &policy, &has_policy);
  }
};

}  // namespace

TEST(PollControlJson, OwnersFromStubWithEmbeddedPolicy) {
  Owners o;
  fill(o.targets[0], 0xdd);
  fill(o.targets[1], 0xaa);
  fill(o.targets[2], 0xee);
  ASSERT_TRUE(o.parse(kOwnersEmbedded, 3));
  EXPECT_TRUE(o.nodes_ok);

  EXPECT_TRUE(o.out[0].present);
  EXPECT_EQ(poll::OWNER_ROLE_STANDBY, o.out[0].role);
  EXPECT_FALSE(o.out[0].mine);
  EXPECT_EQ(2, o.out[0].standby_rank);
  EXPECT_EQ(1, o.out[0].owner_hops);
  uint32_t t = 0;
  ASSERT_TRUE(poll::parseIsoUtc("2026-11-12T09:30:00Z", &t));
  EXPECT_EQ(t, o.out[0].latest_ts);
  EXPECT_EQ(poll::ROLE_STANDBY, poll::mapRole(true, o.out[0]));

  EXPECT_TRUE(o.out[1].present);
  EXPECT_EQ(poll::OWNER_ROLE_POLLING, o.out[1].role);
  EXPECT_TRUE(o.out[1].mine);
  EXPECT_EQ(1, o.out[1].standby_rank);
  EXPECT_EQ(0, o.out[1].owner_hops);
  EXPECT_EQ(poll::ROLE_POLLER, poll::mapRole(true, o.out[1]));

  EXPECT_FALSE(o.out[2].present);
  EXPECT_FALSE(o.out[3].present);

  ASSERT_TRUE(o.has_policy);
  EXPECT_TRUE(o.policy.paused);
  ASSERT_TRUE(poll::parseIsoUtc("2026-11-12T18:00:00Z", &t));
  EXPECT_EQ(t, o.policy.paused_until);
  EXPECT_STREQ("stub-7", o.policy.version);
  EXPECT_STREQ("0.11.0", o.policy.min_plugin_version);
  EXPECT_TRUE(o.policy.has_version_ok);
  EXPECT_TRUE(o.policy.version_ok);
  EXPECT_FALSE(o.policy.observer_blocked);
  ASSERT_EQ(1, o.policy.excluded_count);
  uint8_t bb[poll::kPubKeySize];
  fill(bb, 0xbb);
  EXPECT_TRUE(poll::policyExcluded(o.policy, bb));
  ASSERT_EQ(1, o.policy.node_window_count);
  EXPECT_EQ(5400u, o.policy.node_windows[0].window_s);
  EXPECT_EQ(poll::GATE_PAUSED, poll::policyGate(o.policy, poll::kPluginVersion, t - 1));
}

TEST(PollControlJson, OwnersWithoutPolicy) {
  Owners o;
  fill(o.targets[0], 0xaa);
  ASSERT_TRUE(o.parse(kOwnersPlain, 1));
  EXPECT_TRUE(o.nodes_ok);
  EXPECT_FALSE(o.has_policy);
  EXPECT_TRUE(o.out[0].present);
  // The untouched policy holds the defaults.
  EXPECT_EQ(poll::kDirectDefaultS, o.policy.direct_s);
  EXPECT_FALSE(o.policy.has_version_ok);
}

TEST(PollControlJson, OwnersNullPolicyArgIsAllowed) {
  uint8_t targets[1][poll::kPubKeySize];
  fill(targets[0], 0xaa);
  poll::OwnerEntry out[poll::kMaxTargets];
  bool ok = false;
  ASSERT_TRUE(poll::parseOwnersReply(kOwnersEmbedded, strlen(kOwnersEmbedded), targets, 1, out, &ok,
                                     nullptr, nullptr));
  EXPECT_TRUE(ok);
  EXPECT_TRUE(out[0].present);
}

TEST(PollControlJson, OwnersUppercaseKeysMatch) {
  Owners o;
  fill(o.targets[0], 0xab);
  std::string body = "{\"nodes\":{\"" + std::string(32, 'A') + std::string(32, 'b') + "\":{}," +
                     "\"" + std::string("AB") + std::string(62, 'b') + "\":{}," +
                     "\"" + [] { std::string s; for (int i = 0; i < 32; i++) s += (i % 2) ? "aB" : "Ab"; return s; }() +
                     "\":{\"role\":\"independent\"}}}";
  ASSERT_TRUE(o.parse(body, 1));
  EXPECT_TRUE(o.out[0].present);
  EXPECT_EQ(poll::OWNER_ROLE_INDEPENDENT, o.out[0].role);
}

TEST(PollControlJson, OwnersWithoutNodesIsNotOk) {
  Owners o;
  fill(o.targets[0], 0xaa);
  ASSERT_TRUE(o.parse("{\"nodes\":[1,2]}", 1));
  EXPECT_FALSE(o.nodes_ok);
  ASSERT_TRUE(o.parse("{\"observer\":\"x\"}", 1));
  EXPECT_FALSE(o.nodes_ok);
  ASSERT_TRUE(o.parse("{\"nodes\":{}}", 1));
  EXPECT_TRUE(o.nodes_ok);
  EXPECT_FALSE(o.out[0].present);
}

TEST(PollControlJson, InvalidOwnersEnvelopeCannotSupplyPolicy) {
  Owners o;
  const char* invalid[] = {
      "{\"nodes\":null,\"policy\":{}}",
      "{\"policy\":{\"paused\":true}}",
      "{\"nodes\":[],\"policy\":{\"paused\":true}}",
      "{\"nodes\":42,\"policy\":{\"paused\":true}}",
      "{\"nodes\":\"bad\",\"policy\":{\"paused\":true}}"};
  for (const char* body : invalid) {
    ASSERT_TRUE(o.parse("{\"nodes\":{},\"policy\":{\"paused\":true}}", 0));
    ASSERT_TRUE(o.nodes_ok);
    ASSERT_TRUE(o.has_policy);
    ASSERT_TRUE(o.parse(body, 0)) << body;
    EXPECT_FALSE(o.nodes_ok) << body;
    EXPECT_FALSE(o.has_policy) << body;
    EXPECT_FALSE(o.policy.paused) << body;
    EXPECT_EQ(poll::kDirectDefaultS, o.policy.direct_s);
  }
}

TEST(PollControlJson, OwnersEntryFieldTypes) {
  Owners o;
  fill(o.targets[0], 0x01);
  fill(o.targets[1], 0x02);
  fill(o.targets[2], 0x03);
  fill(o.targets[3], 0x04);
  fill(o.targets[4], 0x05);
  std::string body = "{\"nodes\":{"
      "\"" + hexKey('9') + "\":{},"
      "\"0101010101010101010101010101010101010101010101010101010101010101\":{\"role\":null,\"mine\":1,"
          "\"standby_rank\":0,\"owner_hops\":-3,\"latest_ts\":12345},"
      "\"0202020202020202020202020202020202020202020202020202020202020202\":{\"role\":7,\"mine\":\"\","
          "\"excluded\":\"yes\",\"standby_rank\":\"9\",\"owner_hops\":400,\"latest_ts\":\"bad\"},"
      "\"0303030303030303030303030303030303030303030303030303030303030303\":{\"role\":\"\","
          "\"standby_rank\":1000,\"latest_ts\":\"2026-11-12T10:00:00\"},"
      "\"0404040404040404040404040404040404040404040404040404040404040404\":\"not a dict\","
      "\"0505050505050505050505050505050505050505050505050505050505050505\":{\"role\":\"weird\","
          "\"standby_rank\":\"x\",\"extra\":{\"deep\":[1,2,3]}}"
      "}}";
  ASSERT_TRUE(o.parse(body, 5));

  EXPECT_TRUE(o.out[0].present);
  EXPECT_EQ(poll::OWNER_ROLE_ABSENT, o.out[0].role);
  EXPECT_TRUE(o.out[0].mine);
  EXPECT_EQ(1, o.out[0].standby_rank);
  EXPECT_EQ(0, o.out[0].owner_hops);
  EXPECT_EQ(0u, o.out[0].latest_ts);
  EXPECT_EQ(poll::ROLE_POLLER, poll::mapRole(true, o.out[0]));

  EXPECT_EQ(poll::OWNER_ROLE_UNKNOWN, o.out[1].role);
  EXPECT_FALSE(o.out[1].mine);
  EXPECT_TRUE(o.out[1].excluded);
  EXPECT_EQ(9, o.out[1].standby_rank);
  EXPECT_EQ(255, o.out[1].owner_hops);
  EXPECT_EQ(0u, o.out[1].latest_ts);
  EXPECT_EQ(poll::ROLE_EXCLUDED, poll::mapRole(true, o.out[1]));

  EXPECT_EQ(poll::OWNER_ROLE_UNKNOWN, o.out[2].role);
  EXPECT_EQ(255, o.out[2].standby_rank);
  EXPECT_EQ(0u, o.out[2].latest_ts);  // no zone: rejected

  EXPECT_FALSE(o.out[3].present);

  EXPECT_TRUE(o.out[4].present);
  EXPECT_EQ(poll::OWNER_ROLE_UNKNOWN, o.out[4].role);
  EXPECT_EQ(1, o.out[4].standby_rank);
  EXPECT_EQ(poll::ROLE_STANDBY, poll::mapRole(true, o.out[4]));
}

TEST(PollControlJson, MoreNodesThanTargets) {
  Owners o;
  fill(o.targets[0], 0x07);
  std::string body = "{\"nodes\":{";
  for (int i = 0; i < 40; i++) {
    char key[65];
    for (int j = 0; j < 32; j++) snprintf(key + 2 * j, 3, "%02x", (unsigned)(i + 1));
    if (i) body += ",";
    body += std::string("\"") + key + "\":{\"role\":\"standby\",\"standby_rank\":" + std::to_string(i + 1) + "}";
  }
  body += "}}";
  ASSERT_TRUE(o.parse(body, 1));
  EXPECT_TRUE(o.out[0].present);
  EXPECT_EQ(7, o.out[0].standby_rank);
  for (int i = 1; i < poll::kMaxTargets; i++) EXPECT_FALSE(o.out[i].present);
}

TEST(PollControlJson, DuplicateTargetsBothFilled) {
  Owners o;
  fill(o.targets[0], 0xaa);
  fill(o.targets[1], 0xaa);
  ASSERT_TRUE(o.parse(kOwnersPlain, 2));
  EXPECT_TRUE(o.out[0].present);
  EXPECT_TRUE(o.out[1].present);
}

TEST(PollControlJson, PolicyFromStub) {
  poll::Policy p;
  ASSERT_TRUE(poll::parsePolicyReply(kPolicyPlain, strlen(kPolicyPlain), p));
  EXPECT_FALSE(p.paused);
  EXPECT_EQ(0u, p.paused_until);
  EXPECT_STREQ("stub-7", p.version);
  EXPECT_FALSE(p.has_version_ok);
  EXPECT_FALSE(p.observer_blocked);
  EXPECT_EQ(1, p.excluded_count);
  EXPECT_EQ(1, p.node_window_count);
  EXPECT_EQ(3600u, p.direct_s);
  EXPECT_EQ(2, p.max_requests);
  EXPECT_FALSE(p.ever_fetched);
  EXPECT_EQ(0u, p.fetched_at);
  EXPECT_EQ(0, p.last_status);
}

TEST(PollControlJson, PolicyMissingFieldsKeepDefaults) {
  poll::Policy p;
  ASSERT_TRUE(poll::parsePolicyReply("{}", 2, p));
  poll::Policy d;
  poll::policyDefaults(d);
  EXPECT_EQ(0, memcmp(&d, &p, sizeof(p)));
}

TEST(PollControlJson, PolicyWrongTypesIgnoredAndUnclamped) {
  const std::string body =
      "{\"direct_s\":\"60\",\"one_hop_s\":null,\"grace_s\":[1],\"max_requests\":99,"
      "\"version\":42,\"paused\":\"true\",\"paused_until\":\"2026-11-12T18:00:00Z\","
      "\"excluded_nodes\":\"" + hexKey('a') + "\",\"node_windows\":[1,2],"
      "\"min_plugin_version\":\"0.12.0-very-long-label\",\"version_ok\":null,\"observer_blocked\":1,"
      "\"unknown\":{\"x\":1}}";
  poll::Policy p;
  ASSERT_TRUE(poll::parsePolicyReply(body.data(), body.size(), p));
  EXPECT_EQ(60u, p.direct_s);                   // a numeric string counts, unclamped
  EXPECT_EQ(poll::kOneHopDefaultS, p.one_hop_s);
  EXPECT_EQ(poll::kGraceDefaultS, p.grace_s);
  EXPECT_EQ(99, p.max_requests);
  EXPECT_STREQ("42", p.version);
  EXPECT_FALSE(p.paused);                       // literal true only
  EXPECT_NE(0u, p.paused_until);
  EXPECT_EQ(0, p.excluded_count);
  EXPECT_EQ(0, p.node_window_count);
  EXPECT_EQ(sizeof(p.min_plugin_version) - 1, strlen(p.min_plugin_version));
  EXPECT_TRUE(p.has_version_ok);
  EXPECT_TRUE(p.version_ok);                    // anything but literal false
  EXPECT_FALSE(p.observer_blocked);             // literal true only
  poll::policyClamp(p);
  EXPECT_EQ(poll::kDirectMinS, p.direct_s);
  EXPECT_EQ(poll::kMaxRequestsMax, p.max_requests);
}

TEST(PollControlJson, PolicyNumbersFloatsNegativesBools) {
  const std::string body =
      "{\"direct_s\":5400.9,\"one_hop_s\":-5,\"grace_s\":true,\"max_requests\":-1,"
      "\"version_ok\":false,\"observer_blocked\":true,\"paused\":true,\"paused_until\":\"soon\"}";
  poll::Policy p;
  ASSERT_TRUE(poll::parsePolicyReply(body.data(), body.size(), p));
  EXPECT_EQ(5400u, p.direct_s);
  EXPECT_EQ(0u, p.one_hop_s);
  EXPECT_EQ(1u, p.grace_s);
  EXPECT_EQ(0, p.max_requests);
  EXPECT_TRUE(p.has_version_ok);
  EXPECT_FALSE(p.version_ok);
  EXPECT_TRUE(p.observer_blocked);
  EXPECT_TRUE(p.paused);
  EXPECT_EQ(0u, p.paused_until);  // unparsable: until lifted
}

TEST(PollControlJson, PolicyExcludedOverflowAndValidity) {
  std::string body = "{\"excluded_nodes\":[\"short\",5,null,\"" + hexKey('g') + "\"";
  for (int i = 0; i < 24; i++) {
    char key[65];
    for (int j = 0; j < 32; j++) snprintf(key + 2 * j, 3, "%02X", (unsigned)(i + 1));
    body += std::string(",\"") + key + "\"";
    if (i == 2) body += std::string(",\"") + key + "\"";  // duplicate
  }
  body += "]}";
  poll::Policy p;
  ASSERT_TRUE(poll::parsePolicyReply(body.data(), body.size(), p));
  ASSERT_EQ(poll::kMaxExcluded, p.excluded_count);
  for (int i = 0; i < poll::kMaxExcluded; i++) EXPECT_EQ(i + 1, p.excluded[i][0]);
  EXPECT_TRUE(p.excluded_truncated);
  EXPECT_FALSE(p.windows_truncated);
}

// 24 excluded and 12 windows: the entries for this node's targets are kept even past the limit.
TEST(PollControlJson, PolicyOverflowKeepsTheTargetsEntries) {
  std::string ex = "{\"excluded_nodes\":[";
  std::string nw = "\"node_windows\":{";
  for (int i = 0; i < 24; i++) {
    char key[65];
    for (int j = 0; j < 32; j++) snprintf(key + 2 * j, 3, "%02x", (unsigned)(i + 1));
    ex += std::string(i ? "," : "") + "\"" + key + "\"";
    if (i < 12) nw += std::string(i ? "," : "") + "\"" + key + "\":" + std::to_string(4000 + i);
  }
  const std::string body = ex + "]," + nw + "}}";
  uint8_t targets[2][poll::kPubKeySize];
  memset(targets[0], 24, sizeof(targets[0]));   // the last excluded entry
  memset(targets[1], 12, sizeof(targets[1]));   // the last window entry
  poll::Policy p;
  ASSERT_TRUE(poll::parsePolicyReply(body.data(), body.size(), p, nullptr, targets, 2));
  EXPECT_TRUE(p.excluded_truncated);
  EXPECT_TRUE(p.windows_truncated);
  ASSERT_EQ(poll::kMaxExcluded, p.excluded_count);
  EXPECT_TRUE(poll::policyExcluded(p, targets[0]));
  EXPECT_TRUE(poll::policyExcluded(p, targets[1]));
  ASSERT_EQ(poll::kMaxNodeWindows, p.node_window_count);
  EXPECT_EQ(4011U, poll::policyWindow(p, targets[1], 0, 0));

  // The owners reply's embedded policy is read the same way.
  const std::string owners = "{\"nodes\":{},\"policy\":" + body + "}";
  poll::OwnerEntry out[poll::kMaxTargets];
  bool nodes_ok = false, has_policy = false;
  poll::Policy q;
  ASSERT_TRUE(poll::parseOwnersReply(owners.data(), owners.size(), targets, 2, out, &nodes_ok, &q, &has_policy));
  ASSERT_TRUE(has_policy);
  EXPECT_TRUE(poll::policyExcluded(q, targets[0]));
  EXPECT_EQ(4011U, poll::policyWindow(q, targets[1], 0, 0));
}

TEST(PollControlJson, PolicyThatFitsIsNotTruncated) {
  poll::Policy p;
  const std::string body = "{\"excluded_nodes\":[\"" + hexKey('a') + "\"],\"node_windows\":{\"" + hexKey('b') + "\":4000}}";
  ASSERT_TRUE(poll::parsePolicyReply(body.data(), body.size(), p));
  EXPECT_FALSE(p.excluded_truncated);
  EXPECT_FALSE(p.windows_truncated);
}

TEST(PollControlJson, PolicyNodeWindowsOverflowAndValidity) {
  std::string body = "{\"node_windows\":{\"bad\":100,\"" + hexKey('a') + "\":0,\"" + hexKey('b') +
                     "\":\"x\",\"" + hexKey('c') + "\":-60";
  for (int i = 0; i < 12; i++) {
    char key[65];
    for (int j = 0; j < 32; j++) snprintf(key + 2 * j, 3, "%02x", (unsigned)(i + 1));
    body += std::string(",\"") + key + "\":" + std::to_string(1000 * (i + 1));
  }
  body += "}}";
  poll::Policy p;
  ASSERT_TRUE(poll::parsePolicyReply(body.data(), body.size(), p));
  ASSERT_EQ(poll::kMaxNodeWindows, p.node_window_count);
  for (int i = 0; i < poll::kMaxNodeWindows; i++) {
    EXPECT_EQ(i + 1, p.node_windows[i].node[0]);
    EXPECT_EQ((uint32_t)(1000 * (i + 1)), p.node_windows[i].window_s);
  }
  EXPECT_TRUE(p.windows_truncated);
}

TEST(PollControlJson, RegistrationReply) {
  EXPECT_TRUE(poll::parseRegistrationReply("{\"ok\":true,\"expires_at\":\"2026-11-13T10:00:00Z\"}", 47));
  const char* no[] = {"{\"ok\":1}", "{\"ok\":\"true\"}", "{\"ok\":false}", "{}",
                      "{\"error\":\"observer_id must be a hex string\"}", "[true]", "true", ""};
  for (const char* b : no) EXPECT_FALSE(poll::parseRegistrationReply(b, strlen(b))) << b;
}

TEST(PollControlJson, SharedKeyFromStub) {
  uint8_t pub[32], prv[64];
  uint8_t n = 0;
  ASSERT_TRUE(poll::parseSharedKeyReply(kSharedKey, strlen(kSharedKey), pub, prv, &n));
  EXPECT_EQ(32, n);
  EXPECT_EQ(0x03, pub[0]);
  EXPECT_EQ(0xb8, pub[31]);
  for (int i = 0; i < 32; i++) EXPECT_EQ(i, prv[i]);
}

TEST(PollControlJson, SharedKeyExpandedAndBad) {
  uint8_t pub[32], prv[64];
  uint8_t n = 0;
  std::string body = "{\"public_key\":\"" + hexKey('A') + "\",\"private_key\":\"" + std::string(128, 'f') + "\"}";
  ASSERT_TRUE(poll::parseSharedKeyReply(body.data(), body.size(), pub, prv, &n));
  EXPECT_EQ(64, n);
  EXPECT_EQ(0xaa, pub[0]);
  EXPECT_EQ(0xff, prv[63]);

  const std::string bad[] = {
      "{\"public_key\":\"" + hexKey('a') + "\"}",
      "{\"public_key\":\"" + hexKey('a') + "\",\"private_key\":\"" + std::string(96, 'f') + "\"}",
      "{\"public_key\":\"" + std::string(62, 'a') + "\",\"private_key\":\"" + std::string(64, 'f') + "\"}",
      "{\"public_key\":\"" + hexKey('a') + "\",\"private_key\":\"" + std::string(63, 'f') + "z\"}",
      "{\"public_key\":\"" + hexKey('a') + "\",\"private_key\":12}",
      "{\"error\":\"no shared guest key\"}",
  };
  for (const std::string& b : bad) {
    memset(prv, 0x5a, sizeof(prv));
    EXPECT_FALSE(poll::parseSharedKeyReply(b.data(), b.size(), pub, prv, &n)) << b;
    EXPECT_EQ(0, n);
  }
}

TEST(PollControlJson, RejectsNonObjectsTruncatedAndJunk) {
  Owners o;
  fill(o.targets[0], 0xaa);
  poll::Policy p;
  uint8_t pub[32], prv[64], n = 0;

  const std::string truncated = std::string(kOwnersEmbedded).substr(0, strlen(kOwnersEmbedded) - 7);
  EXPECT_FALSE(o.parse(truncated, 1));
  EXPECT_FALSE(o.parse("[]", 1));
  EXPECT_FALSE(o.parse("null", 1));
  EXPECT_FALSE(o.parse("<html>502 Bad Gateway</html>", 1));
  EXPECT_FALSE(o.parse("", 1));
  EXPECT_FALSE(poll::parsePolicyReply("\"text\"", 6, p));
  EXPECT_FALSE(poll::parseSharedKeyReply("42", 2, pub, prv, &n));

  std::string junk;
  uint32_t x = 12345;
  for (int i = 0; i < 6144; i++) {
    x = x * 1103515245u + 12345u;
    junk += (char)(32 + (x >> 16) % 95);
  }
  EXPECT_FALSE(o.parse(junk, 1));
  EXPECT_FALSE(poll::parsePolicyReply(junk.data(), junk.size(), p));
  EXPECT_FALSE(poll::parseRegistrationReply(junk.data(), junk.size()));
  EXPECT_FALSE(poll::parseSharedKeyReply(junk.data(), junk.size(), pub, prv, &n));

  // Deep nesting is refused, even inside a field the filter drops.
  std::string deep = "{\"x\":" + std::string(40, '[') + std::string(40, ']') + ",\"ok\":true}";
  EXPECT_FALSE(poll::parseRegistrationReply(deep.data(), deep.size()));

  // Length is honored: trailing bytes past len are not read.
  const char padded[] = "{\"ok\":true}garbage";
  EXPECT_TRUE(poll::parseRegistrationReply(padded, 11));
}

TEST(PollControlJson, SixKilobyteValidBodyParses) {
  Owners o;
  fill(o.targets[0], 0xaa);
  std::string body = "{\"pad\":\"" + std::string(5800, 'p') + "\",\"nodes\":{\"" + hexKey('a') +
                     "\":{\"role\":\"polling\",\"mine\":true}}}";
  ASSERT_TRUE(o.parse(body, 1));
  EXPECT_TRUE(o.out[0].present);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
