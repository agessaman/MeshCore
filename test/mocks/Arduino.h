#pragma once

#include <cstdint>
#include <cmath>
// The real Arduino.h pulls in stdlib.h, so device code reaches atoi/atol/atof/strtoul
// without including it. Mirror that here or those sources fail only on the native build.
#include <cstdlib>
#include "Stream.h"

using std::atof;
using std::atoi;
using std::atol;

inline uint32_t g_mock_millis = 0;

using std::isnan;

inline uint32_t millis() {
  return g_mock_millis;
}

inline void delay(uint32_t ms) {
  g_mock_millis += ms;
}
