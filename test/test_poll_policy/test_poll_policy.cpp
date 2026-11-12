#include <gtest/gtest.h>

#include <stdint.h>
#include <string.h>

#include "helpers/poll/PollPolicy.h"

namespace {

void fillKey(uint8_t key[poll::kPubKeySize], uint8_t v) {
  memset(key, v, poll::kPubKeySize);
}

poll::Policy defaults() {
  poll::Policy p;
  poll::policyDefaults(p);
  return p;
}

bool atLeast(const char* mine, const char* minimum) {
  return poll::compareVersions(mine, minimum) >= 0;
}

}  // namespace

TEST(PollPolicy, DefaultsMatchTheUnclampedPolicy) {
  const poll::Policy p = defaults();
  EXPECT_EQ(3600U, p.direct_s);
  EXPECT_EQ(7200U, p.one_hop_s);
  EXPECT_EQ(1800U, p.grace_s);
  EXPECT_EQ(2, p.max_requests);
  EXPECT_STREQ("", p.version);
  EXPECT_FALSE(p.paused);
  EXPECT_EQ(0U, p.paused_until);
  EXPECT_FALSE(p.observer_blocked);
  EXPECT_FALSE(p.has_version_ok);
  EXPECT_EQ(0, p.excluded_count);
  EXPECT_EQ(0, p.node_window_count);
  EXPECT_FALSE(p.ever_fetched);
  EXPECT_EQ(0U, p.fetched_at);
  EXPECT_EQ(0, p.last_status);
}

TEST(PollPolicy, ClampPullsEveryLimitOntoItsEdge) {
  poll::Policy p = defaults();
  p.direct_s = 900;
  p.one_hop_s = 999999;
  p.grace_s = 60;
  p.max_requests = 9;
  p.paused = true;  // a real true stays; only the caller maps JSON
  fillKey(p.excluded[0], 0x11);
  p.excluded_count = 1;
  fillKey(p.node_windows[0].node, 0x22);
  p.node_windows[0].window_s = 600;
  fillKey(p.node_windows[1].node, 0x33);
  p.node_windows[1].window_s = 14400;
  p.node_window_count = 2;

  poll::policyClamp(p);

  EXPECT_EQ(1800U, p.direct_s);
  EXPECT_EQ(86400U, p.one_hop_s);
  EXPECT_EQ(600U, p.grace_s);
  EXPECT_EQ(3, p.max_requests);
  EXPECT_TRUE(p.paused);
  EXPECT_EQ(1800U, p.node_windows[0].window_s);
  EXPECT_EQ(14400U, p.node_windows[1].window_s);
  EXPECT_EQ(1, p.excluded_count);
}

TEST(PollPolicy, ClampLeavesInteriorValuesAndCapsCounts) {
  poll::Policy p = defaults();
  p.direct_s = 1800;
  p.one_hop_s = 3600;
  p.grace_s = 7200;
  p.max_requests = 1;
  p.excluded_count = 255;
  p.node_window_count = 255;
  p.node_windows[0].window_s = 86400;
  memset(p.version, 'A', sizeof(p.version));
  memset(p.min_plugin_version, 'B', sizeof(p.min_plugin_version));

  poll::policyClamp(p);

  EXPECT_EQ(1800U, p.direct_s);
  EXPECT_EQ(3600U, p.one_hop_s);
  EXPECT_EQ(7200U, p.grace_s);
  EXPECT_EQ(1, p.max_requests);
  EXPECT_EQ(poll::kMaxExcluded, p.excluded_count);
  EXPECT_EQ(poll::kMaxNodeWindows, p.node_window_count);
  EXPECT_EQ(86400U, p.node_windows[0].window_s);
  EXPECT_EQ('\0', p.version[sizeof(p.version) - 1]);
  EXPECT_EQ('\0', p.min_plugin_version[sizeof(p.min_plugin_version) - 1]);
}

TEST(PollPolicy, ZeroNodeWindowRisesToTheDirectFloor) {
  // A stored 0 would mean "poll continuously". The slot stays; the floor applies.
  poll::Policy p = defaults();
  p.node_windows[0].window_s = 0;
  p.node_window_count = 1;
  poll::policyClamp(p);
  EXPECT_EQ(1800U, p.node_windows[0].window_s);
}

TEST(PollPolicy, VersionCompareIsNumericAndTreatsGarbageAsZero) {
  EXPECT_TRUE(atLeast("0.8.0", ""));
  EXPECT_TRUE(atLeast("0.8.0", "0.8.0"));
  EXPECT_TRUE(atLeast("0.8.10", "0.8.2"));
  EXPECT_FALSE(atLeast("0.8.1", "0.8.2"));
  EXPECT_FALSE(atLeast("garbage", "0.0.1"));

  EXPECT_EQ(0, poll::compareVersions("v1.2.3", "1.2.3"));
  EXPECT_EQ(0, poll::compareVersions("  v1.2.3-beta", "1.2.3"));
  EXPECT_EQ(0, poll::compareVersions("1.2.3.4", "1.2.3"));
  EXPECT_EQ(0, poll::compareVersions("01.02.03", "1.2.3"));
  EXPECT_LT(poll::compareVersions("V1.2.3", "1.2.3"), 0);
  EXPECT_LT(poll::compareVersions("1.2", "1.2.0"), 0);
  EXPECT_LT(poll::compareVersions(nullptr, "0.0.1"), 0);
  EXPECT_GT(poll::compareVersions("1.0.0", nullptr), 0);
  EXPECT_GT(poll::compareVersions("1.0.0", "garbage"), 0);
  EXPECT_GT(poll::compareVersions("2.0.0", "1.9.9"), 0);
  EXPECT_EQ(0, poll::compareVersions(nullptr, ""));
  EXPECT_GT(poll::compareVersions("999999999999.0.0", "1.0.0"), 0);
}

TEST(PollPolicy, GateOrderIsBlockedThenVersionThenPause) {
  poll::Policy p = defaults();
  p.observer_blocked = true;
  p.has_version_ok = true;
  p.version_ok = false;
  p.paused = true;
  p.paused_until = 0;
  EXPECT_EQ(poll::GATE_BLOCKED, poll::policyGate(p, "0.11.1", 1'000'000));
  EXPECT_STREQ("blocked", poll::gateName(poll::policyGate(p, "0.11.1", 1'000'000)));

  p.observer_blocked = false;
  EXPECT_EQ(poll::GATE_VERSION, poll::policyGate(p, "9.9.9", 1'000'000));
  EXPECT_STREQ("version_disabled", poll::gateName(poll::GATE_VERSION));

  p.version_ok = true;
  EXPECT_EQ(poll::GATE_PAUSED, poll::policyGate(p, "9.9.9", 1'000'000));
  EXPECT_STREQ("paused", poll::gateName(poll::GATE_PAUSED));
  EXPECT_STREQ("", poll::gateName(poll::GATE_OPEN));
}

TEST(PollPolicy, VersionOkIsForTheBuildThatAsked) {
  poll::Policy p = defaults();
  strcpy(p.min_plugin_version, "9.9.9");
  p.has_version_ok = true;
  p.version_ok = false;
  // The server said no, even though a high local version would pass the minimum.
  EXPECT_EQ(poll::GATE_VERSION, poll::policyGate(p, "9.9.9", 50));

  p.version_ok = true;
  // The server said yes, even though this build is below the minimum.
  EXPECT_EQ(poll::GATE_OPEN, poll::policyGate(p, "0.11.1", 50));

  // Upgrade: the saved answer was about the old build, so it is judged afresh.
  p.has_version_ok = false;
  p.version_ok = false;
  EXPECT_EQ(poll::GATE_VERSION, poll::policyGate(p, "0.11.1", 50));
  EXPECT_EQ(poll::GATE_OPEN, poll::policyGate(p, "9.9.9", 50));
  EXPECT_EQ(poll::GATE_OPEN, poll::policyGate(p, "v9.9.10", 50));
  EXPECT_EQ(poll::GATE_VERSION, poll::policyGate(p, "garbage", 50));
  strcpy(p.min_plugin_version, "0.0.1");
  EXPECT_EQ(poll::GATE_VERSION, poll::policyGate(p, "garbage", 50));
  EXPECT_EQ(poll::GATE_VERSION, poll::policyGate(p, nullptr, 50));
  strcpy(p.min_plugin_version, "");
  EXPECT_EQ(poll::GATE_OPEN, poll::policyGate(p, nullptr, 50));
  EXPECT_EQ(poll::GATE_OPEN, poll::policyGate(p, "garbage", 50));
}

TEST(PollPolicy, PauseWithNoEndStaysAndAnEndLiftsItself) {
  poll::Policy p = defaults();
  p.paused = true;
  p.paused_until = 0;
  p.last_status = 0;
  p.ever_fetched = true;
  EXPECT_EQ(poll::GATE_PAUSED, poll::policyGate(p, "0.11.1", 0));
  EXPECT_EQ(poll::GATE_PAUSED, poll::policyGate(p, "0.11.1", 4'000'000'000U));

  const uint32_t now = 1'700'000'000U;
  p.paused_until = now + 600;
  EXPECT_EQ(poll::GATE_PAUSED, poll::policyGate(p, "0.11.1", now));
  EXPECT_EQ(poll::GATE_PAUSED, poll::policyGate(p, "0.11.1", p.paused_until - 1));
  // CoreScope can be down when the end arrives; the flag itself is not cleared.
  EXPECT_EQ(poll::GATE_OPEN, poll::policyGate(p, "0.11.1", p.paused_until));
  EXPECT_EQ(poll::GATE_OPEN, poll::policyGate(p, "0.11.1", p.paused_until + 1));
  EXPECT_TRUE(p.paused);
}

TEST(PollPolicy, SavedGateKeepsTheSwitchesAndStillLiftsAPause) {
  poll::Policy p = defaults();
  p.observer_blocked = true;
  p.has_version_ok = true;
  p.version_ok = false;
  p.paused = true;
  p.paused_until = 5'000;
  const poll::SavedGate saved = poll::savedGateFrom(p, "0.11.1");
  EXPECT_TRUE(saved.blocked);
  EXPECT_TRUE(saved.version_disabled);
  EXPECT_TRUE(saved.paused);
  EXPECT_EQ(poll::GATE_BLOCKED, poll::savedGate(saved, 4'000));

  poll::SavedGate lifted = saved;
  lifted.blocked = false;
  EXPECT_EQ(poll::GATE_VERSION, poll::savedGate(lifted, 4'000));
  lifted.version_disabled = false;
  EXPECT_EQ(poll::GATE_PAUSED, poll::savedGate(lifted, 4'999));
  EXPECT_EQ(poll::GATE_OPEN, poll::savedGate(lifted, 5'000));

  // A later build re-judges the minimum; the stored snapshot does not.
  p.observer_blocked = false;
  p.has_version_ok = false;
  p.paused = false;
  strcpy(p.min_plugin_version, "9.9.9");
  const poll::SavedGate old_build = poll::savedGateFrom(p, "0.11.1");
  const poll::SavedGate new_build = poll::savedGateFrom(p, "9.9.9");
  EXPECT_TRUE(old_build.version_disabled);
  EXPECT_FALSE(new_build.version_disabled);
  EXPECT_EQ(poll::GATE_VERSION, poll::savedGate(old_build, 10));
  EXPECT_EQ(poll::GATE_OPEN, poll::savedGate(new_build, 10));
}

TEST(PollPolicy, ExclusionIsAnExactKey) {
  poll::Policy p = defaults();
  uint8_t node[poll::kPubKeySize];
  uint8_t other[poll::kPubKeySize];
  fillKey(node, 0xAB);
  fillKey(other, 0xAB);
  other[31] = 0xAC;
  fillKey(p.excluded[0], 0x11);
  memcpy(p.excluded[1], node, poll::kPubKeySize);
  p.excluded_count = 2;

  EXPECT_TRUE(poll::policyExcluded(p, node));
  EXPECT_FALSE(poll::policyExcluded(p, other));
  EXPECT_FALSE(poll::policyExcluded(p, nullptr));

  p.excluded_count = 0;
  EXPECT_FALSE(poll::policyExcluded(p, node));
  p.excluded_count = 255;
  EXPECT_TRUE(poll::policyExcluded(p, node));
}

TEST(PollPolicy, StaleCopyDoublesTheWindowAndA404DoesNot) {
  poll::Policy p = defaults();
  uint8_t direct[poll::kPubKeySize];
  uint8_t relay[poll::kPubKeySize];
  uint8_t custom[poll::kPubKeySize];
  fillKey(direct, 1);
  fillKey(relay, 2);
  fillKey(custom, 3);
  memcpy(p.node_windows[0].node, custom, poll::kPubKeySize);
  p.node_windows[0].window_s = 4U * 3600U;
  p.node_window_count = 1;

  const uint32_t now = 1'000'000 + 7U * 3600U;
  EXPECT_FALSE(poll::policyStale(p, now));
  EXPECT_EQ(3600U, poll::policyWindow(p, direct, 0, now));
  EXPECT_EQ(7200U, poll::policyWindow(p, relay, 1, now));
  EXPECT_EQ(7200U, poll::policyWindow(p, relay, 5, now));
  EXPECT_EQ(4U * 3600U, poll::policyWindow(p, custom, 0, now));

  p.ever_fetched = true;
  p.fetched_at = now - 7U * 3600U;
  p.last_status = 200;
  EXPECT_FALSE(poll::policyStale(p, now));
  EXPECT_EQ(3600U, poll::policyWindow(p, direct, 0, now));

  p.last_status = 0;
  EXPECT_TRUE(poll::policyStale(p, now));
  EXPECT_EQ(7200U, poll::policyWindow(p, direct, 0, now));
  EXPECT_EQ(8U * 3600U, poll::policyWindow(p, custom, 1, now));

  p.last_status = 404;
  EXPECT_FALSE(poll::policyStale(p, now));
  EXPECT_EQ(3600U, poll::policyWindow(p, direct, 0, now));

  p.last_status = 500;
  p.fetched_at = now - poll::kStalePolicyS;
  EXPECT_FALSE(poll::policyStale(p, now));
  p.fetched_at = now - poll::kStalePolicyS - 1;
  EXPECT_TRUE(poll::policyStale(p, now));

  p.fetched_at = now + 10;
  EXPECT_FALSE(poll::policyStale(p, now));
  p.fetched_at = 0;
  EXPECT_FALSE(poll::policyStale(p, now));
  EXPECT_EQ(3600U, poll::policyWindow(p, nullptr, 0, now));
}

TEST(PollPolicy, IsoUtcRoundTripsAndRejectsWhatIsNotUtc) {
  char tiny[8];
  EXPECT_EQ(0U, poll::formatIsoUtc(0, nullptr, 32));
  EXPECT_EQ(0U, poll::formatIsoUtc(0, tiny, sizeof(tiny)));
  char buf[32];
  EXPECT_EQ(0U, poll::formatIsoUtc(0, buf, 20));
  EXPECT_EQ(20U, poll::formatIsoUtc(0, buf, 21));
  EXPECT_STREQ("1970-01-01T00:00:00Z", buf);

  EXPECT_EQ(20U, poll::formatIsoUtc(946684800U, buf, sizeof(buf)));
  EXPECT_STREQ("2000-01-01T00:00:00Z", buf);
  EXPECT_EQ(20U, poll::formatIsoUtc(1609459200U, buf, sizeof(buf)));
  EXPECT_STREQ("2021-01-01T00:00:00Z", buf);

  uint32_t back = 1;
  ASSERT_TRUE(poll::parseIsoUtc(buf, &back));
  EXPECT_EQ(1609459200U, back);

  for (uint32_t t = 0; t < 86400U * 40U; t += 3600U) {
    ASSERT_EQ(20U, poll::formatIsoUtc(t, buf, sizeof(buf)));
    ASSERT_TRUE(poll::parseIsoUtc(buf, &back)) << buf;
    EXPECT_EQ(t, back) << buf;
  }
  ASSERT_EQ(20U, poll::formatIsoUtc(UINT32_MAX, buf, sizeof(buf)));
  ASSERT_TRUE(poll::parseIsoUtc(buf, &back));
  EXPECT_EQ(UINT32_MAX, back);

  uint32_t leap = 0, next = 0;
  ASSERT_TRUE(poll::parseIsoUtc("2024-02-29T00:00:00Z", &leap));
  ASSERT_TRUE(poll::parseIsoUtc("2024-03-01T00:00:00Z", &next));
  EXPECT_EQ(86400U, next - leap);
  ASSERT_TRUE(poll::parseIsoUtc("2024-02-28T00:00:00Z", &back));
  EXPECT_EQ(86400U, leap - back);

  uint32_t zulu = 0, offset = 0, frac = 0, space = 0, basic = 0;
  ASSERT_TRUE(poll::parseIsoUtc("2026-11-01T10:00:00Z", &zulu));
  ASSERT_TRUE(poll::parseIsoUtc("2026-11-01T10:00:00+00:00", &offset));
  ASSERT_TRUE(poll::parseIsoUtc("2026-11-01T10:00:00.123456Z", &frac));
  ASSERT_TRUE(poll::parseIsoUtc("2026-11-01 10:00:00z", &space));
  ASSERT_TRUE(poll::parseIsoUtc("2026-11-01T10:00:00+0000", &basic));
  EXPECT_EQ(zulu, offset);
  EXPECT_EQ(zulu, frac);
  EXPECT_EQ(zulu, space);
  EXPECT_EQ(zulu, basic);
  ASSERT_TRUE(poll::parseIsoUtc("2026-11-01T11:00:00+01:00", &offset));
  EXPECT_EQ(zulu, offset);
  ASSERT_TRUE(poll::parseIsoUtc("2026-11-01T05:00:00-05:00", &offset));
  EXPECT_EQ(zulu, offset);
  ASSERT_TRUE(poll::parseIsoUtc("1970-01-01T00:00:01.999Z", &frac));
  EXPECT_EQ(1U, frac);
  ASSERT_TRUE(poll::parseIsoUtc("1970-01-01T01:00:00+01:00", &offset));
  EXPECT_EQ(0U, offset);

  EXPECT_FALSE(poll::parseIsoUtc(nullptr, &back));
  EXPECT_FALSE(poll::parseIsoUtc("", &back));
  EXPECT_FALSE(poll::parseIsoUtc("2026-11-01T10:00:00Z", nullptr));
  EXPECT_FALSE(poll::parseIsoUtc("2026-11-01T10:00:00", &back));
  EXPECT_FALSE(poll::parseIsoUtc("2026-11-01t10:00:00Z", &back));
  EXPECT_FALSE(poll::parseIsoUtc("2026-11-01", &back));
  EXPECT_FALSE(poll::parseIsoUtc("nope", &back));
  EXPECT_FALSE(poll::parseIsoUtc("2026-13-01T00:00:00Z", &back));
  EXPECT_FALSE(poll::parseIsoUtc("2023-02-29T00:00:00Z", &back));
  EXPECT_FALSE(poll::parseIsoUtc("2024-02-30T00:00:00Z", &back));
  EXPECT_FALSE(poll::parseIsoUtc("2024-01-01T24:00:00Z", &back));
  EXPECT_FALSE(poll::parseIsoUtc("2024-01-01T00:60:00Z", &back));
  EXPECT_FALSE(poll::parseIsoUtc("2024-01-01T00:00:60Z", &back));
  EXPECT_FALSE(poll::parseIsoUtc("2026-11-01T10:00:00Z ", &back));
  EXPECT_FALSE(poll::parseIsoUtc("2026-11-01T10:00:00.Z", &back));
  EXPECT_FALSE(poll::parseIsoUtc("1970-01-01T00:00:00+01:00", &back));
  EXPECT_FALSE(poll::parseIsoUtc("9999-12-31T23:59:59Z", &back));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
