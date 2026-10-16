#include <gtest/gtest.h>
#include <string.h>
#include "MeshCore.h"

class LegacyOTABoard : public mesh::MainBoard {
public:
  bool called = false;

  uint16_t getBattMilliVolts() override { return 0; }
  const char* getManufacturerName() const override { return "test"; }
  void reboot() override { }
  uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }

  bool startOTAUpdate(const char* id, char reply[]) override {
    called = true;
    strcpy(reply, id);
    return true;
  }
};

TEST(MainBoardAPI, ThreeArgumentOtaFallsBackToLegacyOverride) {
  LegacyOTABoard board;
  mesh::MainBoard* base = &board;
  char reply[16] = { 0 };

  EXPECT_TRUE(base->startOTAUpdate("legacy", reply, true));
  EXPECT_TRUE(board.called);
  EXPECT_STREQ("legacy", reply);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
