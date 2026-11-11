#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "helpers/JWTHelper.h"

// Expected tokens were minted by JWTHelper at 984b294e7 (before the in-place rewrite) on this
// same host harness; Ed25519 signatures are deterministic, so any byte change shows up here.

namespace {

mesh::LocalIdentity makeIdentity(uint8_t base, uint8_t step) {
  uint8_t seed[32];
  for (int i = 0; i < 32; i++) seed[i] = static_cast<uint8_t>(base + i * step);
  return mesh::LocalIdentity(seed);
}

struct Case {
  const char* name;
  uint8_t seed_base;
  uint8_t seed_step;
  const char* audience;
  unsigned long iat;
  unsigned long exp;
  const char* owner;
  const char* client;
  const char* email;
  const char* expected;
};

const char* const kMaxAudience = "a23456789012345678901234567890123456789012345678901234567890123";
const char* const kMaxOwner = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
const char* const kMaxClient = "c23456789012345678901234567890123456789012345678901234567890123";

const Case kCases[] = {
  {"minimal", 0x01, 0x01, "mqtt-us-v1.letsmesh.net", 1700000000UL, 0, nullptr, nullptr, nullptr,
   "eyJhbGciOiJFZDI1NTE5IiwidHlwIjoiSldUIn0."
   "eyJwdWJsaWNLZXkiOiI3OUI1NTYyRThGRTY1NEY5NDA3OEIxMTJFOEE5OEJBNzkwMUY4NTNBRTY5NUJFRDdFMEUzOTEw"
   "QkFEMDQ5NjY0IiwiYXVkIjoibXF0dC11cy12MS5sZXRzbWVzaC5uZXQiLCJpYXQiOjE3MDAwMDAwMDB9."
   "C7855346F304487F8A770B7E25CE6DAACF89E31BA2ED0F179616E93C45ABF030"
   "74B06BC3C1199D367D06C882A304774C70F571F9AD897F6A02B15D3D6DD4DA0F"},
  {"all_claims", 0x42, 0x03, "mqtt-eu-v1.letsmesh.net", 1762800000UL, 86400,
   "8F1C2B3A4D5E6F708192A3B4C5D6E7F80918273645546372819AABBCCDDEEFF0",
   "meshcore-observer/v1.18.0.9", "observer.operator@example.org",
   "eyJhbGciOiJFZDI1NTE5IiwidHlwIjoiSldUIn0."
   "eyJwdWJsaWNLZXkiOiI1NjQ2RjgxQTlCMjM1MzI0RkE5NDg4REI0NTA3QjUyM0JDNDhGQkU2NjEyOEE5QkQyMDBCNzQw"
   "Nzk4NTRBMzczIiwiYXVkIjoibXF0dC1ldS12MS5sZXRzbWVzaC5uZXQiLCJpYXQiOjE3NjI4MDAwMDAsImV4cCI6MTc2"
   "Mjg4NjQwMCwib3duZXIiOiI4RjFDMkIzQTRENUU2RjcwODE5MkEzQjRDNUQ2RTdGODA5MTgyNzM2NDU1NDYzNzI4MTlB"
   "QUJCQ0NEREVFRkYwIiwiY2xpZW50IjoibWVzaGNvcmUtb2JzZXJ2ZXIvdjEuMTguMC45IiwiZW1haWwiOiJvYnNlcnZl"
   "ci5vcGVyYXRvckBleGFtcGxlLm9yZyJ9."
   "591D997B952ED54A13744B4AC93866BBE00D33A11D56AB1ED856FFB79F50FCEB"
   "4AC060B85443F08C79D2169AB32704EBDF0C4D1F2A06B6A47D34FD5E695AD904"},
  {"long_audience", 0x9C, 0x07, "mqtt-observer-ingest-v1.regional-mesh-analyzer.example-net.org",
   1893456000UL, 604800, "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF",
   "meshcore-observer/v1.18.0.9 (heltec_v4)", "ops@example.org",
   "eyJhbGciOiJFZDI1NTE5IiwidHlwIjoiSldUIn0."
   "eyJwdWJsaWNLZXkiOiIzQzNCMjkwRjlGODY2QzQ2NkU2Rjc2MkVDNDUzODJFMkQ5MkI5OTMwNzVDN0M1RTE2MTBFQUZE"
   "OURDM0U2QThFIiwiYXVkIjoibXF0dC1vYnNlcnZlci1pbmdlc3QtdjEucmVnaW9uYWwtbWVzaC1hbmFseXplci5leGFt"
   "cGxlLW5ldC5vcmciLCJpYXQiOjE4OTM0NTYwMDAsImV4cCI6MTg5NDA2MDgwMCwib3duZXIiOiIwMDExMjIzMzQ0NTU2"
   "Njc3ODg5OUFBQkJDQ0RERUVGRjAwMTEyMjMzNDQ1NTY2Nzc4ODk5QUFCQkNDRERFRUZGIiwiY2xpZW50IjoibWVzaGNv"
   "cmUtb2JzZXJ2ZXIvdjEuMTguMC45IChoZWx0ZWNfdjQpIiwiZW1haWwiOiJvcHNAZXhhbXBsZS5vcmcifQ."
   "89EE2B75A7910C600BBCF7C5CB518E2C33411AB4971529D763E47BF887F8F4BF"
   "BCFE13AAA668E5BE90D07C56701773497A4BBFE2C6C000F681FABD58CE50E60E"},
  // Every field at its prefs maximum with a 34-char email: the largest payload that still fits.
  {"largest_payload", 0x10, 0x05, kMaxAudience, 4000000000UL, 31536000UL, kMaxOwner, kMaxClient,
   "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
   "eyJhbGciOiJFZDI1NTE5IiwidHlwIjoiSldUIn0."
   "eyJwdWJsaWNLZXkiOiI4QkJFMEIzRDRBMkJGQTAxNDRDMUVDMDE2QzhFQTkxQTMxNDg0MkY4QjY0NTcxNzg1QTFGNUE4"
   "QkQxMzY4QjBFIiwiYXVkIjoiYTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNDU2Nzg5MDEyMzQ1Njc4OTAx"
   "MjM0NTY3ODkwMTIzIiwiaWF0Ijo0MDAwMDAwMDAwLCJleHAiOjQwMzE1MzYwMDAsIm93bmVyIjoiMDEyMzQ1Njc4OUFC"
   "Q0RFRjAxMjM0NTY3ODlBQkNERUYwMTIzNDU2Nzg5QUJDREVGMDEyMzQ1Njc4OUFCQ0RFRiIsImNsaWVudCI6ImMyMzQ1"
   "Njc4OTAxMjM0NTY3ODkwMTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNDU2Nzg5MDEyMyIsImVtYWlsIjoi"
   "ZWVlZWVlZWVlZWVlZWVlZWVlZWVlZWVlZWVlZWVlZWVlZSJ9."
   "7FC6DDAE8DAAF335839F8C1154A25729A7AE8B1924EC2E79BC59FBF0B1746466"
   "10896746E61A111B83069A806F36478BB81E2E5BD4CE2542BCB1499AA6093803"},
};

const size_t kTokenBufSize = 768;  // MQTTBridge::AUTH_TOKEN_SIZE

bool mint(const Case& c, char* token, size_t tokenSize, const char* email) {
  mesh::LocalIdentity id = makeIdentity(c.seed_base, c.seed_step);
  return JWTHelper::createAuthToken(id, c.audience, c.iat, c.exp, token, tokenSize, c.owner,
                                    c.client, email);
}

bool mint(const Case& c, char* token, size_t tokenSize) { return mint(c, token, tokenSize, c.email); }

bool isFilled(const char* buf, size_t len, char fill) {
  for (size_t i = 0; i < len; i++) {
    if (buf[i] != fill) return false;
  }
  return true;
}

}  // namespace

TEST(JWTHelper, TokensMatchCapturedBytes) {
  for (const Case& c : kCases) {
    char token[kTokenBufSize];
    ASSERT_TRUE(mint(c, token, sizeof(token))) << c.name;
    EXPECT_STREQ(c.expected, token) << c.name;
  }
}

TEST(JWTHelper, TokenSignatureVerifiesOverHeaderAndPayload) {
  for (const Case& c : kCases) {
    char token[kTokenBufSize];
    ASSERT_TRUE(mint(c, token, sizeof(token))) << c.name;
    const char* dot = strrchr(token, '.');
    ASSERT_NE(nullptr, dot);
    ASSERT_EQ(128u, strlen(dot + 1));
    uint8_t sig[64];
    ASSERT_TRUE(mesh::Utils::fromHex(sig, sizeof(sig), dot + 1));
    mesh::LocalIdentity id = makeIdentity(c.seed_base, c.seed_step);
    EXPECT_EQ(1, ed25519_verify(sig, reinterpret_cast<const uint8_t*>(token), dot - token,
                                id.pub_key)) << c.name;
  }
}

TEST(JWTHelper, EmptyOptionalClaimsAreOmitted) {
  const Case& c = kCases[0];
  mesh::LocalIdentity id = makeIdentity(c.seed_base, c.seed_step);
  char token[kTokenBufSize];
  ASSERT_TRUE(JWTHelper::createAuthToken(id, c.audience, c.iat, c.exp, token, sizeof(token), "",
                                         "", ""));
  EXPECT_STREQ(c.expected, token);
}

TEST(JWTHelper, ExactFitBufferSucceedsAndOneShortFailsUntouched) {
  for (const Case& c : kCases) {
    const size_t len = strlen(c.expected);
    char token[kTokenBufSize];

    memset(token, 'Z', sizeof(token));
    ASSERT_TRUE(mint(c, token, len + 1)) << c.name;
    EXPECT_STREQ(c.expected, token) << c.name;
    EXPECT_TRUE(isFilled(token + len + 1, sizeof(token) - len - 1, 'Z')) << c.name;

    memset(token, 'Z', sizeof(token));
    EXPECT_FALSE(mint(c, token, len)) << c.name;
    EXPECT_TRUE(isFilled(token, sizeof(token), 'Z')) << c.name;
  }
}

TEST(JWTHelper, TinyBuffersFailUntouched) {
  const size_t sizes[] = {1, 39, 40, 41, 169, 170, 171, 172, 200};
  for (size_t size : sizes) {
    char token[kTokenBufSize];
    memset(token, 'Z', sizeof(token));
    EXPECT_FALSE(mint(kCases[0], token, size)) << size;
    EXPECT_TRUE(isFilled(token, sizeof(token), 'Z')) << size;
  }
}

TEST(JWTHelper, OversizedPayloadFailsUntouched) {
  // One more email byte than largest_payload pushes the encoded payload past its cap.
  const Case& c = kCases[3];
  std::string email(35, 'e');
  char token[kTokenBufSize];
  memset(token, 'Z', sizeof(token));
  EXPECT_FALSE(mint(c, token, sizeof(token), email.c_str()));
  EXPECT_TRUE(isFilled(token, sizeof(token), 'Z'));

  std::string longest(63, 'e');
  EXPECT_FALSE(mint(c, token, sizeof(token), longest.c_str()));
  EXPECT_TRUE(isFilled(token, sizeof(token), 'Z'));
}

TEST(JWTHelper, RejectsMissingArguments) {
  mesh::LocalIdentity id = makeIdentity(1, 1);
  char token[kTokenBufSize];
  memset(token, 'Z', sizeof(token));
  EXPECT_FALSE(JWTHelper::createAuthToken(id, nullptr, 1700000000UL, 0, token, sizeof(token)));
  EXPECT_FALSE(JWTHelper::createAuthToken(id, "aud", 1700000000UL, 0, nullptr, sizeof(token)));
  EXPECT_FALSE(JWTHelper::createAuthToken(id, "aud", 1700000000UL, 0, token, 0));
  EXPECT_TRUE(isFilled(token, sizeof(token), 'Z'));
}

TEST(JWTHelper, FailedSelfCheckLeavesNoToken) {
  // A public key that does not match the private key signs fine but cannot verify.
  const Case& c = kCases[1];
  mesh::LocalIdentity id = makeIdentity(c.seed_base, c.seed_step);
  id.pub_key[0] ^= 0x01;
  char token[kTokenBufSize];
  memset(token, 'Z', sizeof(token));
  EXPECT_FALSE(JWTHelper::createAuthToken(id, c.audience, c.iat, c.exp, token, sizeof(token),
                                          c.owner, c.client, c.email));
  const size_t len = strlen(c.expected);
  EXPECT_TRUE(isFilled(token, len + 1, '\0'));
  EXPECT_TRUE(isFilled(token + len + 1, sizeof(token) - len - 1, 'Z'));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
