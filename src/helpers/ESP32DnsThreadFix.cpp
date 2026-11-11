// The raw lwIP API may only be called on the lwIP thread.
// This framework builds with LWIP_TCPIP_CORE_LOCKING off, so there is no core lock to take.
// Arduino 2.0.17 WiFiGenericClass::hostByName() calls dns_gethostbyname() from application tasks.
// AsyncTCP does the same.
// [esp32_base] wraps the symbol, so those calls are marshaled with tcpip_api_call.
// The found callback still runs on the lwIP thread, as before.
// The file and the flag can go when the fork moves to Arduino core 3.x.

#ifdef ESP_PLATFORM

#include <stddef.h>

#include <lwip/opt.h>
#include <lwip/dns.h>
#include <lwip/err.h>
#include <lwip/sys.h>
#include <lwip/priv/tcpip_priv.h>

extern "C" err_t __real_dns_gethostbyname(const char* hostname, ip_addr_t* addr,
                                          dns_found_callback found, void* callback_arg);

#if !LWIP_TCPIP_CORE_LOCKING

namespace {

struct DnsCall {
  struct tcpip_api_call_data call;  // must stay first: the callback casts back from it
  const char* hostname;
  ip_addr_t* addr;
  dns_found_callback found;
  void* callback_arg;
};

static_assert(offsetof(DnsCall, call) == 0, "tcpip_api_call_data must be the first member");

err_t dnsCallOnTcpipThread(struct tcpip_api_call_data* call) {
  DnsCall* d = reinterpret_cast<DnsCall*>(call);
  return __real_dns_gethostbyname(d->hostname, d->addr, d->found, d->callback_arg);
}

}  // namespace

extern "C" err_t __wrap_dns_gethostbyname(const char* hostname, ip_addr_t* addr,
                                          dns_found_callback found, void* callback_arg) {
  // Before lwIP starts, or already on its thread: call straight through.
  if (sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER)) {
    return __real_dns_gethostbyname(hostname, addr, found, callback_arg);
  }
  // tcpip_api_call waits on this task's lwIP semaphore and does not check that it exists.
  if (sys_thread_sem_get() == nullptr) return ERR_MEM;
  DnsCall d = {};
  d.hostname = hostname;
  d.addr = addr;
  d.found = found;
  d.callback_arg = callback_arg;
  return tcpip_api_call(dnsCallOnTcpipThread, &d.call);
}

#else  // core locking builds take the lock in the caller

extern "C" err_t __wrap_dns_gethostbyname(const char* hostname, ip_addr_t* addr,
                                          dns_found_callback found, void* callback_arg) {
  return __real_dns_gethostbyname(hostname, addr, found, callback_arg);
}

#endif

#endif  // ESP_PLATFORM
