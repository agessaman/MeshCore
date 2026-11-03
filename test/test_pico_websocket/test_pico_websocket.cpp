#include <gtest/gtest.h>
#include "helpers/mqtt/WebSocketStreamClient.h"
#include "helpers/mqtt/PicoObserverMqttClient.h"

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

class WebsocketTransport : public testing::Test {
protected:
  WebSocketStreamClient stream;
  WebSocketsClient* wire = WebSocketsClient::instance;
  ObserverTransportPolicy::Endpoint endpoint;
  BearSSL::X509List ca;
  void connect(const char* uri = "wss://broker.example:443/mqtt?x=1") {
    ASSERT_TRUE(ObserverTransportPolicy::parseEndpoint(uri, endpoint));
    stream.configure(endpoint, &ca);
    wire->events.push_back({WStype_CONNECTED, {}});
    ASSERT_EQ(stream.connect("broker.example", 443), 1);
  }
};

TEST_F(WebsocketTransport, UsesOriginalHostPathSubprotocolAndOwnedTrust) {
  connect();
  EXPECT_EQ(wire->host, "broker.example");
  EXPECT_EQ(wire->path, "/mqtt?x=1");
  EXPECT_EQ(wire->protocol, "mqtt");
  EXPECT_EQ(wire->trust, &ca);
}

TEST_F(WebsocketTransport, RefusesSecureTransportWithoutTrust) {
  ASSERT_TRUE(ObserverTransportPolicy::parseEndpoint("wss://broker.example/mqtt", endpoint));
  stream.configure(endpoint, nullptr);
  EXPECT_EQ(stream.connect("broker.example", 443), 0);
  EXPECT_EQ(wire->begin_count, 0);
}

TEST_F(WebsocketTransport, ReadsCoalescedAndFragmentedBinaryAsOneStream) {
  connect();
  wire->events.push_back({WStype_BIN, {1,2}});
  wire->events.push_back({WStype_PING, {}});
  wire->events.push_back({WStype_FRAGMENT_BIN_START, {3}});
  wire->events.push_back({WStype_FRAGMENT, {4}});
  wire->events.push_back({WStype_FRAGMENT_FIN, {5}});
  uint8_t output[5]{};
  size_t count = 0;
  for (int pass = 0; pass < 10 && count < sizeof(output); ++pass)
    count += stream.read(output + count, sizeof(output) - count);
  ASSERT_EQ(count, 5U);
  for (int i = 0; i < 5; ++i) EXPECT_EQ(output[i], i + 1);
}

TEST_F(WebsocketTransport, OverflowClosesAndCannotReconnectFromReads) {
  connect();
  wire->events.push_back({WStype_BIN, std::vector<uint8_t>(2049, 1)});
  EXPECT_EQ(stream.available(), 0);
  EXPECT_FALSE(stream.connected());
  const int loops = wire->loop_count;
  EXPECT_EQ(stream.read(), -1);
  EXPECT_EQ(stream.available(), 0);
  EXPECT_EQ(wire->loop_count, loops);
}

TEST_F(WebsocketTransport, TextAndUnexpectedContinuationFailClosed) {
  connect();
  wire->events.push_back({WStype_FRAGMENT, {1}});
  EXPECT_EQ(stream.available(), 0);
  EXPECT_FALSE(stream.connected());
  connect();
  wire->events.push_back({WStype_TEXT, {1}});
  EXPECT_EQ(stream.available(), 0);
  EXPECT_FALSE(stream.connected());
}

TEST_F(WebsocketTransport, FailedConnectDoesNotStartAnotherAttempt) {
  ASSERT_TRUE(ObserverTransportPolicy::parseEndpoint("ws://broker.example/mqtt", endpoint));
  stream.configure(endpoint, nullptr);
  wire->fail_connect = true;
  EXPECT_EQ(stream.connect("broker.example", 80), 0);
  EXPECT_EQ(wire->begin_count, 1);
  EXPECT_EQ(wire->loop_count, 1);
  stream.available();
  EXPECT_EQ(wire->loop_count, 1);
}

TEST_F(WebsocketTransport, FailedWritesAndStopPreventFurtherSends) {
  connect();
  wire->fail_write = true;
  EXPECT_EQ(stream.write(uint8_t(1)), 0U);
  EXPECT_EQ(stream.write(uint8_t(2)), 0U);
  EXPECT_EQ(wire->sends, 1);
  stream.stop();
  EXPECT_FALSE(stream.connected());
}

TEST_F(WebsocketTransport, RequiresTheMqttResponseSubprotocol) {
  ASSERT_TRUE(ObserverTransportPolicy::parseEndpoint("wss://broker.example/mqtt", endpoint));
  stream.configure(endpoint, &ca);
  wire->responseProtocol("other");
  wire->events.push_back({WStype_CONNECTED, {}});
  EXPECT_EQ(stream.connect("broker.example", 443), 0);
  EXPECT_FALSE(stream.connected());
}

TEST(PicoMqttBackend, ReconfigurationClearsCredentialsAndPreservesQosRetain) {
  PicoObserverMqttClient client;
  ObserverTransportPolicy::Endpoint endpoint;
  ASSERT_TRUE(ObserverTransportPolicy::parseEndpoint("mqtt://localhost:1883", endpoint));
  ASSERT_TRUE(client.connect(endpoint, nullptr, "device", "user", "secret", 75));
  EXPECT_EQ(MQTTClient::user, "user");
  EXPECT_EQ(MQTTClient::password, "secret");
  ASSERT_TRUE(client.publish("topic", "value", 5, true, 1));
  EXPECT_TRUE(MQTTClient::retained);
  EXPECT_EQ(MQTTClient::qos, 1);
  ASSERT_TRUE(client.connect(endpoint, nullptr, "device", "", "", 75));
  EXPECT_TRUE(MQTTClient::user.empty());
  EXPECT_TRUE(MQTTClient::password.empty());
}

TEST(PicoMqttBackend, SecureConnectionsRequireTrustAndTls12) {
  PicoObserverMqttClient client;
  ObserverTransportPolicy::Endpoint endpoint;
  ASSERT_TRUE(ObserverTransportPolicy::parseEndpoint("mqtts://broker.example:8883", endpoint));
  const int attempts = MQTTClient::connects;
  EXPECT_FALSE(client.connect(endpoint, nullptr, "device", "", "", 75));
  EXPECT_EQ(MQTTClient::connects, attempts);
  ASSERT_TRUE(client.connect(endpoint, "CA", "device", "", "", 75));
  EXPECT_EQ(BearSSL::WiFiClientSecure::minimum, BR_TLS12);
  EXPECT_EQ(BearSSL::WiFiClientSecure::maximum, BR_TLS12);
  EXPECT_NE(BearSSL::WiFiClientSecure::trust, nullptr);
}

TEST(PicoMqttBackend, FailedAdditionalTrustIsRetriedAndAllAnchorsAreReleased) {
  const int anchors = BearSSL::X509List::instances;
  {
    PicoObserverMqttClient client;
    ObserverTransportPolicy::Endpoint endpoint;
    ASSERT_TRUE(ObserverTransportPolicy::parseEndpoint("mqtts://broker.example", endpoint));
    BearSSL::X509List::fail_append = true;
    EXPECT_FALSE(client.connect(endpoint, "CA", "device", "", "", 75, "EXTRA"));
    EXPECT_EQ(BearSSL::X509List::instances, anchors);
    BearSSL::X509List::fail_append = false;
    EXPECT_TRUE(client.connect(endpoint, "CA", "device", "", "", 75, "EXTRA"));
  }
  EXPECT_EQ(BearSSL::X509List::instances, anchors);
}
