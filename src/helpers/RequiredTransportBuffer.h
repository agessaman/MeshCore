#pragma once
#include <stddef.h>

// Allocate the protection buffer first. A successful init must install it;
// neither allocation failure is allowed to return an unprotected transport.
template<class Handle, class Allocate, class Init, class Install, class Free>
Handle initWithRequiredTransportBuffer(size_t size, Allocate allocate, Init init,
                                       Install install, Free release) {
  void* buffer = allocate(size);
  if (!buffer) return nullptr;
  Handle transport = init();
  if (!transport) {
    release(buffer);
    return nullptr;
  }
  install(transport, buffer);
  return transport;
}
