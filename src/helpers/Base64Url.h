#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Base64Url {
inline size_t encode(const uint8_t* data, size_t length, char* output, size_t capacity) {
  if (!data || !output || !capacity || length > (SIZE_MAX - 2) / 4 * 3) return 0;
  const size_t needed = length / 3 * 4 + (length % 3 ? length % 3 + 1 : 0);
  if (needed >= capacity) { output[0] = 0; return 0; }
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t written = 0;
  for (size_t i = 0; i < length; i += 3) {
    const size_t left = length - i;
    uint32_t bits = static_cast<uint32_t>(data[i]) << 16;
    if (left > 1) bits |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (left > 2) bits |= data[i + 2];
    output[written++] = alphabet[(bits >> 18) & 63];
    output[written++] = alphabet[(bits >> 12) & 63];
    if (left > 1) output[written++] = alphabet[(bits >> 6) & 63];
    if (left > 2) output[written++] = alphabet[bits & 63];
  }
  output[written] = 0;
  return written;
}
}
