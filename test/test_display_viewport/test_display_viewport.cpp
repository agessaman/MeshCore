#include "helpers/ui/DisplayFrameSignature.h"
#include "helpers/ui/DisplayViewport.h"

#include <gtest/gtest.h>

namespace {

const DisplayViewport::Geometry kPortrait{ 128, 64, 240, 320 };

} // namespace

TEST(DisplayViewport, MapsLogicalBoundsToPortraitPanel) {
  EXPECT_EQ(0, kPortrait.mapX(0));
  EXPECT_EQ(240, kPortrait.mapX(128));
  EXPECT_EQ(0, kPortrait.mapY(0));
  EXPECT_EQ(320, kPortrait.mapY(64));
}

TEST(DisplayViewport, MapsObserverRowsAcrossPortraitHeight) {
  EXPECT_EQ(0, kPortrait.mapY(0));
  EXPECT_EQ(70, kPortrait.mapY(14));
  EXPECT_EQ(100, kPortrait.mapY(20));
  EXPECT_EQ(120, kPortrait.mapY(24));
  EXPECT_EQ(150, kPortrait.mapY(30));
  EXPECT_EQ(200, kPortrait.mapY(40));
  EXPECT_EQ(240, kPortrait.mapY(48));
  EXPECT_EQ(250, kPortrait.mapY(50));
}

TEST(DisplayViewport, FractionalHorizontalSpansHaveNoGapsOrOverlaps) {
  int16_t previous_end = 0;
  int total_width = 0;

  for (int x = 0; x < 128; ++x) {
    int16_t start = kPortrait.mapX(x);
    int16_t end = kPortrait.mapX(x + 1);
    EXPECT_EQ(previous_end, start);
    EXPECT_TRUE(end - start == 1 || end - start == 2);
    total_width += end - start;
    previous_end = end;
  }

  EXPECT_EQ(240, previous_end);
  EXPECT_EQ(240, total_width);
}

TEST(DisplayViewport, ConvertsFittedPhysicalWidthBackToLogicalWidth) {
  EXPECT_EQ(0, kPortrait.logicalWidthForPhysical(0));
  EXPECT_EQ(64, kPortrait.logicalWidthForPhysical(120));
  EXPECT_EQ(122, kPortrait.logicalWidthForPhysical(228));
  EXPECT_EQ(128, kPortrait.logicalWidthForPhysical(240));
}

TEST(DisplayViewport, SelectsPreferredTextScaleOnlyWhenItFits) {
  EXPECT_EQ(2, DisplayViewport::selectTextScale(114, 2, 1, 240));
  EXPECT_EQ(1, DisplayViewport::selectTextScale(150, 2, 1, 240));
  EXPECT_EQ(1, DisplayViewport::selectTextScale(192, 2, 1, 229));
  EXPECT_EQ(1, DisplayViewport::selectTextScale(300, 2, 1, 240));
}

TEST(DisplayFrameSignature, ChangesWithVisibleContent) {
  uint32_t initial = DisplayFrameSignature::INITIAL;
  uint32_t home = DisplayFrameSignature::append(initial, "home");

  EXPECT_EQ(home, DisplayFrameSignature::append(initial, "home"));
  EXPECT_NE(home, DisplayFrameSignature::append(initial, "setup"));
  EXPECT_NE(DisplayFrameSignature::append(home, "10.0.0.1"), DisplayFrameSignature::append(home, "10.0.0.2"));
}

TEST(DisplayFrameSignature, KeepsAdjacentFieldsDistinct) {
  uint32_t first = DisplayFrameSignature::append(DisplayFrameSignature::INITIAL, "ab");
  first = DisplayFrameSignature::append(first, "c");

  uint32_t second = DisplayFrameSignature::append(DisplayFrameSignature::INITIAL, "a");
  second = DisplayFrameSignature::append(second, "bc");

  EXPECT_NE(first, second);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
