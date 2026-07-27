#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "helpers/RemoteControl.h"

namespace {

// A 64-hex-char public key (RC_PUB_KEY_SIZE bytes).
const char* KEY_A = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
const char* KEY_B = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
const char* DEVICE = "1234567890ABCDEF1234567890ABCDEF1234567890ABCDEF1234567890ABCDEF";

// The fake "token" is a caret-delimited string the fake crypto understands:
//   command ^ target ^ nonce ^ payloadPubkey ^ signerPubkey ^ signatureValid(0/1)
// This lets one string drive both parseRequest() and verifySignature() without
// any real JSON / base64 / crypto.
std::string makeToken(const std::string& cmd, const std::string& target,
                      const std::string& nonce, const std::string& payload_key,
                      const std::string& signer_key, bool valid) {
  return cmd + "^" + target + "^" + nonce + "^" + payload_key + "^" + signer_key +
         "^" + (valid ? "1" : "0");
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) { out.push_back(cur); cur.clear(); }
    else cur += c;
  }
  out.push_back(cur);
  return out;
}

class FakeClock : public RemoteControlClock {
public:
  unsigned long ms = 1000;
  unsigned long unix = 1700000000;  // some time after 2001
  unsigned long millisNow() override { return ms; }
  unsigned long unixNow() override { return unix; }
};

class FakeCrypto : public RemoteControlCrypto {
public:
  bool sign_should_succeed = true;
  bool malformed = false;

  // Recorded from the last signResponse().
  int sign_calls = 0;
  bool last_success = false;
  std::string last_response;
  std::string last_command;
  std::string last_request_id;
  std::string last_device_id;

  bool parseRequest(const char* token, RemoteCommandRequest& out) override {
    if (malformed) return false;
    auto f = split(token, '^');
    if (f.size() < 6) return false;
    strncpy(out.command, f[0].c_str(), sizeof(out.command) - 1);
    strncpy(out.target, f[1].c_str(), sizeof(out.target) - 1);
    strncpy(out.nonce, f[2].c_str(), sizeof(out.nonce) - 1);
    strncpy(out.public_key, f[3].c_str(), sizeof(out.public_key) - 1);
    return true;
  }

  bool verifySignature(const char* token, char* out_pubkey_hex, size_t out_size) override {
    auto f = split(token, '^');
    if (f.size() < 6) return false;
    bool valid = f[5] == "1";
    if (!valid) return false;
    strncpy(out_pubkey_hex, f[4].c_str(), out_size - 1);
    out_pubkey_hex[out_size - 1] = '\0';
    return true;
  }

  bool signResponse(const RemoteCommandResponse& resp, char* out_jwt, size_t out_size) override {
    sign_calls++;
    last_success = resp.success;
    last_response = resp.response ? resp.response : "";
    last_command = resp.command ? resp.command : "";
    last_request_id = resp.request_id ? resp.request_id : "";
    last_device_id = resp.device_id ? resp.device_id : "";
    if (!sign_should_succeed) return false;
    snprintf(out_jwt, out_size, "JWT:%d:%s", resp.success ? 1 : 0, last_response.c_str());
    return true;
  }
};

class FakeAuthorizer : public RemoteControlAuthorizer {
public:
  bool use_acl = true;
  bool authorize_result = true;
  int authorize_calls = 0;
  bool useACL() override { return use_acl; }
  bool authorize(const uint8_t* pubkey, size_t len) override {
    authorize_calls++;
    return authorize_result;
  }
};

class FakeExecutor : public RemoteControlExecutor {
public:
  FakeClock* clock = nullptr;
  unsigned long advance_ms = 0;   // simulate a slow command
  std::string reply = "ok";
  int calls = 0;
  std::string last_command;
  void execute(const char* command, char* out, size_t out_size) override {
    calls++;
    last_command = command;
    strncpy(out, reply.c_str(), out_size - 1);
    out[out_size - 1] = '\0';
    if (clock) clock->ms += advance_ms;
  }
};

struct Harness {
  FakeClock clock;
  FakeCrypto crypto;
  FakeAuthorizer authz;
  FakeExecutor exec;
  RemoteControl rc;
  char out[1024];

  Harness() : rc(&crypto, &authz, &exec, &clock) {
    exec.clock = &clock;
    out[0] = '\0';
  }
  RemoteControl::Outcome run(const std::string& token) {
    return rc.process(token.c_str(), DEVICE, out, sizeof(out));
  }
};

using Outcome = RemoteControl::Outcome;

TEST(RemoteControl, HappyPathExecutesAndSignsSuccess) {
  Harness h;
  auto o = h.run(makeToken("get bat", "", "n1", KEY_A, KEY_A, true));
  EXPECT_EQ(o, Outcome::ResponseReady);
  EXPECT_EQ(h.exec.calls, 1);
  EXPECT_EQ(h.exec.last_command, "get bat");
  EXPECT_TRUE(h.crypto.last_success);
  EXPECT_EQ(h.crypto.last_response, "ok");
  EXPECT_EQ(h.crypto.last_request_id, "n1");
  EXPECT_EQ(h.crypto.last_device_id, DEVICE);
}

TEST(RemoteControl, TargetForAnotherDeviceIsSilentlyIgnored) {
  Harness h;
  auto o = h.run(makeToken("get bat", KEY_B, "n1", KEY_A, KEY_A, true));
  EXPECT_EQ(o, Outcome::SilentIgnore);
  EXPECT_EQ(h.exec.calls, 0);
  EXPECT_EQ(h.crypto.sign_calls, 0);
}

TEST(RemoteControl, TargetMatchesDeviceCaseInsensitive) {
  Harness h;
  std::string lower_device = "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef";
  auto o = h.run(makeToken("get bat", lower_device, "n1", KEY_A, KEY_A, true));
  EXPECT_EQ(o, Outcome::ResponseReady);
  EXPECT_EQ(h.exec.calls, 1);
}

TEST(RemoteControl, EmptyCommandIsRejected) {
  Harness h;
  auto o = h.run(makeToken("", "", "n1", KEY_A, KEY_A, true));
  EXPECT_EQ(o, Outcome::ResponseReady);
  EXPECT_FALSE(h.crypto.last_success);
  EXPECT_EQ(h.exec.calls, 0);
}

TEST(RemoteControl, MalformedTokenProducesSignedError) {
  Harness h;
  h.crypto.malformed = true;
  auto o = h.run("garbage");
  EXPECT_EQ(o, Outcome::ResponseReady);
  EXPECT_FALSE(h.crypto.last_success);
  EXPECT_EQ(h.crypto.last_request_id, "");
  EXPECT_EQ(h.exec.calls, 0);
}

TEST(RemoteControl, InvalidSignatureIsRejected) {
  Harness h;
  auto o = h.run(makeToken("get bat", "", "n1", KEY_A, KEY_A, false));
  EXPECT_EQ(o, Outcome::ResponseReady);
  EXPECT_FALSE(h.crypto.last_success);
  EXPECT_NE(h.crypto.last_response.find("signature"), std::string::npos);
  EXPECT_EQ(h.exec.calls, 0);
}

TEST(RemoteControl, PayloadKeyMustMatchSigner) {
  Harness h;
  auto o = h.run(makeToken("get bat", "", "n1", KEY_A, KEY_B, true));
  EXPECT_EQ(o, Outcome::ResponseReady);
  EXPECT_FALSE(h.crypto.last_success);
  EXPECT_NE(h.crypto.last_response.find("mismatch"), std::string::npos);
  EXPECT_EQ(h.exec.calls, 0);
}

TEST(RemoteControl, NonceReplayIsRejectedOnSecondUse) {
  Harness h;
  EXPECT_EQ(h.run(makeToken("get bat", "", "dup", KEY_A, KEY_A, true)), Outcome::ResponseReady);
  EXPECT_TRUE(h.crypto.last_success);
  // Same nonce again -> rejected before verification/execution.
  auto o = h.run(makeToken("get bat", "", "dup", KEY_A, KEY_A, true));
  EXPECT_EQ(o, Outcome::ResponseReady);
  EXPECT_FALSE(h.crypto.last_success);
  EXPECT_NE(h.crypto.last_response.find("replay"), std::string::npos);
  EXPECT_EQ(h.exec.calls, 1);  // only the first executed
}

TEST(RemoteControl, NonceNotBurnedWhenAuthorizationFails) {
  Harness h;
  h.authz.authorize_result = false;
  EXPECT_EQ(h.run(makeToken("get bat", "", "n1", KEY_A, KEY_A, true)), Outcome::ResponseReady);
  EXPECT_FALSE(h.crypto.last_success);
  // A later authorized command reusing the nonce must still succeed.
  h.authz.authorize_result = true;
  h.clock.ms += 2000;  // avoid rate limit
  auto o = h.run(makeToken("get bat", "", "n1", KEY_A, KEY_A, true));
  EXPECT_EQ(o, Outcome::ResponseReady);
  EXPECT_TRUE(h.crypto.last_success);
}

TEST(RemoteControl, RateLimitRejectsRapidSecondCommand) {
  Harness h;
  EXPECT_EQ(h.run(makeToken("get bat", "", "n1", KEY_A, KEY_A, true)), Outcome::ResponseReady);
  EXPECT_TRUE(h.crypto.last_success);
  // Different nonce so replay does not mask the rate-limit path; same millis.
  auto o = h.run(makeToken("get bat", "", "n2", KEY_A, KEY_A, true));
  EXPECT_EQ(o, Outcome::ResponseReady);
  EXPECT_FALSE(h.crypto.last_success);
  EXPECT_NE(h.crypto.last_response.find("Rate limit"), std::string::npos);
  EXPECT_EQ(h.exec.calls, 1);
}

TEST(RemoteControl, RateLimitClearsAfterInterval) {
  Harness h;
  EXPECT_EQ(h.run(makeToken("get bat", "", "n1", KEY_A, KEY_A, true)), Outcome::ResponseReady);
  h.clock.ms += 1500;  // past MIN_INTERVAL_MS
  auto o = h.run(makeToken("get bat", "", "n2", KEY_A, KEY_A, true));
  EXPECT_EQ(o, Outcome::ResponseReady);
  EXPECT_TRUE(h.crypto.last_success);
  EXPECT_EQ(h.exec.calls, 2);
}

TEST(RemoteControl, RateLimitIsPerKey) {
  Harness h;
  EXPECT_EQ(h.run(makeToken("get bat", "", "n1", KEY_A, KEY_A, true)), Outcome::ResponseReady);
  auto o = h.run(makeToken("get bat", "", "n2", KEY_B, KEY_B, true));  // different key, same millis
  EXPECT_EQ(o, Outcome::ResponseReady);
  EXPECT_TRUE(h.crypto.last_success);
}

TEST(RemoteControl, DefaultBlacklistBlocksWifiPassword) {
  Harness h;
  auto o = h.run(makeToken("get wifi.pwd", "", "n1", KEY_A, KEY_A, true));
  EXPECT_EQ(o, Outcome::ResponseReady);
  EXPECT_FALSE(h.crypto.last_success);
  EXPECT_NE(h.crypto.last_response.find("not allowed"), std::string::npos);
  EXPECT_EQ(h.exec.calls, 0);
}

TEST(RemoteControl, DefaultBlacklistBlocksSetMqttAdmin) {
  Harness h;
  auto o = h.run(makeToken("set mqtt.admin DEADBEEF", "", "n1", KEY_A, KEY_A, true));
  EXPECT_EQ(o, Outcome::ResponseReady);
  EXPECT_FALSE(h.crypto.last_success);
  EXPECT_EQ(h.exec.calls, 0);
}

TEST(RemoteControl, RebootIsRejected) {
  Harness h;
  auto o = h.run(makeToken("reboot", "", "n1", KEY_A, KEY_A, true));
  EXPECT_EQ(o, Outcome::ResponseReady);
  EXPECT_FALSE(h.crypto.last_success);
  EXPECT_NE(h.crypto.last_response.find("Reboot"), std::string::npos);
  EXPECT_EQ(h.exec.calls, 0);
}

TEST(RemoteControl, UnauthorizedMessageMentionsAclWhenUsingAcl) {
  Harness h;
  h.authz.use_acl = true;
  h.authz.authorize_result = false;
  h.run(makeToken("get bat", "", "n1", KEY_A, KEY_A, true));
  EXPECT_FALSE(h.crypto.last_success);
  EXPECT_NE(h.crypto.last_response.find("ACL"), std::string::npos);
}

TEST(RemoteControl, UnauthorizedMessageMentionsKeyWhenNotUsingAcl) {
  Harness h;
  h.authz.use_acl = false;
  h.authz.authorize_result = false;
  h.run(makeToken("get bat", "", "n1", KEY_A, KEY_A, true));
  EXPECT_FALSE(h.crypto.last_success);
  EXPECT_EQ(h.crypto.last_response.find("ACL"), std::string::npos);
}

TEST(RemoteControl, SignFailureReportsSignFailed) {
  Harness h;
  h.crypto.sign_should_succeed = false;
  auto o = h.run(makeToken("get bat", "", "n1", KEY_A, KEY_A, true));
  EXPECT_EQ(o, Outcome::SignFailed);
}

TEST(RemoteControl, SlowCommandTimesOut) {
  Harness h;
  h.exec.advance_ms = RemoteControl::COMMAND_TIMEOUT_MS + 1000;
  auto o = h.run(makeToken("get bat", "", "n1", KEY_A, KEY_A, true));
  EXPECT_EQ(o, Outcome::ResponseReady);
  EXPECT_FALSE(h.crypto.last_success);
  EXPECT_NE(h.crypto.last_response.find("timeout"), std::string::npos);
  EXPECT_EQ(h.exec.calls, 1);  // it ran, but the reply is rejected
}

TEST(RemoteControl, CustomBlacklistEntryIsEnforced) {
  Harness h;
  h.rc.blacklist().add("set freq");
  auto o = h.run(makeToken("set freq 915", "", "n1", KEY_A, KEY_A, true));
  EXPECT_FALSE(h.crypto.last_success);
  EXPECT_EQ(h.exec.calls, 0);
}

// --- direct unit tests of the pure structures ---

TEST(RCNonceTracker, DetectsReplayAndWrapsAround) {
  RCNonceTracker t;
  EXPECT_FALSE(t.isUsed("a"));
  t.add("a");
  EXPECT_TRUE(t.isUsed("a"));
  // Fill past capacity; "a" should eventually be evicted.
  for (int i = 0; i < RCNonceTracker::MAX_NONCES; i++) {
    char buf[16];
    snprintf(buf, sizeof(buf), "x%d", i);
    t.add(buf);
  }
  EXPECT_FALSE(t.isUsed("a"));
}

TEST(RCRateLimiter, LimitsWithinIntervalOnly) {
  RCRateLimiter r;
  uint8_t key[RC_PUB_KEY_SIZE] = {0};
  EXPECT_FALSE(r.isRateLimited(key, 1000));
  EXPECT_TRUE(r.isRateLimited(key, 1500));
  EXPECT_FALSE(r.isRateLimited(key, 2000));
}

TEST(RCCommandBlacklist, MatchesByPrefix) {
  RCCommandBlacklist b;
  EXPECT_TRUE(b.isBlacklisted("get wifi.pwd"));
  EXPECT_TRUE(b.isBlacklisted("get wifi.pwd extra"));
  EXPECT_FALSE(b.isBlacklisted("get bat"));
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
