#pragma once

#include <atomic>
#include <stdint.h>

// Ownership guard for the two AsyncWebServer users. It prevents `start ota`
// from claiming port 80 while WebConfig is listening (and vice versa), even
// when the requests arrive from different tasks.
namespace HttpPort80Lease {

enum class Owner : uint8_t { None = 0, WebConfig, Ota };

inline std::atomic<Owner>& current() {
  static std::atomic<Owner> owner{Owner::None};
  return owner;
}

inline bool acquire(Owner requested) {
  if (requested == Owner::None) return false;
  Owner expected = Owner::None;
  return current().compare_exchange_strong(
      expected, requested, std::memory_order_acq_rel,
      std::memory_order_relaxed);
}

inline void release(Owner expected) {
  current().compare_exchange_strong(
      expected, Owner::None, std::memory_order_acq_rel,
      std::memory_order_relaxed);
}

inline const char* ownerName() {
  switch (current().load(std::memory_order_acquire)) {
    case Owner::WebConfig: return "webconfig";
    case Owner::Ota: return "ota";
    default: return "none";
  }
}

}  // namespace HttpPort80Lease
