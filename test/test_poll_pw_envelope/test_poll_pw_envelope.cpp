#include <gtest/gtest.h>

#include <string.h>

extern "C" {
#include <ed_25519.h>
}

#include "helpers/poll/PollPwEnvelope.h"

#include "vectors.inc"

namespace {

struct Good {
  const uint8_t* obs_prv;
  const uint8_t* obs_pub;
  const uint8_t* target;
  const uint8_t* epk;
  const uint8_t* secret;
  const uint8_t* ct;
  const uint8_t* tag;
  const uint8_t* pw;
  size_t pw_len;
  uint32_t ver;
};

const Good kGoods[] = {
    {kV1ObsPrv, kV1ObsPub, kV1Target, kV1Epk, kV1Secret, kV1Ct, kV1Tag, kV1Pw, kV1PwLen, kV1Ver},
    {kV2ObsPrv, kV2ObsPub, kV2Target, kV2Epk, kV2Secret, kV2Ct, kV2Tag, kV2Pw, kV2PwLen, kV2Ver},
    {kV3ObsPrv, kV3ObsPub, kV3Target, kV3Epk, kV3Secret, kV3Ct, kV3Tag, kV3Pw, kV3PwLen, kV3Ver},
};

void expectZeros(const char out[16]) {
  char zeros[16];
  memset(zeros, 0, sizeof(zeros));
  EXPECT_EQ(0, memcmp(out, zeros, sizeof(zeros)));
}

void expectOpenFails(const uint8_t secret[32], const uint8_t obs[32], const uint8_t target[32],
                     const uint8_t epk[32], const uint8_t ct[16], const uint8_t tag[32], uint32_t ver) {
  char out[16];
  memset(out, 0xA5, sizeof(out));
  EXPECT_FALSE(poll::openPwEnvelope(secret, obs, target, epk, ct, tag, ver, out));
  expectZeros(out);
}

}  // namespace

TEST(PollPwEnvelope, OpensIndependentVectors) {
  for (const Good& g : kGoods) {
    char out[16];
    memset(out, 0xA5, sizeof(out));
    ASSERT_TRUE(poll::openPwEnvelope(g.secret, g.obs_pub, g.target, g.epk, g.ct, g.tag, g.ver, out));
    uint8_t block[16];
    memset(block, 0, sizeof(block));
    memcpy(block, g.pw, g.pw_len);
    EXPECT_EQ(0, memcmp(out, block, sizeof(block)));
  }
}

TEST(PollPwEnvelope, SealsIndependentVectors) {
  for (const Good& g : kGoods) {
    uint8_t ct[16], tag[32];
    ASSERT_TRUE(poll::sealPwEnvelope(g.secret, g.obs_pub, g.target, g.epk, g.ver, g.pw, g.pw_len, ct, tag));
    EXPECT_EQ(0, memcmp(ct, g.ct, sizeof(ct)));
    EXPECT_EQ(0, memcmp(tag, g.tag, sizeof(tag)));
  }
}

TEST(PollPwEnvelope, KeyExchangeMatchesVectors) {
  for (const Good& g : kGoods) {
    uint8_t secret[32], pub[32];
    ed25519_key_exchange(secret, g.epk, g.obs_prv);
    EXPECT_EQ(0, memcmp(secret, g.secret, sizeof(secret)));
    ed25519_derive_pub(pub, g.obs_prv);
    EXPECT_EQ(0, memcmp(pub, g.obs_pub, sizeof(pub)));
  }
}

TEST(PollPwEnvelope, RejectsFlippedCiphertext) {
  uint8_t ct[16];
  memcpy(ct, kV2Ct, sizeof(ct));
  ct[0] ^= 0x01;
  expectOpenFails(kV2Secret, kV2ObsPub, kV2Target, kV2Epk, ct, kV2Tag, kV2Ver);
}

TEST(PollPwEnvelope, RejectsFlippedTag) {
  uint8_t tag[32];
  memcpy(tag, kV2Tag, sizeof(tag));
  tag[31] ^= 0x80;
  expectOpenFails(kV2Secret, kV2ObsPub, kV2Target, kV2Epk, kV2Ct, tag, kV2Ver);
}

TEST(PollPwEnvelope, RejectsFlippedEpk) {
  uint8_t epk[32];
  memcpy(epk, kV2Epk, sizeof(epk));
  epk[4] ^= 0x01;
  expectOpenFails(kV2Secret, kV2ObsPub, kV2Target, epk, kV2Ct, kV2Tag, kV2Ver);
}

TEST(PollPwEnvelope, RejectsFlippedObserver) {
  uint8_t obs[32];
  memcpy(obs, kV2ObsPub, sizeof(obs));
  obs[8] ^= 0x01;
  expectOpenFails(kV2Secret, obs, kV2Target, kV2Epk, kV2Ct, kV2Tag, kV2Ver);
}

TEST(PollPwEnvelope, RejectsFlippedTarget) {
  uint8_t target[32];
  memcpy(target, kV2Target, sizeof(target));
  target[15] ^= 0x01;
  expectOpenFails(kV2Secret, kV2ObsPub, target, kV2Epk, kV2Ct, kV2Tag, kV2Ver);
}

TEST(PollPwEnvelope, RejectsDifferentVersion) {
  expectOpenFails(kV2Secret, kV2ObsPub, kV2Target, kV2Epk, kV2Ct, kV2Tag, kV2Ver + 1);
}

TEST(PollPwEnvelope, RejectsEmptyPassword) {
  expectOpenFails(kBadEmptySecret, kBadEmptyObsPub, kBadEmptyTarget, kBadEmptyEpk, kBadEmptyCt,
                  kBadEmptyTag, kBadEmptyVer);
  uint8_t ct[16], tag[32];
  memset(ct, 0xFF, sizeof(ct));
  memset(tag, 0xFF, sizeof(tag));
  EXPECT_FALSE(poll::sealPwEnvelope(kBadEmptySecret, kBadEmptyObsPub, kBadEmptyTarget, kBadEmptyEpk,
                                    kBadEmptyVer, kV1Pw, 0, ct, tag));
  expectZeros(reinterpret_cast<char*>(ct));
  uint8_t zeros[32];
  memset(zeros, 0, sizeof(zeros));
  EXPECT_EQ(0, memcmp(tag, zeros, sizeof(tag)));
}

TEST(PollPwEnvelope, RejectsSixteenBytePassword) {
  expectOpenFails(kBadSixteenSecret, kBadSixteenObsPub, kBadSixteenTarget, kBadSixteenEpk, kBadSixteenCt,
                  kBadSixteenTag, kBadSixteenVer);
  uint8_t pw[16];
  memset(pw, 0x61, sizeof(pw));
  uint8_t ct[16], tag[32];
  EXPECT_FALSE(poll::sealPwEnvelope(kV1Secret, kV1ObsPub, kV1Target, kV1Epk, kV1Ver, pw, sizeof(pw), ct, tag));
}

TEST(PollPwEnvelope, RejectsNonzeroAfterTerminator) {
  expectOpenFails(kBadTrailSecret, kBadTrailObsPub, kBadTrailTarget, kBadTrailEpk, kBadTrailCt, kBadTrailTag,
                  kBadTrailVer);
  const uint8_t pw[] = {'a', 0x00, 'c'};
  uint8_t ct[16], tag[32];
  EXPECT_FALSE(poll::sealPwEnvelope(kV1Secret, kV1ObsPub, kV1Target, kV1Epk, kV1Ver, pw, sizeof(pw), ct, tag));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
