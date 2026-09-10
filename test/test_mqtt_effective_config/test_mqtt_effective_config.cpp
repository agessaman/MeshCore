#include "helpers/MQTTEffectiveConfig.h"

#include <gtest/gtest.h>

#include <string.h>

namespace {

const char kPemA[] = "-----BEGIN CERTIFICATE-----A";
const char kPemB[] = "-----BEGIN CERTIFICATE-----B";
const char kJwtUser[] = "v1_CC5D3CFD9C4C7B84";
const char kToken[] = "eyJhbGciOi...";

MqttEffectiveConfig build(const char* uri,
                          MqttAuth auth = MqttAuth::None,
                          const char* user = nullptr,
                          const char* pass = nullptr,
                          MqttTrust trust = MqttTrust::Plaintext,
                          const char* pem = nullptr,
                          uint16_t buffer = 896) {
  MqttEffectiveConfig c;
  mqttBuildEffectiveConfig(uri, auth, user, pass, trust, pem, buffer, 75, &c);
  return c;
}

MqttRecreateDecision decide(const MqttEffectiveConfig& applied,
                            const MqttEffectiveConfig& desired,
                            uint16_t allocated = 896) {
  return mqttConfigRecreateDecision(applied, desired, allocated);
}

} // namespace

TEST(MqttEffectiveConfig, TransportComesFromTheUriScheme) {
  EXPECT_EQ(MqttTransport::Tcp, mqttTransportFromUri("mqtt://broker:1883"));
  EXPECT_EQ(MqttTransport::Tls, mqttTransportFromUri("mqtts://broker:8883"));
  EXPECT_EQ(MqttTransport::Ws, mqttTransportFromUri("ws://broker/mqtt"));
  EXPECT_EQ(MqttTransport::Wss, mqttTransportFromUri("wss://broker:443/mqtt"));
  EXPECT_EQ(MqttTransport::Unknown, mqttTransportFromUri("broker:1883"));
  EXPECT_EQ(MqttTransport::Unknown, mqttTransportFromUri(""));
  EXPECT_EQ(MqttTransport::Unknown, mqttTransportFromUri(nullptr));

  EXPECT_TRUE(mqttTransportIsEncrypted(MqttTransport::Tls));
  EXPECT_TRUE(mqttTransportIsEncrypted(MqttTransport::Wss));
  EXPECT_FALSE(mqttTransportIsEncrypted(MqttTransport::Tcp));
  EXPECT_FALSE(mqttTransportIsEncrypted(MqttTransport::Ws));
}

TEST(MqttEffectiveConfig, BuildRejectsUnusableUris) {
  MqttEffectiveConfig c;
  EXPECT_FALSE(mqttBuildEffectiveConfig("", MqttAuth::None, nullptr, nullptr,
                                        MqttTrust::Plaintext, nullptr, 896, 75, &c));
  EXPECT_FALSE(c.valid);
  EXPECT_FALSE(mqttBuildEffectiveConfig(nullptr, MqttAuth::None, nullptr, nullptr,
                                        MqttTrust::Plaintext, nullptr, 896, 75, &c));
  EXPECT_FALSE(mqttBuildEffectiveConfig("broker:1883", MqttAuth::None, nullptr, nullptr,
                                        MqttTrust::Plaintext, nullptr, 896, 75, &c));

  char too_long[MQTT_EFFECTIVE_URI_MAX + 16];
  memset(too_long, 'x', sizeof(too_long));
  memcpy(too_long, "mqtt://", 7);
  too_long[sizeof(too_long) - 1] = '\0';
  EXPECT_FALSE(mqttBuildEffectiveConfig(too_long, MqttAuth::None, nullptr, nullptr,
                                        MqttTrust::Plaintext, nullptr, 896, 75, &c));
}

TEST(MqttEffectiveConfig, BuildOwnsItsUriCopy) {
  char uri[64];
  strcpy(uri, "mqtt://192.168.50.231:1883");
  MqttEffectiveConfig c = build(uri);
  ASSERT_TRUE(c.valid);

  strcpy(uri, "mqtt://other:1883");   // slot.broker_uri is rewritten in place
  EXPECT_STREQ("mqtt://192.168.50.231:1883", c.uri);
}

// An unencrypted transport verifies nothing, whatever certificate material was
// requested. Recording anything else would make a plaintext endpoint look like
// it kept a verified policy.
TEST(MqttEffectiveConfig, TrustIsNormalisedAgainstTheTransport) {
  MqttEffectiveConfig plain = build("mqtt://broker:1883", MqttAuth::None, nullptr, nullptr,
                                    MqttTrust::Bundle, kPemA);
  EXPECT_EQ(MqttTrust::Plaintext, plain.trust);
  EXPECT_EQ(nullptr, plain.pem);

  MqttEffectiveConfig bundle = build("wss://broker:443/mqtt", MqttAuth::Jwt, kJwtUser, kToken,
                                     MqttTrust::Bundle, nullptr);
  EXPECT_EQ(MqttTrust::Bundle, bundle.trust);

  MqttEffectiveConfig pem = build("mqtts://broker:8883", MqttAuth::UserPass, "u", "p",
                                  MqttTrust::PemCert, kPemA);
  EXPECT_EQ(MqttTrust::PemCert, pem.trust);
  EXPECT_EQ(kPemA, pem.pem);

  // Encrypted transport with no usable material is not a verified policy.
  MqttEffectiveConfig none = build("mqtts://broker:8883", MqttAuth::None, nullptr, nullptr,
                                   MqttTrust::PemCert, nullptr);
  EXPECT_EQ(MqttTrust::Plaintext, none.trust);
  EXPECT_EQ(nullptr, none.pem);
}

// The six F02 transitions from the review, plus the one reproduced on hardware.
// None of them needs a new client: credentials are rewritten unconditionally.
TEST(MqttEffectiveConfig, CredentialAndAuthChangesReuseTheClient) {
  const MqttEffectiveConfig jwt = build("wss://broker:443/mqtt", MqttAuth::Jwt, kJwtUser, kToken,
                                        MqttTrust::Bundle, nullptr);
  const MqttEffectiveConfig anon = build("wss://broker:443/mqtt", MqttAuth::None, nullptr, nullptr,
                                         MqttTrust::Bundle, nullptr);
  const MqttEffectiveConfig userpass = build("wss://broker:443/mqtt", MqttAuth::UserPass, "u", "p",
                                             MqttTrust::Bundle, nullptr);
  const MqttEffectiveConfig renewed = build("wss://broker:443/mqtt", MqttAuth::Jwt, kJwtUser,
                                            "eyJhbGciOi...new", MqttTrust::Bundle, nullptr);

  EXPECT_FALSE(decide(jwt, anon).recreate);        // JWT -> anonymous
  EXPECT_FALSE(decide(userpass, anon).recreate);   // user/pass -> anonymous
  EXPECT_FALSE(decide(anon, jwt).recreate);        // none -> configured
  EXPECT_FALSE(decide(userpass, jwt).recreate);    // user/pass -> JWT
  EXPECT_FALSE(decide(jwt, renewed).recreate);     // token renewal
  EXPECT_FALSE(decide(jwt, jwt).recreate);         // ordinary reconnect
  EXPECT_STREQ("reuse", decide(jwt, jwt).reason);
}

// Host/port/path within one scheme is an endpoint move, not a structural one.
TEST(MqttEffectiveConfig, EndpointMoveWithinOneSchemeReusesTheClient) {
  const MqttEffectiveConfig a = build("mqtt://192.168.50.231:1883");
  const MqttEffectiveConfig b = build("mqtt://192.168.50.9:1884");
  EXPECT_FALSE(decide(a, b).recreate);

  const MqttEffectiveConfig wss_a = build("wss://a.example:443/mqtt", MqttAuth::Jwt, kJwtUser,
                                          kToken, MqttTrust::Bundle, nullptr);
  const MqttEffectiveConfig wss_b = build("wss://b.example:443/other", MqttAuth::Jwt, kJwtUser,
                                          kToken, MqttTrust::Bundle, nullptr);
  EXPECT_FALSE(decide(wss_a, wss_b).recreate);
}

TEST(MqttEffectiveConfig, TransportChangeForcesRecreate) {
  const MqttEffectiveConfig wss = build("wss://broker:443/mqtt", MqttAuth::Jwt, kJwtUser, kToken,
                                        MqttTrust::Bundle, nullptr);
  const MqttEffectiveConfig tcp = build("mqtt://192.168.50.231:1883");
  const MqttEffectiveConfig tls = build("mqtts://broker:8883", MqttAuth::UserPass, "u", "p",
                                        MqttTrust::Bundle, nullptr);
  const MqttEffectiveConfig ws = build("ws://broker/mqtt");

  EXPECT_TRUE(decide(wss, tcp).recreate);
  EXPECT_STREQ("transport-change", decide(wss, tcp).reason);
  EXPECT_TRUE(decide(tcp, wss).recreate);
  EXPECT_TRUE(decide(wss, tls).recreate);   // both encrypted, still a scheme change
  EXPECT_TRUE(decide(tcp, ws).recreate);    // both plaintext, still a scheme change
}

TEST(MqttEffectiveConfig, TrustChangeForcesRecreate) {
  const MqttEffectiveConfig bundle = build("mqtts://broker:8883", MqttAuth::None, nullptr, nullptr,
                                           MqttTrust::Bundle, nullptr);
  const MqttEffectiveConfig pem_a = build("mqtts://broker:8883", MqttAuth::None, nullptr, nullptr,
                                          MqttTrust::PemCert, kPemA);
  const MqttEffectiveConfig pem_b = build("mqtts://broker:8883", MqttAuth::None, nullptr, nullptr,
                                          MqttTrust::PemCert, kPemB);

  EXPECT_TRUE(decide(bundle, pem_a).recreate);
  EXPECT_STREQ("trust-change", decide(bundle, pem_a).reason);
  EXPECT_TRUE(decide(pem_a, bundle).recreate);
  EXPECT_TRUE(decide(pem_a, pem_b).recreate);
  EXPECT_STREQ("ca-cert-change", decide(pem_a, pem_b).reason);
  EXPECT_FALSE(decide(pem_a, pem_a).recreate);
}

// IDF 4.4 allocates the MQTT buffers at client init and does not resize them,
// and the wrapper's reassembly buffer is allocated once for the client's life.
TEST(MqttEffectiveConfig, BufferGrowthForcesRecreateButShrinkDoesNot) {
  const MqttEffectiveConfig small = build("mqtt://broker:1883", MqttAuth::None, nullptr, nullptr,
                                          MqttTrust::Plaintext, nullptr, 512);
  const MqttEffectiveConfig large = build("mqtt://broker:1883", MqttAuth::Jwt, kJwtUser, kToken,
                                          MqttTrust::Plaintext, nullptr, 896);

  EXPECT_TRUE(decide(small, large, /*allocated=*/512).recreate);
  EXPECT_STREQ("buffer-growth", decide(small, large, 512).reason);
  EXPECT_FALSE(decide(small, large, /*allocated=*/896).recreate)
      << "a client that already owns the capacity does not need recreating";
  EXPECT_FALSE(decide(large, small, /*allocated=*/896).recreate);
}

TEST(MqttEffectiveConfig, FirstApplyAndInvalidDesiredNeverRecreate) {
  const MqttEffectiveConfig unset;
  const MqttEffectiveConfig wss = build("wss://broker:443/mqtt", MqttAuth::Jwt, kJwtUser, kToken,
                                        MqttTrust::Bundle, nullptr);
  EXPECT_FALSE(unset.valid);
  EXPECT_FALSE(decide(unset, wss).recreate);
  EXPECT_STREQ("first-apply", decide(unset, wss).reason);

  EXPECT_FALSE(decide(wss, unset).recreate);
  EXPECT_STREQ("invalid-desired", decide(wss, unset).reason);
}

// Rule 1: absent is a value that gets written, never an omission.
TEST(MqttEffectiveConfig, AbsentFieldsAreWrittenAsEmptyStrings) {
  EXPECT_STREQ("", mqttFieldOrEmpty(nullptr));
  EXPECT_STREQ("alice", mqttFieldOrEmpty("alice"));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
