#pragma once
inline bool mock_lwip_ready = false;
inline int mock_lwip_init_calls = 0;
inline void lwip_init() {
  ++mock_lwip_init_calls;
  mock_lwip_ready = true;
}
