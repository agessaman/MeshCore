#pragma once

#include "MQTTSelfHealPolicy.h"

// MQTT self-heal boot-loop record, kept in RTC no-init memory on ESP32 observer
// builds so it survives a software reboot. Defined in ObserverSelfHealRtc.cpp.
namespace ObserverSelfHealRtc {

MQTTSelfHealPolicy::RtcRecord load();
void store(const MQTTSelfHealPolicy::RtcRecord& rec);

// Consecutive self-heal reboots and the last cause, both zero/None when cleared or invalid.
static inline uint8_t lastCount(const MQTTSelfHealPolicy::RtcRecord& rec) {
  return MQTTSelfHealPolicy::rtcConsecutive(rec);
}
static inline MQTTSelfHealPolicy::Cause lastCause(const MQTTSelfHealPolicy::RtcRecord& rec) {
  return lastCount(rec) > 0 ? (MQTTSelfHealPolicy::Cause)rec.last_cause
                            : MQTTSelfHealPolicy::Cause::None;
}

}  // namespace ObserverSelfHealRtc
