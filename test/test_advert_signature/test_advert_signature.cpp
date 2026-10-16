#include <gtest/gtest.h>

#include <vector>

#include "helpers/AdvertSignature.h"

namespace {

std::vector<uint8_t> fromHex(const char* hex) {
  std::vector<uint8_t> out;
  for (const char* p = hex; p[0] && p[1]; p += 2) {
    char byte[3] = {p[0], p[1], 0};
    out.push_back(static_cast<uint8_t>(strtoul(byte, nullptr, 16)));
  }
  return out;
}

// ADVERT payload for "LCC Andice Observer" as heard by a working observer.
const char* kAdvertPayload =
    "1EEB3527ED66C732185A2F29264A7450DBA40F1686CAE580F72BC80E65B2D835"
    "F18EC66A"
    "1A4FBA70C4BF0A2BCA115F131A6A0F2D54A6E3F8AFCD893F3414DB9DE31EC87E"
    "AD2D21F54C4C9ED1CD40147F10515BFE47CB83D27B8C20B29BF5B2A61BDA430B"
    "92FCEFD50106D52AFA4C434320416E64696365204F62736572766572";

}  // namespace

TEST(AdvertSignature, BuildsPubKeyTimestampAndAppData) {
  std::vector<uint8_t> payload = fromHex(kAdvertPayload);
  uint8_t message[AdvertSignature::kMaxMessageLen];
  const uint8_t* sig = nullptr;

  int len = AdvertSignature::buildMessage(payload.data(), static_cast<int>(payload.size()),
                                          message, &sig);

  int app_data_len = static_cast<int>(payload.size()) - AdvertSignature::kHeaderLen;
  ASSERT_EQ(PUB_KEY_SIZE + 4 + app_data_len, len);
  EXPECT_EQ(payload.data() + PUB_KEY_SIZE + 4, sig);
  EXPECT_EQ(0, memcmp(message, payload.data(), PUB_KEY_SIZE + 4));
  EXPECT_EQ(0, memcmp(message + PUB_KEY_SIZE + 4, payload.data() + AdvertSignature::kHeaderLen,
                      app_data_len));
}

TEST(AdvertSignature, RejectsPayloadShorterThanHeader) {
  uint8_t payload[AdvertSignature::kHeaderLen - 1] = {};
  uint8_t message[AdvertSignature::kMaxMessageLen];
  const uint8_t* sig = nullptr;
  EXPECT_EQ(-1, AdvertSignature::buildMessage(payload, sizeof(payload), message, &sig));
  EXPECT_EQ(-1, AdvertSignature::buildMessage(nullptr, 0, message, &sig));
}

TEST(AdvertSignature, HeaderOnlyPayloadSignsPubKeyAndTimestamp) {
  uint8_t payload[AdvertSignature::kHeaderLen];
  for (int i = 0; i < AdvertSignature::kHeaderLen; i++) payload[i] = static_cast<uint8_t>(i);
  uint8_t message[AdvertSignature::kMaxMessageLen];
  const uint8_t* sig = nullptr;
  EXPECT_EQ(PUB_KEY_SIZE + 4,
            AdvertSignature::buildMessage(payload, sizeof(payload), message, &sig));
}

// Mesh::onRecvPacket only signs the first MAX_ADVERT_DATA_SIZE bytes of app
// data; a longer tail must not change the message.
TEST(AdvertSignature, ClampsAppDataLikeMesh) {
  uint8_t payload[AdvertSignature::kHeaderLen + MAX_ADVERT_DATA_SIZE + 10];
  for (size_t i = 0; i < sizeof(payload); i++) payload[i] = static_cast<uint8_t>(i);
  uint8_t message[AdvertSignature::kMaxMessageLen];
  const uint8_t* sig = nullptr;

  int len = AdvertSignature::buildMessage(payload, sizeof(payload), message, &sig);

  ASSERT_EQ(AdvertSignature::kMaxMessageLen, len);
  EXPECT_EQ(0, memcmp(message + PUB_KEY_SIZE + 4, payload + AdvertSignature::kHeaderLen,
                      MAX_ADVERT_DATA_SIZE));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
