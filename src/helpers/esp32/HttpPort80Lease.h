#pragma once

#include <stdint.h>

// Tiny in-process ownership guard for the two existing AsyncWebServer users.
// Calls are made from the Arduino loop task; no cross-task locking is needed.
namespace HttpPort80Lease {

enum class Owner : uint8_t { None = 0, WebConfig, Ota };

inline Owner& current() {
  static Owner owner = Owner::None;
  return owner;
}

inline bool acquire(Owner requested) {
  if (requested == Owner::None || current() != Owner::None) return false;
  current() = requested;
  return true;
}

inline void release(Owner expected) {
  if (current() == expected) current() = Owner::None;
}

inline const char* ownerName() {
  switch (current()) {
    case Owner::WebConfig: return "webconfig";
    case Owner::Ota:       return "ota";
    default:               return "none";
  }
}

}  // namespace HttpPort80Lease
