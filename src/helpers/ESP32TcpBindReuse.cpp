// Lets a new port-80 listener bind while an old HTTP connection is still in TIME_WAIT.

#ifdef ESP_PLATFORM

#include <lwip/opt.h>
#include <lwip/ip.h>
#include <lwip/tcp.h>

// Arduino 2.0.17 names the switch SO_REUSE; later cores name it LWIP_SO_REUSE.
#ifndef LWIP_SO_REUSE
#ifdef SO_REUSE
#define LWIP_SO_REUSE SO_REUSE
#else
#define LWIP_SO_REUSE 0
#endif
#endif

extern "C" err_t __real_tcp_bind(struct tcp_pcb* pcb, const ip_addr_t* ipaddr, u16_t port);

extern "C" err_t __wrap_tcp_bind(struct tcp_pcb* pcb, const ip_addr_t* ipaddr, u16_t port) {
#if LWIP_SO_REUSE
  if (pcb != NULL && port == 80) {
    ip_set_option(pcb, SOF_REUSEADDR);
  }
#endif
  return __real_tcp_bind(pcb, ipaddr, port);
}

#endif  // ESP_PLATFORM
