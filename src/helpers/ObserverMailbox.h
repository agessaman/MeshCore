#pragma once

#include <type_traits>
#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#else
#include <mutex>
#endif

// Task-context lock for bounded value copies. Every holder is a FreeRTOS task
// (loop, bridge worker, SDK event task, AsyncTCP), never an ISR, so a mutex is
// the right primitive: a critical section would disable interrupts on the radio
// core for the whole copy, and some of these records are kilobytes. Priority
// inheritance covers a preempted holder. Never hold this across allocation,
// I/O, flash writes or SDK calls.
class ObserverLock {
 public:
  ObserverLock() {
#ifdef ESP_PLATFORM
    _mux = xSemaphoreCreateMutexStatic(&_storage);
#endif
  }
  ObserverLock(const ObserverLock&) = delete;
  ObserverLock& operator=(const ObserverLock&) = delete;
  void lock() {
#ifdef ESP_PLATFORM
    xSemaphoreTake(_mux, portMAX_DELAY);
#else
    _mux.lock();
#endif
  }
  void unlock() {
#ifdef ESP_PLATFORM
    xSemaphoreGive(_mux);
#else
    _mux.unlock();
#endif
  }
 private:
#ifdef ESP_PLATFORM
  StaticSemaphore_t _storage;
  SemaphoreHandle_t _mux;
#else
  std::mutex _mux;
#endif
};

class ObserverGuard {
 public:
  explicit ObserverGuard(ObserverLock& lock) : _lock(lock) { _lock.lock(); }
  ~ObserverGuard() { _lock.unlock(); }
  ObserverGuard(const ObserverGuard&) = delete;
  ObserverGuard& operator=(const ObserverGuard&) = delete;
 private:
  ObserverLock& _lock;
};

template<class T>
class ObserverMailbox {
  static_assert(std::is_trivially_copyable<T>::value, "mailbox requires value records");
 public:
  void publish(const T& value) {
    ObserverGuard guard(_lock);
    _value = value;
  }
  T read() const {
    ObserverGuard guard(_lock);
    return _value;
  }
 private:
  mutable ObserverLock _lock;
  T _value{};
};
