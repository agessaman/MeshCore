#pragma once

#include "BridgeBase.h"
#include "helpers/NetworkLink.h"
#include "helpers/MQTTPresets.h"
#include "helpers/MQTTLifecycle.h"
#include "helpers/AlertFaultPolicy.h"
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <semphr.h>
#include <atomic>

class MQTTBridge : public BridgeBase {
  struct Configuration {
    MQTTPrefs observer{};
    char origin[32]{}, device[65]{}, firmware[64]{}, model[64]{}, date[32]{};
    mesh::LocalIdentity identity;
    float frequency = 0, bandwidth = 0;
    uint8_t sf = 0, cr = 0;
    bool repeat = false;
    uint32_t generation = 0;
  };
  struct Capture {
    mesh::Packet packet;
    uint8_t raw[256];
    uint16_t length = 0;
    float snr = 0, rssi = 0, score = 0;
    uint32_t captured_ms = 0;
    bool tx = false;
  };
  struct Health {
    bool connected = false, time_valid = false, attempted = false;
    uint32_t published = 0, errors = 0, dropped = 0, filtered = 0, unsynced = 0;
    uint32_t connected_at = 0, outage_at = 0, stack_free = 0, min_heap = UINT32_MAX;
    int error_code = 0;
    char state[24] = "starting";
  };
  struct RadioStats {
    int errors = -1;
    int battery = -1, noise = -999, rx_errors = -1, sent = -1, received = -1;
    int tx_air = -1, rx_air = -1;
  };
  MQTTPrefs* _obs;
  mesh::LocalIdentity* _identity;
  mesh::Dispatcher* _dispatcher = nullptr;
  mesh::Radio* _radio = nullptr;
  mesh::MainBoard* _board = nullptr;
  Configuration _last_config{}, _config{};
  Health _health{};
  RadioStats _radio_stats{};
  Capture _staged{};
  SemaphoreHandle_t _health_mutex = nullptr;
  QueueHandle_t _captures = nullptr, _config_queue = nullptr, _rtc_updates = nullptr;
  TaskHandle_t _task = nullptr;
  std::atomic<bool> _stop{false}, _acked{false}, _force_ntp{false}, _time_valid{false};
  std::atomic<uint32_t> _dropped{0}, _filtered{0}, _unsynced{0};
  uint32_t _last_sample = 0, _last_config_sample = 0;
  uint32_t _stop_started = 0;
  MQTTLifecycle::State _state = MQTTLifecycle::State::Stopped;
  static MQTTBridge* _instance;
  void snapshotConfiguration();
  void capture(mesh::Packet* packet, bool tx);
  static void taskEntry(void*);
  void run();
  void release();
  Health readHealth() const;
public:
  MQTTBridge(NodePrefs*, MQTTPrefs*, mesh::PacketManager*, mesh::RTCClock*, mesh::LocalIdentity*);
  ~MQTTBridge() override;
  void begin() override;
  void end() override;
  void loop() override;
  void onPacketReceived(mesh::Packet* p) override { capture(p, false); }
  void sendPacket(mesh::Packet* p) override { capture(p, true); }
  void storeRawRadioData(const uint8_t*, int, float, float);
  void setSlotPreset(int, const char*) { snapshotConfiguration(); }
  void setDeviceID(const char*);
  void setFirmwareVersion(const char*);
  void setBoardModel(const char*);
  void setBuildDate(const char*);
  void setStatsSources(mesh::Dispatcher*, mesh::Radio*, mesh::MainBoard*, mesh::MillisecondClock*);
  int getQueueSize() const { return _captures ? uxQueueMessagesWaiting(_captures) : 0; }
  bool canFlashAfterStop() const { return false; }
  bool isStopUnproven() const { return _state == MQTTLifecycle::State::StopRequested || _state == MQTTLifecycle::State::StopUnproven; }
  bool stopAcknowledgedLate() const { return isStopUnproven() && _acked.load(std::memory_order_acquire); }
  void pollLateStopAck();
  static bool stopUnprovenLatched() { return _instance && _instance->isStopUnproven(); }
  static int getRuntimeSlotCount() { return 1; }
  static int getMaxActiveSlots() { return 1; }
  static bool isConfigValid(const MQTTPrefs*);
  static void getEffectiveMqttOrigin(const NodePrefs*, const MQTTPrefs*, char*, size_t);
  static const char* effectiveNtpPrimary(const MQTTPrefs* p) { return p && p->mqtt_ntp_server[0] ? p->mqtt_ntp_server : "pool.ntp.org"; }
  static void formatMqttStatusReply(char*, size_t, const MQTTPrefs*);
  static void formatMqttStatsReply(char*, size_t);
  static void formatSlotDiagReply(char*, size_t, int);
  static unsigned long getWifiConnectedAtMillis() { return activeNetworkLink().connectedAtMillis(); }
  static const char* wifiReasonStr(uint8_t) { return "link lost"; }
  AlertFaultPolicy::OutageSnapshot getNetworkOutageSnapshot() const { return activeNetworkLink().alertOutageSnapshot(); }
  NetworkMedium getNetworkAlertMedium() const { return NetworkMedium::WiFi; }
  unsigned long getSlotCurrentOutageStartMs(int i) const { return i == 0 ? readHealth().outage_at : 0; }
  bool isSlotEnabledAndAttempted(int i) const { return i == 0 && readHealth().attempted; }
  const char* getSlotPresetName(int i) const { return i == 0 ? _obs->mqtt_slot_preset[0] : "none"; }
  bool requestForcedNtpSync(uint32_t = 0) { if (!_initialized) return false; _force_ntp.store(true); return true; }
  bool ntpDiag(char*, size_t, bool);
};
