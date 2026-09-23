#pragma once
#include <stdint.h>
#include "ObserverMailbox.h"

template<class Result>
class ObserverAsyncJob {
 public:
  enum class State : uint8_t { Idle, Queued, Running, Complete, Cancelled };
  struct Snapshot {
    State state = State::Idle;
    uint32_t id = 0, started_ms = 0, finished_ms = 0;
    Result result{};
  };
  bool request(uint32_t now) {
    ObserverGuard guard(_lock);
    if (_value.state == State::Queued || _value.state == State::Running) return false;
    if (++_value.id == 0) ++_value.id;
    _value.started_ms = now;
    _value.finished_ms = 0;
    _value.result = Result{};
    _value.state = State::Queued;
    return true;
  }
  uint32_t begin() {
    ObserverGuard guard(_lock);
    if (_value.state != State::Queued) return 0;
    _value.state = State::Running;
    return _value.id;
  }
  void complete(uint32_t id, uint32_t now, const Result& result) {
    ObserverGuard guard(_lock);
    if (_value.id != id || _value.state != State::Running) return;
    _value.result = result;
    _value.finished_ms = now;
    _value.state = State::Complete;
  }
  void cancel(uint32_t now) {
    ObserverGuard guard(_lock);
    if (_value.state != State::Queued && _value.state != State::Running) return;
    _value.finished_ms = now;
    _value.state = State::Cancelled;
  }
  Snapshot read() const {
    ObserverGuard guard(_lock);
    return _value;
  }
 private:
  mutable ObserverLock _lock;
  Snapshot _value{};
};
