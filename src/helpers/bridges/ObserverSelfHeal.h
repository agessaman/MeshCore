#pragma once

#include <stdint.h>

#include "helpers/MQTTSelfHealPolicy.h"

// Drives MQTTSelfHealPolicy from the role's main loop: samples the bridge about
// every 10 s and performs the returned action. Holds only the policy state.
//
// Platform provides (all static):
//   bool kSupported;                          false compiles the driver to nothing
//   uint32_t nowMs();
//   MQTTSelfHealPolicy::RtcRecord rtcLoad();
//   void rtcStore(const MQTTSelfHealPolicy::RtcRecord&);
// Host provides: getObserverPrefs() (mqtt_selfheal_enabled, alert_enabled),
//   bool sendAlertText(const char*), void rebootForSelfHeal().
// Bridge provides: isRunning, isStopUnproven, anyClientQuarantined, isNetworkUp,
//   ntpEvidenceAgeMs, candidateSlotCount, connectedSlotCount.

#if defined(ESP_PLATFORM) && defined(WITH_MQTT_BRIDGE)
#include <Arduino.h>
#include "helpers/ObserverSelfHealRtc.h"
struct ObserverSelfHealPlatform {
  static const bool kSupported = true;
  static uint32_t nowMs() { return (uint32_t)millis(); }
  static MQTTSelfHealPolicy::RtcRecord rtcLoad() { return ObserverSelfHealRtc::load(); }
  static void rtcStore(const MQTTSelfHealPolicy::RtcRecord& rec) { ObserverSelfHealRtc::store(rec); }
};
#else
struct ObserverSelfHealPlatform {
  static const bool kSupported = false;
  static uint32_t nowMs() { return 0; }
  static MQTTSelfHealPolicy::RtcRecord rtcLoad() { return MQTTSelfHealPolicy::rtcCleared(); }
  static void rtcStore(const MQTTSelfHealPolicy::RtcRecord&) {}
};
#endif

// Log provides line(msg, detail = nullptr) and flush().
template <class Log, class Platform>
class ObserverSelfHeal {
public:
  static const uint32_t kTickMs = 10000;

  ObserverSelfHeal() : _st(), _last_tick_ms(0) {}

  template <class Host, class Bridge>
  void poll(Host& host, Bridge* bridge) {
    if (!Platform::kSupported) return;
    namespace Heal = MQTTSelfHealPolicy;
    const uint32_t now = Platform::nowMs();
    if ((uint32_t)(now - _last_tick_ms) < kTickMs) return;
    _last_tick_ms = now;

    const auto* obs = host.getObserverPrefs();
    Heal::Inputs in{};
    in.enabled = obs && obs->mqtt_selfheal_enabled;
    in.now_ms = now;
    in.evidence_age_ms = Heal::kNoEvidence;
    if (bridge) {
      in.bridge_running = bridge->isRunning();
      in.stop_unproven = bridge->isStopUnproven();
      in.client_quarantined = bridge->anyClientQuarantined();
      in.network_up = bridge->isNetworkUp();
      in.evidence_age_ms = bridge->ntpEvidenceAgeMs();
      in.candidate_slots = bridge->candidateSlotCount();
      in.connected_slots = bridge->connectedSlotCount();
    }

    const Heal::Config cfg = Heal::defaultConfig();
    const Heal::RtcRecord rec = Platform::rtcLoad();
    const Heal::Result r = Heal::tick(_st, in, cfg, Heal::rtcConsecutive(rec));
    switch (r.action) {
      case Heal::Action::Alert: {
        char msg[80];
        Heal::formatAlert(r, cfg, msg, sizeof(msg));
        Log::line(msg);
        if (obs && obs->alert_enabled) host.sendAlertText(msg);   // queued; never blocks
        break;
      }
      case Heal::Action::Reboot:
        // Count the reboot before it happens: the boot-loop limit depends on it.
        Platform::rtcStore(Heal::rtcBeforeReboot(rec, r.cause));
        Log::line("MQTT self-heal: rebooting for ", Heal::causeLabel(r.cause));
        Log::flush();
        host.rebootForSelfHeal();
        break;
      case Heal::Action::ClearBootCounter:
        Platform::rtcStore(Heal::rtcCleared());
        Log::line("MQTT self-heal: healthy, reboot counter cleared");
        break;
      default:
        break;
    }
  }

private:
  MQTTSelfHealPolicy::State _st;
  uint32_t _last_tick_ms;
};
