#include <gtest/gtest.h>

#include <vector>

#include "helpers/MapUploadPolicy.h"

namespace Policy = MapUploadPolicy;

namespace {

// Build an over-the-air advert frame the way Dispatcher::tryParsePacket() reads
// one back: header | [transport codes] | path_len | path | payload.
struct FrameSpec {
  uint8_t route = ROUTE_TYPE_FLOOD;
  uint8_t payload_type = PAYLOAD_TYPE_ADVERT;
  uint8_t payload_ver = PAYLOAD_VER_1;
  uint8_t path_hops = 0;
  uint8_t path_mode = 0;  // hash size - 1
  uint32_t timestamp = 1000000;
  uint8_t adv_type = Policy::kAdvTypeRepeater;
  uint8_t key_seed = 0xA0;
  bool include_app_data = true;
  int app_data_extra = 4;  // bytes after the flags byte
};

std::vector<uint8_t> makeFrame(const FrameSpec& spec) {
  std::vector<uint8_t> f;
  f.push_back(static_cast<uint8_t>((spec.payload_ver << PH_VER_SHIFT) |
                                   (spec.payload_type << PH_TYPE_SHIFT) | spec.route));
  if (spec.route == ROUTE_TYPE_TRANSPORT_FLOOD || spec.route == ROUTE_TYPE_TRANSPORT_DIRECT) {
    for (int i = 0; i < 4; i++) f.push_back(static_cast<uint8_t>(0x10 + i));
  }
  f.push_back(static_cast<uint8_t>((spec.path_mode << 6) | (spec.path_hops & 63)));
  for (int i = 0; i < spec.path_hops * (spec.path_mode + 1); i++) {
    f.push_back(static_cast<uint8_t>(0x70 + i));
  }
  for (int i = 0; i < PUB_KEY_SIZE; i++) f.push_back(static_cast<uint8_t>(spec.key_seed + i));
  f.push_back(static_cast<uint8_t>(spec.timestamp & 0xFF));
  f.push_back(static_cast<uint8_t>((spec.timestamp >> 8) & 0xFF));
  f.push_back(static_cast<uint8_t>((spec.timestamp >> 16) & 0xFF));
  f.push_back(static_cast<uint8_t>((spec.timestamp >> 24) & 0xFF));
  for (int i = 0; i < SIGNATURE_SIZE; i++) f.push_back(static_cast<uint8_t>(0xC0 + (i & 0x0F)));
  if (spec.include_app_data) {
    f.push_back(static_cast<uint8_t>(0x80 | spec.adv_type));  // name flag + type nibble
    for (int i = 0; i < spec.app_data_extra; i++) f.push_back(static_cast<uint8_t>('a' + i));
  }
  return f;
}

std::vector<uint8_t> pubKeyOf(uint8_t seed) {
  std::vector<uint8_t> k;
  for (int i = 0; i < PUB_KEY_SIZE; i++) k.push_back(static_cast<uint8_t>(seed + i));
  return k;
}

}  // namespace

// ---- parseAdvertFrame -----------------------------------------------------

TEST(MapUploadPolicyParse, DecodesAPlainFloodAdvert) {
  const FrameSpec spec;
  const auto frame = makeFrame(spec);

  Policy::AdvertView view{};
  ASSERT_TRUE(Policy::parseAdvertFrame(frame.data(), frame.size(), &view));
  EXPECT_EQ(Policy::kAdvTypeRepeater, view.type);
  EXPECT_EQ(1000000u, view.timestamp);
  EXPECT_EQ(0, memcmp(view.pub_key, pubKeyOf(0xA0).data(), PUB_KEY_SIZE));
  EXPECT_EQ(5u, view.app_data_len);  // flags byte + 4 extra
}

TEST(MapUploadPolicyParse, SkipsTransportCodesWhenTheRouteCarriesThem) {
  for (uint8_t route : {ROUTE_TYPE_TRANSPORT_FLOOD, ROUTE_TYPE_TRANSPORT_DIRECT}) {
    FrameSpec spec;
    spec.route = route;
    spec.timestamp = 777;
    const auto frame = makeFrame(spec);

    Policy::AdvertView view{};
    ASSERT_TRUE(Policy::parseAdvertFrame(frame.data(), frame.size(), &view)) << (int)route;
    EXPECT_EQ(777u, view.timestamp) << (int)route;
    EXPECT_EQ(Policy::kAdvTypeRepeater, view.type) << (int)route;
  }
}

TEST(MapUploadPolicyParse, SkipsThePathAtEveryHashSize) {
  for (uint8_t mode = 0; mode <= 2; mode++) {
    FrameSpec spec;
    spec.path_mode = mode;
    spec.path_hops = 3;
    spec.timestamp = 4242;
    const auto frame = makeFrame(spec);

    Policy::AdvertView view{};
    ASSERT_TRUE(Policy::parseAdvertFrame(frame.data(), frame.size(), &view)) << (int)mode;
    EXPECT_EQ(4242u, view.timestamp) << (int)mode;
  }
}

TEST(MapUploadPolicyParse, RejectsReservedPathMode) {
  FrameSpec spec;
  spec.path_mode = 3;
  const auto frame = makeFrame(spec);

  Policy::AdvertView view{};
  EXPECT_FALSE(Policy::parseAdvertFrame(frame.data(), frame.size(), &view));
}

TEST(MapUploadPolicyParse, RejectsNonAdvertPayloadTypes) {
  for (uint8_t type : {PAYLOAD_TYPE_TXT_MSG, PAYLOAD_TYPE_ACK, PAYLOAD_TYPE_TRACE}) {
    FrameSpec spec;
    spec.payload_type = type;
    const auto frame = makeFrame(spec);

    Policy::AdvertView view{};
    EXPECT_FALSE(Policy::parseAdvertFrame(frame.data(), frame.size(), &view)) << (int)type;
  }
}

TEST(MapUploadPolicyParse, RejectsUnsupportedPayloadVersion) {
  FrameSpec spec;
  spec.payload_ver = PAYLOAD_VER_2;
  const auto frame = makeFrame(spec);

  Policy::AdvertView view{};
  EXPECT_FALSE(Policy::parseAdvertFrame(frame.data(), frame.size(), &view));
}

TEST(MapUploadPolicyParse, RejectsFramesTruncatedAnywhere) {
  const auto frame = makeFrame(FrameSpec{});
  // Every prefix short of the full fixed advert header must be refused rather
  // than read past the end.
  for (size_t len = 0; len < 2 + Policy::kAdvertHeaderLen; len++) {
    Policy::AdvertView view{};
    EXPECT_FALSE(Policy::parseAdvertFrame(frame.data(), len, &view)) << "len=" << len;
  }
}

TEST(MapUploadPolicyParse, RejectsPathLongerThanTheFrame) {
  FrameSpec spec;
  spec.path_hops = 60;
  auto frame = makeFrame(spec);
  frame.resize(10);  // claims 60 hops but the bytes are not there

  Policy::AdvertView view{};
  EXPECT_FALSE(Policy::parseAdvertFrame(frame.data(), frame.size(), &view));
}

TEST(MapUploadPolicyParse, AdvertWithNoAppDataHasNoType) {
  FrameSpec spec;
  spec.include_app_data = false;
  const auto frame = makeFrame(spec);

  Policy::AdvertView view{};
  ASSERT_TRUE(Policy::parseAdvertFrame(frame.data(), frame.size(), &view));
  EXPECT_EQ(Policy::kAdvTypeNone, view.type);
  EXPECT_EQ(0u, view.app_data_len);
  EXPECT_EQ(nullptr, view.app_data);
}

TEST(MapUploadPolicyParse, ClampsOversizedAppData) {
  FrameSpec spec;
  spec.app_data_extra = MAX_ADVERT_DATA_SIZE + 40;
  const auto frame = makeFrame(spec);

  Policy::AdvertView view{};
  ASSERT_TRUE(Policy::parseAdvertFrame(frame.data(), frame.size(), &view));
  EXPECT_EQ(static_cast<size_t>(MAX_ADVERT_DATA_SIZE), view.app_data_len);
}

TEST(MapUploadPolicyParse, RejectsNullArguments) {
  const auto frame = makeFrame(FrameSpec{});
  Policy::AdvertView view{};
  EXPECT_FALSE(Policy::parseAdvertFrame(nullptr, frame.size(), &view));
  EXPECT_FALSE(Policy::parseAdvertFrame(frame.data(), frame.size(), nullptr));
}

// ---- signed advert message ------------------------------------------------

TEST(MapUploadPolicySignedMessage, MatchesWhatTheAdvertiserSigned) {
  const auto frame = makeFrame(FrameSpec{});
  Policy::AdvertView view{};
  ASSERT_TRUE(Policy::parseAdvertFrame(frame.data(), frame.size(), &view));

  uint8_t msg[PUB_KEY_SIZE + 4 + MAX_ADVERT_DATA_SIZE];
  const size_t n = Policy::buildSignedAdvertMessage(view, msg, sizeof(msg));
  ASSERT_EQ(PUB_KEY_SIZE + 4u + view.app_data_len, n);

  EXPECT_EQ(0, memcmp(msg, view.pub_key, PUB_KEY_SIZE));
  // Little-endian timestamp, exactly as it appears on the wire.
  EXPECT_EQ(0, memcmp(msg + PUB_KEY_SIZE, frame.data() + 2 + PUB_KEY_SIZE, 4));
  EXPECT_EQ(0, memcmp(msg + PUB_KEY_SIZE + 4, view.app_data, view.app_data_len));
}

TEST(MapUploadPolicySignedMessage, RefusesAnUndersizedBuffer) {
  const auto frame = makeFrame(FrameSpec{});
  Policy::AdvertView view{};
  ASSERT_TRUE(Policy::parseAdvertFrame(frame.data(), frame.size(), &view));

  uint8_t msg[PUB_KEY_SIZE];
  EXPECT_EQ(0u, Policy::buildSignedAdvertMessage(view, msg, sizeof(msg)));
  EXPECT_EQ(0u, Policy::buildSignedAdvertMessage(view, nullptr, 999));
}

// ---- decideForAdvert ------------------------------------------------------

TEST(MapUploadPolicyDecide, UploadsMappableTypesNeverSeenBefore) {
  for (uint8_t type : {Policy::kAdvTypeRepeater, Policy::kAdvTypeRoom, Policy::kAdvTypeSensor}) {
    EXPECT_EQ(Policy::Verdict::Upload, Policy::decideForAdvert(type, 5000, false, 0)) << (int)type;
  }
}

TEST(MapUploadPolicyDecide, SkipsChatAndTypelessAdverts) {
  EXPECT_EQ(Policy::Verdict::NotMappable,
            Policy::decideForAdvert(Policy::kAdvTypeChat, 5000, false, 0));
  EXPECT_EQ(Policy::Verdict::NotMappable,
            Policy::decideForAdvert(Policy::kAdvTypeNone, 5000, false, 0));
  // A future type nibble is not plotted until this policy learns about it.
  EXPECT_EQ(Policy::Verdict::NotMappable, Policy::decideForAdvert(9, 5000, false, 0));
}

TEST(MapUploadPolicyDecide, TreatsANonAdvancingTimestampAsReplay) {
  EXPECT_EQ(Policy::Verdict::Replay,
            Policy::decideForAdvert(Policy::kAdvTypeRepeater, 5000, true, 5000));
  EXPECT_EQ(Policy::Verdict::Replay,
            Policy::decideForAdvert(Policy::kAdvTypeRepeater, 4999, true, 5000));
}

TEST(MapUploadPolicyDecide, HoldsOffUntilAFullHourHasPassed) {
  const uint32_t last = 5000;
  EXPECT_EQ(Policy::Verdict::TooSoon,
            Policy::decideForAdvert(Policy::kAdvTypeRepeater, last + 1, true, last));
  EXPECT_EQ(Policy::Verdict::TooSoon,
            Policy::decideForAdvert(Policy::kAdvTypeRepeater,
                                    last + Policy::kMinReuploadSecs - 1, true, last));
  EXPECT_EQ(Policy::Verdict::Upload,
            Policy::decideForAdvert(Policy::kAdvTypeRepeater,
                                    last + Policy::kMinReuploadSecs, true, last));
}

// ---- pacing ---------------------------------------------------------------

TEST(MapUploadPolicyPacing, FirstUploadIsNotDelayed) {
  EXPECT_TRUE(Policy::uploadGapElapsed(0, 0, false));
  EXPECT_TRUE(Policy::uploadGapElapsed(1234, 0, false));
}

TEST(MapUploadPolicyPacing, EnforcesTheGapBetweenUploads) {
  const uint32_t last = 100000;
  EXPECT_FALSE(Policy::uploadGapElapsed(last, last, true));
  EXPECT_FALSE(Policy::uploadGapElapsed(last + Policy::kMinUploadGapMs - 1, last, true));
  EXPECT_TRUE(Policy::uploadGapElapsed(last + Policy::kMinUploadGapMs, last, true));
}

TEST(MapUploadPolicyPacing, SurvivesTheMillisRollover) {
  const uint32_t last = 0xFFFFFF00u;  // 256 ms before wrap
  EXPECT_FALSE(Policy::uploadGapElapsed(last + 100, last, true));
  // Wrapped past the gap.
  EXPECT_TRUE(Policy::uploadGapElapsed(
      static_cast<uint32_t>(last + Policy::kMinUploadGapMs), last, true));
}

TEST(MapUploadPolicyPacing, ExpiresAStaleStagedAdvert) {
  const uint32_t staged = 50000;
  EXPECT_FALSE(Policy::pendingExpired(staged, staged));
  EXPECT_FALSE(Policy::pendingExpired(staged + Policy::kMaxPendingAgeMs - 1, staged));
  EXPECT_TRUE(Policy::pendingExpired(staged + Policy::kMaxPendingAgeMs, staged));
}

// ---- SeenTable ------------------------------------------------------------

TEST(MapUploadPolicySeenTable, StartsEmpty) {
  Policy::SeenTable<4> table;
  EXPECT_EQ(0u, table.size());
  EXPECT_EQ(4u, table.capacity());
  EXPECT_EQ(nullptr, table.find(pubKeyOf(0x10).data()));
  EXPECT_EQ(nullptr, table.find(nullptr));
}

TEST(MapUploadPolicySeenTable, RecordsAndFindsByKeyPrefix) {
  Policy::SeenTable<4> table;
  const auto key = pubKeyOf(0x10);
  table.record(key.data(), 9000);

  ASSERT_NE(nullptr, table.find(key.data()));
  EXPECT_EQ(9000u, *table.find(key.data()));
  EXPECT_EQ(1u, table.size());
}

TEST(MapUploadPolicySeenTable, UpdatingAKeyDoesNotGrowTheTable) {
  Policy::SeenTable<4> table;
  const auto key = pubKeyOf(0x10);
  table.record(key.data(), 9000);
  table.record(key.data(), 20000);

  EXPECT_EQ(1u, table.size());
  EXPECT_EQ(20000u, *table.find(key.data()));
}

TEST(MapUploadPolicySeenTable, EvictsTheLeastRecentlyRecordedWhenFull) {
  Policy::SeenTable<3> table;
  const auto a = pubKeyOf(0x10), b = pubKeyOf(0x30), c = pubKeyOf(0x50), d = pubKeyOf(0x70);
  table.record(a.data(), 1);
  table.record(b.data(), 2);
  table.record(c.data(), 3);
  // Refresh a, so b becomes the least recently recorded.
  table.record(a.data(), 4);
  table.record(d.data(), 5);

  EXPECT_EQ(3u, table.size());
  EXPECT_EQ(nullptr, table.find(b.data()));
  ASSERT_NE(nullptr, table.find(a.data()));
  EXPECT_EQ(4u, *table.find(a.data()));
  ASSERT_NE(nullptr, table.find(d.data()));
  EXPECT_EQ(5u, *table.find(d.data()));
}

TEST(MapUploadPolicySeenTable, LookupsDoNotKeepAnEntryAlive) {
  // A node whose adverts are all rejected must not be able to hold its slot
  // against a node we are actually uploading.
  Policy::SeenTable<2> table;
  const auto a = pubKeyOf(0x10), b = pubKeyOf(0x30), c = pubKeyOf(0x50);
  table.record(a.data(), 1);
  table.record(b.data(), 2);
  for (int i = 0; i < 10; i++) (void)table.find(a.data());
  table.record(c.data(), 3);

  EXPECT_EQ(nullptr, table.find(a.data()));  // still the oldest *record*
  EXPECT_NE(nullptr, table.find(b.data()));
  EXPECT_NE(nullptr, table.find(c.data()));
}

TEST(MapUploadPolicySeenTable, KeysDifferingOnlyAfterThePrefixCollide) {
  // Documented consequence of prefix keying: worth one suppressed upload.
  Policy::SeenTable<4> table;
  auto a = pubKeyOf(0x10);
  auto b = a;
  b[Policy::kSeenKeyBytes] ^= 0xFF;  // differs only past the prefix

  table.record(a.data(), 1234);
  ASSERT_NE(nullptr, table.find(b.data()));
  EXPECT_EQ(1u, table.size());
}

TEST(MapUploadPolicySeenTable, ClearForgetsEverything) {
  Policy::SeenTable<4> table;
  const auto key = pubKeyOf(0x10);
  table.record(key.data(), 1234);
  table.clear();

  EXPECT_EQ(0u, table.size());
  EXPECT_EQ(nullptr, table.find(key.data()));
}

// ---- table + decision together -------------------------------------------

TEST(MapUploadPolicyIntegration, RepeaterUploadsThenHoldsOffForAnHour) {
  Policy::SeenTable<8> table;
  const auto key = pubKeyOf(0xA0);

  const uint32_t first_ts = 1000000;
  const uint32_t* last = table.find(key.data());
  ASSERT_EQ(Policy::Verdict::Upload,
            Policy::decideForAdvert(Policy::kAdvTypeRepeater, first_ts, last != nullptr,
                                    last ? *last : 0));
  table.record(key.data(), first_ts);

  // A re-advert 10 minutes later is held.
  last = table.find(key.data());
  EXPECT_EQ(Policy::Verdict::TooSoon,
            Policy::decideForAdvert(Policy::kAdvTypeRepeater, first_ts + 600, last != nullptr,
                                    last ? *last : 0));

  // An hour on, it goes.
  last = table.find(key.data());
  EXPECT_EQ(Policy::Verdict::Upload,
            Policy::decideForAdvert(Policy::kAdvTypeRepeater,
                                    first_ts + Policy::kMinReuploadSecs, last != nullptr,
                                    last ? *last : 0));
}

TEST(MapUploadPolicyIntegration, ReplayedFrameIsRejectedAfterTheFirstUpload) {
  Policy::SeenTable<8> table;
  const auto frame = makeFrame(FrameSpec{});
  Policy::AdvertView view{};
  ASSERT_TRUE(Policy::parseAdvertFrame(frame.data(), frame.size(), &view));

  table.record(view.pub_key, view.timestamp);

  const uint32_t* last = table.find(view.pub_key);
  ASSERT_NE(nullptr, last);
  EXPECT_EQ(Policy::Verdict::Replay,
            Policy::decideForAdvert(view.type, view.timestamp, true, *last));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
