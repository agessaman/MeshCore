#pragma once

#include "MeshCore.h"
#include <cstring>

namespace mesh {

// Host stand-in for src/Utils.h: toHex() and fromHex() copied from src/Utils.cpp.
class Utils {
public:
  static void toHex(char* dest, const uint8_t* src, size_t len) {
    static const char hex_chars[] = "0123456789ABCDEF";
    while (len > 0) {
      uint8_t b = *src++;
      *dest++ = hex_chars[b >> 4];
      *dest++ = hex_chars[b & 0x0F];
      len--;
    }
    *dest = 0;
  }

  static bool fromHex(uint8_t* dest, int dest_size, const char* src_hex) {
    int len = strlen(src_hex);
    if (len != dest_size * 2) return false;
    uint8_t* dp = dest;
    while (dp - dest < dest_size) {
      char ch = *src_hex++;
      char cl = *src_hex++;
      *dp++ = (hexVal(ch) << 4) | hexVal(cl);
    }
    return true;
  }

private:
  static uint8_t hexVal(char c) {
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= '0' && c <= '9') return c - '0';
    return 0;
  }
};

}
