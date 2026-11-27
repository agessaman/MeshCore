#pragma once

// Host stand-in for mbedtls_base64_encode with the same contract as mbedtls 2.x:
// on a short buffer it writes nothing, sets *olen to the size needed and fails.
#include <cstddef>
#include <cstdint>

#define MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL -0x002A

inline int mbedtls_base64_encode(unsigned char* dst, size_t dlen, size_t* olen,
                                 const unsigned char* src, size_t slen) {
  static const unsigned char enc[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (slen == 0) {
    *olen = 0;
    return 0;
  }
  size_t n = (slen / 3 + (slen % 3 != 0)) * 4;
  if (dlen < n + 1 || dst == nullptr) {
    *olen = n + 1;
    return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
  }
  unsigned char* p = dst;
  size_t i = 0;
  for (; i + 3 <= slen; i += 3) {
    uint32_t v = (uint32_t(src[i]) << 16) | (uint32_t(src[i + 1]) << 8) | src[i + 2];
    *p++ = enc[(v >> 18) & 0x3F];
    *p++ = enc[(v >> 12) & 0x3F];
    *p++ = enc[(v >> 6) & 0x3F];
    *p++ = enc[v & 0x3F];
  }
  if (i < slen) {
    uint32_t v = uint32_t(src[i]) << 16;
    if (i + 1 < slen) v |= uint32_t(src[i + 1]) << 8;
    *p++ = enc[(v >> 18) & 0x3F];
    *p++ = enc[(v >> 12) & 0x3F];
    *p++ = (i + 1 < slen) ? enc[(v >> 6) & 0x3F] : '=';
    *p++ = '=';
  }
  *olen = p - dst;
  *p = 0;
  return 0;
}
