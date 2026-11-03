#include <gtest/gtest.h>
#include "helpers/mqtt/ObserverTransportPolicy.h"
#include "helpers/Base64Url.h"

using namespace ObserverTransportPolicy;

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

TEST(ObserverEndpoint, PreservesWebsocketPathQueryAndTlsHost) {
  Endpoint e;
  ASSERT_TRUE(parseEndpoint("wss://mqtt-us-v1.letsmesh.net:443/mqtt?test=1", e));
  EXPECT_STREQ(e.host, "mqtt-us-v1.letsmesh.net");
  EXPECT_STREQ(e.path, "/mqtt?test=1");
  EXPECT_EQ(e.port, 443);
  EXPECT_EQ(e.transport, MqttTransport::Wss);
  ASSERT_TRUE(parseEndpoint("ws://localhost?token=x", e));
  EXPECT_STREQ(e.path, "/?token=x");
  ASSERT_TRUE(parseEndpoint("mqtts://broker.local", e));
  EXPECT_EQ(e.port, 8883);
}

TEST(ObserverEndpoint, RejectsAmbiguousOrTruncatedEndpoints) {
  Endpoint e;
  const char* invalid[] = {nullptr, "", "https://host", "wss://", "wss://user@host",
    "wss://host:0", "wss://host:65536", "wss://host:99999999999", "wss://host:-1",
    "wss://host:", "wss://host#fragment", "mqtt://host/path", "wss://host bad"};
  for (auto uri : invalid) EXPECT_FALSE(parseEndpoint(uri, e)) << (uri ? uri : "null");
}

TEST(ObserverEndpoint, CustomUrisKeepSecureSchemeAndExplicitPort) {
  Endpoint e;
  ASSERT_TRUE(customEndpoint("wss://broker.example:443/mqtt", 1883, e));
  EXPECT_EQ(e.transport, MqttTransport::Wss);
  EXPECT_EQ(e.port, 443);
  EXPECT_STREQ(e.path, "/mqtt");
  ASSERT_TRUE(customEndpoint("ws://localhost/mqtt", 9001, e));
  EXPECT_EQ(e.port, 9001);
  ASSERT_TRUE(customEndpoint("localhost", 1883, e));
  EXPECT_EQ(e.transport, MqttTransport::Tcp);
  ASSERT_TRUE(customEndpoint("broker.example", 8883, e));
  EXPECT_EQ(e.transport, MqttTransport::Tls);
}

TEST(WebsocketBytes, CoalescingFragmentationAndWrapPreserveByteStream) {
  ByteQueue<5> q;
  uint8_t a[] = {1, 2, 3}, b[] = {4, 5, 6, 7};
  ASSERT_TRUE(q.append(a, sizeof(a)));
  EXPECT_EQ(q.read(), 1);
  EXPECT_EQ(q.read(), 2);
  ASSERT_TRUE(q.append(b, sizeof(b)));
  EXPECT_FALSE(q.append(a, 1));
  for (int expected = 3; expected <= 7; ++expected) EXPECT_EQ(q.read(), expected);
  EXPECT_EQ(q.read(), -1);
  EXPECT_EQ(q.peek(), -1);
  q.clear();
  EXPECT_FALSE(q.append(nullptr, 1));
}

TEST(ObserverBase64, MatchesUnpaddedUrlVectorsAndBounds) {
  const char* inputs[] = {"f", "fo", "foo", "foob", "fooba", "foobar"};
  const char* expected[] = {"Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy"};
  char out[32];
  for (int i = 0; i < 6; ++i) {
    EXPECT_EQ(Base64Url::encode((const uint8_t*)inputs[i], strlen(inputs[i]), out, sizeof(out)), strlen(expected[i]));
    EXPECT_STREQ(out, expected[i]);
  }
  const uint8_t binary[] = {0xfb, 0xff, 0xff};
  EXPECT_EQ(Base64Url::encode(binary, sizeof(binary), out, sizeof(out)), 4U);
  EXPECT_STREQ(out, "-___");
  EXPECT_EQ(Base64Url::encode(binary, sizeof(binary), out, 4), 0U);
  EXPECT_STREQ(out, "");
}
