#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "helpers/poll/PollRegistration.h"

namespace {

constexpr uint32_t kTs = 1700000000;

uint8_t g_observer[poll::kPubKeySize];
uint8_t g_poll[poll::kPubKeySize];
uint8_t g_prv[32];
uint8_t g_prv64[64];
uint8_t g_sig[64];

void initKeys() {
  for (uint8_t i = 0; i < poll::kPubKeySize; i++) {
    g_observer[i] = i;
    g_poll[i] = 0xa5;
  }
  for (uint8_t i = 0; i < 32; i++) g_prv[i] = (uint8_t)(32 + i);
  for (uint8_t i = 0; i < 64; i++) {
    g_prv64[i] = i;
    g_sig[i] = i;
  }
}

void expectMessage(const std::string& expected) {
  std::vector<char> roomy(expected.size() + 16, 'X');
  size_t len = poll::buildRegistrationMessage(roomy.data(), roomy.size(), g_observer, g_poll, kTs);
  ASSERT_EQ(expected.size(), len);
  EXPECT_EQ(expected, std::string(roomy.data(), len));

  std::vector<char> exact(expected.size() + 1 + 8, 'X');
  len = poll::buildRegistrationMessage(exact.data(), expected.size() + 1, g_observer, g_poll, kTs);
  ASSERT_EQ(expected.size(), len);
  for (size_t i = expected.size() + 1; i < exact.size(); i++) EXPECT_EQ('X', exact[i]);

  std::vector<char> tight(expected.size() + 8, 'X');
  len = poll::buildRegistrationMessage(tight.data(), expected.size(), g_observer, g_poll, kTs);
  EXPECT_EQ(0u, len);
  for (size_t i = expected.size(); i < tight.size(); i++) EXPECT_EQ('X', tight[i]);
}

void expectBody(const std::string& expected, const uint8_t* prv, size_t prv_len) {
  std::vector<char> roomy(expected.size() + 16, 'X');
  size_t len = poll::buildRegistrationBody(roomy.data(), roomy.size(), g_observer, g_poll, prv,
                                           prv_len, kTs, g_sig);
  ASSERT_EQ(expected.size(), len);
  EXPECT_EQ(expected, std::string(roomy.data(), len));

  std::vector<char> exact(expected.size() + 1 + 8, 'X');
  len = poll::buildRegistrationBody(exact.data(), expected.size() + 1, g_observer, g_poll, prv,
                                    prv_len, kTs, g_sig);
  ASSERT_EQ(expected.size(), len);
  for (size_t i = expected.size() + 1; i < exact.size(); i++) EXPECT_EQ('X', exact[i]);

  std::vector<char> tight(expected.size() + 8, 'X');
  len = poll::buildRegistrationBody(tight.data(), expected.size(), g_observer, g_poll, prv,
                                    prv_len, kTs, g_sig);
  EXPECT_EQ(0u, len);
  for (size_t i = expected.size(); i < tight.size(); i++) EXPECT_EQ('X', tight[i]);
}

TEST(PollRegistration, MessageMatchesPlugin) {
  initKeys();
  expectMessage(
      "corescope-guest-key/v1|000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f|"
      "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5|1700000000");
}

TEST(PollRegistration, BodyMatchesPlugin) {
  initKeys();
  expectBody(
      "{\"observer_id\": "
      "\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\", "
      "\"public_key\": "
      "\"a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5\", "
      "\"private_key\": "
      "\"202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f\", "
      "\"timestamp\": 1700000000, \"signature\": "
      "\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
      "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f\"}",
      g_prv, sizeof(g_prv));
}

TEST(PollRegistration, BodyAcceptsExpandedPrivateKey) {
  initKeys();
  expectBody(
      "{\"observer_id\": "
      "\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\", "
      "\"public_key\": "
      "\"a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5\", "
      "\"private_key\": "
      "\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
      "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f\", "
      "\"timestamp\": 1700000000, \"signature\": "
      "\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
      "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f\"}",
      g_prv64, sizeof(g_prv64));
}

TEST(PollRegistration, NeverUploadsTheRepeatersOwnKey) {
  initKeys();
  char buf[1024];
  memset(buf, 'X', sizeof(buf));
  EXPECT_EQ(0u, poll::buildRegistrationBody(buf, sizeof(buf), g_observer, g_observer, g_prv,
                                            sizeof(g_prv), kTs, g_sig));
  EXPECT_EQ('X', buf[0]);
}

TEST(PollRegistration, RefusesMissingPieces) {
  initKeys();
  char buf[1024];
  EXPECT_EQ(0u, poll::buildRegistrationBody(buf, sizeof(buf), g_observer, g_poll, nullptr, 32,
                                            kTs, g_sig));
  EXPECT_EQ(0u, poll::buildRegistrationBody(buf, sizeof(buf), g_observer, g_poll, g_prv, 0, kTs,
                                            g_sig));
  EXPECT_EQ(0u, poll::buildRegistrationBody(buf, sizeof(buf), g_observer, g_poll, g_prv, 32, kTs,
                                            nullptr));
  EXPECT_EQ(0u, poll::buildRegistrationBody(nullptr, sizeof(buf), g_observer, g_poll, g_prv, 32,
                                            kTs, g_sig));
  EXPECT_EQ(0u, poll::buildRegistrationBody(buf, 0, g_observer, g_poll, g_prv, 32, kTs, g_sig));
  EXPECT_EQ(0u, poll::buildRegistrationMessage(buf, sizeof(buf), nullptr, g_poll, kTs));
  EXPECT_EQ(0u, poll::buildRegistrationMessage(buf, sizeof(buf), g_observer, nullptr, kTs));
}

TEST(PollRegistration, RetriesEverySixHoursAfterSuccess) {
  const uint32_t base = kTs;
  poll::RegistrationState s{false, false, 0, 0, 0};
  EXPECT_TRUE(poll::registrationDue(s, base));

  poll::registrationResult(s, 200, true, base);
  EXPECT_TRUE(s.ok);
  EXPECT_FALSE(s.unsupported);
  EXPECT_EQ(base, s.ok_at);
  EXPECT_EQ(base, s.tried_at);
  EXPECT_EQ(200, s.last_status);
  EXPECT_FALSE(poll::registrationDue(s, base + 21599));
  EXPECT_TRUE(poll::registrationDue(s, base + 21600));
}

TEST(PollRegistration, RetriesEverySixHoursAfter404) {
  const uint32_t base = kTs;
  poll::RegistrationState s{false, false, 0, 0, 0};
  poll::registrationResult(s, 404, false, base);
  EXPECT_FALSE(s.ok);
  EXPECT_TRUE(s.unsupported);
  EXPECT_EQ(0u, s.ok_at);
  EXPECT_FALSE(poll::registrationDue(s, base + 21599));
  EXPECT_TRUE(poll::registrationDue(s, base + 21600));
}

TEST(PollRegistration, RetriesFailedAttemptAfterFiveMinutes) {
  const uint32_t base = kTs;
  poll::RegistrationState s{false, false, 0, 0, 0};
  poll::registrationResult(s, 0, false, base);
  EXPECT_FALSE(s.ok);
  EXPECT_FALSE(s.unsupported);
  EXPECT_EQ(0, s.last_status);
  EXPECT_FALSE(poll::registrationDue(s, base + 299));
  EXPECT_TRUE(poll::registrationDue(s, base + 300));

  poll::registrationResult(s, 500, false, base + 300);
  EXPECT_EQ(500, s.last_status);
  EXPECT_FALSE(poll::registrationDue(s, base + 599));
  EXPECT_TRUE(poll::registrationDue(s, base + 600));
}

TEST(PollRegistration, AFailedRefreshKeepsTheEarlierAnswer) {
  const uint32_t base = kTs;
  poll::RegistrationState s{false, false, 0, 0, 0};
  poll::registrationResult(s, 200, true, base);
  poll::registrationResult(s, 503, false, base + 100);
  EXPECT_TRUE(s.ok);
  EXPECT_EQ(base, s.ok_at);
  EXPECT_EQ(base + 100, s.tried_at);
  EXPECT_FALSE(poll::registrationDue(s, base + 21699));
  EXPECT_TRUE(poll::registrationDue(s, base + 21700));
}

TEST(PollRegistration, A200WithoutOkIsAFailure) {
  const uint32_t base = kTs;
  poll::RegistrationState s{false, false, 0, 0, 0};
  poll::registrationResult(s, 200, false, base);
  EXPECT_FALSE(s.ok);
  EXPECT_FALSE(s.unsupported);
  EXPECT_EQ(0u, s.ok_at);
  EXPECT_FALSE(poll::registrationDue(s, base + 299));
  EXPECT_TRUE(poll::registrationDue(s, base + 300));
}

TEST(PollRegistration, UnsupportedOutlivesLaterFailures) {
  const uint32_t base = kTs;
  poll::RegistrationState s{false, false, 0, 0, 0};
  poll::registrationResult(s, 404, false, base);
  poll::registrationResult(s, 0, false, base + 21600);
  EXPECT_TRUE(s.unsupported);
  EXPECT_FALSE(poll::registrationDue(s, base + 43199));
  EXPECT_TRUE(poll::registrationDue(s, base + 43200));
}

TEST(PollRegistration, OwnKeyLastsTwentyFourHours) {
  const uint32_t base = kTs;
  poll::RegistrationState s{false, false, 0, 0, 0};
  poll::registrationResult(s, 200, true, base);
  EXPECT_EQ(poll::KEY_OWN, poll::chooseKey(s, true, base));
  EXPECT_EQ(poll::KEY_OWN, poll::chooseKey(s, false, base + 86399));
  EXPECT_EQ(poll::KEY_SHARED, poll::chooseKey(s, true, base + 86400));
  EXPECT_EQ(poll::KEY_NONE, poll::chooseKey(s, false, base + 86400));
}

TEST(PollRegistration, NoOwnKeyFallsBackToShared) {
  const uint32_t base = kTs;
  poll::RegistrationState s{false, false, 0, 0, 0};
  EXPECT_EQ(poll::KEY_SHARED, poll::chooseKey(s, true, base));
  EXPECT_EQ(poll::KEY_NONE, poll::chooseKey(s, false, base));

  poll::registrationResult(s, 404, false, base);
  EXPECT_EQ(poll::KEY_SHARED, poll::chooseKey(s, true, base));
  EXPECT_EQ(poll::KEY_NONE, poll::chooseKey(s, false, base));
}

TEST(PollRegistration, NeverTriedIsDueEvenWithStaleFlags) {
  const uint32_t base = kTs;
  poll::RegistrationState s{true, false, base - 100000, 0, 200};
  EXPECT_TRUE(poll::registrationDue(s, base));
}

TEST(PollRegistration, FutureStampIsNotDue) {
  const uint32_t base = kTs;
  poll::RegistrationState s{false, false, 0, base + 100, 0};
  EXPECT_FALSE(poll::registrationDue(s, base));
  s.ok = true;
  s.ok_at = base + 100;
  EXPECT_FALSE(poll::registrationDue(s, base));
  EXPECT_EQ(poll::KEY_OWN, poll::chooseKey(s, true, base));
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
