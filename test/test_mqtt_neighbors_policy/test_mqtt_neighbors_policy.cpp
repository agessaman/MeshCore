// mqtt.neighbors auto/on/off and the per-preset opt-in that drives auto.

#define WITH_MQTT_BRIDGE 1  // preset table and prefs layout are behind this flag
#define PROGMEM             // host build: CA strings are PROGMEM-qualified
#include <gtest/gtest.h>
#include <cstring>
#include "helpers/MQTTNeighborsPolicy.h"

namespace {

MQTTPrefs prefsWith(uint8_t mode, const char* slot1, const char* slot2 = "none") {
  MQTTPrefs prefs;
  memset(&prefs, 0, sizeof(prefs));
  for (int i = 0; i < MQTT_PREFS_SLOT_COUNT; i++) strcpy(prefs.mqtt_slot_preset[i], "none");
  strcpy(prefs.mqtt_slot_preset[0], slot1);
  strcpy(prefs.mqtt_slot_preset[1], slot2);
  prefs.mqtt_neighbors_enabled = mode;
  return prefs;
}

}  // namespace

TEST(MQTTNeighborsPolicy, ZeroIsAutoSoUntouchedNodesUpgradeToAuto) {
  EXPECT_EQ(0, MQTT_NEIGHBORS_AUTO);
  EXPECT_EQ(1, MQTT_NEIGHBORS_ON);   // the old "on" keeps its meaning
  EXPECT_EQ(2, MQTT_NEIGHBORS_OFF);  // out of range for older firmware, which repairs it to 0 (off)
}

TEST(MQTTNeighborsPolicy, ParsesAndNamesEveryMode) {
  uint8_t mode = 0xff;
  ASSERT_TRUE(mqttNeighborsParseMode("auto", &mode));
  EXPECT_EQ(MQTT_NEIGHBORS_AUTO, mode);
  ASSERT_TRUE(mqttNeighborsParseMode("on", &mode));
  EXPECT_EQ(MQTT_NEIGHBORS_ON, mode);
  ASSERT_TRUE(mqttNeighborsParseMode("off\r\n", &mode));
  EXPECT_EQ(MQTT_NEIGHBORS_OFF, mode);
  EXPECT_STREQ("auto", mqttNeighborsModeName(MQTT_NEIGHBORS_AUTO));
  EXPECT_STREQ("on", mqttNeighborsModeName(MQTT_NEIGHBORS_ON));
  EXPECT_STREQ("off", mqttNeighborsModeName(MQTT_NEIGHBORS_OFF));
}

TEST(MQTTNeighborsPolicy, RejectsUnknownModesWithoutTouchingTheOutput) {
  uint8_t mode = MQTT_NEIGHBORS_ON;
  EXPECT_FALSE(mqttNeighborsParseMode("onn", &mode));
  EXPECT_FALSE(mqttNeighborsParseMode("o", &mode));
  EXPECT_FALSE(mqttNeighborsParseMode("", &mode));
  EXPECT_FALSE(mqttNeighborsParseMode("AUTO", &mode));
  EXPECT_FALSE(mqttNeighborsParseMode(nullptr, &mode));
  EXPECT_EQ(MQTT_NEIGHBORS_ON, mode);
}

TEST(MQTTNeighborsPolicy, OptInPresets) {
  for (const char* name : {"cascadiamesh", "chimesh", "comchan", "meshmapper"}) {
    ASSERT_NE(nullptr, findMQTTPreset(name)) << name;  // a renamed preset would silently drop out
    EXPECT_TRUE(mqttPresetWantsNeighbors(findMQTTPreset(name))) << name;
  }
  for (const char* name : {"analyzer-us", "analyzer-eu", "waev", "meshrank"}) {
    EXPECT_FALSE(mqttPresetWantsNeighbors(findMQTTPreset(name))) << name;
  }
  EXPECT_FALSE(mqttPresetWantsNeighbors(nullptr));  // custom slot
  EXPECT_FALSE(mqttPresetNameWantsNeighbors("custom"));
  EXPECT_FALSE(mqttPresetNameWantsNeighbors("none"));
  EXPECT_FALSE(mqttPresetNameWantsNeighbors(nullptr));
}

TEST(MQTTNeighborsPolicy, OptInCountMatchesTheTable) {
  int opted_in = 0;
  for (int i = 0; i < MQTT_PRESET_COUNT; i++) {
    if (mqttPresetWantsNeighbors(&MQTT_PRESETS[i])) opted_in++;
  }
  EXPECT_EQ(4, opted_in);  // every name in the opt-in list resolves to exactly one preset
}

TEST(MQTTNeighborsPolicy, AutoCollectsOnlyWithAnOptedInSlot) {
  MQTTPrefs plain = prefsWith(MQTT_NEIGHBORS_AUTO, "analyzer-us", "analyzer-eu");
  EXPECT_FALSE(mqttNeighborsCollectionEnabled(&plain, RUNTIME_MQTT_SLOTS));

  MQTTPrefs with_comchan = prefsWith(MQTT_NEIGHBORS_AUTO, "analyzer-us", "comchan");
  EXPECT_TRUE(mqttNeighborsCollectionEnabled(&with_comchan, RUNTIME_MQTT_SLOTS));
}

TEST(MQTTNeighborsPolicy, AutoIgnoresSlotsBeyondTheRuntimeCount) {
  MQTTPrefs prefs = prefsWith(MQTT_NEIGHBORS_AUTO, "analyzer-us");
  strcpy(prefs.mqtt_slot_preset[MQTT_PREFS_SLOT_COUNT - 1], "comchan");
  EXPECT_FALSE(mqttNeighborsCollectionEnabled(&prefs, MQTT_PREFS_SLOT_COUNT - 1));
  EXPECT_TRUE(mqttNeighborsCollectionEnabled(&prefs, MQTT_PREFS_SLOT_COUNT));
  EXPECT_TRUE(mqttNeighborsCollectionEnabled(&prefs, MQTT_PREFS_SLOT_COUNT + 4));  // clamped
}

TEST(MQTTNeighborsPolicy, OnAndOffIgnoreThePresets) {
  MQTTPrefs on = prefsWith(MQTT_NEIGHBORS_ON, "analyzer-us");
  EXPECT_TRUE(mqttNeighborsCollectionEnabled(&on, RUNTIME_MQTT_SLOTS));
  MQTTPrefs off = prefsWith(MQTT_NEIGHBORS_OFF, "comchan");
  EXPECT_FALSE(mqttNeighborsCollectionEnabled(&off, RUNTIME_MQTT_SLOTS));
  MQTTPrefs bogus = prefsWith(7, "comchan");
  EXPECT_FALSE(mqttNeighborsCollectionEnabled(&bogus, RUNTIME_MQTT_SLOTS));
  EXPECT_FALSE(mqttNeighborsCollectionEnabled(nullptr, RUNTIME_MQTT_SLOTS));
}

TEST(MQTTNeighborsPolicy, PeriodicMaskInAutoFollowsThePresetList) {
  MQTTPrefs prefs = prefsWith(MQTT_NEIGHBORS_AUTO, "analyzer-us", "comchan");
  strcpy(prefs.mqtt_slot_preset[2], "custom");
  EXPECT_EQ(0b010, mqttNeighborsSlotMask(&prefs, RUNTIME_MQTT_SLOTS, true));
}

TEST(MQTTNeighborsPolicy, PeriodicMaskWhenOnReachesEveryConfiguredSlot) {
  MQTTPrefs prefs = prefsWith(MQTT_NEIGHBORS_ON, "analyzer-us", "comchan");
  strcpy(prefs.mqtt_slot_preset[2], "custom");
  EXPECT_EQ(0b111, mqttNeighborsSlotMask(&prefs, RUNTIME_MQTT_SLOTS, true));
}

TEST(MQTTNeighborsPolicy, SlotOverridesBeatThePresetListAndNodeMode) {
  MQTTPrefs prefs = prefsWith(MQTT_NEIGHBORS_AUTO, "analyzer-us", "comchan");
  strcpy(prefs.mqtt_slot_preset[2], "custom");
  prefs.mqtt_slot_neighbors[0] = MQTT_NEIGHBORS_ON;   // not on the list, forced on
  prefs.mqtt_slot_neighbors[1] = MQTT_NEIGHBORS_OFF;  // on the list, forced off
  prefs.mqtt_slot_neighbors[2] = MQTT_NEIGHBORS_ON;   // custom broker opts in
  EXPECT_EQ(0b101, mqttNeighborsSlotMask(&prefs, RUNTIME_MQTT_SLOTS, true));

  prefs.mqtt_neighbors_enabled = MQTT_NEIGHBORS_ON;
  EXPECT_EQ(0b101, mqttNeighborsSlotMask(&prefs, RUNTIME_MQTT_SLOTS, true));  // slot off still wins
}

TEST(MQTTNeighborsPolicy, NodeOffStopsPeriodicEvenForSlotsSetOn) {
  MQTTPrefs prefs = prefsWith(MQTT_NEIGHBORS_OFF, "comchan", "analyzer-us");
  prefs.mqtt_slot_neighbors[1] = MQTT_NEIGHBORS_ON;
  EXPECT_EQ(0, mqttNeighborsSlotMask(&prefs, RUNTIME_MQTT_SLOTS, true));
  EXPECT_FALSE(mqttNeighborsCollectionEnabled(&prefs, RUNTIME_MQTT_SLOTS));
}

TEST(MQTTNeighborsPolicy, ManualSnapshotsSkipOnlySlotsSetOff) {
  MQTTPrefs prefs = prefsWith(MQTT_NEIGHBORS_OFF, "analyzer-us", "comchan");
  strcpy(prefs.mqtt_slot_preset[2], "custom");
  EXPECT_EQ(0b111, mqttNeighborsSlotMask(&prefs, RUNTIME_MQTT_SLOTS, false));
  prefs.mqtt_slot_neighbors[1] = MQTT_NEIGHBORS_OFF;
  EXPECT_EQ(0b101, mqttNeighborsSlotMask(&prefs, RUNTIME_MQTT_SLOTS, false));
}

TEST(MQTTNeighborsPolicy, UnconfiguredAndCorruptSlotsNeverReceive) {
  MQTTPrefs prefs = prefsWith(MQTT_NEIGHBORS_ON, "none", "");
  prefs.mqtt_slot_neighbors[0] = MQTT_NEIGHBORS_ON;
  EXPECT_EQ(0, mqttNeighborsSlotMask(&prefs, RUNTIME_MQTT_SLOTS, false));
  EXPECT_FALSE(mqttNeighborsCollectionEnabled(&prefs, RUNTIME_MQTT_SLOTS));

  MQTTPrefs corrupt = prefsWith(MQTT_NEIGHBORS_ON, "comchan");
  corrupt.mqtt_slot_neighbors[0] = 9;
  EXPECT_EQ(0, mqttNeighborsSlotMask(&corrupt, RUNTIME_MQTT_SLOTS, true));
}

TEST(MQTTNeighborsPolicy, SlotCountBoundsTheMask) {
  MQTTPrefs prefs = prefsWith(MQTT_NEIGHBORS_ON, "comchan");
  for (int i = 0; i < MQTT_PREFS_SLOT_COUNT; i++) strcpy(prefs.mqtt_slot_preset[i], "comchan");
  EXPECT_EQ(0b11, mqttNeighborsSlotMask(&prefs, 2, true));
  EXPECT_EQ(0x3f, mqttNeighborsSlotMask(&prefs, MQTT_PREFS_SLOT_COUNT + 3, true));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
