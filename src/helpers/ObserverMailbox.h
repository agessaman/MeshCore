#pragma once

#include <type_traits>
#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#else
#include <mutex>
#endif

// Only bounded value copies belong under this lock, never allocation or I/O.
class ObserverLock {
 public:
  void lock() {
#ifdef ESP_PLATFORM
    portENTER_CRITICAL(&_mux);
#else
    _mux.lock();
#endif
  }
  void unlock() {
#ifdef ESP_PLATFORM
    portEXIT_CRITICAL(&_mux);
#else
    _mux.unlock();
#endif
  }
 private:
#ifdef ESP_PLATFORM
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
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
