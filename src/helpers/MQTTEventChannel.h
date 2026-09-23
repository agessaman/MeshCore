#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ObserverMailbox.h"

struct MqttSlotEvent {
  enum class Kind : uint8_t { Connected, Disconnected, Error };
  Kind kind = Kind::Disconnected;
  uint8_t slot = 0;
  uint32_t incarnation = 0;
  uint32_t at_ms = 0;
  int32_t tls = 0;
  int32_t tls_stack = 0;
  int32_t socket = 0;
  uint8_t connack = 0;
};

// SDK tasks publish values; only the bridge worker consumes them. An overflow
// latches the slot even if the lost event was DISCONNECTED.
template<size_t Capacity>
class MqttEventChannel {
 public:
  bool push(const MqttSlotEvent& event) {
    ObserverGuard guard(_lock);
    if (_size == Capacity) {
      _overflow_slots |= uint32_t(1) << event.slot;
      ++_overflows;
      return false;
    }
    _events[(_head + _size) % Capacity] = event;
    ++_size;
    return true;
  }
  bool pop(MqttSlotEvent& event) {
    ObserverGuard guard(_lock);
    if (!_size) return false;
    event = _events[_head];
    _head = (_head + 1) % Capacity;
    --_size;
    return true;
  }
  uint32_t takeOverflowSlots() {
    ObserverGuard guard(_lock);
    uint32_t slots = _overflow_slots;
    _overflow_slots = 0;
    return slots;
  }
  uint32_t overflows() const {
    ObserverGuard guard(_lock);
    return _overflows;
  }
  // Only after the SDK task has joined or acknowledged its disconnect.
  void discardSlot(uint8_t slot) {
    ObserverGuard guard(_lock);
    size_t kept = 0;
    for (size_t i = 0; i < _size; ++i) {
      const MqttSlotEvent event = _events[(_head + i) % Capacity];
      if (event.slot != slot) _events[(_head + kept++) % Capacity] = event;
    }
    _size = kept;
    _overflow_slots &= ~(uint32_t(1) << slot);
  }
 private:
  mutable ObserverLock _lock;
  MqttSlotEvent _events[Capacity]{};
  size_t _head = 0;
  size_t _size = 0;
  uint32_t _overflow_slots = 0;
  uint32_t _overflows = 0;
};
