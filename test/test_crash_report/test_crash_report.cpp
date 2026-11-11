// Host tests for the crash record formatter (src/helpers/CrashReportFormat.h).
#include <gtest/gtest.h>

#include <ArduinoJson.h>
#include <cstring>
#include <string>

#include "helpers/CrashReportFormat.h"

namespace {

const size_t GUARD = 8;

// Buffer with a canary tail so any write past the logical size is caught.
struct Canaried {
  char mem[256 + GUARD];
  size_t logical;
  explicit Canaried(size_t n) : logical(n) { memset(mem, 0xAA, sizeof(mem)); }
  bool guardIntact() const {
    for (size_t i = logical; i < logical + GUARD; i++) {
      if ((unsigned char)mem[i] != 0xAA) return false;
    }
    return true;
  }
};

CrashRecord typicalRecord() {
  CrashRecord r = {};
  r.arch = CrashRecord::ARCH_XTENSA;
  r.pc = 0x400d1a2b;
  r.cause = 28;
  r.vaddr = 0x00000010;
  r.bt_depth = 3;
  r.bt[0] = 0x400d1a2b;
  r.bt[1] = 0x400d5678;
  r.bt[2] = 0x40089abc;
  strcpy(r.task, "mqtt_bridge");
  strcpy(r.elf_sha, "1a2b3c4d");
  return r;
}

// Every field at its widest: 15-char task, 10-digit cause, full backtrace, corrupt flag.
CrashRecord widestRecord() {
  CrashRecord r = {};
  r.arch = CrashRecord::ARCH_XTENSA;
  r.pc = 0xffffffff;
  r.cause = 0xffffffff;
  r.vaddr = 0xffffffff;
  r.bt_depth = CrashRecord::kMaxBacktrace;
  for (int i = 0; i < CrashRecord::kMaxBacktrace; i++) r.bt[i] = 0x40000000u + (uint32_t)i;
  r.bt_corrupted = 1;
  memset(r.task, 'T', 15);
  r.task[15] = '\0';
  strcpy(r.elf_sha, "ffffffff");
  return r;
}

}  // namespace

TEST(CrashReportFormat, RecordFitsStaticBudget) {
  EXPECT_LE(sizeof(CrashRecord), 128u);
}

TEST(CrashReportFormat, CliNoneWithoutRecord) {
  char buf[160];
  CrashReportFormat::formatCli(nullptr, buf, sizeof(buf));
  EXPECT_STREQ("> none", buf);
  CrashRecord empty = {};
  CrashReportFormat::formatCli(&empty, buf, sizeof(buf));
  EXPECT_STREQ("> none", buf);
}

TEST(CrashReportFormat, CliTypicalRecord) {
  CrashRecord r = typicalRecord();
  char buf[160];
  size_t n = CrashReportFormat::formatCli(&r, buf, sizeof(buf));
  EXPECT_STREQ("> task=mqtt_bridge pc=400d1a2b exc=28 va=00000010 sha=1a2b3c4d "
               "bt=400d1a2b 400d5678 40089abc", buf);
  EXPECT_EQ(strlen(buf), n);
}

TEST(CrashReportFormat, CliWidestRecordFitsReplyBuffer) {
  CrashRecord r = widestRecord();
  char buf[160];
  CrashReportFormat::formatCli(&r, buf, sizeof(buf));
  std::string s(buf);
  EXPECT_NE(std::string::npos, s.find("bt=40000000 40000001 40000002 40000003 "
                                      "40000004 40000005 40000006 40000007 !"));
  EXPECT_LT(s.size(), sizeof(buf));
}

TEST(CrashReportFormat, CliTruncationKeepsWholeTokensAndBounds) {
  CrashRecord r = widestRecord();
  for (size_t cap = 0; cap <= 200; cap++) {
    Canaried c(cap);
    size_t n = CrashReportFormat::formatCli(&r, cap ? c.mem : nullptr, cap);
    ASSERT_TRUE(c.guardIntact()) << "cap=" << cap;
    if (cap == 0) {
      EXPECT_EQ(0u, n);
      continue;
    }
    ASSERT_LT(n, cap);
    ASSERT_EQ(n, strlen(c.mem)) << "cap=" << cap;
    // Never ends mid-address: the last backtrace token is always 8 hex digits.
    std::string s(c.mem);
    size_t bt = s.find(" bt=");
    if (bt != std::string::npos) {
      size_t last_space = s.find_last_of(' ');
      std::string tail = s.substr(last_space + 1);
      if (tail != "!" && tail.rfind("bt=", 0) != 0) EXPECT_EQ(8u, tail.size()) << s;
    }
  }
}

TEST(CrashReportFormat, TaskNameIsSanitized) {
  CrashRecord r = typicalRecord();
  strcpy(r.task, "a\"b\\c d\x01");
  char buf[160];
  CrashReportFormat::formatJson(&r, buf, sizeof(buf));
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, buf)) << buf;
  EXPECT_STREQ("a?b?c?d?", doc["task"].as<const char*>());
}

TEST(CrashReportFormat, UnterminatedTaskNameStopsAtFifteen) {
  CrashRecord r = typicalRecord();
  memset(r.task, 'x', sizeof(r.task));  // no terminator
  char buf[160];
  CrashReportFormat::formatJson(&r, buf, sizeof(buf));
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, buf)) << buf;
  EXPECT_EQ(15u, strlen(doc["task"].as<const char*>()));
}

TEST(CrashReportFormat, JsonEmptyWithoutRecord) {
  char buf[160];
  memset(buf, 'z', sizeof(buf));
  EXPECT_EQ(0u, CrashReportFormat::formatJson(nullptr, buf, sizeof(buf)));
  EXPECT_STREQ("", buf);
}

TEST(CrashReportFormat, JsonTypicalRecordRoundTrips) {
  CrashRecord r = typicalRecord();
  char buf[CrashReportFormat::kJsonMax];
  size_t n = CrashReportFormat::formatJson(&r, buf, sizeof(buf));
  ASSERT_GT(n, 0u);
  EXPECT_STREQ("{\"task\":\"mqtt_bridge\",\"pc\":\"400d1a2b\",\"exc\":28,\"va\":\"00000010\","
               "\"sha\":\"1a2b3c4d\",\"bt\":\"400d1a2b 400d5678 40089abc\"}", buf);
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, buf));
  EXPECT_EQ(28, doc["exc"].as<int>());
}

TEST(CrashReportFormat, JsonFullBacktraceWithShortTaskFitsFragmentCap) {
  CrashRecord r = typicalRecord();
  strcpy(r.task, "loopTask");
  r.bt_depth = CrashRecord::kMaxBacktrace;
  for (int i = 0; i < CrashRecord::kMaxBacktrace; i++) r.bt[i] = 0x400d0000u + (uint32_t)i;
  char buf[CrashReportFormat::kJsonMax];
  ASSERT_GT(CrashReportFormat::formatJson(&r, buf, sizeof(buf)), 0u);
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, buf));
  EXPECT_STREQ("400d0000 400d0001 400d0002 400d0003 400d0004 400d0005 400d0006 400d0007",
               doc["bt"].as<const char*>());
}

TEST(CrashReportFormat, JsonWidestRecordDropsTrailingBacktraceButStaysValid) {
  CrashRecord r = widestRecord();
  char buf[CrashReportFormat::kJsonMax];
  size_t n = CrashReportFormat::formatJson(&r, buf, sizeof(buf));
  ASSERT_GT(n, 0u);
  ASSERT_LT(n, CrashReportFormat::kJsonMax);
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, buf)) << buf;
  EXPECT_STREQ("ffffffff", doc["pc"].as<const char*>());
  EXPECT_EQ(4294967295u, doc["exc"].as<uint32_t>());
  std::string bt = doc["bt"].as<const char*>();
  EXPECT_EQ(0u, bt.rfind("40000000", 0));
}

TEST(CrashReportFormat, JsonEveryBufferSizeIsValidOrEmpty) {
  CrashRecord r = widestRecord();
  for (size_t cap = 0; cap <= 200; cap++) {
    Canaried c(cap);
    size_t n = CrashReportFormat::formatJson(&r, cap ? c.mem : nullptr, cap);
    ASSERT_TRUE(c.guardIntact()) << "cap=" << cap;
    if (cap == 0) continue;
    ASSERT_EQ(n, strlen(c.mem)) << "cap=" << cap;
    if (n == 0) continue;
    ASSERT_LT(n, cap);
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, c.mem)) << "cap=" << cap << " " << c.mem;
    EXPECT_TRUE(doc["task"].is<const char*>());
  }
}

TEST(CrashReportFormat, JsonCorruptBacktraceIsMarked) {
  CrashRecord r = typicalRecord();
  r.bt_corrupted = 1;
  char buf[CrashReportFormat::kJsonMax];
  ASSERT_GT(CrashReportFormat::formatJson(&r, buf, sizeof(buf)), 0u);
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, buf));
  EXPECT_STREQ("400d1a2b 400d5678 40089abc !", doc["bt"].as<const char*>());
}

TEST(CrashReportFormat, JsonOmitsEmptyShaAndBacktrace) {
  CrashRecord r = typicalRecord();
  r.elf_sha[0] = '\0';
  r.bt_depth = 0;
  char buf[CrashReportFormat::kJsonMax];
  ASSERT_GT(CrashReportFormat::formatJson(&r, buf, sizeof(buf)), 0u);
  EXPECT_STREQ("{\"task\":\"mqtt_bridge\",\"pc\":\"400d1a2b\",\"exc\":28,\"va\":\"00000010\"}", buf);
}

TEST(CrashReportFormat, OversizedDepthIsClamped) {
  CrashRecord r = typicalRecord();
  r.bt_depth = 200;
  char buf[200];
  CrashReportFormat::formatCli(&r, buf, sizeof(buf));
  std::string s(buf);
  size_t bt = s.find("bt=");
  ASSERT_NE(std::string::npos, bt);
  // 8 entries, separated by 7 spaces.
  EXPECT_EQ(8u * 8u + 7u, s.size() - (bt + 3));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
