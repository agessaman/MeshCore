#pragma once

// Initialization is incomplete until lifecycle events have a registered sink.
// The candidate has never started, so registration failure can destroy it.
template<class Handle, class Init, class Register, class Destroy>
int initializeMqttClient(Handle& target, Init init, Register register_events,
                         Destroy destroy, int no_memory) {
  Handle candidate = init();
  if (!candidate) return no_memory;
  const int result = register_events(candidate);
  if (result != 0) {
    destroy(candidate);
    return result;
  }
  target = candidate;
  return 0;
}
