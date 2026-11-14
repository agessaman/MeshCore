#include <gtest/gtest.h>
#include <ed_25519.h>
extern "C" {
#include <sha512.h>
}

namespace {
void fromHex(const char* hex, unsigned char* out, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned v;
    ASSERT_EQ(1, sscanf(hex + 2 * i, "%2x", &v));
    out[i] = (unsigned char)v;
  }
}
}

TEST(PollControlCrypto, SeedExpansionAndExpandedPublicKeyMatchRfc8032) {
  unsigned char seed[32], expected[32], pub[32], prv[64], derived[32];
  fromHex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed, 32);
  fromHex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", expected, 32);
  ed25519_create_keypair(pub, prv, seed);
  EXPECT_EQ(0, memcmp(expected, pub, sizeof(pub)));
  ed25519_derive_pub(derived, prv);
  EXPECT_EQ(0, memcmp(pub, derived, sizeof(pub)));
}

TEST(PollControlCrypto, Sha512MatchesKnownDigest) {
  unsigned char out[64], expected[64];
  fromHex("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", expected, 64);
  ASSERT_EQ(0, sha512(reinterpret_cast<const unsigned char*>("abc"), 3, out));
  EXPECT_EQ(0, memcmp(expected, out, sizeof(out)));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
