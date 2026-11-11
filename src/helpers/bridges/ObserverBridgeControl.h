#pragma once

#include <stdint.h>
#include <stdio.h>

#include "helpers/bridges/ObserverSelfHeal.h"

// Start/stop/restart and deferred-OTA sequencing for the observer MQTT bridge,
// shared by the repeater and room-server roles.
//
// Templated on the bridge type so host tests can drive it with a fake; member
// templates take the role (Host) by reference, so the object holds only flags.
//
// Bridge must provide: isRunning, isStopUnproven, stopAcknowledgedLate,
// pollLateStopAck, canFlashAfterStop, begin, end, setSlotPreset, and the self-heal
// sampling accessors listed in ObserverSelfHeal.h.
// Host must provide:
//   Bridge* newBridge();                                  construct (may return null)
//   void prepareBridgeStart(Bridge* b, bool restart);     metadata before begin()
//   void setAlertBridge(Bridge* b);                       attach/detach fault alerts
//   NodePrefs-like* getNodePrefs();                       reads bridge_enabled
//   MQTTPrefs-like* getObserverPrefs();                   reads mqtt_slot_preset[]
//   void drainOutbound(uint32_t timeout_ms);              flush queued TX before OTA
//   bool flashFromManifest(char* reply);                  reboots on success
//   void otaAlert(const char* msg);                       OTA milestone alert (may be a no-op)
//   bool sendAlertText(const char* msg);                  mesh alert channel (self-heal)
//   void rebootForSelfHeal();                             reboot through the board
//   bool isOtaInProgress();                               OTA scheduled, portal open, or flashing

#if defined(ARDUINO)
#include <Arduino.h>
struct ObserverBridgeSerialLog {
  static void line(const char* msg, const char* detail = nullptr) {
    if (detail) {
      Serial.print(msg);
      Serial.println(detail);
    } else {
      Serial.println(msg);
    }
  }
  static void flush() { Serial.flush(); }
};
#else
struct ObserverBridgeSerialLog;
#endif

template <class Bridge, class Log = ObserverBridgeSerialLog,
          class SelfHealPlatform = ObserverSelfHealPlatform>
class ObserverBridgeControl {
public:
  using SelfHeal = ObserverSelfHeal<Log, SelfHealPlatform>;

  static const uint32_t OTA_TX_DRAIN_TIMEOUT_MS = 5000;  // bounded flush before OTA blocks the loop
  static const uint32_t CONFIG_BATCH_BUSY_MS = 120000;
  static const int OTA_REPLY_SIZE = 160;

  enum OtaStep : uint8_t {
    OTA_IDLE = 0,
    OTA_BEGIN,          // log, drain queued TX, record bridge state
    OTA_STOP_BRIDGE,    // stop a running bridge and decide whether flashing is safe
    OTA_FLASH,          // download and flash; reboots on success
    OTA_RESUME,         // abort path: restore a bridge that was meant to be running
  };

  ObserverBridgeControl()
    : _resume_pending(0), _batch_active(0), _batch_restart(0), _ota_was_running(0),
      _ota_resume(0), _ota_stop_refused(0), _ota_strict_idle(0), _ota_step(OTA_IDLE),
      _slot_restart_mask(0), _batch_started_ms(0) {}

  bool resumePending() const { return _resume_pending; }
  OtaStep otaStep() const { return (OtaStep)_ota_step; }

  template <class Host>
  void setState(Host& host, Bridge*& bridge, bool enable) {
    if (!bridge) {
      bridge = host.newBridge();
      if (!bridge) return;
    }
    if (!enable) _resume_pending = 0;   // an explicit stop cancels a pending resume
    if (enable == bridge->isRunning()) return;
    _heal.onBridgeRestart();
    if (enable) {
      host.prepareBridgeStart(bridge, false);
      bridge->begin();
      host.setAlertBridge(bridge);
      _resume_pending = !bridge->isRunning() && bridge->isStopUnproven();
    } else {
      bridge->end();
      host.setAlertBridge(nullptr);
      _resume_pending = 0;
    }
  }

  template <class Host>
  void restart(Host& host, Bridge*& bridge) {
    if (!bridge || !bridge->isRunning()) return;
    if (_batch_active) {   // coalesced: applied once in endConfigBatch()
      _batch_restart = 1;
      return;
    }
    _heal.onBridgeRestart();
    bridge->end();
    host.prepareBridgeStart(bridge, true);
    bridge->begin();
    _resume_pending = !bridge->isRunning() && bridge->isStopUnproven();
  }

  template <class Host>
  void restartSlot(Host& host, Bridge*& bridge, int slot) {
    if (!bridge || !bridge->isRunning()) return;
    if (_batch_active && slot >= 0 && slot < 8) {
      _slot_restart_mask |= (uint8_t)(1u << slot);
      return;
    }
    _heal.onBridgeRestart();
    bridge->setSlotPreset(slot, host.getObserverPrefs()->mqtt_slot_preset[slot]);
  }

  void beginConfigBatch() {
    _batch_started_ms = SelfHealPlatform::nowMs();
    _batch_active = 1;
    _batch_restart = 0;
    _slot_restart_mask = 0;
  }

  template <class Host>
  void endConfigBatch(Host& host, Bridge*& bridge, int slot_count) {
    _batch_active = 0;
    if (_batch_restart) {
      // A full restart re-applies every slot config; drop the per-slot requests.
      _batch_restart = 0;
      _slot_restart_mask = 0;
      restart(host, bridge);
    } else if (_slot_restart_mask) {
      uint8_t mask = _slot_restart_mask;
      _slot_restart_mask = 0;
      for (int i = 0; i < slot_count; i++) {
        if (mask & (1u << i)) restartSlot(host, bridge, i);
      }
    }
  }

  // Called once per role loop(): finishes late-acknowledged stops and drives self-heal,
  // whether or not the bridge is running.
  template <class Host>
  void poll(Host& host, Bridge*& bridge) {
    // Release withheld resources when a timed-out stop is acknowledged late, and
    // restart only a bridge that is still meant to be running.
    if (bridge && bridge->stopAcknowledgedLate()) {
      bridge->pollLateStopAck();
      if (_resume_pending && host.getNodePrefs()->bridge_enabled) {
        Log::line("MQTT: stop acknowledged late - resuming bridge");
        setState(host, bridge, true);
      }
    }
    const bool batch_busy = _batch_active &&
      (uint32_t)(SelfHealPlatform::nowMs() - _batch_started_ms) < CONFIG_BATCH_BUSY_MS;
    _heal.poll(host, bridge, _ota_step != OTA_IDLE || batch_busy);
  }

  // Runs the deferred OTA to completion (blocking); strict_idle also refuses a stopped
  // bridge whose last stop timed out, not only one whose stop is still unproven.
  template <class Host>
  void runOta(Host& host, Bridge*& bridge, bool strict_idle) {
    _ota_strict_idle = strict_idle;
    _ota_step = OTA_BEGIN;
    while (_ota_step != OTA_IDLE) stepOta(host, bridge);
  }

  template <class Host>
  void stepOta(Host& host, Bridge*& bridge) {
    switch (_ota_step) {
      case OTA_BEGIN: {
        Log::line("OTA: starting update");
        host.drainOutbound(OTA_TX_DRAIN_TIMEOUT_MS);
        if (bridge) bridge->pollLateStopAck();
        // Capture state after the drain so a start processed during it is seen.
        _ota_was_running = bridge && bridge->isRunning();
        // A bridge waiting on a late stop ack is still meant to run if the flash aborts.
        _ota_resume = _ota_was_running || (_resume_pending && host.getNodePrefs()->bridge_enabled);
        _ota_stop_refused = 0;
        _ota_step = OTA_STOP_BRIDGE;
        break;
      }
      case OTA_STOP_BRIDGE: {
        bool may_flash = true;
        if (_ota_was_running) {
          setState(host, bridge, false);
          // A forced/timed-out stop leaves TLS/heap ownership uncertain: do not flash.
          may_flash = bridge && bridge->canFlashAfterStop();
        } else if (bridge && (_ota_strict_idle ? !bridge->canFlashAfterStop() : bridge->isStopUnproven())) {
          // Reads as stopped, but an unacknowledged MQTT task may still own TLS/client state.
          may_flash = false;
        }
        if (may_flash) {
          _ota_step = OTA_FLASH;
        } else {
          Log::line("OTA: aborted, MQTT stop did not complete cleanly");
          _ota_stop_refused = 1;
          _ota_step = OTA_RESUME;
        }
        break;
      }
      case OTA_FLASH: {
        char ota_reply[OTA_REPLY_SIZE];
        ota_reply[0] = 0;
        if (host.flashFromManifest(ota_reply)) {   // a real flash reboots and never returns
          _ota_step = OTA_IDLE;
          break;
        }
        Log::line("OTA: aborted - ", ota_reply);
        char ota_alert_msg[OTA_REPLY_SIZE];
        snprintf(ota_alert_msg, sizeof(ota_alert_msg), "OTA aborted: %s", ota_reply);
        host.otaAlert(ota_alert_msg);
        _ota_step = OTA_RESUME;
        break;
      }
      case OTA_RESUME: {
        // Restore only a bridge that was meant to be running; an OTA must not enable
        // MQTT that an operator had deliberately stopped.
        if (_ota_resume) {
          Log::line("OTA: resuming bridge");
          setState(host, bridge, true);
        }
        if (_ota_stop_refused) {
          if (!_ota_resume) {
            host.otaAlert("OTA aborted: MQTT stop unclean, bridge left stopped - reboot to clear");
          } else if (bridge && bridge->isRunning()) {
            host.otaAlert("OTA aborted: MQTT stop unclean, bridge resumed");
          } else {
            host.otaAlert("OTA aborted: MQTT stop unproven, bridge resumes when it completes");
          }
        }
        _ota_step = OTA_IDLE;
        break;
      }
      default:
        _ota_step = OTA_IDLE;
        break;
    }
  }

private:
  uint8_t _resume_pending : 1;    // begin() was refused after an unproven stop; restart once acknowledged
  uint8_t _batch_active : 1;      // coalesce bridge restarts during a web config batch
  uint8_t _batch_restart : 1;
  uint8_t _ota_was_running : 1;
  uint8_t _ota_resume : 1;
  uint8_t _ota_stop_refused : 1;
  uint8_t _ota_strict_idle : 1;
  uint8_t _ota_step : 3;
  uint8_t _slot_restart_mask;
  uint32_t _batch_started_ms;
  SelfHeal _heal;
};
