#include <gtest/gtest.h>
#include "helpers/NetworkLink.h"
#include "WiFi.h"
#include <limits>

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

class PicoWifi : public testing::Test {
protected:
  NetworkLink& link = activeNetworkLink();
  void SetUp() override {
    static unsigned sequence = 0;
    WiFi = MockWiFi{};
    mock_lwip_ready = false;
    mock_lwip_init_calls = 0;
    g_mock_millis = 1000;
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "test-%u", ++sequence);
    link.setHostname("pico-observer");
    link.begin(ssid, "secret");
  }
  NetworkTransition poll(uint32_t now) {
    g_mock_millis = now;
    return link.maintain(now, 0);
  }
  void associated() { mock_link = CYW43_LINK_JOIN; cyw43_state.wifi_join_state = 1; }
  void lease() {
    associated(); WiFi.code = WL_CONNECTED;
    WiFi.ip = IPAddress(192,168,1,42); WiFi.dns = IPAddress(192,168,1,1);
  }
  std::string diag() { char value[160]; link.formatDiagnostics(value, sizeof(value)); return value; }
};

TEST_F(PicoWifi, StartsOnDriverTaskAndCliReadsOnlySnapshot) {
  EXPECT_TRUE(mock_lwip_ready);
  EXPECT_GT(mock_lwip_init_calls, 0);
  EXPECT_EQ(WiFi.begins, 1);
  EXPECT_EQ(WiFi.hostname, "pico-observer");
  EXPECT_NE(diag().find("state:joining"), std::string::npos);
  const int callbacks = mock_lwip_calls;
  link.statusCode(); link.isConnected(); link.localIP(); link.rssi(); diag();
  EXPECT_EQ(mock_lwip_calls, callbacks);
}

TEST_F(PicoWifi, ColdBeginInitializesDispatcherWithoutHostnameCall) {
  mock_lwip_ready = false;
  mock_lwip_init_calls = 0;
  link.begin("cold-network", "secret");
  EXPECT_TRUE(mock_lwip_ready);
  EXPECT_GT(mock_lwip_init_calls, 0);
  EXPECT_EQ(WiFi.ssid, "cold-network");
}

TEST_F(PicoWifi, ObservationInitializesDispatcherBeforeQueuingWork) {
  mock_lwip_ready = false;
  mock_lwip_init_calls = 0;
  poll(2000);
  EXPECT_TRUE(mock_lwip_ready);
  EXPECT_GT(mock_lwip_init_calls, 0);
}

TEST_F(PicoWifi, IncompleteHandshakeDoesNotReportDhcpOrConnectedEvenWithStaleIp) {
  WiFi.code = WL_CONNECTED; WiFi.ip = IPAddress(192,168,1,42);
  poll(2000);
  EXPECT_FALSE(link.isConnected());
  EXPECT_STREQ(link.statusName(), "joining");
  EXPECT_EQ(link.localIP(), IPAddress(0,0,0,0));
}

TEST_F(PicoWifi, GivesAssociationThirtySecondsBeforeRetrying) {
  poll(16000); EXPECT_EQ(WiFi.begins, 1);
  poll(30999); EXPECT_EQ(WiFi.begins, 1);
  poll(31000); EXPECT_EQ(WiFi.begins, 2);
}

TEST_F(PicoWifi, GivesDhcpSixtySecondsAfterAssociation) {
  associated(); poll(29000);
  EXPECT_STREQ(link.statusName(), "waiting DHCP");
  poll(31000); poll(88999); EXPECT_EQ(WiFi.begins, 1);
  poll(89000); EXPECT_EQ(WiFi.begins, 2);
  EXPECT_STREQ(link.statusName(), "DHCP timeout");
}

TEST_F(PicoWifi, LeaseAfterOldRetryDeadlineBringsLinkUp) {
  associated(); poll(2000); poll(20000);
  lease(); EXPECT_EQ(poll(30000), NetworkTransition::Up);
  EXPECT_TRUE(link.isConnected()); EXPECT_EQ(WiFi.begins, 1);
  EXPECT_EQ(link.localIP(), WiFi.ip); EXPECT_EQ(link.rssi(), -62);
}

TEST_F(PicoWifi, SamplesRssiEveryFiveSeconds) {
  lease(); poll(2000); EXPECT_EQ(WiFi.signals, 1);
  poll(2010); poll(6999); EXPECT_EQ(WiFi.signals, 1);
  poll(7000); EXPECT_EQ(WiFi.signals, 2);
}

TEST_F(PicoWifi, ReportsDriverFailuresAndBacksOff) {
  mock_link = CYW43_LINK_BADAUTH; poll(2000);
  EXPECT_STREQ(link.statusName(), "auth failed");
  EXPECT_EQ(WiFi.begins, 1);
  poll(16000); EXPECT_EQ(WiFi.begins, 2);
  mock_link = CYW43_LINK_NONET; poll(17000);
  EXPECT_STREQ(link.statusName(), "SSID not found");
  poll(45999); EXPECT_EQ(WiFi.begins, 2);
  poll(46000); EXPECT_EQ(WiFi.begins, 3);
}

TEST_F(PicoWifi, LostLinkClearsAddressDnsAndRssi) {
  lease(); poll(2000);
  mock_link = CYW43_LINK_DOWN;
  EXPECT_EQ(poll(2100), NetworkTransition::Down);
  EXPECT_EQ(link.localIP(), IPAddress(0,0,0,0)); EXPECT_EQ(link.rssi(), 0);
  char dns[160]; link.formatDns(dns, sizeof(dns)); EXPECT_STREQ(dns, "> 0.0.0.0");
}

TEST_F(PicoWifi, ChangedCredentialsRestartOnceAndUnchangedCredentialsKeepLease) {
  link.begin("replacement", "new password"); EXPECT_EQ(WiFi.begins, 2);
  EXPECT_EQ(WiFi.password, "new password");
  lease(); poll(2000); link.begin("replacement", "new password");
  EXPECT_EQ(WiFi.begins, 2); EXPECT_TRUE(link.isConnected());
}

TEST_F(PicoWifi, EmptyPasswordUsesOpenAuthenticationAndEmptySsidStopsRetries) {
  link.begin("open-network", ""); EXPECT_TRUE(WiFi.open);
  link.begin("", ""); const int attempts = WiFi.begins;
  EXPECT_EQ(WiFi.disconnects, 1); EXPECT_STREQ(link.statusName(), "no SSID");
  poll(400000); EXPECT_EQ(WiFi.begins, attempts);
}

TEST_F(PicoWifi, RetryDeadlineSurvivesMillisWrap) {
  g_mock_millis = std::numeric_limits<uint32_t>::max() - 10000;
  link.begin("wrap-network", "secret"); const int attempts = WiFi.begins;
  poll(19998); EXPECT_EQ(WiFi.begins, attempts);
  poll(19999); EXPECT_EQ(WiFi.begins, attempts + 1);
}
