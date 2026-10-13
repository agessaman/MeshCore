#pragma once

#include <stdint.h>

namespace mesh {
namespace radio {

constexpr uint8_t selectRxBoostedGainDefault(bool sx_configured, int sx_value,
                                            bool generic_configured, int generic_value,
                                            uint8_t fallback) {
  return sx_configured ? (sx_value != 0)
                      : (generic_configured ? (generic_value != 0) : fallback);
}

constexpr uint8_t configuredRxBoostedGainDefault() {
#ifdef SX126X_RX_BOOSTED_GAIN
  return selectRxBoostedGainDefault(true, SX126X_RX_BOOSTED_GAIN, false, 0, 0);
#elif defined(RX_BOOSTED_GAIN)
  return selectRxBoostedGainDefault(false, 0, true, RX_BOOSTED_GAIN, 0);
#elif defined(USE_SX1262) || defined(USE_SX1268)
  return selectRxBoostedGainDefault(false, 0, false, 0, 1);
#else
  return selectRxBoostedGainDefault(false, 0, false, 0, 0);
#endif
}

} // namespace radio
} // namespace mesh
