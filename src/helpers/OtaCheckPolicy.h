#pragma once

#include <stdint.h>

namespace OtaCheckPolicy {

constexpr uint32_t kRefreshMs = 60000;
enum class Activity : uint8_t { Idle, Check, Update };
enum class Reply : uint8_t { Started, Running, Cached, Busy };

struct State {
  uint32_t completed_ms = 0;
  uint8_t channel = 0;
  bool cached = false;
  Activity activity = Activity::Idle;
};

struct Read {
  Reply reply;
  bool start;
  uint32_t age_ms;
};

inline Read read(State& state, uint32_t now, uint8_t channel) {
  const bool cached = state.cached && state.channel == channel;
  const uint32_t age = now - state.completed_ms;
  const bool start = state.activity == Activity::Idle &&
                     (!cached || age >= kRefreshMs);
  if (start) state.activity = Activity::Check;
  return {cached ? Reply::Cached : start ? Reply::Started :
          state.activity == Activity::Check ? Reply::Running : Reply::Busy,
          start, age};
}

inline void complete(State& state, uint32_t now, uint8_t channel) {
  state.completed_ms = now;
  state.channel = channel;
  state.cached = true;
  state.activity = Activity::Idle;
}

inline bool beginUpdate(State& state) {
  if (state.activity != Activity::Idle) return false;
  state.activity = Activity::Update;
  return true;
}

inline void endUpdate(State& state) { state.activity = Activity::Idle; }

} // namespace OtaCheckPolicy
