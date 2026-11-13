#include <gtest/gtest.h>
#include <string>

#include "helpers/poll/PollHttp.h"

TEST(PollHttp, BaseUrl) {
  poll::HttpBase b;
  ASSERT_TRUE(poll::parseHttpBase("https://corescope.example.org", b));
  EXPECT_TRUE(b.tls);
  EXPECT_EQ(443, b.port);
  EXPECT_STREQ("corescope.example.org", b.host);
  EXPECT_STREQ("", b.prefix);

  ASSERT_TRUE(poll::parseHttpBase("HTTP://127.0.0.1:8188/", b));
  EXPECT_FALSE(b.tls);
  EXPECT_EQ(8188, b.port);
  EXPECT_STREQ("127.0.0.1", b.host);
  EXPECT_STREQ("", b.prefix);

  ASSERT_TRUE(poll::parseHttpBase("http://h/cs/api-root//", b));
  EXPECT_EQ(80, b.port);
  EXPECT_STREQ("/cs/api-root", b.prefix);

  const char* bad[] = {"", "ftp://h", "http://", "http://:80", "http://h:0", "http://h:65536",
                       "http://h:x", "http://u@h", "http://h/a?b", "http://[::1]/", "http://h h",
                       "https://h/p#f", nullptr};
  for (const char** p = bad; *p; p++) EXPECT_FALSE(poll::parseHttpBase(*p, b)) << *p;
  EXPECT_FALSE(poll::parseHttpBase(nullptr, b));
}

TEST(PollHttp, StatusLine) {
  bool h10 = true;
  EXPECT_EQ(200, poll::parseHttpStatusLine("HTTP/1.1 200 OK", &h10));
  EXPECT_FALSE(h10);
  EXPECT_EQ(404, poll::parseHttpStatusLine("HTTP/1.0 404", &h10));
  EXPECT_TRUE(h10);
  EXPECT_EQ(-1, poll::parseHttpStatusLine("HTTP/2 200 OK", &h10));
  EXPECT_EQ(-1, poll::parseHttpStatusLine("HTTP/1.1 20 OK", &h10));
  EXPECT_EQ(-1, poll::parseHttpStatusLine("HTTP/1.1 2000 OK", &h10));
  EXPECT_EQ(-1, poll::parseHttpStatusLine("<html>", &h10));
}

TEST(PollHttp, Framing) {
  poll::HttpFraming f;
  poll::httpFramingInit(f);
  EXPECT_TRUE(poll::applyHttpHeader("Content-Length: 512", f));
  EXPECT_EQ(512, f.content_length);
  EXPECT_TRUE(poll::applyHttpHeader("content-length:512 ", f));
  EXPECT_FALSE(poll::applyHttpHeader("Content-Length: 513", f));
  EXPECT_FALSE(poll::applyHttpHeader("Content-Length: -1", f));
  EXPECT_FALSE(poll::applyHttpHeader("Content-Length: 99999999999", f));
  EXPECT_TRUE(poll::applyHttpHeader("Transfer-Encoding: gzip, Chunked", f));
  EXPECT_TRUE(f.chunked);
  EXPECT_TRUE(poll::applyHttpHeader("Connection: Keep-Alive", f));
  EXPECT_TRUE(f.keep_alive);
  EXPECT_FALSE(f.close);
  EXPECT_TRUE(poll::applyHttpHeader("Connection: close", f));
  EXPECT_TRUE(f.close);
  EXPECT_TRUE(poll::applyHttpHeader("X-Connection: nothing", f));
  EXPECT_TRUE(poll::applyHttpHeader("Content-Lengthy: 3", f));
  EXPECT_EQ(512, f.content_length);
}

TEST(PollHttp, ChunkSize) {
  EXPECT_EQ(0x1a, poll::parseHttpChunkSize("1A"));
  EXPECT_EQ(0, poll::parseHttpChunkSize("0"));
  EXPECT_EQ(255, poll::parseHttpChunkSize("ff;name=val"));
  EXPECT_EQ(-1, poll::parseHttpChunkSize(""));
  EXPECT_EQ(-1, poll::parseHttpChunkSize("zz"));
  EXPECT_EQ(-1, poll::parseHttpChunkSize("fffffffff"));
}

TEST(PollHttp, LinesStopBeforeAnUnterminatedLineCanGrow) {
  size_t reads = 0, total = 0;
  char line[8];
  auto endless = [&](uint8_t* b) { ++reads; *b = 'x'; return 1; };
  EXPECT_EQ(-3, poll::readHttpLine(endless, line, sizeof(line), &total));
  EXPECT_EQ(sizeof(line), reads);
  EXPECT_EQ(reads, total);
}

TEST(PollHttp, TrailersShareTheFramingBudget) {
  size_t reads = 0, total = 0;
  char line[32];
  const std::string trailer = "X: y\r\n";
  auto repeated = [&](uint8_t* b) { *b = trailer[reads++ % trailer.size()]; return 1; };
  int r;
  do { r = poll::readHttpLine(repeated, line, sizeof(line), &total); } while (r == 1);
  EXPECT_EQ(-3, r);
  EXPECT_EQ(poll::kHttpFramingMax, reads);
  EXPECT_EQ(reads, total);
  EXPECT_EQ(-3, poll::readHttpLine(repeated, line, sizeof(line), &total));
  EXPECT_EQ(poll::kHttpFramingMax, reads);
}

TEST(PollHttp, LineAcceptsFinalBudgetByteAndPropagatesReadFailures) {
  char line[8];
  size_t total = poll::kHttpFramingMax - 3, reads = 0;
  auto input = [&](uint8_t* b) { *b = "x\r\n"[reads++]; return 1; };
  EXPECT_EQ(1, poll::readHttpLine(input, line, sizeof(line), &total));
  EXPECT_STREQ("x", line);
  EXPECT_EQ(poll::kHttpFramingMax, total);
  for (int failure : {0, -1, -2}) {
    total = 0;
    auto stopped = [&](uint8_t*) { return failure; };
    EXPECT_EQ(failure, poll::readHttpLine(stopped, line, sizeof(line), &total));
    EXPECT_EQ(0u, total);
  }
}

TEST(PollHttp, LateConnectOrCancellationPreventsAnyWrite) {
  const uint8_t data[] = {1, 2, 3};
  size_t writes = 0;
  auto write = [&](const uint8_t*, size_t n) { ++writes; return (int)n; };
  for (int stopped : {0, -2}) {
    EXPECT_EQ(stopped, poll::writeHttpBytes(data, sizeof(data), [&]() { return stopped; },
                                          write, []() {}));
  }
  EXPECT_EQ(0u, writes);
}

TEST(PollHttp, WritesStopOnBudgetEvenWithPartialProgress) {
  const uint8_t data[] = {1, 2, 3, 4};
  size_t sent = 0;
  auto ready = [&]() { return sent < 2 ? 1 : 0; };
  auto write = [&](const uint8_t* p, size_t) { EXPECT_EQ(data[sent], *p); ++sent; return 1; };
  EXPECT_EQ(0, poll::writeHttpBytes(data, sizeof(data), ready, write, []() {}));
  EXPECT_EQ(2u, sent);
}

TEST(PollHttp, BlockedWriteYieldsAndRegistrationBodyChecksCancellation) {
  const uint8_t data[] = {1, 2, 3};
  int state = 1;
  size_t writes = 0;
  auto ready = [&]() { return state; };
  auto blocked = [&](const uint8_t*, size_t) { ++writes; return 0; };
  EXPECT_EQ(0, poll::writeHttpBytes(data, sizeof(data), ready, blocked, [&]() { state = 0; }));
  EXPECT_EQ(1u, writes);

  state = 1;
  auto header = [&](const uint8_t*, size_t n) { state = -2; ++writes; return (int)n; };
  EXPECT_EQ(-2, poll::writeHttpBytes(data, sizeof(data), ready, header, []() {}));
  EXPECT_EQ(-2, poll::writeHttpBytes(data, sizeof(data), ready, blocked, []() {}));
  EXPECT_EQ(2u, writes);
}

TEST(PollHttp, CompleteWriteAndTransportFailure) {
  const uint8_t data[] = {1, 2};
  EXPECT_EQ(1, poll::writeHttpBytes(data, sizeof(data), []() { return 1; },
                                   [](const uint8_t*, size_t n) { return (int)n; }, []() {}));
  EXPECT_EQ(-1, poll::writeHttpBytes(data, sizeof(data), []() { return 1; },
                                    [](const uint8_t*, size_t) { return -1; }, []() {}));
}

TEST(PollHttp, ControlPathsAppendTheClientIdentity) {
  char path[256];
  poll::buildOwnersPath("abc123", "0.11.1", "", path, sizeof(path));
  EXPECT_STREQ("/api/guest-polling/owners?observer=abc123&plugin_version=0.11.1"
               "&client=meshcore.observer_fw", path);

  poll::buildPolicyPath("abc123", "0.11.1", "", path, sizeof(path));
  EXPECT_STREQ("/api/guest-polling/policy?plugin_version=0.11.1&observer=abc123"
               "&client=meshcore.observer_fw", path);

  poll::buildOwnersPath("abc123", "0.11.1", "1.2.3", path, sizeof(path));
  EXPECT_STREQ("/api/guest-polling/owners?observer=abc123&plugin_version=0.11.1"
               "&client=meshcore.observer_fw&client_version=1.2.3", path);

  poll::buildPolicyPath("abc123", "0.11.1", "1.2.3", path, sizeof(path));
  EXPECT_STREQ("/api/guest-polling/policy?plugin_version=0.11.1&observer=abc123"
               "&client=meshcore.observer_fw&client_version=1.2.3", path);
}

TEST(PollHttp, ClientVersionKeepsOnlyUrlSafeBytes) {
  char version[24];
  poll::sanitizeClientVersion("v1.2.3 beta+x/y", version, sizeof(version));
  EXPECT_STREQ("v1.2.3_beta_x_y", version);
  poll::sanitizeClientVersion("firmware_A-1.0", version, sizeof(version));
  EXPECT_STREQ("firmware_A-1.0", version);
  poll::sanitizeClientVersion(nullptr, version, sizeof(version));
  EXPECT_STREQ("", version);
  poll::sanitizeClientVersion("abcdefghijklmnopqrstuvwxyz", version, sizeof(version));
  EXPECT_EQ(poll::kClientVersionMax - 1, strlen(version));

  char path[256];
  poll::buildPolicyPath("abc", "0.11.1", "1.2 (rc 1)+x", path, sizeof(path));
  EXPECT_STREQ("/api/guest-polling/policy?plugin_version=0.11.1&observer=abc"
               "&client=meshcore.observer_fw&client_version=1.2__rc_1__x", path);
}

TEST(PollHttp, ControlPathRefusesAShortBuffer) {
  char tiny[8];
  EXPECT_EQ(0u, poll::buildOwnersPath("abc", "0.11.1", "", tiny, sizeof(tiny)));
  EXPECT_STREQ("", tiny);
  EXPECT_EQ(0u, poll::buildPolicyPath("abc", "0.11.1", "1.2", tiny, sizeof(tiny)));
  EXPECT_STREQ("", tiny);
  EXPECT_EQ(0u, poll::buildOwnersPath("abc", "0.11.1", "", nullptr, 0));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
