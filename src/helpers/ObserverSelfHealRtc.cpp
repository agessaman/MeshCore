#if defined(ESP_PLATFORM) && defined(WITH_MQTT_BRIDGE)

#include <Arduino.h>
#include <esp_attr.h>
#include "ObserverSelfHealRtc.h"

// Survives esp_restart(); a power-on leaves garbage that rtcValid() rejects.
static RTC_NOINIT_ATTR MQTTSelfHealPolicy::RtcRecord s_selfheal_rtc;

namespace ObserverSelfHealRtc {

MQTTSelfHealPolicy::RtcRecord load() { return s_selfheal_rtc; }

void store(const MQTTSelfHealPolicy::RtcRecord& rec) { s_selfheal_rtc = rec; }

}  // namespace ObserverSelfHealRtc

#endif
