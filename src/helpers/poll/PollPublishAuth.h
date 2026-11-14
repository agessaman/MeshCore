#pragma once

// Whether a queued poll publication may still go to the slot it was matched to.
// Hosts are the broker hostname only (no scheme, port, or path). No Arduino.

#include <ctype.h>
#include <stdint.h>

namespace poll {

enum PublishAuthorization : uint8_t {
  PUBLISH_SEND = 0,
  PUBLISH_WRONG_HOST = 1,  // automatic slot no longer has the matched broker
};

// Operator slot skips the host check. A new generation with the same host still sends.
static inline uint8_t authorizePublish(bool explicit_slot, uint32_t authorized_gen,
                                       uint32_t current_gen, const char* authorized_host,
                                       const char* current_host) {
  (void)authorized_gen;
  (void)current_gen;
  if (explicit_slot) return PUBLISH_SEND;
  if (!authorized_host || !current_host || authorized_host[0] == '\0' || current_host[0] == '\0') {
    return PUBLISH_WRONG_HOST;
  }
  const char* a = authorized_host;
  const char* b = current_host;
  while (*a != '\0' && *b != '\0') {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return PUBLISH_WRONG_HOST;
    a++;
    b++;
  }
  if (*a != '\0' || *b != '\0') return PUBLISH_WRONG_HOST;
  return PUBLISH_SEND;
}

}  // namespace poll
