#pragma once

#include <stdint.h>

namespace DisplayViewport {

struct Geometry {
  int16_t logical_width;
  int16_t logical_height;
  int16_t physical_width;
  int16_t physical_height;

  int16_t mapX(int16_t x) const {
    return static_cast<int16_t>((static_cast<int32_t>(x) * physical_width) / logical_width);
  }

  int16_t mapY(int16_t y) const {
    return static_cast<int16_t>((static_cast<int32_t>(y) * physical_height) / logical_height);
  }

  uint16_t spanX(int16_t x, int16_t width) const { return static_cast<uint16_t>(mapX(x + width) - mapX(x)); }

  uint16_t spanY(int16_t y, int16_t height) const {
    return static_cast<uint16_t>(mapY(y + height) - mapY(y));
  }

  uint16_t logicalWidthForPhysical(uint16_t width) const {
    return static_cast<uint16_t>((static_cast<uint32_t>(width) * logical_width + physical_width - 1) /
                                 physical_width);
  }
};

inline uint8_t selectTextScale(uint16_t width_at_scale_one, uint8_t preferred_scale, uint8_t minimum_scale,
                               uint16_t available_width) {
  if (static_cast<uint32_t>(width_at_scale_one) * preferred_scale <= available_width) {
    return preferred_scale;
  }
  return minimum_scale;
}

} // namespace DisplayViewport
