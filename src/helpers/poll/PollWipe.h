#pragma once

// Zeroes memory through a volatile pointer so the compiler keeps the writes.

#include <stddef.h>
#include <stdint.h>

namespace poll {
namespace detail {

static inline void wipe(void* p, size_t n) {
  volatile uint8_t* v = (volatile uint8_t*)p;
  while (n--) *v++ = 0;
}

}  // namespace detail
}  // namespace poll
