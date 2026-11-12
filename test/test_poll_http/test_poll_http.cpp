#include <gtest/gtest.h>

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

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
