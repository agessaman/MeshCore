#pragma once

// Guest-key polling of neighbor repeaters for battery and telemetry, under a
// polling identity of its own. Radio glue around the pure modules in
// src/helpers/poll: drives one PollExchange (or PollAdminDrill) at a time from
// the mesh loop, builds its packets, reads replies off the RX tap, and keeps
// the small flash record. CoreScope control (policy, owners, registration,
// claims) is not here yet: controlFor() is where it will plug in.

#include <helpers/poll/PollBuild.h>

#ifdef WITH_REPEATER_POLL

#include <Mesh.h>
#include <helpers/IdentityStore.h>
#include <helpers/poll/PollAdminDrill.h>
#include <helpers/poll/PollExchange.h>
#include <helpers/poll/PollPolicy.h>
#include <helpers/poll/PollReplyParsers.h>
#include <helpers/poll/PollStateStore.h>

// Operator settings, filled in one place by the host from /mqtt.json.
struct PollSettings {
  bool     enabled;
  bool     consent;
  uint16_t interval_min;
  uint8_t  status_hours;
  uint8_t  count;                                       // packed: targets[0..count)
  uint8_t  targets[poll::kMaxTargets][poll::kPubKeySize];
  uint8_t  pw_mode[poll::kMaxTargets];                  // PasswordMode
};

// What the poller needs from the mesh. MyMesh implements it; the poller never
// sees the node identity, so a poll packet cannot be built under it.
class RepeaterPollHost {
public:
  virtual ~RepeaterPollHost() {}
  // A pool packet, or NULL when fewer than the reserve are free.
  virtual mesh::Packet* pollObtainPacket() = 0;
  // createAnonDatagram from the polling identity, with the same pool preflight.
  virtual mesh::Packet* pollCreateLogin(const mesh::LocalIdentity& poll_id, const mesh::Identity& target,
                                        const uint8_t* secret, const uint8_t* plain, size_t len) = 0;
  // As sendDirect(pkt, NULL, 0) sends it, but behind forwarding (priority 2).
  virtual void pollSendDirect(mesh::Packet* pkt) = 0;
  // Removes a still-queued packet and releases it; false when it is not queued.
  virtual bool pollCancelQueued(const mesh::Packet* pkt) = 0;
  virtual bool pollIsTransmitting(const mesh::Packet* pkt) = 0;
  virtual void pollReleasePacket(mesh::Packet* pkt) = 0;
  // Neighbor table: heard time (RTC) and SNR x4 of a full key.
  virtual bool pollNeighbor(const uint8_t* pub, uint32_t* heard_at, int8_t* snr_x4) = 0;
  // Number of heard neighbors whose key starts with hex; pub_out is set when exactly one.
  virtual int pollResolveNeighborPrefix(const char* hex, uint8_t* pub_out) = 0;
  virtual uint32_t pollNow() = 0;
  virtual uint32_t pollNowUnique() = 0;
  virtual bool pollNtpSeen() = 0;               // a validated NTP reply has been seen
  virtual mesh::RNG* pollRng() = 0;
  virtual uint32_t pollReplyWindowMs() = 0;     // discovery's reply window for one request
  virtual bool pollDiscoveryBusy() = 0;
  virtual uint8_t pollNodeHash() = 0;           // the node key's first byte
  virtual void pollReadSettings(PollSettings& out) = 0;
};

// The "may I poll this target now" inputs. Fixed in this phase; CoreScope fills it later.
struct PollControl {
  uint8_t  role;           // poll::Role
  uint8_t  gate;           // poll::Gate
  uint32_t window_s;
  uint8_t  max_requests;
  bool     own_key;
};

class RepeaterPoller {
public:
  RepeaterPoller();

  void begin(RepeaterPollHost* host, FILESYSTEM* fs);
  void loop();

  // Mesh taps. None of them modify or consume the packet.
  void onRx(const mesh::Packet* pkt);
  void onTx(const mesh::Packet* pkt);
  void onTxFail(const mesh::Packet* pkt);

  // An exchange or admin drill is on the air; discovery must wait.
  bool busy() const { return run_kind_ != RUN_NONE; }

  // Top-level commands the poller owns; false when the command is not one of them.
  bool handleCommand(const char* command, char* reply);

  // Creates the polling identity if there is none; false if it cannot.
  bool ensureIdentity();

  // Saves a tag high-water mark that has not reached flash yet.
  void flush();

  // For `get mqtt.status`: targets whose last poll got a reading, and all targets.
  void summary(uint8_t* ok, uint8_t* total) const;
  bool enabled() const { return enabled_; }

private:
  enum RunKind : uint8_t { RUN_NONE = 0, RUN_EXCHANGE, RUN_DRILL };
  enum Note : uint8_t { NOTE_NONE = 0, NOTE_WAIT, NOTE_DUE, NOTE_CAP, NOTE_NOT_DIRECT, NOTE_ADMIN };

  // One slot more than the target list so an admin drill can finish on a target just removed.
  static const uint8_t kSlots = poll::kMaxTargets + 1;
  static const uint8_t kMaxPollTags = 4;
  static const uint8_t kEventQueue = 4;

  struct Meta {
    bool     used;
    bool     orphan;       // removed from the list while its drill runs
    bool     secret_ok;
    uint8_t  pw_mode;
    uint8_t  note;         // last schedule decision logged
    uint8_t  list_pos;     // position in the operator's list
    uint32_t phase_s;
    uint32_t interval_s;
    uint8_t  pub[poll::kPubKeySize];
    uint8_t  secret[PUB_KEY_SIZE];
  };

  struct Event {
    uint8_t ev;
    bool    flooded;
  };

  RepeaterPollHost* host_;
  FILESYSTEM* fs_;

  // Identity and flash record
  mesh::LocalIdentity poll_id_;
  bool     has_id_;
  uint8_t  store_state_;   // poll::StateLoad of the last load
  uint32_t seq_;
  uint32_t key_created_at_;
  uint32_t tag_hw_;
  uint32_t tag_hw_saved_;
  poll::SavedGate gate_;
  poll::StoredTarget stored_[poll::kMaxTargets];   // persisted per-key flags, incl. removed admin stops

  // Targets
  Meta meta_[kSlots];
  poll::TargetState states_[kSlots];
  uint8_t  count_;         // targets in the operator's list
  bool     enabled_;
  bool     consent_;
  uint16_t interval_min_;
  uint8_t  status_hours_;

  // Run
  uint8_t  run_kind_;
  int8_t   run_idx_;
  bool     run_scheduled_;
  bool     run_quick_;
  uint8_t  run_learned_before_;
  poll::PollExchange exchange_;
  poll::PollAdminDrill drill_;
  uint32_t poll_tags_[kMaxPollTags];
  uint8_t  poll_tag_count_;
  const mesh::Packet* inflight_;
  uint32_t inflight_ms_;
  uint8_t  inflight_act_;
  uint8_t  inflight_pw_;
  uint32_t inflight_tag_;
  bool     timer_on_;
  uint32_t timer_at_;
  bool     listening_;
  uint8_t  listen_act_;
  uint8_t  listen_pw_;
  uint32_t listen_tag_;
  Event    events_[kEventQueue];
  uint8_t  event_count_;

  // Reading from this run's data reply, applied at the finish
  bool     rd_voltage_;
  uint16_t rd_mv_;
  bool     rd_temp_;
  int16_t  rd_temp_c10_;
  uint32_t rd_uptime_;
  int16_t  rd_noise_;

  // Schedule
  uint32_t last_tick_ms_;
  uint32_t last_scheduled_at_;
  uint32_t id_retry_ms_;
  bool     ntp_seen_;
  bool     save_due_;
  uint8_t  hold_note_;     // why scheduled polling is waiting, logged on change

  void tick();
  void readSettings();
  bool clockOk();
  PollControl controlFor(uint8_t idx) const;
  uint32_t intervalFor(const PollControl& c) const;
  bool directRoute(uint8_t idx, int8_t* snr_x4);
  void note(uint8_t idx, uint8_t what, uint32_t detail);
  int8_t findTarget(const char* hex, int* matches) const;
  int8_t slotFor(const uint8_t* pub) const;
  void forgetKeyState();
  void restoreStored(uint8_t idx);

  void startPoll(uint8_t idx, bool scheduled);
  void apply(poll::Step s);
  poll::Step feed(uint8_t ev, bool flooded);
  bool sendFor(const poll::Step& s);
  mesh::Packet* buildDatagram(uint8_t type, const mesh::LocalIdentity& sender, uint8_t idx,
                              const uint8_t* plain, size_t len);
  uint32_t nextTag();
  void pushEvent(uint8_t ev, bool flooded);
  void processEvents();
  void finishRun();
  void endRun();
  void abortRun(const char* why);

  uint8_t readStateFile(const char* name, poll::StateRecord* out);
  void loadState();
  bool saveState(const char* why);

  void cmdGetPoll(char* reply);
  void cmdGetStatus(const char* arg, char* reply);
  void cmdGetKey(char* reply);
  void cmdNow(const char* arg, char* reply);
  void cmdEnable(const char* arg, char* reply);
};

#endif  // WITH_REPEATER_POLL
