#pragma once
#include <cstdint>
#include "lwip_wrap.h"
#include <cassert>
constexpr int CYW43_ITF_STA = 0;
constexpr int CYW43_LINK_DOWN = 0, CYW43_LINK_JOIN = 1;
constexpr int CYW43_LINK_FAIL = -1, CYW43_LINK_NONET = -2, CYW43_LINK_BADAUTH = -3;
struct cyw43_t { uint32_t wifi_join_state = 0; };
inline cyw43_t cyw43_state;
inline int mock_link = CYW43_LINK_DOWN;
inline int cyw43_wifi_link_status(cyw43_t*, int) {
  assert(mock_lwip_context);
  return mock_link;
}
