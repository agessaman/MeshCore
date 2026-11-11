#include <gtest/gtest.h>
#include "helpers/OtaCheckPolicy.h"

namespace OTA = OtaCheckPolicy;

TEST(OtaCheckPolicy, FirstReadStartsAndRepeatedReadsDoNotDuplicate) {
  OTA::State state;
  auto first = OTA::read(state, 0, 0);
  EXPECT_EQ(OTA::Reply::Started, first.reply);
  EXPECT_TRUE(first.start);
  EXPECT_EQ(OTA::Activity::Check, state.activity);
  auto next = OTA::read(state, 1000, 0);
  EXPECT_EQ(OTA::Reply::Running, next.reply);
  EXPECT_FALSE(next.start);
  EXPECT_FALSE(OTA::beginUpdate(state));
}

TEST(OtaCheckPolicy, CompletedCheckIsCachedEvenAtZeroMillis) {
  OTA::State state;
  OTA::read(state, 0, 1);
  OTA::complete(state, 0, 1);
  auto result = OTA::read(state, 1234, 1);
  EXPECT_EQ(OTA::Reply::Cached, result.reply);
  EXPECT_FALSE(result.start);
  EXPECT_EQ(1234u, result.age_ms);
  EXPECT_EQ(OTA::Activity::Idle, state.activity);
}

TEST(OtaCheckPolicy, RefreshStartsAtSixtySecondsAndKeepsCachedResult) {
  OTA::State state;
  OTA::complete(state, 123, 0);
  EXPECT_FALSE(OTA::read(state, 60122, 0).start);
  auto stale = OTA::read(state, 60123, 0);
  EXPECT_EQ(OTA::Reply::Cached, stale.reply);
  EXPECT_TRUE(stale.start);
  auto repeat = OTA::read(state, 60124, 0);
  EXPECT_EQ(OTA::Reply::Cached, repeat.reply);
  EXPECT_FALSE(repeat.start);
  OTA::complete(state, 60125, 0);
  EXPECT_EQ(1u, OTA::read(state, 60126, 0).age_ms);
}

TEST(OtaCheckPolicy, AgeAndRefreshHandleMillisWrap) {
  OTA::State state;
  OTA::complete(state, UINT32_MAX - 999, 0);
  EXPECT_EQ(1000u, OTA::read(state, 0, 0).age_ms);
  EXPECT_FALSE(OTA::read(state, 58999, 0).start);
  EXPECT_TRUE(OTA::read(state, 59000, 0).start);
}

TEST(OtaCheckPolicy, ChannelChangeNeverReturnsAnotherChannelsResult) {
  OTA::State state;
  OTA::complete(state, 100, 0);
  auto changed = OTA::read(state, 101, 1);
  EXPECT_EQ(OTA::Reply::Started, changed.reply);
  EXPECT_TRUE(changed.start);
  EXPECT_EQ(OTA::Reply::Running, OTA::read(state, 102, 1).reply);
  EXPECT_FALSE(OTA::read(state, 103, 2).start);
  OTA::complete(state, 104, 1);
  EXPECT_EQ(OTA::Reply::Cached, OTA::read(state, 105, 1).reply);
  EXPECT_EQ(OTA::Reply::Started, OTA::read(state, 106, 2).reply);
}

TEST(OtaCheckPolicy, UpdateExcludesChecksAndOtherUpdates) {
  OTA::State state;
  EXPECT_TRUE(OTA::beginUpdate(state));
  EXPECT_FALSE(OTA::beginUpdate(state));
  auto busy = OTA::read(state, 100, 0);
  EXPECT_EQ(OTA::Reply::Busy, busy.reply);
  EXPECT_FALSE(busy.start);
  OTA::endUpdate(state);
  EXPECT_TRUE(OTA::read(state, 101, 0).start);
}

TEST(OtaCheckPolicy, UpdateCanReadCacheButCannotRefreshIt) {
  OTA::State state;
  OTA::complete(state, 0, 0);
  EXPECT_TRUE(OTA::beginUpdate(state));
  auto read = OTA::read(state, 60000, 0);
  EXPECT_EQ(OTA::Reply::Cached, read.reply);
  EXPECT_FALSE(read.start);
  OTA::endUpdate(state);
  EXPECT_TRUE(OTA::read(state, 60001, 0).start);
}

TEST(OtaCheckPolicy, ErrorsAndSpawnFailuresUseTheSameCacheAndPermitUpdates) {
  OTA::State state;
  OTA::read(state, 100, 0);
  OTA::complete(state, 101, 0);
  EXPECT_EQ(OTA::Reply::Cached, OTA::read(state, 102, 0).reply);
  EXPECT_TRUE(OTA::beginUpdate(state));
  OTA::endUpdate(state);
  EXPECT_TRUE(OTA::read(state, 60101, 0).start);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
