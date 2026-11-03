#pragma once
#include "lwip/init.h"
#include <stdexcept>
inline bool mock_lwip_context = false;
inline int mock_lwip_calls = 0;
inline void lwip_callback(void (*callback)(void*), void* context) {
  if (!mock_lwip_ready) throw std::logic_error("lwIP work queue does not exist before lwip_init");
  const bool previous = mock_lwip_context;
  mock_lwip_context = true;
  ++mock_lwip_calls;
  callback(context);
  mock_lwip_context = previous;
}
