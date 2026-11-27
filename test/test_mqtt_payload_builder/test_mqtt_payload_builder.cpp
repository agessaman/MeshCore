#include <gtest/gtest.h>

#include <ArduinoJson.h>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "helpers/CrashReportFormat.h"
#include "helpers/MQTTPayloadBuilder.h"

namespace {

constexpr const char* kTimestamp = "2026-07-18T12:34:56.123456+00:00";

struct RejectAllJsonAllocations : ArduinoJson::Allocator {
  void* allocate(size_t) override { return nullptr; }
  void deallocate(void*) override {}
  void* reallocate(void*, size_t) override { return nullptr; }
};

static int buildMinimalStatus(JsonDocument& scratch, char* buffer, size_t buffer_size) {
  return MQTTPayloadBuilder::buildStatusMessage(
      scratch, "DEN Repeater", "0123456789ABCDEF", "Heltec V3", "v1.16.0",
      "915.000000,62.5,7,5", "MeshCore", "online", kTimestamp,
      buffer, buffer_size);
}

static int buildRepresentativePacket(JsonDocument& scratch, const char* direction,
                                     float score, const uint8_t* path, int path_hops,
                                     int path_hash_size, const char* raw,
                                     char* buffer, size_t buffer_size) {
  return MQTTPayloadBuilder::buildPacketMessage(
      scratch, "DEN Repeater", "0123456789ABCDEF", kTimestamp, direction,
      "12:34:56", "18/07/2026", 42, 4, "D", 20, raw,
      10.26f, -87, score, "89ABCDEF01234567", path, path_hops,
      path_hash_size, 64, buffer, buffer_size);
}

static int buildPacketWithMetrics(JsonDocument& scratch, const char* direction,
                                 float snr, int rssi, float score,
                                 char* buffer, size_t buffer_size) {
  return MQTTPayloadBuilder::buildPacketMessage(
      scratch, "DEN Repeater", "0123456789ABCDEF", kTimestamp, direction,
      "12:34:56", "18/07/2026", 42, 4, "D", 20, "A0B1C2D3",
      snr, rssi, score, "89ABCDEF01234567", nullptr, 0, 0, 64,
      buffer, buffer_size);
}

TEST(MQTTPayloadBuilder, MinimalStatusHasExactRequiredContract) {
  JsonDocument scratch;
  char buffer[768];
  int len = buildMinimalStatus(scratch, buffer, sizeof(buffer));

  ASSERT_GT(len, 0);
  EXPECT_EQ(static_cast<size_t>(len), strlen(buffer));
  EXPECT_STREQ(
      "{\"status\":\"online\",\"timestamp\":\"2026-07-18T12:34:56.123456+00:00\","
      "\"origin\":\"DEN Repeater\",\"origin_id\":\"0123456789ABCDEF\","
      "\"model\":\"Heltec V3\",\"firmware_version\":\"v1.16.0\","
      "\"radio\":\"915.000000,62.5,7,5\",\"client_version\":\"MeshCore\"}",
      buffer);

  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_FALSE(parsed["repeat"].is<JsonVariant>());
  EXPECT_FALSE(parsed["stats"].is<JsonVariant>());
}

TEST(MQTTPayloadBuilder, StatusIncludesRepeatAndEveryRequestedStatistic) {
  JsonDocument scratch;
  char buffer[1024];
  int len = MQTTPayloadBuilder::buildStatusMessage(
      scratch, "node", "id", "model", "firmware", "radio", "client", "online",
      kTimestamp, buffer, sizeof(buffer), 4200, 86400, 3, 6, -112,
      11, 22, 4, 180864, 31, 47, "on");

  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_STREQ("on", parsed["repeat"].as<const char*>());
  JsonObject stats = parsed["stats"].as<JsonObject>();
  ASSERT_FALSE(stats.isNull());
  EXPECT_EQ(4200, stats["battery_mv"].as<int>());
  EXPECT_EQ(86400, stats["uptime_secs"].as<int>());
  EXPECT_EQ(3, stats["errors"].as<int>());
  EXPECT_EQ(6, stats["queue_len"].as<int>());
  EXPECT_EQ(-112, stats["noise_floor"].as<int>());
  EXPECT_EQ(11, stats["tx_air_secs"].as<int>());
  EXPECT_EQ(22, stats["rx_air_secs"].as<int>());
  EXPECT_EQ(4, stats["recv_errors"].as<int>());
  EXPECT_EQ(180864, stats["internal_heap"].as<int>());
  EXPECT_EQ(31, stats["packets_sent"].as<int>());
  EXPECT_EQ(47, stats["packets_received"].as<int>());
  EXPECT_FALSE(stats["heap_min"].is<JsonVariant>());
  EXPECT_FALSE(stats["reset_reason"].is<JsonVariant>());
  EXPECT_FALSE(stats["selfheal"].is<JsonVariant>());
  EXPECT_FALSE(stats["selfheal_n"].is<JsonVariant>());
}

TEST(MQTTPayloadBuilder, StatusReportsConnectionHealthWhenSupplied) {
  JsonDocument scratch;
  char buffer[1024];
  MQTTConnHealth health;
  health.slots_up = 2;
  health.slots_total = 3;
  health.worst_outage_secs = 18432;
  health.heap_largest = 16116;

  int len = MQTTPayloadBuilder::buildStatusMessage(
      scratch, "node", "id", "model", "firmware", "radio", "client", "online",
      kTimestamp, buffer, sizeof(buffer), -1, -1, -1, -1, -999,
      -1, -1, -1, -1, -1, -1, nullptr, health);

  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  JsonObject stats = parsed["stats"].as<JsonObject>();
  ASSERT_FALSE(stats.isNull());
  EXPECT_EQ(2, stats["mqtt_slots_up"].as<int>());
  EXPECT_EQ(3, stats["mqtt_slots_total"].as<int>());
  EXPECT_EQ(18432, stats["mqtt_outage_secs"].as<int>());
  EXPECT_EQ(16116, stats["heap_largest"].as<int>());
}

TEST(MQTTPayloadBuilder, StatusReportsConnectFailuresAndBreakerOnlyWhenNonZero) {
  JsonDocument scratch;
  char buffer[1024];
  MQTTConnHealth health;
  health.slots_up = 1;
  health.slots_total = 3;
  health.connect_failures = 57;
  health.slots_breaker = 1;

  int len = MQTTPayloadBuilder::buildStatusMessage(
      scratch, "node", "id", "model", "firmware", "radio", "client", "online",
      kTimestamp, buffer, sizeof(buffer), -1, -1, -1, -1, -999,
      -1, -1, -1, -1, -1, -1, nullptr, health);

  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  JsonObject stats = parsed["stats"].as<JsonObject>();
  EXPECT_EQ(57, stats["mqtt_connect_failures"].as<int>());
  EXPECT_EQ(1, stats["mqtt_slots_breaker"].as<int>());

  // A healthy board must not carry either key: zero connect failures and zero
  // tripped breakers are the normal case, and the steady-state payload is
  // already close to the buffer bound.
  MQTTConnHealth healthy;
  healthy.slots_up = 3;
  healthy.slots_total = 3;
  healthy.connect_failures = 0;
  healthy.slots_breaker = 0;
  ASSERT_GT(MQTTPayloadBuilder::buildStatusMessage(
      scratch, "node", "id", "model", "firmware", "radio", "client", "online",
      kTimestamp, buffer, sizeof(buffer), -1, -1, -1, -1, -999,
      -1, -1, -1, -1, -1, -1, nullptr, healthy), 0);
  JsonDocument healthy_parsed;
  ASSERT_FALSE(deserializeJson(healthy_parsed, buffer));
  JsonObject healthy_stats = healthy_parsed["stats"].as<JsonObject>();
  EXPECT_FALSE(healthy_stats["mqtt_connect_failures"].is<JsonVariant>());
  EXPECT_FALSE(healthy_stats["mqtt_slots_breaker"].is<JsonVariant>());
}

TEST(MQTTPayloadBuilder, StatusReportsResetReasonAndMinimumHeap) {
  JsonDocument scratch;
  char buffer[1024];
  int len = MQTTPayloadBuilder::buildStatusMessage(
      scratch, "node", "id", "model", "firmware", "radio", "client", "online",
      kTimestamp, buffer, sizeof(buffer), -1, -1, -1, -1, -999,
      -1, -1, -1, -1, -1, -1, nullptr, MQTTConnHealth(), 4096, "panic");

  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  JsonObject stats = parsed["stats"].as<JsonObject>();
  ASSERT_FALSE(stats.isNull());
  EXPECT_EQ(4096, stats["heap_min"].as<int>());
  EXPECT_STREQ("panic", stats["reset_reason"].as<const char*>());

  // Zero is a real minimum (the heap was exhausted). An empty token is not a reason.
  ASSERT_GT(MQTTPayloadBuilder::buildStatusMessage(
      scratch, "node", "id", "model", "firmware", "radio", "client", "online",
      kTimestamp, buffer, sizeof(buffer), -1, -1, -1, -1, -999,
      -1, -1, -1, -1, -1, -1, nullptr, MQTTConnHealth(), 0, ""), 0);
  JsonDocument zero_min;
  ASSERT_FALSE(deserializeJson(zero_min, buffer));
  EXPECT_EQ(0, zero_min["stats"]["heap_min"].as<int>());
  EXPECT_FALSE(zero_min["stats"]["reset_reason"].is<JsonVariant>());
}

TEST(MQTTPayloadBuilder, StatusReportsSelfHealOnlyWhileTheCountIsNonZero) {
  JsonDocument scratch;
  char buffer[1024];
  int len = MQTTPayloadBuilder::buildStatusMessage(
      scratch, "node", "id", "model", "firmware", "radio", "client", "online",
      kTimestamp, buffer, sizeof(buffer), -1, -1, -1, -1, -999,
      -1, -1, -1, -1, -1, -1, nullptr, MQTTConnHealth(), -1, "sw", nullptr, nullptr, "stop_unproven", 2);
  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  JsonObject stats = parsed["stats"].as<JsonObject>();
  ASSERT_FALSE(stats.isNull());
  EXPECT_STREQ("sw", stats["reset_reason"].as<const char*>());
  EXPECT_STREQ("stop_unproven", stats["selfheal"].as<const char*>());
  EXPECT_EQ(2, stats["selfheal_n"].as<int>());

  // A cleared counter omits both keys, and alone does not create a stats object.
  ASSERT_GT(MQTTPayloadBuilder::buildStatusMessage(
      scratch, "node", "id", "model", "firmware", "radio", "client", "online",
      kTimestamp, buffer, sizeof(buffer), -1, -1, -1, -1, -999,
      -1, -1, -1, -1, -1, -1, nullptr, MQTTConnHealth(), -1, nullptr, nullptr, nullptr, "none", 0), 0);
  JsonDocument cleared;
  ASSERT_FALSE(deserializeJson(cleared, buffer));
  EXPECT_FALSE(cleared["stats"].is<JsonVariant>());
}

// Zero counters with nothing else supplied must not create an empty stats object.
TEST(MQTTPayloadBuilder, StatusOmitsStatsWhenOnlyZeroCountersSupplied) {
  JsonDocument scratch;
  char buffer[768];
  MQTTConnHealth health;
  health.connect_failures = 0;
  health.slots_breaker = 0;

  ASSERT_GT(MQTTPayloadBuilder::buildStatusMessage(
      scratch, "node", "id", "model", "firmware", "radio", "client", "online",
      kTimestamp, buffer, sizeof(buffer), -1, -1, -1, -1, -999,
      -1, -1, -1, -1, -1, -1, nullptr, health), 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_FALSE(parsed["stats"].is<JsonVariant>());
}

// The outage key doubles as the "something is down" flag, so its absence has to be
// meaningful: a fully healthy board must not emit it.
TEST(MQTTPayloadBuilder, StatusOmitsOutageKeyWhenEverySlotIsUp) {
  JsonDocument scratch;
  char buffer[768];
  MQTTConnHealth health;
  health.slots_up = 3;
  health.slots_total = 3;
  health.heap_largest = 54260;   // worst_outage_secs left at -1

  int len = MQTTPayloadBuilder::buildStatusMessage(
      scratch, "node", "id", "model", "firmware", "radio", "client", "online",
      kTimestamp, buffer, sizeof(buffer), -1, -1, -1, -1, -999,
      -1, -1, -1, -1, -1, -1, nullptr, health);

  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  JsonObject stats = parsed["stats"].as<JsonObject>();
  ASSERT_FALSE(stats.isNull());
  EXPECT_EQ(3, stats["mqtt_slots_up"].as<int>());
  EXPECT_FALSE(stats["mqtt_outage_secs"].is<JsonVariant>());
}

// serializeComplete() returns 0 and clears the buffer when the payload does not fit,
// and MQTTBridge publishes only a positive length — so overflowing STATUS_JSON_BUFFER_SIZE
// silently stops status publication altogether. That is the same class of blindness this
// field set exists to fix, so the worst realistic payload must fit with margin.
// Field widths are the real MQTTBridge maxima, not plausible-looking strings:
//   _origin[32] _device_id[65] _firmware_version[64] _board_model[64]
// and client_version is snprintf("meshcore/%s", _firmware_version) into char[64].
// isValidName() rejects [ ] / \ : , ? * but ALLOWS '"', so a node name of 31 quotes
// is legal and every one escapes to two bytes — that is the true worst case for the
// only free-text field an operator controls.
// heap_min shares heap_largest's 8388608 ceiling; the longest reset token is "deepsleep";
// the longest self-heal token is "client_quarantined" and the count tops out at 255.
TEST(MQTTPayloadBuilder, WorstCaseStatusWithConnectionHealthFitsProductionBuffer) {
  constexpr size_t kStatusJsonBufferSize = 1280;  // MQTTBridge::STATUS_JSON_BUFFER_SIZE
  const std::string origin(31, '"');                 // _origin[32], worst-case escaping
  const std::string origin_id(64, 'A');              // _device_id[65]
  const std::string model(63, 'M');                  // _board_model[64]
  const std::string firmware(63, 'F');               // _firmware_version[64]
  const std::string client_version(63, 'C');         // char client_version[64]
  const std::string radio("9999.999999,1000.0,12,8");

  JsonDocument scratch;
  char buffer[kStatusJsonBufferSize];
  MQTTConnHealth health;
  health.slots_up = 0;
  health.slots_total = 6;              // RUNTIME_MQTT_SLOTS on PSRAM, the widest
  health.worst_outage_secs = 4294967;  // millis() rollover / 1000, the widest value
  health.heap_largest = 8388608;
  health.connect_failures = 2147483647;
  health.slots_breaker = 6;

  int len = MQTTPayloadBuilder::buildStatusMessage(
      scratch, origin.c_str(), origin_id.c_str(), model.c_str(), firmware.c_str(),
      radio.c_str(), client_version.c_str(), "online", kTimestamp,
      buffer, sizeof(buffer),
      4200, 2147483, 2147483647, 6, -128, 2147483, 2147483, 2147483647,
      8388608, 2147483647, 2147483647,
      "off", health, 8388608, "deepsleep", nullptr, nullptr, "client_quarantined", 255);

  ASSERT_GT(len, 0) << "worst-case status payload overflowed the " << kStatusJsonBufferSize
                    << "-byte production buffer; serializeComplete() returns 0 and the "
                       "bridge publishes only len>0, so ALL status publication would "
                       "stop silently";
  EXPECT_LT(static_cast<size_t>(len), kStatusJsonBufferSize - 32)
      << "less than 32 bytes of headroom left in the status buffer (len=" << len << ")";
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_EQ(8388608, parsed["stats"]["heap_min"].as<int>());
  EXPECT_STREQ("deepsleep", parsed["stats"]["reset_reason"].as<const char*>());
  EXPECT_EQ(255, parsed["stats"]["selfheal_n"].as<int>());
}

// The widest crash fragment the formatter emits into the bridge's 160-byte slot.
static std::string widestCrashFragment() {
  CrashRecord r = {};
  r.arch = CrashRecord::ARCH_XTENSA;
  r.pc = r.cause = r.vaddr = 0xffffffff;
  r.bt_depth = CrashRecord::kMaxBacktrace;
  for (int i = 0; i < CrashRecord::kMaxBacktrace; i++) r.bt[i] = 0x40000000u;
  r.bt_corrupted = 1;
  memset(r.task, 'T', 15);
  strcpy(r.elf_sha, "ffffffff");
  char buf[CrashReportFormat::kJsonMax];
  CrashReportFormat::formatJson(&r, buf, sizeof(buf));
  return buf;
}

TEST(MQTTPayloadBuilder, StatusCarriesCrashObjectWhenItFits) {
  const char* crash = "{\"task\":\"loopTask\",\"pc\":\"400d1a2b\",\"exc\":28,"
                      "\"va\":\"00000010\",\"bt\":\"400d1a2b 400d5678\"}";
  JsonDocument scratch;
  char buffer[1024];
  bool included = false;
  int len = MQTTPayloadBuilder::buildStatusMessage(
      scratch, "node", "id", "model", "fw", "radio", "client", "online", kTimestamp,
      buffer, sizeof(buffer), 4200, 60, -1, -1, -999, -1, -1, -1, -1, -1, -1,
      "on", MQTTConnHealth(), -1, "panic", crash, &included);
  ASSERT_GT(len, 0);
  EXPECT_TRUE(included);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_STREQ("loopTask", parsed["crash"]["task"].as<const char*>());
  EXPECT_EQ(28, parsed["crash"]["exc"].as<int>());
  EXPECT_STREQ("panic", parsed["stats"]["reset_reason"].as<const char*>());
}

TEST(MQTTPayloadBuilder, StatusWithoutCrashLeavesFlagFalse) {
  JsonDocument scratch;
  char buffer[1024];
  bool included = true;
  int len = MQTTPayloadBuilder::buildStatusMessage(
      scratch, "node", "id", "model", "fw", "radio", "client", "online", kTimestamp,
      buffer, sizeof(buffer), -1, -1, -1, -1, -999, -1, -1, -1, -1, -1, -1,
      nullptr, MQTTConnHealth(), -1, nullptr, "", &included);
  ASSERT_GT(len, 0);
  EXPECT_FALSE(included);
  EXPECT_EQ(std::string::npos, std::string(buffer).find("crash"));
}

// The crash object adds at most ",\"crash\":" (9) + 159 = 168 bytes. A 1024-byte
// buffer is short of the field maxima, so the field must be dropped and the rest
// still published.
TEST(MQTTPayloadBuilder, WorstCaseStatusDropsCrashInsteadOfOverflowing) {
  constexpr size_t kStatusJsonBufferSize = 1024;
  const std::string origin(31, '"');
  const std::string origin_id(64, 'A');
  const std::string model(63, 'M');
  const std::string firmware(63, 'F');
  const std::string client_version(63, 'C');
  const std::string crash = widestCrashFragment();
  ASSERT_GT(crash.size(), 150u);
  ASSERT_LT(crash.size(), CrashReportFormat::kJsonMax);

  MQTTConnHealth health;
  health.slots_up = 0;
  health.slots_total = 6;
  health.worst_outage_secs = 4294967;
  health.heap_largest = 8388608;
  health.connect_failures = 2147483647;
  health.slots_breaker = 6;

  JsonDocument scratch;
  char buffer[kStatusJsonBufferSize];
  bool included = true;
  int len = MQTTPayloadBuilder::buildStatusMessage(
      scratch, origin.c_str(), origin_id.c_str(), model.c_str(), firmware.c_str(),
      "9999.999999,1000.0,12,8", client_version.c_str(), "online", kTimestamp,
      buffer, sizeof(buffer),
      4200, 2147483, 2147483647, 6, -128, 2147483, 2147483, 2147483647,
      8388608, 2147483647, 2147483647,
      "off", health, 8388608, "deepsleep", crash.c_str(), &included);

  ASSERT_GT(len, 0) << "status must survive a crash field that does not fit";
  EXPECT_FALSE(included);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_FALSE(parsed["crash"].is<JsonVariant>());
  EXPECT_EQ(8388608, parsed["stats"]["heap_min"].as<int>());
}

// The production status buffer must keep the crash field at the same field maxima.
// Overflow drops the field and the bridge then never marks the stored dump published.
TEST(MQTTPayloadBuilder, WorstCaseStatusKeepsWidestCrashInProductionBuffer) {
  constexpr size_t kStatusJsonBufferSize = 1280;  // MQTTBridge::STATUS_JSON_BUFFER_SIZE
  const std::string origin(31, '"');
  const std::string origin_id(64, 'A');
  const std::string model(63, 'M');
  const std::string firmware(63, 'F');
  const std::string client_version(63, 'C');
  const std::string radio("9999.999999,1000.0,12,8");
  const std::string crash = widestCrashFragment();
  ASSERT_LT(crash.size(), CrashReportFormat::kJsonMax);

  MQTTConnHealth health;
  health.slots_up = 0;
  health.slots_total = 6;
  health.worst_outage_secs = 4294967;
  health.heap_largest = 8388608;
  health.connect_failures = 2147483647;
  health.slots_breaker = 6;

  JsonDocument scratch;
  char buffer[kStatusJsonBufferSize];
  bool included = false;
  int len = MQTTPayloadBuilder::buildStatusMessage(
      scratch, origin.c_str(), origin_id.c_str(), model.c_str(), firmware.c_str(),
      radio.c_str(), client_version.c_str(), "online", kTimestamp,
      buffer, sizeof(buffer),
      4200, 2147483, 2147483647, 6, -128, 2147483, 2147483, 2147483647,
      8388608, 2147483647, 2147483647,
      "off", health, 8388608, "deepsleep", crash.c_str(), &included,
      "client_quarantined", 255);

  ASSERT_GT(len, 0) << "worst-case status plus crash overflowed the "
                    << kStatusJsonBufferSize << "-byte production buffer";
  EXPECT_TRUE(included) << "len=" << len;
  EXPECT_LT(static_cast<size_t>(len), kStatusJsonBufferSize - 32)
      << "less than 32 bytes of headroom left (len=" << len << ")";
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_TRUE(parsed["crash"].is<JsonObject>());
  EXPECT_STREQ("TTTTTTTTTTTTTTT", parsed["crash"]["task"].as<const char*>());
  EXPECT_EQ(8388608, parsed["stats"]["heap_min"].as<int>());
}

// A realistic node (ordinary name and versions, full health block) keeps the widest crash object.
TEST(MQTTPayloadBuilder, RealisticStatusKeepsWidestCrash) {
  const std::string crash = widestCrashFragment();
  MQTTConnHealth health;
  health.slots_up = 2;
  health.slots_total = 3;
  health.worst_outage_secs = 86400;
  health.heap_largest = 31732;
  health.connect_failures = 12;
  health.slots_breaker = 1;

  JsonDocument scratch;
  char buffer[1024];
  bool included = false;
  int len = MQTTPayloadBuilder::buildStatusMessage(
      scratch, "DEN Repeater North", "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF",
      "Heltec V3", "v1.18.0.9-abcdef1", "910.525000,62.5,7,5", "meshcore/v1.18.0.9-abcdef1",
      "online", kTimestamp, buffer, sizeof(buffer),
      4120, 2147483, 0, 3, -112, 123456, 654321, 9999, 61234, 123456, 654321,
      "on", health, 41000, "panic", crash.c_str(), &included);
  ASSERT_GT(len, 0);
  EXPECT_TRUE(included) << "len=" << len;
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_TRUE(parsed["crash"]["bt"].is<const char*>());
}

TEST(MQTTPayloadBuilder, StatusOmissionSentinelsRemainOmitted) {
  JsonDocument scratch;
  char buffer[768];
  int len = MQTTPayloadBuilder::buildStatusMessage(
      scratch, "node", "id", "model", "firmware", "radio", "client", "online",
      kTimestamp, buffer, sizeof(buffer), -1, -1, -1, -1, -999,
      -1, -1, -1, -1, -1, -1, nullptr);

  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_FALSE(parsed["stats"].is<JsonVariant>());
  EXPECT_FALSE(parsed["repeat"].is<JsonVariant>());
}

TEST(MQTTPayloadBuilder, StringsAreEscapedAndRoundTrip) {
  const char* origin = "node \"north\"\\rack\nline";
  const char* model = "Heltec\tV3";
  JsonDocument scratch;
  char buffer[1024];
  int len = MQTTPayloadBuilder::buildStatusMessage(
      scratch, origin, "id", model, "v1", "radio", "client", "online",
      kTimestamp, buffer, sizeof(buffer));

  ASSERT_GT(len, 0);
  EXPECT_NE(std::string::npos, std::string(buffer).find("\\\"north\\\""));
  EXPECT_NE(std::string::npos, std::string(buffer).find("\\\\rack\\nline"));
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_STREQ(origin, parsed["origin"].as<const char*>());
  EXPECT_STREQ(model, parsed["model"].as<const char*>());
}

TEST(MQTTPayloadBuilder, RxPacketIncludesMetricsScaledScoreAndPath) {
  const uint8_t path[] = {0xAA, 0xBB, 0x01, 0x2F};
  JsonDocument scratch;
  char buffer[2048];
  int len = buildRepresentativePacket(
      scratch, "rx", 0.125f, path, 2, 2, "A0B1C2D3", buffer, sizeof(buffer));

  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_STREQ("PACKET", parsed["type"].as<const char*>());
  EXPECT_STREQ("rx", parsed["direction"].as<const char*>());
  EXPECT_STREQ("42", parsed["len"].as<const char*>());
  EXPECT_STREQ("4", parsed["packet_type"].as<const char*>());
  EXPECT_STREQ("20", parsed["payload_len"].as<const char*>());
  EXPECT_STREQ("10.3", parsed["SNR"].as<const char*>());
  EXPECT_STREQ("-87", parsed["RSSI"].as<const char*>());
  EXPECT_STREQ("125", parsed["score"].as<const char*>());
  JsonArray parsed_path = parsed["path"].as<JsonArray>();
  ASSERT_EQ(2U, parsed_path.size());
  EXPECT_STREQ("aabb", parsed_path[0].as<const char*>());
  EXPECT_STREQ("012f", parsed_path[1].as<const char*>());
}

TEST(MQTTPayloadBuilder, TxPacketOmitsReceiveOnlyMetricsAndAbsentPath) {
  JsonDocument scratch;
  char buffer[2048];
  int len = buildRepresentativePacket(
      scratch, "tx", 0.5f, nullptr, 0, 0, "A0B1", buffer, sizeof(buffer));

  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_FALSE(parsed["SNR"].is<JsonVariant>());
  EXPECT_FALSE(parsed["RSSI"].is<JsonVariant>());
  EXPECT_FALSE(parsed["score"].is<JsonVariant>());
  EXPECT_FALSE(parsed["path"].is<JsonVariant>());
}

TEST(MQTTPayloadBuilder, RxPacketOmitsUnknownNanScore) {
  JsonDocument scratch;
  char buffer[2048];
  int len = buildRepresentativePacket(
      scratch, "rx", std::nanf(""), nullptr, 0, 0, "A0B1", buffer, sizeof(buffer));

  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_TRUE(parsed["SNR"].is<const char*>());
  EXPECT_TRUE(parsed["RSSI"].is<const char*>());
  EXPECT_FALSE(parsed["score"].is<JsonVariant>());
}

TEST(MQTTPayloadBuilder, RxPacketOmitsUnknownSnrAndRssi) {
  JsonDocument scratch;
  char buffer[2048];
  int len = buildPacketWithMetrics(
      scratch, "rx", NAN, MQTT_RSSI_UNKNOWN, NAN, buffer, sizeof(buffer));

  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_FALSE(parsed["SNR"].is<JsonVariant>());
  EXPECT_FALSE(parsed["RSSI"].is<JsonVariant>());
  EXPECT_FALSE(parsed["score"].is<JsonVariant>());
}

TEST(MQTTPayloadBuilder, RxPacketFormatsRealSnrAndRssi) {
  JsonDocument scratch;
  char buffer[2048];
  int len = buildPacketWithMetrics(
      scratch, "rx", 7.25f, -93, NAN, buffer, sizeof(buffer));

  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_STREQ("7.2", parsed["SNR"].as<const char*>());
  EXPECT_STREQ("-93", parsed["RSSI"].as<const char*>());
  EXPECT_FALSE(parsed["score"].is<JsonVariant>());
}

TEST(MQTTPayloadBuilder, RxPacketOmitsOnlyUnknownMetric) {
  // SNR unknown, RSSI real: only the SNR key is omitted.
  JsonDocument scratch;
  char buffer[2048];
  int len = buildPacketWithMetrics(
      scratch, "rx", NAN, -93, NAN, buffer, sizeof(buffer));

  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_FALSE(parsed["SNR"].is<JsonVariant>());
  EXPECT_STREQ("-93", parsed["RSSI"].as<const char*>());

  // RSSI unknown, SNR real: only the RSSI key is omitted.
  len = buildPacketWithMetrics(
      scratch, "rx", 7.25f, MQTT_RSSI_UNKNOWN, NAN, buffer, sizeof(buffer));

  ASSERT_GT(len, 0);
  parsed.clear();
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_STREQ("7.2", parsed["SNR"].as<const char*>());
  EXPECT_FALSE(parsed["RSSI"].is<JsonVariant>());
}

TEST(MQTTPayloadBuilder, TxPacketOmitsSnrRssiAndScoreWithRealValues) {
  JsonDocument scratch;
  char buffer[2048];
  int len = buildPacketWithMetrics(
      scratch, "tx", 7.25f, -93, 0.5f, buffer, sizeof(buffer));

  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_FALSE(parsed["SNR"].is<JsonVariant>());
  EXPECT_FALSE(parsed["RSSI"].is<JsonVariant>());
  EXPECT_FALSE(parsed["score"].is<JsonVariant>());
}

TEST(MQTTPayloadBuilder, RawMessageHasExactContractAndEscapesData) {
  char buffer[512];
  JsonDocument doc;
  int len = MQTTPayloadBuilder::buildRawMessage(
      doc, "node \"A\"", "id\\1", kTimestamp, "AA\nBB", buffer, sizeof(buffer));

  ASSERT_GT(len, 0);
  EXPECT_EQ(static_cast<size_t>(len), strlen(buffer));
  EXPECT_STREQ(
      "{\"origin\":\"node \\\"A\\\"\",\"origin_id\":\"id\\\\1\","
      "\"timestamp\":\"2026-07-18T12:34:56.123456+00:00\","
      "\"type\":\"RAW\",\"data\":\"AA\\nBB\"}",
      buffer);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_STREQ("AA\nBB", parsed["data"].as<const char*>());
}

TEST(MQTTPayloadBuilder, ExactOutputSizeSucceedsAndOneByteShortFailsCleanly) {
  JsonDocument scratch;
  char reference[768];
  int reference_len = buildMinimalStatus(scratch, reference, sizeof(reference));
  ASSERT_GT(reference_len, 0);

  std::vector<char> exact(static_cast<size_t>(reference_len) + 1);
  EXPECT_EQ(reference_len, buildMinimalStatus(scratch, exact.data(), exact.size()));
  EXPECT_STREQ(reference, exact.data());

  std::vector<char> short_buffer(static_cast<size_t>(reference_len), 'x');
  EXPECT_EQ(0, buildMinimalStatus(scratch, short_buffer.data(), short_buffer.size()));
  EXPECT_EQ('\0', short_buffer[0]);
  EXPECT_EQ(0, buildMinimalStatus(scratch, short_buffer.data(), 1));
  EXPECT_EQ('\0', short_buffer[0]);
  EXPECT_EQ(0, buildMinimalStatus(scratch, nullptr, exact.size()));
}

TEST(MQTTPayloadBuilder, MaximumRepresentativePacketAndRawPayloadsRemainValid) {
  uint8_t path[64];
  for (size_t i = 0; i < sizeof(path); ++i) path[i] = static_cast<uint8_t>(i);
  std::string raw(510, 'A');

  JsonDocument scratch;
  char packet_buffer[2048];
  int packet_len = buildRepresentativePacket(
      scratch, "rx", 1.0f, path, 16, 4, raw.c_str(),
      packet_buffer, sizeof(packet_buffer));
  ASSERT_GT(packet_len, 0);
  JsonDocument packet;
  ASSERT_FALSE(deserializeJson(packet, packet_buffer));
  EXPECT_EQ(510U, strlen(packet["raw"].as<const char*>()));
  JsonArray parsed_path = packet["path"].as<JsonArray>();
  ASSERT_EQ(16U, parsed_path.size());
  EXPECT_STREQ("00010203", parsed_path[0].as<const char*>());
  EXPECT_STREQ("3c3d3e3f", parsed_path[15].as<const char*>());

  char raw_buffer[1024];
  JsonDocument raw_doc;
  int raw_len = MQTTPayloadBuilder::buildRawMessage(
      raw_doc, "node", "0123456789ABCDEF", kTimestamp, raw.c_str(),
      raw_buffer, sizeof(raw_buffer));
  ASSERT_GT(raw_len, 0);
  JsonDocument parsed_raw;
  ASSERT_FALSE(deserializeJson(parsed_raw, raw_buffer));
  EXPECT_EQ(510U, strlen(parsed_raw["data"].as<const char*>()));
}

TEST(MQTTPayloadBuilder, NeighborsMessageRoundTripsSelfAndEntries) {
  MQTTPayloadBuilder::NeighborsMessageEntry neighbors[] = {
      {"0011223344556677", 9.75f, 42, "DEN,APRS", "active"},
      {"8899AABBCCDDEEFF", -3.5f, 3600, "", "stale"},
  };

  JsonDocument scratch;
  char buffer[1024];
  int len = MQTTPayloadBuilder::buildNeighborsMessage(
      scratch, "DEN Repeater", "0123456789ABCDEF", kTimestamp, "DEN,APRS", "*",
      neighbors, 2, buffer, sizeof(buffer), 5, 2, true);

  ASSERT_GT(len, 0);
  EXPECT_EQ(static_cast<size_t>(len), strlen(buffer));
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_STREQ("2026-07-18T12:34:56.123456+00:00", parsed["timestamp"].as<const char*>());
  EXPECT_STREQ("DEN Repeater", parsed["origin"].as<const char*>());
  EXPECT_STREQ("0123456789ABCDEF", parsed["origin_id"].as<const char*>());
  EXPECT_STREQ("DEN,APRS", parsed["self"]["scopes"].as<const char*>());
  EXPECT_STREQ("*", parsed["self"]["default_scope"].as<const char*>());
  EXPECT_EQ(5, parsed["total_neighbors"].as<int>());
  EXPECT_EQ(2, parsed["queried_neighbors"].as<int>());
  EXPECT_TRUE(parsed["truncated"].as<bool>());

  JsonArray arr = parsed["neighbors"].as<JsonArray>();
  ASSERT_EQ(2U, arr.size());
  EXPECT_STREQ("0011223344556677", arr[0]["pubkey"].as<const char*>());
  EXPECT_FLOAT_EQ(9.75f, arr[0]["snr"].as<float>());
  EXPECT_EQ(42U, arr[0]["heard_secs_ago"].as<uint32_t>());
  EXPECT_STREQ("DEN,APRS", arr[0]["scopes"].as<const char*>());
  EXPECT_STREQ("active", arr[0]["status"].as<const char*>());
  EXPECT_STREQ("8899AABBCCDDEEFF", arr[1]["pubkey"].as<const char*>());
  EXPECT_STREQ("", arr[1]["scopes"].as<const char*>());
  EXPECT_STREQ("stale", arr[1]["status"].as<const char*>());
}

TEST(MQTTPayloadBuilder, NeighborsMessageMeasurementsMatchCompletePayload) {
  MQTTPayloadBuilder::NeighborsMessageEntry neighbors[] = {
      {"0011223344556677", 9.75f, UINT32_MAX, "DEN,APRS", "responded"},
      {"8899AABBCCDDEEFF", -3.5f, UINT32_MAX, "", "timeout"},
  };

  size_t measured =
      MQTTPayloadBuilder::measureNeighborsMessageBase(
          "DEN Repeater", "0123456789ABCDEF", kTimestamp, "DEN,APRS", "*", 2)
      + MQTTPayloadBuilder::measureNeighborsMessageEntry(neighbors[0])
      + 1  // array comma
      + MQTTPayloadBuilder::measureNeighborsMessageEntry(neighbors[1]);

  JsonDocument scratch;
  char buffer[1024];
  int len = MQTTPayloadBuilder::buildNeighborsMessage(
      scratch, "DEN Repeater", "0123456789ABCDEF", kTimestamp, "DEN,APRS", "*",
      neighbors, 2, buffer, sizeof(buffer), 2, 2, false);

  ASSERT_GT(len, 0);
  EXPECT_EQ(measured, static_cast<size_t>(len));
}

TEST(MQTTPayloadBuilder, NeighborsMessageMeasuredPrefixStopsBeforeOverflow) {
  MQTTPayloadBuilder::NeighborsMessageEntry neighbors[20];
  static char keys[20][17];
  static char long_scopes[96];
  memset(long_scopes, 'S', sizeof(long_scopes) - 1);
  long_scopes[sizeof(long_scopes) - 1] = '\0';
  for (int i = 0; i < 20; i++) {
    snprintf(keys[i], sizeof(keys[i]), "%016X", i);
    neighbors[i] = {
        keys[i], static_cast<float>(i), UINT32_MAX,
        long_scopes, "responded"};
  }

  constexpr size_t kBufferSize = 512;
  size_t used = MQTTPayloadBuilder::measureNeighborsMessageBase(
      "node", "id", kTimestamp, "DEN", "*", 20);
  int published = 0;
  int queried = 0;
  while (queried < 20) {
    size_t added =
        MQTTPayloadBuilder::measureNeighborsMessageEntry(neighbors[queried])
        + (published > 0 ? 1U : 0U);
    queried++;
    if (used + added >= kBufferSize) break;
    used += added;
    published++;
  }

  ASSERT_GT(published, 0);
  ASSERT_LT(published, 20);
  ASSERT_EQ(published + 1, queried);  // only the non-fitting candidate was extra

  JsonDocument scratch;
  char buffer[kBufferSize];
  int len = MQTTPayloadBuilder::buildNeighborsMessage(
      scratch, "node", "id", kTimestamp, "DEN", "*",
      neighbors, published, buffer, sizeof(buffer),
      20, queried, true);

  ASSERT_GT(len, 0);
  EXPECT_LT(static_cast<size_t>(len), sizeof(buffer));
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_EQ(static_cast<size_t>(published),
            parsed["neighbors"].as<JsonArray>().size());
  EXPECT_EQ(20, parsed["total_neighbors"].as<int>());
  EXPECT_EQ(queried, parsed["queried_neighbors"].as<int>());
  EXPECT_TRUE(parsed["truncated"].as<bool>());
}

TEST(MQTTPayloadBuilder, NeighborsMessageFallbackMarksTruncated) {
  MQTTPayloadBuilder::NeighborsMessageEntry neighbors[] = {
      {"0011223344556677", 9.75f, 1, "DEN", "responded"},
      {"8899AABBCCDDEEFF", -3.5f, 2, "APRS", "responded"},
  };
  size_t first_only_size =
      MQTTPayloadBuilder::measureNeighborsMessageBase(
          "node", "id", kTimestamp, "DEN", "*", 2)
      + MQTTPayloadBuilder::measureNeighborsMessageEntry(neighbors[0]);
  std::vector<char> buffer(first_only_size + 1);

  JsonDocument scratch;
  int len = MQTTPayloadBuilder::buildNeighborsMessage(
      scratch, "node", "id", kTimestamp, "DEN", "*",
      neighbors, 2, buffer.data(), buffer.size(),
      2, 2, false);

  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer.data()));
  EXPECT_EQ(1U, parsed["neighbors"].as<JsonArray>().size());
  EXPECT_TRUE(parsed["truncated"].as<bool>());
}

TEST(MQTTPayloadBuilder, NeighborsMessageFailsCleanlyOnAllocationFailure) {
  MQTTPayloadBuilder::NeighborsMessageEntry neighbor = {
      "0011223344556677", 9.75f, 1, "DEN", "responded"};
  RejectAllJsonAllocations allocator;
  JsonDocument scratch(&allocator);
  char buffer[512];

  EXPECT_EQ(0, MQTTPayloadBuilder::buildNeighborsMessage(
      scratch, "node", "id", kTimestamp, "DEN", "*",
      &neighbor, 1, buffer, sizeof(buffer),
      1, 1, false));
}

TEST(MQTTPayloadBuilder, NeighborsMessageHandlesEmptyTableAndNullScopes) {
  JsonDocument scratch;
  char buffer[256];
  int len = MQTTPayloadBuilder::buildNeighborsMessage(
      scratch, "node", "id", kTimestamp, nullptr, nullptr, nullptr, 0,
      buffer, sizeof(buffer));

  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  EXPECT_STREQ("", parsed["self"]["scopes"].as<const char*>());
  EXPECT_STREQ("", parsed["self"]["default_scope"].as<const char*>());
  JsonArray arr = parsed["neighbors"].as<JsonArray>();
  ASSERT_TRUE(arr.isNull() == false);
  EXPECT_EQ(0U, arr.size());
}

TEST(MQTTPayloadBuilder, NeighborsMessageDropsTailWhenBufferFills) {
  // Twenty entries far exceed a tight buffer; the builder must emit a prefix
  // that still parses as complete JSON rather than truncating mid-document.
  MQTTPayloadBuilder::NeighborsMessageEntry neighbors[20];
  static char keys[20][17];
  for (int i = 0; i < 20; i++) {
    snprintf(keys[i], sizeof(keys[i]), "%016X", i);
    neighbors[i].pubkey_hex = keys[i];
    neighbors[i].snr = static_cast<float>(i);
    neighbors[i].heard_secs_ago = static_cast<uint32_t>(i) * 10U;
    neighbors[i].heard_unknown = false;
    neighbors[i].scopes = "DEN";
    neighbors[i].status = "active";
  }

  JsonDocument scratch;
  char buffer[512];
  int len = MQTTPayloadBuilder::buildNeighborsMessage(
      scratch, "node", "id", kTimestamp, "DEN", "*", neighbors, 20,
      buffer, sizeof(buffer));

  ASSERT_GT(len, 0);
  EXPECT_LT(static_cast<size_t>(len), sizeof(buffer));
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  JsonArray arr = parsed["neighbors"].as<JsonArray>();
  ASSERT_FALSE(arr.isNull());
  EXPECT_GT(arr.size(), 0U);
  EXPECT_LT(arr.size(), 20U);
  EXPECT_FALSE(parsed["truncated"].is<JsonVariant>());
  // Kept entries are the head of the input, in order.
  EXPECT_STREQ(keys[0], arr[0]["pubkey"].as<const char*>());
}

TEST(MQTTPayloadBuilder, NeighborsMessageRendersUnknownHeardAgeAsNull) {
  // A neighbour stamped before the clock was set has no usable age. The key
  // stays present and null so a consumer cannot read it as "heard just now".
  MQTTPayloadBuilder::NeighborsMessageEntry neighbors[2];
  neighbors[0] = {"0011223344556677", 9.75f, 42, "DEN", "responded"};
  neighbors[1] = {"8899AABBCCDDEEFF", -3.5f, 0, "DEN", "responded"};
  neighbors[1].heard_unknown = true;

  JsonDocument scratch;
  char buffer[1024];
  int len = MQTTPayloadBuilder::buildNeighborsMessage(
      scratch, "node", "id", kTimestamp, "DEN", "*", neighbors, 2,
      buffer, sizeof(buffer), 2, 2, false);

  ASSERT_GT(len, 0);
  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, buffer));
  JsonArray arr = parsed["neighbors"].as<JsonArray>();
  ASSERT_EQ(2U, arr.size());
  EXPECT_EQ(42U, arr[0]["heard_secs_ago"].as<uint32_t>());
  EXPECT_FALSE(arr[0]["heard_secs_ago"].isNull());
  EXPECT_TRUE(arr[1]["heard_secs_ago"].isNull());
  EXPECT_NE(nullptr, strstr(buffer, "\"heard_secs_ago\":null"));
  // The rest of the entry still renders.
  EXPECT_STREQ("8899AABBCCDDEEFF", arr[1]["pubkey"].as<const char*>());
  EXPECT_STREQ("responded", arr[1]["status"].as<const char*>());
}

TEST(MQTTPayloadBuilder, NeighborsMessageUnknownHeardAgeFitsMeasuredWidth) {
  // Paced discovery measures each entry with a known UINT32_MAX age; a null age
  // must never serialize wider than what that reserved.
  MQTTPayloadBuilder::NeighborsMessageEntry measured = {
      "0011223344556677", 9.75f, UINT32_MAX, "DEN", "responded"};
  MQTTPayloadBuilder::NeighborsMessageEntry unknown = measured;
  unknown.heard_unknown = true;
  unknown.heard_secs_ago = 0;

  EXPECT_LE(MQTTPayloadBuilder::measureNeighborsMessageEntry(unknown),
            MQTTPayloadBuilder::measureNeighborsMessageEntry(measured));
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
