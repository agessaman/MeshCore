#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "helpers/MapUploadRequest.h"

namespace {

MapUploadRequest::RadioParams usParams() {
  MapUploadRequest::RadioParams p{};
  p.freq_mhz = 910.525f;
  p.bw_khz = 250.0f;
  p.sf = 10;
  p.cr = 5;
  return p;
}

std::vector<uint8_t> bytes(size_t n, uint8_t seed = 0) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i < n; i++) v.push_back(static_cast<uint8_t>(seed + i));
  return v;
}

}  // namespace

// ---- formatNumber ---------------------------------------------------------

TEST(MapUploadRequestNumber, DropsTrailingZerosLikeJavaScript) {
  struct Case { float value; const char* expected; };
  const Case cases[] = {
    {250.0f,   "250"},
    {125.0f,   "125"},
    {62.5f,    "62.5"},
    {910.525f, "910.525"},
    {869.525f, "869.525"},
    {0.5f,     "0.5"},
  };
  for (const auto& c : cases) {
    char buf[16];
    const size_t n = MapUploadRequest::formatNumber(buf, sizeof(buf), c.value);
    EXPECT_EQ(strlen(c.expected), n) << c.expected;
    EXPECT_STREQ(c.expected, buf);
  }
}

TEST(MapUploadRequestNumber, RejectsNonFiniteValues) {
  char buf[16];
  EXPECT_EQ(0u, MapUploadRequest::formatNumber(buf, sizeof(buf), NAN));
  EXPECT_STREQ("", buf);
  EXPECT_EQ(0u, MapUploadRequest::formatNumber(buf, sizeof(buf), INFINITY));
  EXPECT_STREQ("", buf);
}

TEST(MapUploadRequestNumber, RejectsAnUndersizedBuffer) {
  char buf[4];
  EXPECT_EQ(0u, MapUploadRequest::formatNumber(buf, sizeof(buf), 910.525f));
  EXPECT_STREQ("", buf);
  EXPECT_EQ(0u, MapUploadRequest::formatNumber(nullptr, 16, 250.0f));
}

// ---- toHexLower -----------------------------------------------------------

TEST(MapUploadRequestHex, EncodesLowercase) {
  const uint8_t in[] = {0x00, 0x0f, 0xa0, 0xff, 0xde, 0xad};
  char buf[16];
  ASSERT_EQ(12u, MapUploadRequest::toHexLower(buf, sizeof(buf), in, sizeof(in)));
  EXPECT_STREQ("000fa0ffdead", buf);
}

TEST(MapUploadRequestHex, RefusesWhenTheBufferCannotHoldTheNul) {
  const uint8_t in[] = {0xAB, 0xCD};
  char buf[4];  // needs 5
  EXPECT_EQ(0u, MapUploadRequest::toHexLower(buf, sizeof(buf), in, sizeof(in)));
  EXPECT_STREQ("", buf);
}

// ---- escapeJsonString -----------------------------------------------------

TEST(MapUploadRequestEscape, EscapesQuotesAndBackslashes) {
  char buf[64];
  ASSERT_NE(0u, MapUploadRequest::escapeJsonString(buf, sizeof(buf), "a\"b\\c"));
  EXPECT_STREQ("a\\\"b\\\\c", buf);
}

TEST(MapUploadRequestEscape, EscapesControlCharacters) {
  char buf[64];
  ASSERT_NE(0u, MapUploadRequest::escapeJsonString(buf, sizeof(buf), "a\nb\tc\x01"));
  EXPECT_STREQ("a\\nb\\tc\\u0001", buf);
}

TEST(MapUploadRequestEscape, PassesOrdinaryTextThrough) {
  char buf[64];
  ASSERT_NE(0u, MapUploadRequest::escapeJsonString(buf, sizeof(buf), "meshcore://abcdef"));
  EXPECT_STREQ("meshcore://abcdef", buf);
}

TEST(MapUploadRequestEscape, RefusesToTruncate) {
  char buf[6];
  EXPECT_EQ(0u, MapUploadRequest::escapeJsonString(buf, sizeof(buf), "aaaa\"aaaa"));
  EXPECT_STREQ("", buf);
}

// ---- paramsValid ----------------------------------------------------------

TEST(MapUploadRequestParams, AcceptsPopulatedRadioPrefs) {
  EXPECT_TRUE(MapUploadRequest::paramsValid(usParams()));
}

TEST(MapUploadRequestParams, RejectsUnpopulatedPrefs) {
  MapUploadRequest::RadioParams p{};  // all zero, as NodePrefs starts out
  EXPECT_FALSE(MapUploadRequest::paramsValid(p));

  p = usParams(); p.freq_mhz = 0.0f;
  EXPECT_FALSE(MapUploadRequest::paramsValid(p));
  p = usParams(); p.bw_khz = 0.0f;
  EXPECT_FALSE(MapUploadRequest::paramsValid(p));
  p = usParams(); p.sf = 0;
  EXPECT_FALSE(MapUploadRequest::paramsValid(p));
  p = usParams(); p.cr = 0;
  EXPECT_FALSE(MapUploadRequest::paramsValid(p));
  p = usParams(); p.freq_mhz = NAN;
  EXPECT_FALSE(MapUploadRequest::paramsValid(p));
}

// ---- buildSignedData ------------------------------------------------------

TEST(MapUploadRequestData, ProducesExactlyTheExpectedString) {
  const uint8_t frame[] = {0x11, 0x00, 0xAB, 0xCD};
  char buf[MapUploadRequest::kDataBufferSize];
  const int n = MapUploadRequest::buildSignedData(buf, sizeof(buf), usParams(), frame,
                                                  sizeof(frame));
  ASSERT_GT(n, 0);
  const char* expected =
      "{\"params\":{\"freq\":910.525,\"cr\":5,\"sf\":10,\"bw\":250},"
      "\"links\":[\"meshcore://1100abcd\"]}";
  EXPECT_STREQ(expected, buf);
  EXPECT_EQ(static_cast<int>(strlen(expected)), n);
}

TEST(MapUploadRequestData, HoldsAMaximumLengthFrame) {
  const auto frame = bytes(MapUploadRequest::kMaxFrameBytes, 0x40);
  char buf[MapUploadRequest::kDataBufferSize];
  const int n = MapUploadRequest::buildSignedData(buf, sizeof(buf), usParams(), frame.data(),
                                                  frame.size());
  ASSERT_GT(n, 0);
  EXPECT_LT(static_cast<size_t>(n), sizeof(buf));
  // 2 hex chars per frame byte must all be present.
  EXPECT_NE(nullptr, strstr(buf, "meshcore://"));
  EXPECT_EQ(frame.size() * 2, strlen(strstr(buf, "meshcore://") + strlen("meshcore://")) - 3);
}

TEST(MapUploadRequestData, RejectsAnOversizedFrame) {
  const auto frame = bytes(MapUploadRequest::kMaxFrameBytes + 1, 0x40);
  char buf[MapUploadRequest::kDataBufferSize];
  EXPECT_EQ(0, MapUploadRequest::buildSignedData(buf, sizeof(buf), usParams(), frame.data(),
                                                 frame.size()));
  EXPECT_STREQ("", buf);
}

TEST(MapUploadRequestData, RejectsEmptyOrNullFrames) {
  char buf[MapUploadRequest::kDataBufferSize];
  const uint8_t frame[] = {0x11};
  EXPECT_EQ(0, MapUploadRequest::buildSignedData(buf, sizeof(buf), usParams(), nullptr, 4));
  EXPECT_EQ(0, MapUploadRequest::buildSignedData(buf, sizeof(buf), usParams(), frame, 0));
}

TEST(MapUploadRequestData, RejectsUnpopulatedRadioPrefs) {
  const uint8_t frame[] = {0x11, 0x00};
  char buf[MapUploadRequest::kDataBufferSize];
  MapUploadRequest::RadioParams p{};
  EXPECT_EQ(0, MapUploadRequest::buildSignedData(buf, sizeof(buf), p, frame, sizeof(frame)));
  EXPECT_STREQ("", buf);
}

TEST(MapUploadRequestData, EmptiesTheBufferRatherThanTruncating) {
  const auto frame = bytes(200, 0x40);
  char buf[64];  // far too small
  EXPECT_EQ(0, MapUploadRequest::buildSignedData(buf, sizeof(buf), usParams(), frame.data(),
                                                 frame.size()));
  EXPECT_STREQ("", buf);
}

// ---- buildRequestBody -----------------------------------------------------

TEST(MapUploadRequestBody, WrapsDataSignatureAndPublicKey) {
  const uint8_t sig[] = {0xDE, 0xAD};
  const uint8_t pub[] = {0xBE, 0xEF};
  char buf[MapUploadRequest::kBodyBufferSize];
  const int n = MapUploadRequest::buildRequestBody(buf, sizeof(buf), "{\"a\":1}", sig,
                                                   sizeof(sig), pub, sizeof(pub));
  ASSERT_GT(n, 0);
  EXPECT_STREQ("{\"data\":\"{\\\"a\\\":1}\",\"signature\":\"dead\",\"publicKey\":\"beef\"}", buf);
  EXPECT_EQ(static_cast<int>(strlen(buf)), n);
}

TEST(MapUploadRequestBody, CarriesARealDataStringAndFullSizeKeys) {
  const auto frame = bytes(MapUploadRequest::kMaxFrameBytes, 0x40);
  char data[MapUploadRequest::kDataBufferSize];
  ASSERT_GT(MapUploadRequest::buildSignedData(data, sizeof(data), usParams(), frame.data(),
                                              frame.size()), 0);

  const auto sig = bytes(SIGNATURE_SIZE, 0x01);
  const auto pub = bytes(PUB_KEY_SIZE, 0x80);
  char buf[MapUploadRequest::kBodyBufferSize];
  const int n = MapUploadRequest::buildRequestBody(buf, sizeof(buf), data, sig.data(),
                                                   sig.size(), pub.data(), pub.size());
  ASSERT_GT(n, 0) << "worst-case body must fit kBodyBufferSize";
  EXPECT_LT(static_cast<size_t>(n), sizeof(buf));
  EXPECT_NE(nullptr, strstr(buf, "\"signature\":\""));
  EXPECT_NE(nullptr, strstr(buf, "\"publicKey\":\""));
  // The embedded data string must be escaped, not raw.
  EXPECT_NE(nullptr, strstr(buf, "{\\\"params\\\""));
}

TEST(MapUploadRequestBody, RejectsMissingPieces) {
  const uint8_t sig[] = {0xDE};
  const uint8_t pub[] = {0xBE};
  char buf[MapUploadRequest::kBodyBufferSize];
  EXPECT_EQ(0, MapUploadRequest::buildRequestBody(buf, sizeof(buf), nullptr, sig, 1, pub, 1));
  EXPECT_EQ(0, MapUploadRequest::buildRequestBody(buf, sizeof(buf), "", sig, 1, pub, 1));
  EXPECT_EQ(0, MapUploadRequest::buildRequestBody(buf, sizeof(buf), "{}", nullptr, 1, pub, 1));
  EXPECT_EQ(0, MapUploadRequest::buildRequestBody(buf, sizeof(buf), "{}", sig, 0, pub, 1));
  EXPECT_EQ(0, MapUploadRequest::buildRequestBody(buf, sizeof(buf), "{}", sig, 1, nullptr, 1));
  EXPECT_EQ(0, MapUploadRequest::buildRequestBody(buf, sizeof(buf), "{}", sig, 1, pub, 0));
  EXPECT_STREQ("", buf);
}

TEST(MapUploadRequestBody, EmptiesTheBufferRatherThanTruncating) {
  const auto sig = bytes(SIGNATURE_SIZE, 0x01);
  const auto pub = bytes(PUB_KEY_SIZE, 0x80);
  char buf[32];
  EXPECT_EQ(0, MapUploadRequest::buildRequestBody(buf, sizeof(buf), "{\"a\":1}", sig.data(),
                                                  sig.size(), pub.data(), pub.size()));
  EXPECT_STREQ("", buf);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
