#include "RepeaterPoller.h"

#ifdef WITH_REPEATER_POLL

#include <Arduino.h>
#include <Packet.h>
#include <Utils.h>
#include <helpers/MQTTConnectionPolicy.h>
#include <helpers/poll/PollPacketCodec.h>
#include <helpers/poll/PollSchedule.h>

#if defined(MQTT_DEBUG)
  #define POLL_LOG(F, ...) do { if (Serial.availableForWrite() > 0) { Serial.printf("POLL: " F "\n", ##__VA_ARGS__); } } while (0)
#else
  #define POLL_LOG(...) do {} while (0)
#endif

static_assert(PATH_HASH_SIZE == 1, "poll packets carry one-byte hashes");
static_assert(PUB_KEY_SIZE == poll::kPubKeySize, "key size");
static_assert(PRV_KEY_SIZE == poll::kPrvKeySize, "private key size");
static_assert(sizeof(RepeaterPoller) <= 3584, "repeater poller RAM budget");

namespace {

const char* const kStateFile = "/poll_state";
const char* const kStateTmp = "/poll_state.tmp";
const char* const kStateBak = "/poll_state.bak";

const uint32_t kTickMs = 5000;
const uint32_t kTxStuckMs = 30000;          // a packet neither sent nor failed by then was shed
const uint32_t kMinRequestListenMs = 10000;
const uint32_t kNowSpacingS = 60;            // `poll now` on one target at most this often
const uint8_t  kPoolReserve = 5;

// Bench-only hooks; no environment sets these. POLL_TEST_INTERVAL_S replaces
// the interval and window; POLL_TEST_DAY_S shortens the day behind the admin
// recheck, the route age and the status period.
#ifdef POLL_TEST_DAY_S
const uint32_t kPollDay = POLL_TEST_DAY_S;
#else
const uint32_t kPollDay = poll::kDay;
#endif
#ifdef POLL_TEST_INTERVAL_S
const uint32_t kScheduledGapS = (POLL_TEST_INTERVAL_S / 2) < 60 ? (POLL_TEST_INTERVAL_S / 2) : 60;
#else
const uint32_t kScheduledGapS = 60;
#endif

bool millisPassed(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

void hex8(char* out, const uint8_t* pub) { mesh::Utils::toHex(out, pub, 4); }

const char* actLabel(uint8_t act, uint8_t pw) {
  switch (act) {
    case poll::ACT_SEND_LOGIN: return pw == poll::PW_HELLO ? "login hello" : "login blank";
    case poll::ACT_SEND_TEACH: return "teach";
    case poll::ACT_SEND_REQUEST: return "request";
    case poll::ACT_SEND_SETPERM: return "setperm";
    default: return "?";
  }
}

void fmtAge(char* out, size_t cap, uint32_t secs) {
  if (secs < 120) snprintf(out, cap, "%lus", (unsigned long)secs);
  else if (secs < 7200) snprintf(out, cap, "%lum", (unsigned long)(secs / 60));
  else if (secs < 2 * 86400UL) snprintf(out, cap, "%luh", (unsigned long)(secs / 3600));
  else snprintf(out, cap, "%lud", (unsigned long)(secs / 86400UL));
}

bool isHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// One word of hex, no trailing junk; at most a full key.
bool hexWord(const char* s, size_t* len) {
  while (*s == ' ') s++;
  size_t n = 0;
  while (isHex(s[n])) n++;
  const char* rest = s + n;
  while (*rest == ' ') rest++;
  if (n == 0 || n > 64 || *rest != 0) return false;
  *len = n;
  return true;
}

bool hexPrefixMatches(const uint8_t* pub, const char* hex, size_t n) {
  char full[65];
  mesh::Utils::toHex(full, pub, 32);
  for (size_t i = 0; i < n; i++) {
    char a = hex[i];
    if (a >= 'a' && a <= 'f') a = (char)(a - 'a' + 'A');   // toHex writes uppercase
    if (a != full[i]) return false;
  }
  return true;
}

}  // namespace

RepeaterPoller::RepeaterPoller()
    : host_(NULL), fs_(NULL), has_id_(false), store_state_(poll::STATE_EMPTY), seq_(0),
      key_created_at_(0), tag_hw_(0), tag_hw_saved_(0), count_(0), enabled_(false), consent_(false),
      interval_min_(60), status_hours_(6), run_kind_(RUN_NONE), run_idx_(-1), run_scheduled_(false),
      run_quick_(false), run_learned_before_(0), poll_tag_count_(0), inflight_(NULL), inflight_ms_(0),
      inflight_act_(0), inflight_pw_(0), inflight_tag_(0), timer_on_(false), timer_at_(0),
      listening_(false), listen_act_(0), listen_pw_(0), listen_tag_(0), event_count_(0), rd_voltage_(false), rd_mv_(0),
      rd_temp_(false), rd_temp_c10_(0), rd_uptime_(0), rd_noise_(0), last_tick_ms_(0),
      last_scheduled_at_(0), id_retry_ms_(0UL - 60000UL), ntp_seen_(false), save_due_(false), hold_note_(0) {
  memset(&gate_, 0, sizeof(gate_));
  memset(stored_, 0, sizeof(stored_));
  memset(meta_, 0, sizeof(meta_));
  memset(states_, 0, sizeof(states_));
  memset(poll_tags_, 0, sizeof(poll_tags_));
  memset(events_, 0, sizeof(events_));
}

void RepeaterPoller::begin(RepeaterPollHost* host, FILESYSTEM* fs) {
  host_ = host;
  fs_ = fs;
  loadState();
  readSettings();
  last_tick_ms_ = millis();
}

// ---------------------------------------------------------------------------
// Flash record

uint8_t RepeaterPoller::readStateFile(const char* name, poll::StateRecord* out) {
  poll::stateClear(*out);
  if (!fs_ || !fs_->exists(name)) return poll::STATE_EMPTY;
  File f = fs_->open(name, "r");
  if (!f) return poll::STATE_EMPTY;
  uint8_t buf[poll::kStateRecordSize + 16];
  const size_t size = f.size();
  const size_t n = f.read(buf, sizeof(buf));
  f.close();
  uint8_t result;
  if (size > sizeof(buf)) {
    // Too big for this layout: a newer firmware's record keeps its file; anything else is junk.
    bool newer = n >= poll::kStateHeaderSize && poll::stateGet32(buf) == poll::kStateMagic &&
                 poll::stateGet16(buf + 4) > poll::kStateVersion;
    result = newer ? poll::STATE_NEWER : poll::STATE_CORRUPT;
  } else {
    result = poll::decodeState(buf, n, out);
  }
  memset(buf, 0, sizeof(buf));
  if (result != poll::STATE_OK && result != poll::STATE_EMPTY) {
    POLL_LOG("state %s: %s", name, poll::stateLoadName(result));
  }
  return result;
}

void RepeaterPoller::loadState() {
  const char* names[3] = {kStateFile, kStateTmp, kStateBak};
  uint8_t results[3];
  uint32_t seqs[3];
  poll::StateRecord r;
  // Two passes keep one record on the stack: rank the copies, then read the winner again.
  for (uint8_t i = 0; i < 3; i++) {
    results[i] = readStateFile(names[i], &r);
    seqs[i] = r.seq;
  }
  uint8_t why = poll::STATE_EMPTY;
  int best = poll::pickState(results, seqs, 3, &why);
  store_state_ = why;
  if (best < 0 || readStateFile(names[best], &r) != poll::STATE_OK) {
    poll::stateClear(r);
    POLL_LOG("state: %s, starting fresh%s", poll::stateLoadName(why),
             why == poll::STATE_NEWER ? " (read-only, polling disabled)" : "");
    return;
  }
  seq_ = r.seq;
  key_created_at_ = r.key_created_at;
  tag_hw_ = r.tag_high_water;
  tag_hw_saved_ = tag_hw_;
  gate_ = r.gate;
  memcpy(stored_, r.targets, sizeof(stored_));
  if (r.has_key) {
    uint8_t keys[PRV_KEY_SIZE + PUB_KEY_SIZE];
    memcpy(keys, r.prv, PRV_KEY_SIZE);
    memcpy(keys + PRV_KEY_SIZE, r.pub, PUB_KEY_SIZE);
    poll_id_.readFrom(keys, sizeof(keys));
    memset(keys, 0, sizeof(keys));
    has_id_ = true;
  }
  memset(r.prv, 0, sizeof(r.prv));
  char pfx[9];
  hex8(pfx, poll_id_.pub_key);
  POLL_LOG("state loaded from %s: seq=%lu key=%s tag_hw=%lu", names[best], (unsigned long)seq_,
           has_id_ ? pfx : "none", (unsigned long)tag_hw_);
  // Rewrite after an interrupted swap, once the targets are known, so one file remains.
  if (best != 0) save_due_ = true;
}

bool RepeaterPoller::saveState(const char* why) {
  if (!fs_ || store_state_ == poll::STATE_NEWER) return false;

  poll::StateRecord r;
  uint8_t buf[poll::kStateRecordSize];
  poll::stateClear(r);
  r.seq = seq_ + 1;
  r.has_key = has_id_;
  if (has_id_) {
    uint8_t keys[PRV_KEY_SIZE + PUB_KEY_SIZE];
    poll_id_.writeTo(keys, sizeof(keys));
    memcpy(r.prv, keys, PRV_KEY_SIZE);
    memcpy(r.pub, keys + PRV_KEY_SIZE, PUB_KEY_SIZE);
    memset(keys, 0, sizeof(keys));
  }
  r.key_created_at = key_created_at_;
  r.tag_high_water = tag_hw_;
  r.gate = gate_;

  // Current targets first, then admin stops of removed targets while room remains.
  uint8_t n = 0;
  for (uint8_t pos = 0; pos < count_ && n < poll::kMaxTargets; pos++) {
    for (uint8_t i = 0; i < kSlots; i++) {
      if (!meta_[i].used || meta_[i].orphan || meta_[i].list_pos != pos) continue;
      poll::StoredTarget& s = r.targets[n++];
      memcpy(s.pub, meta_[i].pub, poll::kPubKeySize);
      s.admin_seen = states_[i].admin_seen;
      s.admin_removed = states_[i].admin_removed;
      s.learned_pw = states_[i].learned_pw;
      break;
    }
  }
  for (uint8_t k = 0; k < poll::kMaxTargets && n < poll::kMaxTargets; k++) {
    const poll::StoredTarget& old = stored_[k];
    if (!old.admin_seen) continue;
    bool present = false;
    for (uint8_t j = 0; j < n; j++) present = present || memcmp(r.targets[j].pub, old.pub, poll::kPubKeySize) == 0;
    if (!present) r.targets[n++] = old;
  }

  const size_t len = poll::encodeState(r, buf, sizeof(buf));
  memset(r.prv, 0, sizeof(r.prv));
  if (len == 0) return false;

  bool ok = false;
  File f = fs_->open(kStateTmp, "w", true);
  if (f) {
    ok = f.write(buf, len) == len;
    f.close();
  }
  memset(buf, 0, sizeof(buf));
  // SPIFFS will not rename onto an existing name: park the old copy, publish, then drop it.
  if (ok && fs_->exists(kStateFile)) {
    if (fs_->exists(kStateBak)) fs_->remove(kStateBak);
    ok = fs_->rename(kStateFile, kStateBak);
  }
  if (ok) ok = fs_->rename(kStateTmp, kStateFile);
  if (ok && fs_->exists(kStateBak)) fs_->remove(kStateBak);
  if (!ok) {
    POLL_LOG("state save failed (%s)", why);
    return false;
  }
  seq_ = r.seq;
  store_state_ = poll::STATE_OK;
  tag_hw_saved_ = tag_hw_;
  save_due_ = false;
  memcpy(stored_, r.targets, sizeof(stored_));
  POLL_LOG("state saved seq=%lu (%s)", (unsigned long)seq_, why);
  return true;
}

void RepeaterPoller::flush() {
  if (tag_hw_ != tag_hw_saved_ || save_due_) saveState("flush");
}

bool RepeaterPoller::ensureIdentity() {
  if (has_id_) return true;
  if (!host_ || store_state_ == poll::STATE_NEWER) return false;
  const uint8_t node_hash = host_->pollNodeHash();
  mesh::LocalIdentity id(host_->pollRng());
  // Reserved hashes, and the node's own: a reply to the polling key must never look like one to the node.
  for (uint8_t tries = 0; tries < 20; tries++) {
    const uint8_t h = id.pub_key[0];
    if (h != 0x00 && h != 0xFF && h != node_hash) break;
    id = mesh::LocalIdentity(host_->pollRng());
  }
  const uint8_t h = id.pub_key[0];
  if (h == 0x00 || h == 0xFF || h == node_hash) return false;
  poll_id_ = id;
  has_id_ = true;
  key_created_at_ = host_->pollNow();
  forgetKeyState();
  char pfx[9];
  hex8(pfx, poll_id_.pub_key);
  POLL_LOG("polling key created %s", pfx);
  if (!saveState("key created")) {
    // Without a saved key the next boot would make another; do not poll under this one.
    has_id_ = false;
    return false;
  }
  return true;
}

// What the plugin's _forget_other_key does: nothing learned under another key carries over.
void RepeaterPoller::forgetKeyState() {
  for (uint8_t i = 0; i < kSlots; i++) {
    poll::TargetState& t = states_[i];
    t.logged_in_at = 0;
    t.login_checked_at = 0;
    t.learned_pw = poll::LEARNED_NONE;
    t.key_login_fails = 0;
    t.flood_replies = 0;
    meta_[i].secret_ok = false;
  }
  for (uint8_t k = 0; k < poll::kMaxTargets; k++) stored_[k].learned_pw = poll::LEARNED_NONE;
}

// ---------------------------------------------------------------------------
// Settings and targets

int8_t RepeaterPoller::slotFor(const uint8_t* pub) const {
  for (uint8_t i = 0; i < kSlots; i++) {
    if (meta_[i].used && memcmp(meta_[i].pub, pub, poll::kPubKeySize) == 0) return (int8_t)i;
  }
  return -1;
}

void RepeaterPoller::restoreStored(uint8_t idx) {
  for (uint8_t k = 0; k < poll::kMaxTargets; k++) {
    if (memcmp(stored_[k].pub, meta_[idx].pub, poll::kPubKeySize) != 0) continue;
    states_[idx].admin_seen = stored_[k].admin_seen;
    states_[idx].admin_removed = stored_[k].admin_removed;
    states_[idx].learned_pw = stored_[k].learned_pw;
    if (stored_[k].admin_seen) states_[idx].code = poll::CODE_ADMIN_STOP;
    return;
  }
}

void RepeaterPoller::readSettings() {
  PollSettings s;
  memset(&s, 0, sizeof(s));
  host_->pollReadSettings(s);
  if (s.count > poll::kMaxTargets) s.count = poll::kMaxTargets;
  enabled_ = s.enabled;
  consent_ = s.consent;
  interval_min_ = s.interval_min ? s.interval_min : 60;
  status_hours_ = s.status_hours;
  count_ = s.count;

  // Drop targets no longer listed; a running drill keeps its slot until it ends.
  for (uint8_t i = 0; i < kSlots; i++) {
    if (!meta_[i].used) continue;
    bool listed = false;
    for (uint8_t p = 0; p < s.count && !listed; p++) {
      listed = memcmp(s.targets[p], meta_[i].pub, poll::kPubKeySize) == 0;
    }
    if (listed) continue;
    if (run_idx_ == (int8_t)i && run_kind_ == RUN_DRILL) {
      meta_[i].orphan = true;
      continue;
    }
    if (run_idx_ == (int8_t)i && run_kind_ == RUN_EXCHANGE) abortRun("target removed");
    memset(&meta_[i], 0, sizeof(meta_[i]));
    memset(&states_[i], 0, sizeof(states_[i]));
  }
  // Add new ones and refresh the per-target settings.
  for (uint8_t p = 0; p < s.count; p++) {
    int8_t idx = slotFor(s.targets[p]);
    if (idx < 0) {
      for (uint8_t i = 0; i < kSlots; i++) {
        if (!meta_[i].used) {
          idx = (int8_t)i;
          break;
        }
      }
      if (idx < 0) break;   // every slot holds a listed target or a running drill
      memset(&meta_[idx], 0, sizeof(meta_[idx]));
      memset(&states_[idx], 0, sizeof(states_[idx]));
      meta_[idx].used = true;
      memcpy(meta_[idx].pub, s.targets[p], poll::kPubKeySize);
      states_[idx].temperature_c10 = INT16_MIN;
      restoreStored((uint8_t)idx);
    }
    meta_[idx].orphan = false;
    meta_[idx].list_pos = p;
    meta_[idx].pw_mode = s.pw_mode[p] <= poll::PWMODE_HELLO ? s.pw_mode[p] : (uint8_t)poll::PWMODE_AUTO;
  }

  // Phase spread over the targets sorted by key, among those sharing an interval.
  for (uint8_t i = 0; i < kSlots; i++) {
    if (!meta_[i].used || meta_[i].orphan) continue;
    meta_[i].interval_s = intervalFor(controlFor(i));
  }
  for (uint8_t i = 0; i < kSlots; i++) {
    if (!meta_[i].used || meta_[i].orphan) continue;
    uint8_t rank = 0, same = 0;
    for (uint8_t j = 0; j < kSlots; j++) {
      if (!meta_[j].used || meta_[j].orphan || meta_[j].interval_s != meta_[i].interval_s) continue;
      same++;
      if (memcmp(meta_[j].pub, meta_[i].pub, poll::kPubKeySize) < 0) rank++;
    }
    meta_[i].phase_s = poll::spreadPhase(meta_[i].interval_s, rank, same);
  }
}

PollControl RepeaterPoller::controlFor(uint8_t idx) const {
  (void)idx;
  PollControl c;
  c.role = poll::ROLE_POLLER;
  c.gate = poll::GATE_OPEN;
#ifdef POLL_TEST_INTERVAL_S
  c.window_s = POLL_TEST_INTERVAL_S;
#else
  c.window_s = poll::kDirectDefaultS;
#endif
  c.max_requests = poll::kMaxRequestsDefault;
  c.own_key = true;
  return c;
}

uint32_t RepeaterPoller::intervalFor(const PollControl& c) const {
#ifdef POLL_TEST_INTERVAL_S
  (void)c;
  return POLL_TEST_INTERVAL_S;
#else
  uint32_t local = (uint32_t)interval_min_ * 60UL;
  return local > c.window_s ? local : c.window_s;
#endif
}

bool RepeaterPoller::clockOk() {
  if (!ntp_seen_ && host_->pollNtpSeen()) ntp_seen_ = true;
  return ntp_seen_ && host_->pollNow() > MQTTConnectionPolicy::kSyncedClockEpoch;
}

bool RepeaterPoller::directRoute(uint8_t idx, int8_t* snr_x4) {
  uint32_t heard = 0;
  int8_t snr = 0;
  if (!host_->pollNeighbor(meta_[idx].pub, &heard, &snr) || heard == 0) return false;
  uint32_t now = host_->pollNow();
  if (heard > now || now - heard > kPollDay) return false;
  if (snr_x4) *snr_x4 = snr;
  return true;
}

void RepeaterPoller::note(uint8_t idx, uint8_t what, uint32_t detail) {
  if (meta_[idx].note == what) return;
  meta_[idx].note = what;
  char pfx[9];
  hex8(pfx, meta_[idx].pub);
  switch (what) {
    case NOTE_DUE: POLL_LOG("%s due", pfx); break;
    case NOTE_CAP: POLL_LOG("%s cap reached, next in %lus", pfx, (unsigned long)detail); break;
    case NOTE_NOT_DIRECT: POLL_LOG("%s not_direct: not heard as a neighbor in the last day", pfx); break;
    case NOTE_ADMIN: POLL_LOG("%s admin stop: not polled until `poll enable`", pfx); break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// Loop

void RepeaterPoller::loop() {
  if (!host_) return;
  processEvents();

  const uint32_t now_ms = millis();
  if (inflight_ && (uint32_t)(now_ms - inflight_ms_) >= kTxStuckMs) {
    if (host_->pollIsTransmitting(inflight_)) {
      inflight_ms_ = now_ms;   // on the air right now: its report is coming
    } else {
      bool cancelled = host_->pollCancelQueued(inflight_);
      POLL_LOG("tx %s %s after %lus", actLabel(inflight_act_, inflight_pw_),
               cancelled ? "cancelled" : "shed by the queue", (unsigned long)(kTxStuckMs / 1000));
      inflight_ = NULL;
      pushEvent(poll::EV_TX_FAILED, false);
      processEvents();
    }
  }

  if (timer_on_ && millisPassed(millis(), timer_at_)) {
    timer_on_ = false;
    if (listening_) {
      listening_ = false;
      POLL_LOG("timeout %s tag=%lu", actLabel(listen_act_, listen_pw_), (unsigned long)listen_tag_);
    }
    apply(feed(poll::EV_TIMER, false));
  }

  if ((uint32_t)(millis() - last_tick_ms_) >= kTickMs) {
    last_tick_ms_ = millis();
    tick();
  }
}

void RepeaterPoller::tick() {
  readSettings();
  if (enabled_ && consent_ && !has_id_ && (uint32_t)(millis() - id_retry_ms_) >= 60000UL) {
    id_retry_ms_ = millis();
    ensureIdentity();
  }
  if (busy()) return;
  if (save_due_ || tag_hw_ - tag_hw_saved_ > kPollDay) saveState("tag high-water");
  if (!enabled_ || !has_id_ || store_state_ == poll::STATE_NEWER) return;
  const uint8_t hold = !clockOk() ? 1 : host_->pollDiscoveryBusy() ? 2 : 0;
  if (hold != hold_note_) {
    hold_note_ = hold;
    if (hold == 1) POLL_LOG("waiting for an NTP-synced clock");
    else if (hold == 2) POLL_LOG("waiting for neighbor discovery to finish");
  }
  if (hold) return;

  const uint32_t now = host_->pollNow();
  bool due[kSlots];
  for (uint8_t i = 0; i < kSlots; i++) {
    due[i] = false;
    if (!meta_[i].used || meta_[i].orphan) continue;
    poll::TargetState& t = states_[i];
    if (t.admin_seen) {
      note(i, NOTE_ADMIN, 0);
      continue;
    }
    const PollControl c = controlFor(i);
    if (c.role != poll::ROLE_POLLER || c.gate != poll::GATE_OPEN) continue;
    if (!directRoute(i, NULL)) {
      t.code = poll::CODE_NOT_DIRECT;
      note(i, NOTE_NOT_DIRECT, 0);
      continue;
    }
    if (t.next_due_at > now) continue;
    if (!poll::isDue(t, meta_[i].interval_s, now, meta_[i].phase_s)) {
      note(i, NOTE_WAIT, 0);
      continue;
    }
    uint32_t until = 0;
    if (poll::capReached(t, c.window_s, c.max_requests, now, &until)) {
      t.next_due_at = until;
      note(i, NOTE_CAP, until > now ? until - now : 0);
      continue;
    }
    note(i, NOTE_DUE, 0);
    due[i] = true;
  }
  int pick = poll::pickDue(states_, due, kSlots, now, last_scheduled_at_, kScheduledGapS);
  if (pick >= 0) startPoll((uint8_t)pick, true);
}

// ---------------------------------------------------------------------------
// Running an exchange

void RepeaterPoller::startPoll(uint8_t idx, bool scheduled) {
  poll::TargetState& t = states_[idx];
  const uint32_t now = host_->pollNow();
  const PollControl c = controlFor(idx);

  if (!meta_[idx].secret_ok) {
    poll_id_.calcSharedSecret(meta_[idx].secret, meta_[idx].pub);
    meta_[idx].secret_ok = true;
  }

  run_quick_ = t.code == poll::CODE_NO_REPLY && poll::elapsedSince(t.last_success, now) < poll::kDay &&
               t.no_reply_count < poll::kNoReplyFastTries;
  poll::recordSend(t, now);
  t.next_due_at = 0;
  meta_[idx].note = NOTE_NONE;

#ifdef POLL_TEST_DAY_S
  // Clearing a stamp makes the machine treat that check as due.
  if (poll::secondsSince(now, t.login_checked_at) >= kPollDay) t.login_checked_at = 0;
  if (poll::secondsSince(now, t.route_sent_at) >= kPollDay) t.route_sent_at = 0;
  {
    uint16_t h = status_hours_ ? status_hours_ : poll::kDefaultStatusHours;
    if (poll::secondsSince(now, t.status_checked_at) >= (uint32_t)h * kPollDay / 24) t.status_checked_at = 0;
  }
#endif

  poll::ExchangeConfig cfg = poll::defaultExchangeConfig();
  cfg.pw_mode = meta_[idx].pw_mode;
  cfg.personal = c.own_key;
  cfg.status_hours = status_hours_;
  uint32_t listen = host_->pollReplyWindowMs();
  cfg.timing.request_listen_ms = listen > kMinRequestListenMs ? listen : kMinRequestListenMs;

  run_kind_ = RUN_EXCHANGE;
  run_idx_ = (int8_t)idx;
  run_scheduled_ = scheduled;
  run_learned_before_ = t.learned_pw;
  poll_tag_count_ = 0;
  event_count_ = 0;
  rd_voltage_ = rd_temp_ = false;
  rd_mv_ = 0;
  rd_temp_c10_ = INT16_MIN;
  rd_uptime_ = 0;
  rd_noise_ = 0;

  poll::Step s = exchange_.start(t, cfg, now);
  const poll::ExchangeResult& r = exchange_.result();
  char pfx[9];
  hex8(pfx, meta_[idx].pub);
  POLL_LOG("start %s %s %s path%s%s", pfx, r.kind == poll::REQ_STATUS ? "status" : "telemetry",
           r.login_tried ? "login" : "fast", scheduled ? "" : " (poll now)", run_quick_ ? " (quick retry)" : "");
  apply(s);
}

poll::Step RepeaterPoller::feed(uint8_t ev, bool flooded) {
  const uint32_t now = host_->pollNow();
  if (run_kind_ == RUN_EXCHANGE) return exchange_.onEvent(states_[run_idx_], ev, flooded, now);
  if (run_kind_ == RUN_DRILL) return drill_.onEvent(ev, flooded, now);
  return poll::stepNoChange();
}

void RepeaterPoller::apply(poll::Step s) {
  // A machine never asks for more than a handful of steps without waiting on the radio or a timer.
  for (uint8_t guard = 0; guard < 16 && run_kind_ != RUN_NONE; guard++) {
    if (poll::isNoChange(s)) return;
    timer_on_ = false;
    listening_ = false;
    if (s.act == poll::ACT_WAIT) {
      timer_on_ = true;
      timer_at_ = millis() + s.wait_ms;
      POLL_LOG("wait %lu ms", (unsigned long)s.wait_ms);
      return;
    }
    if (s.act == poll::ACT_FINISH) {
      finishRun();
      return;
    }
    if (sendFor(s)) return;
    s = feed(poll::EV_TX_FAILED, false);
  }
}

uint32_t RepeaterPoller::nextTag() {
  // A target drops any timestamp not above the last it accepted from this key, reboots included.
  uint32_t v = host_->pollNowUnique();
  if (v <= tag_hw_) v = tag_hw_ + 1;
  tag_hw_ = v;
  return v;
}

mesh::Packet* RepeaterPoller::buildDatagram(uint8_t type, const mesh::LocalIdentity& sender, uint8_t idx,
                                            const uint8_t* plain, size_t len) {
  // Mesh::createDatagram would write the node's hash as the source, so build it here.
  if (len + 2 + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE > MAX_PACKET_PAYLOAD) return NULL;
  mesh::Packet* pkt = host_->pollObtainPacket();
  if (!pkt) return NULL;
  pkt->header = (uint8_t)(type << PH_TYPE_SHIFT);
  pkt->payload[0] = meta_[idx].pub[0];
  pkt->payload[1] = sender.pub_key[0];
  int n = mesh::Utils::encryptThenMAC(meta_[idx].secret, &pkt->payload[2], plain, (int)len);
  if (n <= 0) {
    host_->pollReleasePacket(pkt);
    return NULL;
  }
  pkt->payload_len = (uint16_t)(2 + n);
  return pkt;
}

bool RepeaterPoller::sendFor(const poll::Step& s) {
  const uint8_t idx = (uint8_t)run_idx_;
  poll::TargetState& t = states_[idx];
  uint8_t plain[96];
  uint8_t rnd[4];
  size_t n = 0;
  uint32_t tag = 0;
  mesh::Packet* pkt = NULL;

  switch (s.act) {
    case poll::ACT_SEND_LOGIN: {
      tag = nextTag();
      n = poll::buildLoginPlain(plain, sizeof(plain), tag, s.pw);
      if (n) {
        poll::rememberTag(t, tag);
        mesh::Identity target(meta_[idx].pub);
        pkt = host_->pollCreateLogin(poll_id_, target, meta_[idx].secret, plain, n);
      }
      break;
    }
    case poll::ACT_SEND_REQUEST: {
      tag = nextTag();
      host_->pollRng()->random(rnd, sizeof(rnd));
      uint8_t req = s.kind == poll::REQ_STATUS ? poll::kReqStatus : poll::kReqTelemetry;
      n = poll::buildRequestPlain(plain, sizeof(plain), tag, req, rnd);
      if (n) {
        poll::rememberTag(t, tag);
        if (poll_tag_count_ < kMaxPollTags) {
          poll_tags_[poll_tag_count_++] = tag;
        } else {
          memmove(poll_tags_, poll_tags_ + 1, sizeof(poll_tags_[0]) * (kMaxPollTags - 1));
          poll_tags_[kMaxPollTags - 1] = tag;
        }
        pkt = buildDatagram(PAYLOAD_TYPE_REQ, poll_id_, idx, plain, n);
      }
      break;
    }
    case poll::ACT_SEND_TEACH: {
      host_->pollRng()->random(rnd, sizeof(rnd));
      n = poll::buildTeachPlain(plain, sizeof(plain), NULL, 0, rnd);
      if (n) pkt = buildDatagram(PAYLOAD_TYPE_PATH, poll_id_, idx, plain, n);
      break;
    }
    case poll::ACT_SEND_SETPERM: {
      tag = nextTag();
      n = poll::buildSetPermPlain(plain, sizeof(plain), tag, poll_id_.pub_key);
      if (n) {
        poll::rememberTag(t, tag);
        pkt = buildDatagram(PAYLOAD_TYPE_TXT_MSG, poll_id_, idx, plain, n);
      }
      break;
    }
    default:
      break;
  }
  memset(plain, 0, sizeof(plain));

  if (!pkt) {
    POLL_LOG("tx %s not queued: %s", actLabel(s.act, s.pw), n ? "packet pool low" : "build failed");
    return false;
  }
  host_->pollSendDirect(pkt);
  inflight_ = pkt;
  inflight_ms_ = millis();
  inflight_act_ = s.act;
  inflight_pw_ = s.pw;
  inflight_tag_ = tag;
  if (tag) POLL_LOG("tx %s tag=%lu queued", actLabel(s.act, s.pw), (unsigned long)tag);
  else POLL_LOG("tx %s queued", actLabel(s.act, s.pw));
  return true;
}

void RepeaterPoller::pushEvent(uint8_t ev, bool flooded) {
  if (event_count_ >= kEventQueue) {
    // Never drop a TX report: the machines wait for it. Replies beyond this are duplicates.
    if (ev != poll::EV_TX_DONE && ev != poll::EV_TX_FAILED) return;
    memmove(events_, events_ + 1, sizeof(events_[0]) * (kEventQueue - 1));
    event_count_--;
  }
  events_[event_count_].ev = ev;
  events_[event_count_].flooded = flooded;
  event_count_++;
}

void RepeaterPoller::processEvents() {
  while (event_count_ > 0 && run_kind_ != RUN_NONE) {
    Event e = events_[0];
    memmove(events_, events_ + 1, sizeof(events_[0]) * (kEventQueue - 1));
    event_count_--;
    poll::Step s = feed(e.ev, e.flooded);
    const bool listen = e.ev == poll::EV_TX_DONE &&
                        (inflight_act_ == poll::ACT_SEND_LOGIN || inflight_act_ == poll::ACT_SEND_REQUEST);
    apply(s);
    if (listen && s.act == poll::ACT_WAIT && !poll::isNoChange(s) && run_kind_ != RUN_NONE) {
      listening_ = true;
      listen_act_ = inflight_act_;
      listen_pw_ = inflight_pw_;
      listen_tag_ = inflight_tag_;
    }
  }
  if (run_kind_ == RUN_NONE) event_count_ = 0;
}

void RepeaterPoller::onTx(const mesh::Packet* pkt) {
  if (!pkt || pkt != inflight_) return;
  inflight_ = NULL;
  if (inflight_tag_) POLL_LOG("tx %s tag=%lu sent", actLabel(inflight_act_, inflight_pw_), (unsigned long)inflight_tag_);
  else POLL_LOG("tx %s sent", actLabel(inflight_act_, inflight_pw_));
  pushEvent(poll::EV_TX_DONE, false);
}

void RepeaterPoller::onTxFail(const mesh::Packet* pkt) {
  if (!pkt || pkt != inflight_) return;
  inflight_ = NULL;
  POLL_LOG("tx %s failed", actLabel(inflight_act_, inflight_pw_));
  pushEvent(poll::EV_TX_FAILED, false);
}

void RepeaterPoller::onRx(const mesh::Packet* pkt) {
  if (run_kind_ == RUN_NONE || !has_id_ || !pkt) return;
  const uint8_t idx = (uint8_t)run_idx_;
  const uint8_t type = pkt->getPayloadType();
  if (!poll::isCandidateReply(type, pkt->payload, pkt->payload_len, poll_id_.pub_key[0], meta_[idx].pub[0])) return;
  const bool expect_login = run_kind_ == RUN_EXCHANGE ? exchange_.expectsLogin() : drill_.expectsLogin();
  const bool expect_data = run_kind_ == RUN_EXCHANGE && exchange_.expectsData();
  if (!expect_login && !expect_data) return;

  // Decrypt a copy; the packet itself goes on to the bridge untouched.
  uint8_t plain[MAX_PACKET_PAYLOAD];
  int n = mesh::Utils::MACThenDecrypt(meta_[idx].secret, plain, &pkt->payload[2], pkt->payload_len - 2);
  if (n <= 0) return;
  const uint8_t* body = plain;
  size_t body_len = (size_t)n;
  if (type == PAYLOAD_TYPE_PATH && !poll::unwrapPathResponse(plain, (size_t)n, &body, &body_len)) return;
  poll::Reply r;
  if (!poll::splitReply(body, body_len, &r)) return;

  poll::TargetState& t = states_[idx];
  const uint8_t cls = poll::classifyReply(r, t, poll_tags_, poll_tag_count_, expect_login, expect_data);
  const bool flooded = pkt->isRouteFlood();
  const float snr = pkt->getSNR();
  switch (cls) {
    case poll::REPLY_DATA:
      if (exchange_.result().kind == poll::REQ_STATUS) {
        poll::StatusReading st;
        if (poll::parseStatus(r.data, r.len, &st)) {
          rd_voltage_ = true;
          rd_mv_ = st.battery_mv;
          rd_uptime_ = st.has_uptime ? st.uptime_secs : 0;
          rd_noise_ = st.noise_floor;
        }
      } else {
        poll::TelemetryReading tl;
        if (poll::parseTelemetry(r.data, r.len, &tl)) {
          rd_voltage_ = tl.has_voltage;
          rd_mv_ = tl.battery_mv;
          rd_temp_ = tl.has_temperature;
          rd_temp_c10_ = tl.temperature_c10;
        }
      }
      POLL_LOG("rx data tag=%lu %s snr=%.2f len=%u", (unsigned long)r.tag, flooded ? "flooded" : "direct", snr,
               (unsigned)r.len);
      pushEvent(poll::EV_DATA_REPLY, flooded);
      break;
    case poll::REPLY_LOGIN_GUEST:
    case poll::REPLY_LOGIN_ADMIN:
      POLL_LOG("rx login %s tag=%lu %s snr=%.2f", cls == poll::REPLY_LOGIN_ADMIN ? "admin" : "guest",
               (unsigned long)r.tag, flooded ? "flooded" : "direct", snr);
      pushEvent(cls == poll::REPLY_LOGIN_ADMIN ? poll::EV_LOGIN_ADMIN : poll::EV_LOGIN_GUEST, flooded);
      break;
    default:
      break;
  }
  memset(plain, 0, sizeof(plain));
}

void RepeaterPoller::finishRun() {
  const uint8_t idx = (uint8_t)run_idx_;
  poll::TargetState& t = states_[idx];
  const uint32_t now = host_->pollNow();
  char pfx[9];
  hex8(pfx, meta_[idx].pub);

  if (run_kind_ == RUN_DRILL) {
    t.admin_removed = drill_.removed();
    POLL_LOG("admin drill done %s: %s (setperm %s, rounds %u, logins %u, teaches %u, admin answers %u)", pfx,
             drill_.removed() ? "removed" : "NOT confirmed", drill_.setpermSent() ? "sent" : "not sent",
             (unsigned)drill_.rounds(), (unsigned)drill_.logins(), (unsigned)drill_.teaches(),
             (unsigned)drill_.adminAnswers());
    saveState("admin drill");
    endRun();
    return;
  }

  const poll::ExchangeResult& r = exchange_.result();
  uint8_t code;
  if (r.admin) {
    code = poll::CODE_ADMIN_STOP;
  } else if (r.got_reply && rd_voltage_) {
    code = rd_mv_ > 0 ? poll::CODE_OK : poll::CODE_NO_BATTERY;
  } else if (r.reason == poll::XR_NOTHING_SENT) {
    code = poll::CODE_ERROR;
  } else {
    code = poll::CODE_NO_REPLY;
  }
  poll::recordResult(t, code, now, run_quick_);
  if (code == poll::CODE_OK || code == poll::CODE_NO_BATTERY) {
    t.battery_mv = rd_mv_;
    if (rd_temp_) t.temperature_c10 = rd_temp_c10_;
    if (rd_uptime_) t.uptime_secs = rd_uptime_;
    if (r.kind == poll::REQ_STATUS) t.noise_floor = rd_noise_;
  }
  if (run_scheduled_) last_scheduled_at_ = now;

  char reading[40] = "";
  if (code == poll::CODE_OK || code == poll::CODE_NO_BATTERY) {
    if (rd_temp_) {
      snprintf(reading, sizeof(reading), " %umV %.1fC", (unsigned)rd_mv_, rd_temp_c10_ / 10.0f);
    } else {
      snprintf(reading, sizeof(reading), " %umV", (unsigned)rd_mv_);
    }
  }
  POLL_LOG("done %s code=%s (%s) tx login=%u teach=%u req=%u%s%s", pfx, poll::codeName(code),
           poll::exchangeReasonName(r.reason), (unsigned)r.logins, (unsigned)r.teaches, (unsigned)r.requests,
           r.reply_flooded ? " reply flooded" : "", reading);

  if (r.admin) {
    t.admin_seen = true;
    t.admin_removed = false;
    note(idx, NOTE_ADMIN, 0);
    saveState("admin stop");
    POLL_LOG("admin drill start %s", pfx);
    run_kind_ = RUN_DRILL;
    event_count_ = 0;
    apply(drill_.start(now));
    return;
  }
  if (t.learned_pw != run_learned_before_) save_due_ = true;
  endRun();
  if (save_due_) saveState("learned password");
}

void RepeaterPoller::endRun() {
  const int8_t idx = run_idx_;
  run_kind_ = RUN_NONE;
  run_idx_ = -1;
  timer_on_ = false;
  listening_ = false;
  event_count_ = 0;
  inflight_ = NULL;
  if (idx >= 0 && meta_[idx].orphan) {
    memset(&meta_[idx], 0, sizeof(meta_[idx]));
    memset(&states_[idx], 0, sizeof(states_[idx]));
  }
}

void RepeaterPoller::abortRun(const char* why) {
  if (run_kind_ == RUN_NONE) return;
  if (inflight_ && !host_->pollCancelQueued(inflight_)) {
    // Already on the air; its report is ignored once the run is gone.
  }
  POLL_LOG("aborted: %s", why);
  endRun();
}

// ---------------------------------------------------------------------------
// Commands

void RepeaterPoller::summary(uint8_t* ok, uint8_t* total) const {
  uint8_t n_ok = 0, n = 0;
  for (uint8_t i = 0; i < kSlots; i++) {
    if (!meta_[i].used || meta_[i].orphan) continue;
    n++;
    if (states_[i].code == poll::CODE_OK || states_[i].code == poll::CODE_NO_BATTERY) n_ok++;
  }
  if (ok) *ok = n_ok;
  if (total) *total = n;
}

int8_t RepeaterPoller::findTarget(const char* hex, int* matches) const {
  size_t n = 0;
  *matches = 0;
  if (!hexWord(hex, &n)) return -1;
  while (*hex == ' ') hex++;
  int8_t found = -1;
  for (uint8_t i = 0; i < kSlots; i++) {
    if (!meta_[i].used || meta_[i].orphan) continue;
    if (!hexPrefixMatches(meta_[i].pub, hex, n)) continue;
    (*matches)++;
    found = (int8_t)i;
  }
  return *matches == 1 ? found : -1;
}

bool RepeaterPoller::handleCommand(const char* command, char* reply) {
  if (!host_) return false;
  if (strncmp(command, "reboot", 6) == 0 || strncmp(command, "clkreboot", 9) == 0) {
    flush();
    return false;   // the reboot itself is CommonCLI's
  }
  if (strcmp(command, "get poll") == 0) {
    cmdGetPoll(reply);
  } else if (strcmp(command, "get poll.key") == 0) {
    cmdGetKey(reply);
  } else if (strcmp(command, "get poll.status") == 0 || strncmp(command, "get poll.status ", 16) == 0) {
    cmdGetStatus(command + 15, reply);
  } else if (strncmp(command, "poll now", 8) == 0 && (command[8] == ' ' || command[8] == 0)) {
    cmdNow(command + 8, reply);
  } else if (strncmp(command, "poll enable", 11) == 0 && (command[11] == ' ' || command[11] == 0)) {
    cmdEnable(command + 11, reply);
  } else {
    return false;
  }
  return true;
}

void RepeaterPoller::cmdGetPoll(char* reply) {
  readSettings();
  uint8_t ok = 0, total = 0;
  summary(&ok, &total);
  // The saved gate is kept in flash for CoreScope control; until then the gate is always open.
  snprintf(reply, 160, "> %s, targets %u (%u ok), key %s, gate %s, clock %s%s%s", enabled_ ? "on" : "off",
           (unsigned)total, (unsigned)ok, has_id_ ? "own" : "none", "open", clockOk() ? "ok" : "not synced",
           busy() ? ", polling now" : "",
           store_state_ == poll::STATE_NEWER ? ", state from newer firmware (read-only)" : "");
}

void RepeaterPoller::cmdGetKey(char* reply) {
  if (!has_id_) {
    strcpy(reply, "Error: no polling key yet (set poll on)");
    return;
  }
  reply[0] = '>';
  reply[1] = ' ';
  mesh::Utils::toHex(&reply[2], poll_id_.pub_key, PUB_KEY_SIZE);
}

void RepeaterPoller::cmdGetStatus(const char* arg, char* reply) {
  readSettings();
  while (*arg == ' ') arg++;
  const uint32_t now = host_->pollNow();

  if (*arg == 0) {
    // One short entry per target; `get poll.status <n>` has the details.
    int pos = snprintf(reply, 160, "> %u:", (unsigned)count_);
    for (uint8_t p = 0; p < count_; p++) {
      for (uint8_t i = 0; i < kSlots; i++) {
        if (!meta_[i].used || meta_[i].orphan || meta_[i].list_pos != p) continue;
        char pfx[9];
        hex8(pfx, meta_[i].pub);
        char item[32];
        int n = snprintf(item, sizeof(item), " %u %s %s", (unsigned)(p + 1), pfx, poll::codeName(states_[i].code));
        if (pos + n > 160 - 5) {
          strcpy(reply + pos, " ...");
          return;
        }
        memcpy(reply + pos, item, (size_t)n + 1);
        pos += n;
      }
    }
    if (count_ == 0) strcpy(reply, "> no targets");
    return;
  }

  int which = atoi(arg);
  int8_t idx = -1;
  for (uint8_t i = 0; i < kSlots && which >= 1; i++) {
    if (meta_[i].used && !meta_[i].orphan && meta_[i].list_pos == which - 1) idx = (int8_t)i;
  }
  if (idx < 0) {
    snprintf(reply, 160, "Error: target number 1-%u", (unsigned)count_);
    return;
  }
  const poll::TargetState& t = states_[idx];
  char pfx[9], age[12], batt[12], temp[12], route[24], next[16];
  hex8(pfx, meta_[idx].pub);
  if (t.last_attempt && t.last_attempt <= now) fmtAge(age, sizeof(age), now - t.last_attempt);
  else strcpy(age, "never");
  if (t.battery_mv) snprintf(batt, sizeof(batt), "%umV", (unsigned)t.battery_mv);
  else strcpy(batt, "-");
  if (t.temperature_c10 != INT16_MIN) {
    snprintf(temp, sizeof(temp), "%.1fC", t.temperature_c10 / 10.0f);
  } else {
    strcpy(temp, "-");
  }
  int8_t snr = 0;
  if (directRoute((uint8_t)idx, &snr)) snprintf(route, sizeof(route), "direct snr %.2f", snr / 4.0f);
  else strcpy(route, "none");
  if (t.admin_seen) {
    strcpy(next, "stopped");
  } else if (!enabled_) {
    strcpy(next, "off");
  } else {
    uint32_t at = poll::nextSlot(t, meta_[idx].interval_s, now, meta_[idx].phase_s);
    if (t.next_due_at > at) at = t.next_due_at;
    snprintf(next, sizeof(next), "%lus", (unsigned long)(at > now ? at - now : 0));
  }
  const char* pw = t.learned_pw == poll::LEARNED_HELLO ? "hello" : t.learned_pw == poll::LEARNED_BLANK ? "blank" : "?";
  snprintf(reply, 160, "> %s %s %s ago, %s, %s, %s, %s, next %s, login %s pw %s, flood %u%s", pfx,
           poll::codeName(t.code), age, batt, temp, route, poll::roleName(controlFor((uint8_t)idx).role), next,
           t.logged_in_at ? "acl" : "none", pw, (unsigned)t.flood_replies,
           t.admin_seen ? (t.admin_removed ? ", admin removed" : ", admin NOT removed") : "");
}

void RepeaterPoller::cmdNow(const char* arg, char* reply) {
  readSettings();
  int matches = 0;
  int8_t idx = findTarget(arg, &matches);
  if (idx < 0) {
    if (matches > 1) snprintf(reply, 160, "Error: prefix matches %d targets", matches);
    else strcpy(reply, "Error: usage poll now <target key or prefix>");
    return;
  }
  poll::TargetState& t = states_[idx];
  const uint32_t now = host_->pollNow();
  const PollControl c = controlFor((uint8_t)idx);
  if (store_state_ == poll::STATE_NEWER) {
    strcpy(reply, "Error: poll state is from newer firmware");
  } else if (!has_id_) {
    strcpy(reply, "Error: no polling key yet (set poll on)");
  } else if (t.admin_seen) {
    strcpy(reply, "Error: admin stop; clear it with poll enable <key>");
  } else if (!clockOk()) {
    strcpy(reply, "Error: clock not synced");
  } else if (c.gate != poll::GATE_OPEN) {
    snprintf(reply, 160, "Error: polling %s", poll::gateName(c.gate));
  } else if (busy() || host_->pollDiscoveryBusy()) {
    strcpy(reply, "busy");
  } else if (t.last_attempt && t.last_attempt <= now && now - t.last_attempt < kNowSpacingS) {
    snprintf(reply, 160, "Error: polled %lus ago; wait %lus", (unsigned long)(now - t.last_attempt),
             (unsigned long)(kNowSpacingS - (now - t.last_attempt)));
  } else if (!directRoute((uint8_t)idx, NULL)) {
    t.code = poll::CODE_NOT_DIRECT;
    strcpy(reply, "Error: not_direct (not heard as a neighbor in the last day)");
  } else {
    startPoll((uint8_t)idx, false);
    strcpy(reply, "queued");
  }
}

void RepeaterPoller::cmdEnable(const char* arg, char* reply) {
  readSettings();
  int matches = 0;
  int8_t idx = findTarget(arg, &matches);
  if (idx < 0) {
    if (matches > 1) snprintf(reply, 160, "Error: prefix matches %d targets", matches);
    else strcpy(reply, "Error: usage poll enable <target key or prefix>");
    return;
  }
  poll::TargetState& t = states_[idx];
  char pfx[9];
  hex8(pfx, meta_[idx].pub);
  if (!t.admin_seen) {
    snprintf(reply, 160, "OK - %s was not stopped", pfx);
    return;
  }
  if (busy() && run_idx_ == idx) {
    strcpy(reply, "busy");
    return;
  }
  t.admin_seen = false;
  t.admin_removed = false;
  t.code = poll::CODE_NONE;
  t.logged_in_at = 0;
  t.login_checked_at = 0;
  meta_[idx].note = NOTE_NONE;
  for (uint8_t k = 0; k < poll::kMaxTargets; k++) {
    if (memcmp(stored_[k].pub, meta_[idx].pub, poll::kPubKeySize) == 0) stored_[k].admin_seen = false;
  }
  POLL_LOG("%s re-enabled by the operator", pfx);
  if (saveState("re-enabled")) snprintf(reply, 160, "OK - %s re-enabled", pfx);
  else snprintf(reply, 160, "OK - %s re-enabled (not saved)", pfx);
}

#endif  // WITH_REPEATER_POLL
