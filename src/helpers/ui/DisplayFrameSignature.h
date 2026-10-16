#pragma once

#include <stdint.h>

namespace DisplayFrameSignature {

static constexpr uint32_t INITIAL = 2166136261u;

inline uint32_t append(uint32_t signature, const char *text) {
  if (text) {
    while (*text) {
      signature ^= static_cast<uint8_t>(*text++);
      signature *= 16777619u;
    }
  }

  // Separate adjacent fields so { "ab", "c" } differs from { "a", "bc" }.
  signature ^= 0xFFu;
  signature *= 16777619u;
  return signature;
}

} // namespace DisplayFrameSignature
