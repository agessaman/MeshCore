#pragma once

// Guest-key polling of neighbor repeaters for battery and telemetry, under a
// polling identity of its own. Radio glue around the pure modules in
// src/helpers/poll: drives one PollExchange (or PollAdminDrill) at a time from
// the mesh loop, builds its packets, reads replies off the RX tap, and keeps
// the small flash record. CoreScope control (policy, owners, registration,
// shared key, claims) is filed with the bridge and decided here, in the order
// the plugin's poll cycle uses.

#include <helpers/poll/PollBuild.h>

#ifdef WITH_REPEATER_POLL

#include <Mesh.h>
#include <helpers/IdentityStore.h>
#include <helpers/poll/PollAdminDrill.h>
#include <helpers/poll/PollClaims.h>
#include <helpers/poll/PollControlTypes.h>
#include <helpers/poll/PollCycle.h>
#include <helpers/poll/PollExchange.h>
#include <helpers/poll/PollPolicy.h>
#include <helpers/poll/PollRegistration.h>
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
  char     url[poll::kControlUrlMax];                   // CoreScope base URL, "" = none
  uint8_t  slot;                                        // 1-based MQTT slot for the claim, 0 = unset
  bool     community;                                   // share polling with other observers
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
  // As sendDirect(pkt, NULL, 0) sends it, but behind forwarding (priority 2). False when the
  // queue refused it; the packet is then already released and must not be touched.
  virtual bool pollSendDirect(mesh::Packet* pkt) = 0;
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
  virtual bool pollNtpSeen() = 0;               // a validated NTP time was applied to the RTC this boot
  virtual mesh::RNG* pollRng() = 0;
  virtual uint32_t pollReplyWindowMs() = 0;     // discovery's reply window for one request
  virtual bool pollDiscoveryBusy() = 0;
  virtual uint8_t pollNodeHash() = 0;           // the node key's first byte
  virtual void pollReadSettings(PollSettings& out) = 0;
  // The node identity, public half only: claim origin and origin_id, registration observer_id.
  virtual void pollNodePub(uint8_t* out) = 0;
  virtual const char* pollNodeName() = 0;
  virtual const char* pollFirmwareVersion() = 0;
  // Signs the polling key's registration with the node key; nothing else is ever signed with it.
  virtual void pollSignRegistration(uint8_t* sig, const uint8_t* msg, size_t len) = 0;
  // The bridge's CoreScope hand-offs; none block, all refuse while the bridge is stopped or busy.
  virtual bool pollRequestControl(const poll::ControlRequest& req) = 0;
  virtual bool pollTakeControlResult(poll::ControlResult& out) = 0;
  virtual bool pollRequestClaim(const char* json, size_t len, uint8_t slot) = 0;
  virtual uint8_t pollTakeClaimResult() = 0;
};

// What identifies a queued packet when only its pool slot is known.
struct PollPacketSig {
  uint8_t  header;
  uint8_t  path_len;
  uint16_t payload_len;
  uint32_t hash;
};

// The "may I poll this target now" inputs, from CoreScope's policy and owners.
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

  // False when listing pub would leave no room for a removed target's unverified admin stop.
  bool roomFor(const uint8_t* pub);

  // A CoreScope control session filed by the poller has not been answered yet.
  bool controlPending() const { return ctl_pending_; }

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
    bool     admin_held;   // an admin answer to this target's exchange not yet fed to it; outside the queue
    bool     admin_held_flooded;
    uint32_t phase_s;
    uint32_t interval_s;
    uint32_t guest_answer_print;   // loginAnswerPrint of the last guest login answer taken
    uint32_t admin_answer_print;   // loginAnswerPrint of the last admin login answer taken
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
  uint8_t  recovered_from_;   // state file holding the newest record when it is not the main one
  uint32_t seq_;
  uint32_t key_created_at_;
  uint32_t tag_hw_;
  uint32_t tag_hw_saved_;
  poll::SavedGate gate_;
  poll::StoredTarget stored_[poll::kStoredTargets];   // per-key flags as written to flash, incl. removed targets

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
  PollPacketSig inflight_sig_;
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
  bool     save_safety_;   // an admin stop or drill result is not on flash yet
  uint32_t save_retry_ms_;
  uint32_t ahead_save_ms_;
  bool     ahead_saved_;   // ahead_save_ms_ is set
  uint8_t  hold_note_;     // why scheduled polling is waiting, logged on change
  bool     fold_full_logged_;   // an entry the last fold could not store was logged

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
  void foldSlots();
  void restoreStored(uint8_t idx);

  void startPoll(uint8_t idx, bool scheduled);
  void apply(poll::Step s);
  poll::Step feed(uint8_t ev, bool flooded);
  bool sendFor(const poll::Step& s);
  mesh::Packet* buildDatagram(uint8_t type, const mesh::LocalIdentity& sender, uint8_t idx,
                              const uint8_t* plain, size_t len);
  uint32_t nextTag();
  bool inflightIntact() const;
  void pushEvent(uint8_t ev, bool flooded);
  void processEvents();
  void recallForAdmin();
  void finishRun();
  void endRun();
  void abortRun(const char* why);

  uint8_t readStateFile(const char* name, poll::StateRecord* out);
  void loadState();
  bool saveState(const char* why);
  bool promoteRecovered();

  void cmdGetPoll(char* reply);
  void cmdGetStatus(const char* arg, char* reply);
  void cmdGetKey(char* reply);
  void cmdNow(const char* arg, char* reply);
  void cmdEnable(const char* arg, char* reply);
  bool enableRemoved(const char* arg, char* reply);

  // CoreScope control
  enum CtlOutcome : uint8_t { CTL_NEVER = 0, CTL_DONE, CTL_NO_SESSION, CTL_TIMEOUT };

  poll::Policy policy_;              // in use: the defaults, or the last 200 this boot
  bool     policy_answered_;         // the policy endpoint answered 200 or 404 this boot
  bool     gate_save_due_;           // the saved gate changed and is not on flash yet
  uint8_t  gate_now_;                // poll::Gate as last worked out
  poll::OwnerEntry owners_[kSlots];  // per slot; cleared when the owners reply lapses
  uint32_t owners_ok_at_;
  poll::ClaimTiming claim_;
  bool     claim_pending_;
  bool     claim_pending_community_;
  bool     claim_pending_listed_;    // the claim in flight names nodes
  uint32_t claim_pending_sig_;
  uint32_t claim_filed_ms_;
  uint32_t claim_sent_at_;           // last claim the bridge took this boot, 0 = none
  poll::RegistrationState reg_;
  mesh::LocalIdentity shared_id_;    // CoreScope's shared key, when one was fetched
  bool     has_shared_;
  bool     shared_renew_failed_;     // a renewal of the expired shared key failed: it is no longer used
  uint32_t shared_at_;
  uint8_t  key_in_use_;              // poll::KeyChoice
  uint8_t  acl_pub_[8];              // the key the cached secrets and login state belong to; zero = the stored own key
  uint32_t started_at_;              // first tick with a synced clock
  uint32_t gated_at_;                // last time the gate was seen closed
  uint32_t url_hash_;                // a new CoreScope starts its state over
  bool     has_url_;
  bool     community_;
  uint8_t  claim_slot_;
  bool     ctl_pending_;
  bool     ctl_deferred_;            // needed and allowed, held back until the claim in flight is answered
  bool     ctl_ever_;
  uint8_t  ctl_want_;
  uint32_t ctl_filed_ms_;
  uint32_t ctl_retry_ms_;            // a refused filing is tried again after this
  uint8_t  ctl_count_;               // targets in the pending request
  int8_t   ctl_slot_[poll::kMaxTargets];
  uint8_t  ctl_pfx_[poll::kMaxTargets][4];
  uint8_t  ctl_last_;                // CtlOutcome
  uint32_t ctl_last_at_;
  int16_t  ctl_status_[4];           // owners, policy, registration, shared key
  uint8_t  skip_[kSlots];            // poll::SkipReason of the last decision
  int8_t   now_idx_;                 // a `poll now` waiting on CoreScope, -1 = none
  uint8_t  now_pfx_[4];
  uint32_t now_ms_;
  uint16_t policy_covered_;          // slot bits: targets named in the request the policy in use answered
  uint8_t  ctl_note_;                // log-once notes
  uint8_t  key_note_;

  const mesh::LocalIdentity& pollKey() const { return key_in_use_ == poll::KEY_SHARED ? shared_id_ : poll_id_; }
  void initControl();
  void resetCorescope();
  uint32_t startupHoldS() const;
  uint32_t windowFor(uint8_t idx, uint32_t now) const;
  uint8_t gateAt(uint32_t now) const;
  bool startingAt(uint32_t now) const;
  bool anyTargetDue(uint32_t now);
  void readControlSettings(const PollSettings& s);
  void serviceControl();
  void takeControl(uint32_t now);
  void noSession(uint8_t outcome, uint32_t now);
  void sharedRenewalFailed(uint32_t now);
  void applyControl(const poll::ControlResult& res, uint32_t now);
  void discardUnasked();
  void fileControl(uint32_t now, bool registering, bool want_shared);
  void copyUrl(char* out, size_t cap);
  bool buildRegistration(poll::ControlRequest& req, uint32_t now);
  void maybeFileControl(uint32_t now);
  void takeClaim(uint32_t now);
  void maybeClaim(uint32_t now);
  void updateKey(uint32_t now);
  void bindKey();
  bool nowRefused(uint8_t idx, uint32_t now, char* reply, bool local_only);
  bool controlFileableNow() const;
  void resolveNow(uint32_t now);
  void detachSlot(uint8_t i);
  void scheduleTick(uint32_t now);
  poll::CycleInput cycleInputFor(uint8_t idx, bool test, uint32_t now) const;
  void noteSkip(uint8_t idx, const poll::Decision& d, uint32_t now);
  void fmtControl(char* out, size_t cap, uint32_t now) const;
  bool policyCovers(uint8_t idx) const { return (policy_covered_ >> idx) & 1U; }
};

#endif  // WITH_REPEATER_POLL
