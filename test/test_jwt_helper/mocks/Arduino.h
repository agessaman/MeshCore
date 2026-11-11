#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

// Minimal Serial: JWTHelper only logs on a failed self-check.
struct MockSerial {
  int availableForWrite() { return 0; }
  size_t println(const char*) { return 0; }
};
inline MockSerial Serial;
