#include "RepeaterPoller.h"

#ifdef WITH_REPEATER_POLL

#include <Arduino.h>
#include <Packet.h>
#include <Utils.h>
#include <helpers/MQTTConnectionPolicy.h>
#include <helpers/poll/PollControlPlan.h>
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
static_assert(sizeof(RepeaterPoller) <= 5204, "repeater poller RAM budget");

namespace {

const char* const kStateFile = "/poll_state";
const char* const kStateTmp = "/poll_state.tmp";
const char* const kStateBak = "/poll_state.bak";
const char* const kStateNames[3] = {kStateFile, kStateTmp, kStateBak};   // pickState's order

const uint32_t kTickMs = 5000;
const uint32_t kTxStuckMs = 30000;          // a packet neither sent nor failed by then was shed
const uint32_t kMinRequestListenMs = 10000;
const uint32_t kNowSpacingS = 60;            // `poll now` on one target at most this often
const uint32_t kAheadSaveMs = 3600000UL;     // saves of a tag mark that ran ahead of the clock, at most this often
const uint32_t kSaveRetryMs = 10000;         // between attempts to get an admin stop or drill result onto flash

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
// Bench-only: POLL_TEST_CONTROL_EVERY_S replaces the spacing of CoreScope control sessions.
#ifdef POLL_TEST_CONTROL_EVERY_S
const uint32_t kCtlEveryS = POLL_TEST_CONTROL_EVERY_S;
#else
const uint32_t kCtlEveryS = poll::kControlEveryS;
#endif
// Under POLL_TEST_INTERVAL_S the policy's time floors drop to a minute, as in the plugin's fast mode.
#ifdef POLL_TEST_INTERVAL_S
const bool kFastPolicy = true;
#else
const bool kFastPolicy = false;
#endif
const uint32_t kCtlRetryMs = 30000;          // a session the bridge refused is filed again after this
const uint32_t kClaimTimeoutMs = 60000;      // a claim the bridge never answered counts as failed

// Log-once bits in ctl_note_.
const uint8_t kNoteWaitCtl = 1;
const uint8_t kNoteRefused = 2;
const uint8_t kNoteClaimRefused = 4;
const uint8_t kNoteClaimsHeld = 8;

bool millisPassed(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

// The whole packet as queued: a shed packet's pool slot reused by forwarded traffic must not read as ours.
PollPacketSig sigOf(const mesh::Packet* pkt) {
  PollPacketSig s;
  s.header = pkt->header;
  s.path_len = pkt->path_len;
  s.payload_len = pkt->payload_len;
  const size_t n = pkt->payload_len < sizeof(pkt->payload) ? pkt->payload_len : sizeof(pkt->payload);
  uint32_t h = 2166136261UL;   // FNV-1a
  for (size_t i = 0; i < n; i++) h = (h ^ pkt->payload[i]) * 16777619UL;
  s.hash = h;
  return s;
}

bool sameSig(const PollPacketSig& a, const PollPacketSig& b) {
  return a.header == b.header && a.path_len == b.path_len && a.payload_len == b.payload_len && a.hash == b.hash;
}

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

const char* modeName(uint8_t m) {
  return m == poll::MODE_ENCRYPTED ? "enc" : m == poll::MODE_JSON ? "json" : m == poll::MODE_TELEMETRY ? "telemetry" : "?";
}

const char* keyName(uint8_t k) {
  return k == poll::KEY_OWN ? "own" : k == poll::KEY_SHARED ? "shared" : "none";
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
    : host_(NULL), fs_(NULL), has_id_(false), store_state_(poll::STATE_EMPTY), recovered_from_(0), seq_(0),
      key_created_at_(0), tag_hw_(0), tag_hw_saved_(0), count_(0), enabled_(false), consent_(false),
      interval_min_(60), status_hours_(6), run_kind_(RUN_NONE), run_idx_(-1), run_scheduled_(false),
      run_quick_(false), run_learned_before_(0), poll_tag_count_(0), inflight_(NULL), inflight_ms_(0),
      inflight_act_(0), inflight_pw_(0), inflight_tag_(0), timer_on_(false), timer_at_(0),
      listening_(false), listen_act_(0), listen_pw_(0), listen_tag_(0), event_count_(0), rd_voltage_(false), rd_mv_(0),
      rd_temp_(false), rd_temp_c10_(0), rd_uptime_(0), rd_noise_(0), last_tick_ms_(0),
      last_scheduled_at_(0), id_retry_ms_(0UL - 60000UL), ntp_seen_(false), save_due_(false), save_safety_(false),
      save_retry_ms_(0), ahead_save_ms_(0), ahead_saved_(false), hold_note_(0), fold_full_logged_(false) {
  memset(&gate_, 0, sizeof(gate_));
  memset(stored_, 0, sizeof(stored_));
  memset(meta_, 0, sizeof(meta_));
  memset(states_, 0, sizeof(states_));
  memset(poll_tags_, 0, sizeof(poll_tags_));
  memset(events_, 0, sizeof(events_));
  memset(&inflight_sig_, 0, sizeof(inflight_sig_));
  initControl();
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
  const char* const* names = kStateNames;
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
  recovered_from_ = (uint8_t)best;
  promoteRecovered();
}

// After an interrupted swap the newest record sits in the temp or backup copy, which the next
// save would write over; move it to the main name first. Older copies go first so the newest
// stays the only valid one at every step.
bool RepeaterPoller::promoteRecovered() {
  if (recovered_from_ == 0) return true;
  const char* from = kStateNames[recovered_from_];
  for (uint8_t i = 0; i < 3; i++) {
    if (i == recovered_from_ || !fs_->exists(kStateNames[i])) continue;
    if (!fs_->remove(kStateNames[i])) {
      POLL_LOG("state: cannot remove %s", kStateNames[i]);
      return false;
    }
  }
  if (!fs_->rename(from, kStateFile)) {
    POLL_LOG("state: cannot move %s to %s", from, kStateFile);
    return false;
  }
  POLL_LOG("state: recovered copy %s moved to %s", from, kStateFile);
  recovered_from_ = 0;
  return true;
}

bool RepeaterPoller::saveState(const char* why) {
  if (!fs_ || store_state_ == poll::STATE_NEWER) return false;
  if (!promoteRecovered()) {
    POLL_LOG("state save failed (%s)%s", why, save_safety_ ? ", will retry" : "");
    save_retry_ms_ = millis();
    return false;
  }

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

  foldSlots();
  memcpy(r.targets, stored_, sizeof(r.targets));

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
  if (ok) {
    // The temp copy is complete and the newest from here on: never reuse its seq.
    seq_ = r.seq;
    // SPIFFS will not rename onto an existing name: park the old copy, publish, then drop it.
    if (fs_->exists(kStateFile)) {
      if (fs_->exists(kStateBak)) fs_->remove(kStateBak);
      ok = fs_->rename(kStateFile, kStateBak);
    }
    if (ok) ok = fs_->rename(kStateTmp, kStateFile);
    if (ok && fs_->exists(kStateBak)) fs_->remove(kStateBak);
    if (!ok) recovered_from_ = 1;   // the next save moves the temp copy into place before writing
  }
  if (!ok) {
    POLL_LOG("state save failed (%s)%s", why, save_safety_ ? ", will retry" : "");
    save_retry_ms_ = millis();
    return false;
  }
  store_state_ = poll::STATE_OK;
  tag_hw_saved_ = tag_hw_;
  save_due_ = false;
  save_safety_ = false;
  gate_save_due_ = false;
  POLL_LOG("state saved seq=%lu (%s)", (unsigned long)seq_, why);
  return true;
}

void RepeaterPoller::flush() {
  if (tag_hw_ != tag_hw_saved_ || save_due_ || save_safety_) saveState("flush");
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
  for (uint8_t k = 0; k < poll::kStoredTargets; k++) stored_[k].learned_pw = poll::LEARNED_NONE;
}

// Brings stored_ up to date with every slot, removed-but-draining ones included.
void RepeaterPoller::foldSlots() {
  uint8_t keep[kSlots][poll::kPubKeySize];
  uint8_t n_keep = 0;
  bool full = false;
  for (uint8_t i = 0; i < kSlots; i++) {
    if (meta_[i].used) memcpy(keep[n_keep++], meta_[i].pub, poll::kPubKeySize);
  }
  for (uint8_t i = 0; i < kSlots; i++) {
    if (!meta_[i].used) continue;
    poll::StoredTarget e;
    memcpy(e.pub, meta_[i].pub, poll::kPubKeySize);
    e.admin_seen = states_[i].admin_seen;
    e.admin_removed = states_[i].admin_removed;
    e.learned_pw = states_[i].learned_pw;
    const uint8_t rank = poll::storedRank(e);
    const int k = poll::storedSlotFor(stored_, poll::kStoredTargets, e.pub, rank, !meta_[i].orphan, keep, n_keep);
    char pfx[9];
    if (k < 0) {
      if (rank == poll::RANK_NONE) continue;
      full = true;
      hex8(pfx, e.pub);
      if (!fold_full_logged_) POLL_LOG("%s of %s not kept: no room", rank == poll::RANK_LEARNED ? "password" : "admin stop", pfx);
      continue;
    }
    poll::StoredTarget& slot = stored_[k];
    if (memcmp(slot.pub, e.pub, poll::kPubKeySize) != 0) {
      if (rank == poll::RANK_NONE) continue;   // nothing to keep yet
      if (poll::storedRank(slot) == poll::RANK_STOP && !poll::storedIsEmpty(slot)) {
        hex8(pfx, slot.pub);
        POLL_LOG("admin stop of removed %s dropped: no room", pfx);
      }
    }
    slot = e;
  }
  fold_full_logged_ = full;   // every save folds: say it once per spell without room
}

// A slot changing hands: nothing asked of CoreScope for its last target, and no waiting `poll now`, carries over.
void RepeaterPoller::detachSlot(uint8_t i) {
  policy_covered_ &= (uint16_t)~(1U << i);
  for (uint8_t k = 0; k < poll::kMaxTargets; k++) {
    if (ctl_slot_[k] == (int8_t)i) ctl_slot_[k] = -1;
  }
  if (now_idx_ == (int8_t)i) now_idx_ = -1;
}

bool RepeaterPoller::roomFor(const uint8_t* pub) {
  if (!host_) return true;
  PollSettings s;
  host_->pollReadSettings(s);
  if (s.count > poll::kMaxTargets) s.count = poll::kMaxTargets;
  if (poll::storedKeyIn(pub, s.targets, s.count) || s.count >= poll::kMaxTargets) return true;
  memcpy(s.targets[s.count++], pub, poll::kPubKeySize);
  foldSlots();
  return poll::storedRoomForList(stored_, poll::kStoredTargets, s.targets, s.count);
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
  for (uint8_t k = 0; k < poll::kStoredTargets; k++) {
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
  readControlSettings(s);

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
    foldSlots();   // its stop must outlive the slot even if no save has succeeded yet
    memset(&meta_[i], 0, sizeof(meta_[i]));
    memset(&states_[i], 0, sizeof(states_[i]));
    detachSlot(i);
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
      detachSlot((uint8_t)idx);   // the policy in use was not read for this target
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
  const uint32_t now = host_->pollNow();
  PollControl c;
  c.role = poll::mapRole(community_, owners_[idx]);
  c.gate = gateAt(now);
  c.window_s = windowFor(idx, now);
  c.max_requests = policy_.max_requests;
  c.own_key = key_in_use_ == poll::KEY_OWN;
  return c;
}

// CoreScope's window is a floor: the local interval can only make polling slower.
uint32_t RepeaterPoller::intervalFor(const PollControl& c) const {
#ifdef POLL_TEST_INTERVAL_S
  const uint32_t local = POLL_TEST_INTERVAL_S;
#else
  const uint32_t local = (uint32_t)interval_min_ * 60UL;
#endif
  return local > c.window_s ? local : c.window_s;
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
    // Pool slots are static, so the bytes are readable; a changed signature means it was shed and reused.
    const bool ours = inflightIntact();
    if (ours && host_->pollIsTransmitting(inflight_)) {
      inflight_ms_ = now_ms;   // on the air right now: its report is coming
    } else {
      bool cancelled = ours && host_->pollCancelQueued(inflight_);
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
  if (enabled_ && poll::identityAllowed(consent_, mode_) && !has_id_ && (uint32_t)(millis() - id_retry_ms_) >= 60000UL) {
    id_retry_ms_ = millis();
    ensureIdentity();
  }
  // An admin stop must reach flash even while the drill runs.
  if (save_safety_ && (uint32_t)(millis() - save_retry_ms_) >= kSaveRetryMs) saveState("retry");
  serviceControl();
  resolveNow(host_->pollNow());
  if (gate_save_due_ && (uint32_t)(millis() - save_retry_ms_) >= kSaveRetryMs) saveState("saved gate");
  if (busy()) return;
  if (save_due_ || tag_hw_ - tag_hw_saved_ > kPollDay) {
    saveState("tag high-water");
  } else if (tag_hw_ != tag_hw_saved_ && tag_hw_ > host_->pollNow() &&
             (!ahead_saved_ || (uint32_t)(millis() - ahead_save_ms_) >= kAheadSaveMs)) {
    // After a backward clock step tags run ahead of it; an unclean reboot must not restart below them.
    ahead_saved_ = true;
    ahead_save_ms_ = millis();
    saveState("tag ahead of the clock");
  }
  if (!enabled_ || !has_id_ || store_state_ == poll::STATE_NEWER) return;
  const uint8_t hold = !clockOk() ? 1 : host_->pollDiscoveryBusy() ? 2 : 0;
  if (hold != hold_note_) {
    hold_note_ = hold;
    if (hold == 1) POLL_LOG("waiting for an NTP-synced clock");
    else if (hold == 2) POLL_LOG("waiting for neighbor discovery to finish");
  }
  if (hold) return;
  scheduleTick(host_->pollNow());
}

// ---------------------------------------------------------------------------
// Running an exchange

void RepeaterPoller::startPoll(uint8_t idx, bool scheduled) {
  poll::TargetState& t = states_[idx];
  const uint32_t now = host_->pollNow();
  const PollControl c = controlFor(idx);

  bindKey();   // a secret is only ever reused with the key it was made from
  if (!meta_[idx].secret_ok) {
    pollKey().calcSharedSecret(meta_[idx].secret, meta_[idx].pub);
    meta_[idx].secret_ok = true;
  }

  run_quick_ = t.code == poll::CODE_NO_REPLY && poll::elapsedSince(t.last_success, now) < poll::kDay &&
               t.no_reply_count < poll::kNoReplyFastTries;
  poll::recordSend(t, now);
  t.next_due_at = 0;
  meta_[idx].note = NOTE_NONE;
  skip_[idx] = poll::SKIP_NONE;

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
        mesh::Identity target(meta_[idx].pub);
        pkt = host_->pollCreateLogin(pollKey(), target, meta_[idx].secret, plain, n);
      }
      break;
    }
    case poll::ACT_SEND_REQUEST: {
      tag = nextTag();
      host_->pollRng()->random(rnd, sizeof(rnd));
      uint8_t req = s.kind == poll::REQ_STATUS ? poll::kReqStatus : poll::kReqTelemetry;
      n = poll::buildRequestPlain(plain, sizeof(plain), tag, req, rnd);
      if (n) {
        if (poll::tagIsEchoed(s.act)) poll::rememberTag(t, tag);
        if (poll_tag_count_ < kMaxPollTags) {
          poll_tags_[poll_tag_count_++] = tag;
        } else {
          memmove(poll_tags_, poll_tags_ + 1, sizeof(poll_tags_[0]) * (kMaxPollTags - 1));
          poll_tags_[kMaxPollTags - 1] = tag;
        }
        pkt = buildDatagram(PAYLOAD_TYPE_REQ, pollKey(), idx, plain, n);
      }
      break;
    }
    case poll::ACT_SEND_TEACH: {
      host_->pollRng()->random(rnd, sizeof(rnd));
      n = poll::buildTeachPlain(plain, sizeof(plain), NULL, 0, rnd);
      if (n) pkt = buildDatagram(PAYLOAD_TYPE_PATH, pollKey(), idx, plain, n);
      break;
    }
    case poll::ACT_SEND_SETPERM: {
      tag = nextTag();
      n = poll::buildSetPermPlain(plain, sizeof(plain), tag, pollKey().pub_key);
      if (n) pkt = buildDatagram(PAYLOAD_TYPE_TXT_MSG, pollKey(), idx, plain, n);
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
  if (!host_->pollSendDirect(pkt)) {
    POLL_LOG("tx %s not queued: refused by the packet queue", actLabel(s.act, s.pw));
    return false;
  }
  inflight_ = pkt;
  inflight_sig_ = sigOf(pkt);
  inflight_ms_ = millis();
  inflight_act_ = s.act;
  inflight_pw_ = s.pw;
  inflight_tag_ = tag;
  if (tag) POLL_LOG("tx %s tag=%lu queued", actLabel(s.act, s.pw), (unsigned long)tag);
  else POLL_LOG("tx %s queued", actLabel(s.act, s.pw));
  return true;
}

bool RepeaterPoller::inflightIntact() const { return inflight_ && sameSig(sigOf(inflight_), inflight_sig_); }

void RepeaterPoller::pushEvent(uint8_t ev, bool flooded) {
  if (event_count_ >= kEventQueue) {
    // Only a TX report (the machines wait for it) makes room; an exchange's admin answer is latched outside the queue.
    if (ev != poll::EV_TX_DONE && ev != poll::EV_TX_FAILED) return;
    uint8_t drop = 0;
    while (drop < kEventQueue && (events_[drop].ev == poll::EV_TX_DONE || events_[drop].ev == poll::EV_TX_FAILED)) drop++;
    if (drop == kEventQueue) return;
    memmove(events_ + drop, events_ + drop + 1, sizeof(events_[0]) * (kEventQueue - 1 - drop));
    event_count_--;
  }
  events_[event_count_].ev = ev;
  events_[event_count_].flooded = flooded;
  event_count_++;
}

void RepeaterPoller::processEvents() {
  while (run_kind_ != RUN_NONE) {
    Event e;
    if (run_kind_ == RUN_EXCHANGE && meta_[run_idx_].admin_held) {
      // Ahead of a queued TX report that could end the exchange first; a pending report still resolves it.
      meta_[run_idx_].admin_held = false;
      e.ev = poll::EV_LOGIN_ADMIN;
      e.flooded = meta_[run_idx_].admin_held_flooded;
    } else if (event_count_ > 0) {
      e = events_[0];
      memmove(events_, events_ + 1, sizeof(events_[0]) * (kEventQueue - 1));
      event_count_--;
    } else {
      break;
    }
    poll::Step s = feed(e.ev, e.flooded);
    const bool listen = e.ev == poll::EV_TX_DONE &&
                        (inflight_act_ == poll::ACT_SEND_LOGIN || inflight_act_ == poll::ACT_SEND_REQUEST);
    apply(s);
    // An event ahead of the admin answer may have queued another packet.
    if (e.ev == poll::EV_LOGIN_ADMIN) recallForAdmin();
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
  if (!pkt || pkt != inflight_ || !inflightIntact()) return;
  inflight_ = NULL;
  if (inflight_tag_) POLL_LOG("tx %s tag=%lu sent", actLabel(inflight_act_, inflight_pw_), (unsigned long)inflight_tag_);
  else POLL_LOG("tx %s sent", actLabel(inflight_act_, inflight_pw_));
  pushEvent(poll::EV_TX_DONE, false);
}

void RepeaterPoller::onTxFail(const mesh::Packet* pkt) {
  if (!pkt || pkt != inflight_ || !inflightIntact()) return;
  inflight_ = NULL;
  POLL_LOG("tx %s failed", actLabel(inflight_act_, inflight_pw_));
  pushEvent(poll::EV_TX_FAILED, false);
}

void RepeaterPoller::onRx(const mesh::Packet* pkt) {
  if (run_kind_ == RUN_NONE || !has_id_ || !pkt) return;
  const uint8_t idx = (uint8_t)run_idx_;
  const uint8_t type = pkt->getPayloadType();
  if (!poll::isCandidateReply(type, pkt->payload, pkt->payload_len, pollKey().pub_key[0], meta_[idx].pub[0])) return;
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
    case poll::REPLY_LOGIN_ADMIN: {
      const uint32_t print = poll::loginAnswerPrint(r);
      if (poll::loginAnswerRepeats(print, meta_[idx].guest_answer_print, meta_[idx].admin_answer_print)) {
        POLL_LOG("rx login %s tag=%lu repeats the last answer, ignored", cls == poll::REPLY_LOGIN_ADMIN ? "admin" : "guest",
                 (unsigned long)r.tag);
        break;
      }
      if (cls == poll::REPLY_LOGIN_ADMIN) meta_[idx].admin_answer_print = print;
      else meta_[idx].guest_answer_print = print;
      POLL_LOG("rx login %s tag=%lu %s snr=%.2f", cls == poll::REPLY_LOGIN_ADMIN ? "admin" : "guest",
               (unsigned long)r.tag, flooded ? "flooded" : "direct", snr);
      if (cls == poll::REPLY_LOGIN_ADMIN && run_kind_ == RUN_EXCHANGE) {
        meta_[idx].admin_held = true;
        meta_[idx].admin_held_flooded = flooded;
      } else {
        pushEvent(cls == poll::REPLY_LOGIN_ADMIN ? poll::EV_LOGIN_ADMIN : poll::EV_LOGIN_GUEST, flooded);
      }
      // The dispatcher sends right after this tap returns, before the loop could stop the next packet.
      if (cls == poll::REPLY_LOGIN_ADMIN) recallForAdmin();
      break;
    }
    default:
      POLL_LOG("rx ignored tag=%lu len=%u %s", (unsigned long)r.tag, (unsigned)r.len, flooded ? "flooded" : "direct");
      break;
  }
  memset(plain, 0, sizeof(plain));
}

// After an admin answer nothing more may go out under the exchange; a packet already on the air cannot be recalled.
void RepeaterPoller::recallForAdmin() {
  if (run_kind_ != RUN_EXCHANGE || !inflightIntact()) return;
  if (!host_->pollCancelQueued(inflight_)) return;
  POLL_LOG("tx %s cancelled: admin answer received", actLabel(inflight_act_, inflight_pw_));
  inflight_ = NULL;
  pushEvent(poll::EV_TX_FAILED, false);
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
    save_safety_ = true;
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
    save_safety_ = true;
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
    foldSlots();
    memset(&meta_[idx], 0, sizeof(meta_[idx]));
    memset(&states_[idx], 0, sizeof(states_[idx]));
  }
}

void RepeaterPoller::abortRun(const char* why) {
  if (run_kind_ == RUN_NONE) return;
  // A packet already on the air finishes; its report is ignored once the run is gone.
  if (inflightIntact()) host_->pollCancelQueued(inflight_);
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
  } else if (strcmp(command, "get poll.control") == 0) {
    readSettings();
    fmtControl(reply, 160, host_->pollNow());
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
  const uint32_t now = host_->pollNow();
  const uint8_t gate = gateAt(now);
  const char* key = key_in_use_ == poll::KEY_OWN ? "own"
                    : key_in_use_ == poll::KEY_SHARED ? "shared"
                    : !has_id_ ? "none (no polling key yet)"
                    : "none (not registered, no shared key)";
  // CoreScope details are in `get poll.control`; this line stays under the reply buffer, with the key reason dropped first.
  for (int pass = 0; pass < 2; pass++) {
    const int n = snprintf(reply, 160, "> %s, mode %s, targets %u (%u ok), key %s, gate %s%s, clock %s%s%s",
                           enabled_ ? "on" : "off", modeName(mode_), (unsigned)total, (unsigned)ok, key,
                           gate == poll::GATE_OPEN ? "open" : poll::gateName(gate),
                           gate != poll::GATE_OPEN && !policy_.ever_fetched ? " (saved)" : "", clockOk() ? "ok" : "not synced",
                           busy() ? ", polling now" : "", store_state_ == poll::STATE_NEWER ? ", state from newer fw (read-only)" : "");
    if (n < 160) break;
    key = keyName(key_in_use_);
  }
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
    const uint32_t at = poll::nextDueAt(t, meta_[idx].interval_s, now, meta_[idx].phase_s);
    snprintf(next, sizeof(next), "%lus", (unsigned long)(at > now ? at - now : 0));
  }
  const char* pw = t.learned_pw == poll::LEARNED_HELLO ? "hello" : t.learned_pw == poll::LEARNED_BLANK ? "blank" : "?";
  const uint8_t role = poll::mapRole(community_, owners_[idx]);
  char role_txt[16];
  if (role == poll::ROLE_STANDBY) {
    snprintf(role_txt, sizeof(role_txt), "standby#%u", (unsigned)(owners_[idx].standby_rank ? owners_[idx].standby_rank : 1));
  } else {
    snprintf(role_txt, sizeof(role_txt), "%s", poll::roleName(role));
  }
  char skip[24] = "";
  if (skip_[idx] != poll::SKIP_NONE && !t.admin_seen) snprintf(skip, sizeof(skip), ", skip %s", poll::skipReasonName(skip_[idx]));
  snprintf(reply, 160, "> %s %s %s ago, %s, %s, %s, %s, next %s%s, login %s pw %s, flood %u%s", pfx,
           poll::codeName(t.code), age, batt, temp, route, role_txt, next, skip,
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
  const uint32_t now = host_->pollNow();
  if (now_idx_ >= 0) {
    strcpy(reply, now_idx_ == idx ? "waiting on CoreScope" : "busy (another poll now waits on CoreScope)");
    return;
  }
  // As the plugin's Test refreshes the policy first: wait for a session under way, or file one when the
  // spacing allows, then decide on what is known.
  const bool under_way = ctl_pending_ || ctl_deferred_;
  reply[0] = 0;
  if (enabled_ && has_url_ && (under_way || controlFileableNow()) && !nowRefused((uint8_t)idx, now, reply, true)) {
    now_idx_ = idx;
    memcpy(now_pfx_, meta_[idx].pub, sizeof(now_pfx_));
    now_ms_ = millis();
    strcpy(reply, under_way ? "waiting on CoreScope (session under way)" : "waiting on CoreScope (checking first)");
    return;
  }
  if (reply[0]) return;
  if (!nowRefused((uint8_t)idx, now, reply, false)) {
    startPoll((uint8_t)idx, false);
    strcpy(reply, "queued");
  }
}

// The `poll now` checks. local_only: only those CoreScope cannot change (run before waiting on it).
bool RepeaterPoller::nowRefused(uint8_t idx, uint32_t now, char* reply, bool local_only) {
  poll::TargetState& t = states_[idx];
  const PollControl c = controlFor(idx);
  // As the plugin's Test: CoreScope's gate and exclusions apply, its cap, roles and freshness do not.
  const poll::Decision d = poll::decideCycle(t, cycleInputFor(idx, true, now), now);
  reply[0] = 0;
  if (store_state_ == poll::STATE_NEWER) {
    strcpy(reply, "Error: poll state is from newer firmware");
  } else if (!has_id_) {
    strcpy(reply, "Error: no polling key yet (set poll on)");
  } else if (t.admin_seen) {
    strcpy(reply, "Error: admin stop; clear it with poll enable <key>");
  } else if (!clockOk()) {
    strcpy(reply, "Error: clock not synced");
  } else if (!local_only && c.gate != poll::GATE_OPEN) {
    snprintf(reply, 160, "Error: polling %s", poll::gateName(c.gate));
  } else if (!local_only && !d.poll) {
    strcpy(reply, "Error: excluded by CoreScope");
  } else if (!local_only && key_in_use_ == poll::KEY_NONE) {
    strcpy(reply, "Error: no polling key in use (not registered with CoreScope, no shared key)");
  } else if (busy() || host_->pollDiscoveryBusy()) {
    strcpy(reply, "busy");
  } else if (t.last_attempt && t.last_attempt <= now && now - t.last_attempt < kNowSpacingS) {
    snprintf(reply, 160, "Error: polled %lus ago; wait %lus", (unsigned long)(now - t.last_attempt),
             (unsigned long)(kNowSpacingS - (now - t.last_attempt)));
  } else if (!directRoute(idx, NULL)) {
    t.code = poll::CODE_NOT_DIRECT;
    strcpy(reply, "Error: not_direct (not heard as a neighbor in the last day)");
  }
  return reply[0] != 0;
}

bool RepeaterPoller::controlFileableNow() const {
  if (ctl_pending_) return false;
  const uint32_t since = (uint32_t)(millis() - ctl_filed_ms_) / 1000UL;
  if (!poll::controlAllowed(false, ctl_ever_, since, kCtlEveryS)) return false;
  return ctl_retry_ms_ == 0 || (uint32_t)(millis() - ctl_retry_ms_) >= kCtlRetryMs;
}

// A waiting `poll now` is decided once no session is under way: answered, refused, or timed out.
void RepeaterPoller::resolveNow(uint32_t now) {
  if (now_idx_ < 0) return;
  const uint8_t idx = (uint8_t)now_idx_;
  if (!enabled_ || !has_url_) {
    // It waited on a CoreScope check that polling being switched off, or the URL cleared, has called off.
    now_idx_ = -1;
    POLL_LOG("poll now dropped: polling or the CoreScope URL was switched off");
    return;
  }
  // The session itself gives up at kControlTimeoutS; a claim ahead of it at kClaimTimeoutMs.
  const bool expired = (uint32_t)(millis() - now_ms_) >= poll::kControlTimeoutS * 1000UL + kClaimTimeoutMs + kTickMs;
  if ((ctl_pending_ || ctl_deferred_) && !expired) return;
  if ((busy() || host_->pollDiscoveryBusy()) && !expired) return;
  now_idx_ = -1;
  char pfx[9];
  hex8(pfx, now_pfx_);
  if (!meta_[idx].used || meta_[idx].orphan || memcmp(meta_[idx].pub, now_pfx_, sizeof(now_pfx_)) != 0) {
    POLL_LOG("poll now %s dropped: target removed", pfx);
    return;
  }
  char why[160];
  if (nowRefused(idx, now, why, false)) {
    POLL_LOG("poll now %s not sent: %s", pfx, why);
    return;
  }
  POLL_LOG("poll now %s: CoreScope checked, sending", pfx);
  startPoll(idx, false);
}

void RepeaterPoller::cmdEnable(const char* arg, char* reply) {
  readSettings();
  int matches = 0;
  int8_t idx = findTarget(arg, &matches);
  if (idx < 0 && matches == 0 && enableRemoved(arg, reply)) return;
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
  for (uint8_t k = 0; k < poll::kStoredTargets; k++) {
    if (memcmp(stored_[k].pub, meta_[idx].pub, poll::kPubKeySize) == 0) {
      stored_[k].admin_seen = false;
      stored_[k].admin_removed = false;
    }
  }
  POLL_LOG("%s re-enabled by the operator", pfx);
  if (saveState("re-enabled")) snprintf(reply, 160, "OK - %s re-enabled", pfx);
  else snprintf(reply, 160, "OK - %s re-enabled (not saved)", pfx);
}

// A removed target's stop is cleared by its key here, so its entry can make room for new targets.
bool RepeaterPoller::enableRemoved(const char* arg, char* reply) {
  size_t n = 0;
  if (!hexWord(arg, &n)) return false;
  while (*arg == ' ') arg++;
  int found = -1, matches = 0;
  for (uint8_t k = 0; k < poll::kStoredTargets; k++) {
    if (!stored_[k].admin_seen || slotFor(stored_[k].pub) >= 0) continue;
    if (!hexPrefixMatches(stored_[k].pub, arg, n)) continue;
    matches++;
    found = k;
  }
  if (matches == 0) return false;
  if (matches > 1) {
    snprintf(reply, 160, "Error: prefix matches %d removed targets", matches);
    return true;
  }
  char pfx[9];
  hex8(pfx, stored_[found].pub);
  stored_[found].admin_seen = false;
  stored_[found].admin_removed = false;
  POLL_LOG("%s (removed) re-enabled by the operator", pfx);
  if (saveState("re-enabled")) snprintf(reply, 160, "OK - %s (removed target) re-enabled", pfx);
  else snprintf(reply, 160, "OK - %s (removed target) re-enabled (not saved)", pfx);
  return true;
}

// ---------------------------------------------------------------------------
// CoreScope control: policy, owners, registration, shared key and claims

namespace {

uint32_t fnv(const char* s) {
  uint32_t h = 2166136261UL;
  while (*s) h = (h ^ (uint8_t)*s++) * 16777619UL;
  return h;
}

void wipe(void* p, size_t n) {
  volatile uint8_t* b = static_cast<volatile uint8_t*>(p);
  for (size_t i = 0; i < n; i++) b[i] = 0;
}

void fmtStatus(char* out, size_t cap, bool wanted, int16_t status) {
  if (!wanted) snprintf(out, cap, "-");
  else snprintf(out, cap, "%d", (int)status);
}

}  // namespace

void RepeaterPoller::initControl() {
  poll::policyDefaults(policy_);
  policy_answered_ = false;
  gate_save_due_ = false;
  gate_now_ = poll::GATE_OPEN;
  memset(owners_, 0, sizeof(owners_));
  owners_ok_at_ = 0;
  memset(&claim_, 0, sizeof(claim_));
  claim_pending_ = false;
  claim_pending_community_ = false;
  claim_pending_listed_ = false;
  claim_pending_sig_ = 0;
  claim_filed_ms_ = 0;
  claim_sent_at_ = 0;
  memset(&reg_, 0, sizeof(reg_));
  has_shared_ = false;
  shared_renew_failed_ = false;
  shared_at_ = 0;
  key_in_use_ = poll::KEY_NONE;
  mode_ = poll::MODE_UNKNOWN;
  memset(acl_pub_, 0, sizeof(acl_pub_));   // the flash record's learned passwords are the own key's
  started_at_ = 0;
  gated_at_ = 0;
  url_hash_ = 0;
  has_url_ = false;
  community_ = true;
  claim_slot_ = 0;
  ctl_pending_ = false;
  ctl_deferred_ = false;
  ctl_ever_ = false;
  ctl_want_ = 0;
  ctl_filed_ms_ = 0;
  ctl_retry_ms_ = 0;
  ctl_count_ = 0;
  memset(ctl_slot_, 0, sizeof(ctl_slot_));
  memset(ctl_pfx_, 0, sizeof(ctl_pfx_));
  ctl_last_ = CTL_NEVER;
  ctl_last_at_ = 0;
  memset(ctl_status_, 0, sizeof(ctl_status_));
  memset(skip_, 0, sizeof(skip_));
  policy_covered_ = 0;
  now_idx_ = -1;
  memset(now_pfx_, 0, sizeof(now_pfx_));
  now_ms_ = 0;
  ctl_note_ = 0;
  key_note_ = 0;
}

// What one CoreScope said belongs to it; the plugin starts over when the URL changes.
void RepeaterPoller::resetCorescope() {
  poll::policyDefaults(policy_);
  policy_answered_ = false;
  policy_covered_ = 0;
  now_idx_ = -1;   // a `poll now` waiting on the old server's answer is not decided on the new one's defaults
  memset(owners_, 0, sizeof(owners_));
  owners_ok_at_ = 0;
  memset(&claim_, 0, sizeof(claim_));
  claim_sent_at_ = 0;
  memset(&reg_, 0, sizeof(reg_));
  has_shared_ = false;
  shared_renew_failed_ = false;
  shared_at_ = 0;
  mode_ = poll::MODE_UNKNOWN;
  // The session spacing is the node's, not the server's: ctl_ever_ and ctl_filed_ms_ stay.
  ctl_want_ = 0;   // an answer still on its way came from the old URL
  ctl_last_ = CTL_NEVER;
  const poll::SavedGate open = {false, false, false, 0};
  if (!poll::savedGateEqual(gate_, open)) {
    gate_ = open;
    gate_save_due_ = true;
  }
  POLL_LOG("CoreScope URL changed: policy, mode, owners, registration and shared key start over");
}

void RepeaterPoller::readControlSettings(const PollSettings& s) {
  has_url_ = s.url[0] != 0;
  community_ = s.community;
  claim_slot_ = s.slot;
  const uint32_t h = fnv(s.url);
  if (h != url_hash_) {
    if (url_hash_ != 0) resetCorescope();
    url_hash_ = h;
  }
}

uint32_t RepeaterPoller::startupHoldS() const {
#ifdef POLL_TEST_INTERVAL_S
  return 2 * kCtlEveryS + 60;
#else
  return poll::kStartupHoldS;
#endif
}

uint32_t RepeaterPoller::windowFor(uint8_t idx, uint32_t now) const {
#ifdef POLL_TEST_INTERVAL_S
  if (!policy_.ever_fetched) return POLL_TEST_INTERVAL_S;
#endif
  // Direct neighbors only, so the one-hop window never applies.
  return poll::policyWindowFor(policy_, meta_[idx].pub, 0, now, policyCovers(idx));
}

uint8_t RepeaterPoller::gateAt(uint32_t now) const {
  return poll::gateInForce(policy_, gate_, poll::kPluginVersion, now);
}

bool RepeaterPoller::startingAt(uint32_t now) const {
  return poll::startupHold(community_, has_url_, started_at_, claim_sent_at_, owners_ok_at_, now, startupHoldS());
}

poll::CycleInput RepeaterPoller::cycleInputFor(uint8_t idx, bool test, uint32_t now) const {
  poll::CycleInput in;
  memset(&in, 0, sizeof(in));
  const poll::OwnerEntry& e = owners_[idx];
  in.test = test;
  in.gate = gateAt(now);
  in.excluded = poll::policyExcludesTarget(policy_, meta_[idx].pub, policyCovers(idx));
  in.role = poll::mapRole(community_, e);
  in.rank = e.standby_rank ? e.standby_rank : 1;
  in.starting = startingAt(now);
  in.has_url = has_url_;
  in.window_s = windowFor(idx, now);
  in.max_requests = policy_.max_requests;
  in.grace_s = policy_.grace_s;
  // The owners reply's latest_ts stands in for the plugin's per-poll battery lookup.
  in.corescope_reachable = owners_ok_at_ != 0 && !poll::ownersLapsed(owners_ok_at_, now, poll::kOwnersReachableS);
  in.latest_ts = e.present ? e.latest_ts : 0;
  in.hold_from = started_at_ > gated_at_ ? started_at_ : gated_at_;
#ifdef POLL_TEST_INTERVAL_S
  in.hold_recheck_s = kCtlEveryS;
#else
  in.hold_recheck_s = poll::kHoldRecheckS;
#endif
  return in;
}

bool RepeaterPoller::anyTargetDue(uint32_t now) {
  for (uint8_t i = 0; i < kSlots; i++) {
    if (!meta_[i].used || meta_[i].orphan || states_[i].admin_seen) continue;
    if (!directRoute(i, NULL)) continue;
    if (poll::isDue(states_[i], meta_[i].interval_s, now, meta_[i].phase_s)) return true;
  }
  return false;
}

void RepeaterPoller::serviceControl() {
  const uint32_t now = host_->pollNow();
  const bool clock = clockOk();
  ctl_deferred_ = false;
  takeControl(now);
  takeClaim(now);
  if (!enabled_ || store_state_ == poll::STATE_NEWER || !clock) return;
  // With consent the key comes first, as before; without it the mode is learned first.
  if (!has_id_ && consent_) return;
  if (started_at_ == 0) started_at_ = now;

  // Owners that stopped answering are forgotten: this observer then polls every target itself.
  if (owners_ok_at_ != 0 && poll::ownersLapsed(owners_ok_at_, now)) {
    bool any = false;
    for (uint8_t i = 0; i < kSlots; i++) any = any || owners_[i].present;
    if (any) {
      memset(owners_, 0, sizeof(owners_));
      POLL_LOG("CoreScope owners not answered for %lus: polling every target", (unsigned long)poll::kOwnersStaleS);
    }
  }

  const uint8_t gate = gateAt(now);
  if (gate != gate_now_) {
    if (gate != poll::GATE_OPEN) {
      POLL_LOG("CoreScope gate %s%s: nothing is sent, poll now included", poll::gateName(gate),
               policy_.ever_fetched ? "" : " (saved)");
    } else {
      POLL_LOG("CoreScope gate %s lifted, polling resumes", poll::gateName(gate_now_));
    }
    gate_now_ = gate;
  }
  if (gate != poll::GATE_OPEN) gated_at_ = now;

  updateKey(now);
  maybeClaim(now);
  maybeFileControl(now);
}

void RepeaterPoller::updateKey(uint32_t now) {
  const bool shared = poll::sharedKeyUsable(has_shared_, shared_at_, shared_renew_failed_, now);
  const uint8_t want = poll::chooseKeyForMode(mode_, has_id_, reg_, shared, now);
  if (want == key_in_use_ || busy()) return;   // a run keeps the key it started with
  char pfx[9] = "-";
  if (want == poll::KEY_OWN) hex8(pfx, poll_id_.pub_key);
  else if (want == poll::KEY_SHARED) hex8(pfx, shared_id_.pub_key);
  POLL_LOG("polling key %s %s (was %s)", keyName(want), pfx, keyName(key_in_use_));
  key_in_use_ = want;
  key_note_ = 0;
  bindKey();
}

// Secrets and login state belong to one public key; any change of identity, a new shared key
// included, starts them over.
void RepeaterPoller::bindKey() {
  if (key_in_use_ == poll::KEY_NONE || busy()) return;
  const uint8_t* pub = pollKey().pub_key;
  static const uint8_t kUnbound[sizeof(acl_pub_)] = {0};
  if (memcmp(acl_pub_, kUnbound, sizeof(acl_pub_)) == 0) {
    if (key_in_use_ == poll::KEY_OWN) {
      memcpy(acl_pub_, pub, sizeof(acl_pub_));
      return;
    }
  } else if (memcmp(acl_pub_, pub, sizeof(acl_pub_)) == 0) {
    return;
  }
  // A target keeps one ACL entry per key: nothing learned under another key carries over.
  bool learned = false;
  for (uint8_t k = 0; k < poll::kStoredTargets; k++) learned = learned || stored_[k].learned_pw != poll::LEARNED_NONE;
  memcpy(acl_pub_, pub, sizeof(acl_pub_));
  forgetKeyState();
  if (learned) save_due_ = true;
}

void RepeaterPoller::takeClaim(uint32_t now) {
  if (!claim_pending_) return;
  uint8_t r = host_->pollTakeClaimResult();
  if (r == poll::CLAIM_PENDING) {
    if ((uint32_t)(millis() - claim_filed_ms_) < kClaimTimeoutMs) return;
    r = poll::CLAIM_FAILED;
  }
  claim_pending_ = false;
  if (r == poll::CLAIM_PUBLISHED) {
    poll::claimSent(claim_, claim_pending_sig_, claim_pending_community_, claim_pending_listed_, now);
    claim_sent_at_ = now;
    POLL_LOG("claim published to slot %u%s", (unsigned)claim_slot_, claim_pending_community_ ? "" : " (community off)");
  } else {
    // A bridge stop drops the answer (CLAIM_NONE): that is a failure too.
    poll::claimFailed(claim_, now);
    POLL_LOG("claim to slot %u failed; retry in %lus", (unsigned)claim_slot_, (unsigned long)poll::kClaimRetrySec);
  }
}

// One claim per target this observer can reach and may poll, as claims.py does. Claims go out
// while paused, gated or on standby so CoreScope keeps the roles current. No polling key: the
// list is withheld, and one empty claim takes back a list already published this boot.
__attribute__((noinline)) void RepeaterPoller::maybeClaim(uint32_t now) {
  if (claim_pending_ || claim_slot_ == 0) return;
  const bool may_list = poll::claimMayList(key_in_use_);
  if (may_list) ctl_note_ &= (uint8_t)~kNoteClaimsHeld;
  else if (!(ctl_note_ & kNoteClaimsHeld)) {
    POLL_LOG("claims held: no polling key in use");
    ctl_note_ |= kNoteClaimsHeld;
  }
  poll::ClaimNode nodes[poll::kMaxTargets];
  uint8_t n = 0;
  if (may_list) {
    for (uint8_t p = 0; p < count_ && n < poll::kMaxTargets; p++) {
      for (uint8_t i = 0; i < kSlots; i++) {
        if (!meta_[i].used || meta_[i].orphan || meta_[i].list_pos != p) continue;
        int8_t snr = 0;
        // An admin-stopped target is never polled here, so it is not offered either.
        if (states_[i].admin_seen || !directRoute(i, &snr) || poll::policyExcludesTarget(policy_, meta_[i].pub, policyCovers(i))) break;
        poll::ClaimNode& c = nodes[n++];
        memset(&c, 0, sizeof(c));
        memcpy(c.node, meta_[i].pub, poll::kPubKeySize);
        c.reply_hops = 0;
        c.personal_key = key_in_use_ == poll::KEY_OWN;
        c.has_snr = true;
        c.snr_x4 = snr;
        break;
      }
    }
  }
  const uint32_t sig = poll::claimSignature(n ? nodes : nullptr, n, community_);
  if (!poll::claimDue(claim_, sig, community_, may_list, now)) return;
  if (may_list && n == 0) return;   // nothing to claim, nothing to take back

  char json[poll::kClaimJsonMax + 1];
  char tag[48];
  uint8_t self_pub[poll::kPubKeySize];
  snprintf(tag, sizeof(tag), "meshcore.observer_fw/%s", host_->pollFirmwareVersion());
  host_->pollNodePub(self_pub);
  const size_t len = poll::buildClaimJson(json, sizeof(json), host_->pollNodeName(), self_pub, now, tag,
                                          policy_.version, community_, n ? nodes : nullptr, n);
  if (len == 0) {
    poll::claimFailed(claim_, now);
    POLL_LOG("claim not built (too long)");
    return;
  }
  if (!host_->pollRequestClaim(json, len, claim_slot_)) {
    poll::claimFailed(claim_, now);
    if (!(ctl_note_ & kNoteClaimRefused)) POLL_LOG("claim not handed to the bridge (stopped or busy); retry in %lus", (unsigned long)poll::kClaimRetrySec);
    ctl_note_ |= kNoteClaimRefused;
    return;
  }
  ctl_note_ &= (uint8_t)~kNoteClaimRefused;
  claim_pending_ = true;
  claim_pending_sig_ = sig;
  claim_pending_community_ = community_;
  claim_pending_listed_ = n > 0;
  claim_filed_ms_ = millis();
  if (n == 0) {
    POLL_LOG("claim filed: empty, no polling key in use, community %s, %u bytes", community_ ? "on" : "off",
             (unsigned)len);
  } else {
    POLL_LOG("claim filed: %u node%s, community %s, %u bytes", (unsigned)n, n == 1 ? "" : "s", community_ ? "on" : "off",
             (unsigned)len);
  }
}

void RepeaterPoller::maybeFileControl(uint32_t now) {
  if (ctl_pending_ || !has_url_) return;
  // The body may go out before the mode is known; the session sends it only on the encrypted path.
  const bool registering = poll::registrationWanted(poll::registrationDue(reg_, now), consent_ && has_id_, mode_);
  poll::ControlNeed need;
  need.enabled = enabled_;
  need.has_url = has_url_;
  need.clock_ok = true;
  need.registration_due = registering;
  need.claim_unanswered = community_ && claim_sent_at_ != 0 && claim_sent_at_ >= owners_ok_at_;
  need.gate_closed = gate_now_ != poll::GATE_OPEN;
  need.starting = startingAt(now);
  need.policy_answered = policy_answered_;
  need.target_due = anyTargetDue(now);
  need.manual_due = now_idx_ >= 0;
  if (!poll::controlNeeded(need)) return;
  const uint32_t since = (uint32_t)(millis() - ctl_filed_ms_) / 1000UL;
  if (!poll::controlAllowed(ctl_pending_, ctl_ever_, since, kCtlEveryS)) return;
  // A claim on its way goes first, so the owners answer already reflects it (the startup hold needs that).
  // No claim is filed while the polling key is none, so the registering session is not held for one.
  if (claim_pending_) {
    ctl_deferred_ = true;
    return;
  }
  if (ctl_retry_ms_ != 0 && (uint32_t)(millis() - ctl_retry_ms_) < kCtlRetryMs) return;
  const bool want_shared = !poll::modeIsJson(mode_) && poll::sharedKeyWanted(has_shared_, shared_at_, reg_, registering, now);
  discardUnasked();
  fileControl(now, registering, want_shared);
}

// A finished session nobody here asked for (a timed-out one, or the bench command's) would keep
// the mailbox from taking a new request; drop it without applying it.
__attribute__((noinline)) void RepeaterPoller::discardUnasked() {
  poll::ControlResult res;
  if (!host_->pollTakeControlResult(res)) return;
  wipe(&res, sizeof(res));
  POLL_LOG("dropped a CoreScope answer the poller was not waiting for");
}

// Own frames, so the request on the stack does not sit on top of the settings copy or the signing buffers.
__attribute__((noinline)) void RepeaterPoller::copyUrl(char* out, size_t cap) {
  PollSettings s;
  host_->pollReadSettings(s);
  strncpy(out, s.url, cap - 1);
  out[cap - 1] = '\0';
}

// Signed by the node key, which only signs; the uploaded private key is the polling key's.
__attribute__((noinline)) bool RepeaterPoller::buildRegistration(poll::ControlRequest& req, uint32_t now) {
  char msg[192];
  uint8_t sig[SIGNATURE_SIZE];
  const size_t mlen = poll::buildRegistrationMessage(msg, sizeof(msg), req.observer_pub, poll_id_.pub_key, now);
  if (mlen == 0) return false;
  host_->pollSignRegistration(sig, (const uint8_t*)msg, mlen);
  mesh::Identity node(req.observer_pub);
  if (!node.verify(sig, (const uint8_t*)msg, (int)mlen)) return false;
  uint8_t keys[PRV_KEY_SIZE + PUB_KEY_SIZE];
  poll_id_.writeTo(keys, sizeof(keys));
  req.reg_body_len = (uint16_t)poll::buildRegistrationBody(req.reg_body, sizeof(req.reg_body), req.observer_pub,
                                                           poll_id_.pub_key, keys, PRV_KEY_SIZE, now, sig);
  wipe(keys, sizeof(keys));
  return req.reg_body_len != 0;
}

__attribute__((noinline)) void RepeaterPoller::fileControl(uint32_t now, bool registering, bool want_shared) {
  poll::ControlRequest req;
  memset(&req, 0, sizeof(req));
  copyUrl(req.url, sizeof(req.url));
  strncpy(req.client_version, host_->pollFirmwareVersion(), sizeof(req.client_version) - 1);   // as in the claim's plugin tag
  req.mode = mode_;
  host_->pollNodePub(req.observer_pub);
  ctl_count_ = 0;
  for (uint8_t p = 0; p < count_; p++) {
    for (uint8_t i = 0; i < kSlots; i++) {
      if (!meta_[i].used || meta_[i].orphan || meta_[i].list_pos != p) continue;
      if (ctl_count_ >= poll::kMaxTargets) break;
      memcpy(req.targets[ctl_count_], meta_[i].pub, poll::kPubKeySize);
      ctl_slot_[ctl_count_] = (int8_t)i;
      memcpy(ctl_pfx_[ctl_count_], meta_[i].pub, sizeof(ctl_pfx_[0]));
      ctl_count_++;
      break;
    }
  }
  req.target_count = ctl_count_;

  if (registering && !buildRegistration(req, now)) {
    registering = false;
    req.reg_body_len = 0;
    poll::registrationResult(reg_, 0, false, now);   // retried like a failed answer
    POLL_LOG("registration not built: the node key gave no verifiable signature");
  }
  req.want = poll::controlWants(community_, registering, want_shared);
  const bool ok = host_->pollRequestControl(req);
  wipe(req.reg_body, sizeof(req.reg_body));
  if (!ok) {
    ctl_retry_ms_ = millis() | 1UL;
    if (!(ctl_note_ & kNoteRefused)) POLL_LOG("CoreScope session not filed: bridge stopped or busy; retry in %lus", (unsigned long)(kCtlRetryMs / 1000));
    ctl_note_ |= kNoteRefused;
    return;
  }
  ctl_note_ &= (uint8_t)~kNoteRefused;
  ctl_retry_ms_ = 0;
  ctl_pending_ = true;
  ctl_ever_ = true;
  ctl_want_ = req.want;
  ctl_filed_ms_ = millis();
  POLL_LOG("CoreScope session filed:%s%s%s%s", (req.want & poll::WANT_OWNERS) ? " owners" : "",
           (req.want & poll::WANT_POLICY) ? " policy" : "", (req.want & poll::WANT_REGISTER) ? " register" : "",
           (req.want & poll::WANT_SHARED_KEY) ? " shared-key" : "");
}

void RepeaterPoller::noSession(uint8_t outcome, uint32_t now) {
  ctl_last_ = outcome;
  ctl_last_at_ = now;
  memset(ctl_status_, 0, sizeof(ctl_status_));
  // Counts toward a stale policy like the plugin's "CoreScope unreachable".
  poll::applyPolicyAnswer(policy_, false, 0, policy_, now, kFastPolicy);
  if (ctl_want_ & poll::WANT_SHARED_KEY) sharedRenewalFailed(now);
}

// Only an expired key is dropped; a running exchange keeps it, since the key switch waits for the run.
void RepeaterPoller::sharedRenewalFailed(uint32_t now) {
  if (!has_shared_ || shared_renew_failed_ || poll::elapsedSec(now, shared_at_) < poll::kSharedKeyMaxAgeS) return;
  shared_renew_failed_ = true;
  POLL_LOG("CoreScope shared key expired and its renewal failed: not used until one is fetched");
}

__attribute__((noinline)) void RepeaterPoller::takeControl(uint32_t now) {
  if (!ctl_pending_) return;
  if ((uint32_t)(millis() - ctl_filed_ms_) >= poll::kControlTimeoutS * 1000UL) {
    // A bridge stop or restart empties the mailbox without an answer.
    ctl_pending_ = false;
    noSession(CTL_TIMEOUT, now);
    POLL_LOG("CoreScope session not answered in %lus: treated as not started", (unsigned long)poll::kControlTimeoutS);
    return;
  }
  poll::ControlResult res;
  if (!host_->pollTakeControlResult(res)) return;
  ctl_pending_ = false;
  applyControl(res, now);
  wipe(&res, sizeof(res));
}

void RepeaterPoller::applyControl(const poll::ControlResult& res, uint32_t now) {
  const uint8_t want = ctl_want_;
  if (want == 0) {
    POLL_LOG("dropped a CoreScope answer for the previous URL");
    return;
  }
  if (!res.started) {
    noSession(CTL_NO_SESSION, now);
    POLL_LOG("CoreScope session did not start (link, clock or handshake guard)");
    return;
  }
  ctl_last_ = CTL_DONE;
  ctl_last_at_ = now;
  ctl_status_[0] = res.owners_status;
  ctl_status_[1] = res.policy_status;
  ctl_status_[2] = res.reg_status;
  ctl_status_[3] = res.shared_status;

  // Policy: only a 200 replaces the copy in use; a 404 or a failure keeps its switches.
  char before[sizeof(policy_.version)];
  memcpy(before, policy_.version, sizeof(before));
  const bool had_policy = policy_.ever_fetched;
  const uint8_t pa = poll::applyPolicyAnswer(policy_, res.policy_ok, res.policy_status, res.policy, now, kFastPolicy);
  if (pa != poll::POLICY_KEPT) policy_answered_ = true;
  if (pa == poll::POLICY_APPLIED) {
    policy_covered_ = 0;
    for (uint8_t k = 0; k < ctl_count_ && k < poll::kMaxTargets; k++) {
      const int8_t i = ctl_slot_[k];
      if (i >= 0 && meta_[i].used && memcmp(meta_[i].pub, ctl_pfx_[k], sizeof(ctl_pfx_[0])) == 0) policy_covered_ |= (uint16_t)(1U << i);
    }
    if (policy_.excluded_truncated || policy_.windows_truncated) {
      POLL_LOG("CoreScope policy lists more than fit: kept the entries for this node's targets");
    }
    if (!had_policy || strcmp(before, policy_.version) != 0) {
      POLL_LOG("CoreScope policy %s: window %lus grace %lus cap %u, %u excluded", policy_.version[0] ? policy_.version : "-",
               (unsigned long)policy_.direct_s, (unsigned long)policy_.grace_s, (unsigned)policy_.max_requests,
               (unsigned)policy_.excluded_count);
    }
    const poll::SavedGate g = poll::savedGateFrom(policy_, poll::kPluginVersion);
    if (!poll::savedGateEqual(g, gate_)) {
      gate_ = g;
      gate_save_due_ = true;   // rare: written only when the switches change
    }
  } else if (pa == poll::POLICY_NONE && !had_policy) {
    POLL_LOG("CoreScope has no polling policy (404): defaults apply");
  } else if (pa == poll::POLICY_KEPT) {
    POLL_LOG("CoreScope policy: status %d; keeping the %s", (int)res.policy_status, had_policy ? "fetched copy" : "saved gate and defaults");
  }

  const uint8_t mode = poll::modeAfterPolicy(mode_, res.policy_ok, res.policy_status, res.policy.reading_format);
  if (mode != mode_) {
    if (poll::registrationVoided(mode_, mode)) memset(&reg_, 0, sizeof(reg_));   // registered again before the key is used
    POLL_LOG("reading mode %s (was %s)", modeName(mode), modeName(mode_));
    mode_ = mode;
  }

  // Owners, index-aligned with the request's targets; a target that left the list meanwhile is skipped.
  if ((want & poll::WANT_OWNERS) && res.owners_ok) {
    memset(owners_, 0, sizeof(owners_));
    for (uint8_t k = 0; k < ctl_count_ && k < poll::kMaxTargets; k++) {
      const int8_t i = ctl_slot_[k];
      if (i < 0 || !meta_[i].used || memcmp(meta_[i].pub, ctl_pfx_[k], sizeof(ctl_pfx_[0])) != 0) continue;
      owners_[i] = res.owners[k];
    }
    owners_ok_at_ = now;
  }

  if ((want & poll::WANT_REGISTER) && !res.keys_skipped) {
    const bool was_ok = reg_.ok, was_unsupported = reg_.unsupported;
    poll::registrationResult(reg_, res.reg_status, res.reg_body_ok, now);
    if (reg_.ok && res.reg_status == 200) {
      if (!was_ok) POLL_LOG("own polling key registered with CoreScope");
    } else if (reg_.unsupported) {
      if (!was_unsupported) POLL_LOG("CoreScope takes no per-observer keys (404): using the shared key");
    } else {
      POLL_LOG("polling key registration failed: status %d%s", (int)res.reg_status, reg_.ok ? " (earlier one still valid)" : "");
    }
  }

  bool shared_taken = false;
  if (res.shared_ok) {
    const uint8_t h = res.shared_pub[0];
    if (h == 0x00 || h == 0xFF || h == host_->pollNodeHash()) {
      // Replies to it would read as reserved or as addressed to the node itself.
      POLL_LOG("CoreScope shared key refused: its hash collides with a reserved or the node's own");
    } else {
      uint8_t keys[PRV_KEY_SIZE + PUB_KEY_SIZE];
      memcpy(keys, res.shared_prv, PRV_KEY_SIZE);
      memcpy(keys + PRV_KEY_SIZE, res.shared_pub, PUB_KEY_SIZE);
      const bool changed = !has_shared_ || memcmp(shared_id_.pub_key, res.shared_pub, PUB_KEY_SIZE) != 0;
      if (changed && busy() && key_in_use_ == poll::KEY_SHARED) {
        POLL_LOG("CoreScope shared key changed during a poll; taken next time");
        shared_taken = true;   // answered; not a failed renewal
      } else {
        shared_id_.readFrom(keys, sizeof(keys));
        has_shared_ = true;
        shared_at_ = now;
        shared_renew_failed_ = false;
        shared_taken = true;
        if (changed) {
          bindKey();
          char pfx[9];
          hex8(pfx, shared_id_.pub_key);
          POLL_LOG("CoreScope shared key %s", pfx);
        }
      }
      wipe(keys, sizeof(keys));
    }
  }
  if ((want & poll::WANT_SHARED_KEY) && !shared_taken && !res.keys_skipped) sharedRenewalFailed(now);

  char o[8], p[8], r[8], k[8];
  fmtStatus(o, sizeof(o), want & poll::WANT_OWNERS, res.owners_status);
  fmtStatus(p, sizeof(p), want & poll::WANT_POLICY, res.policy_status);
  fmtStatus(r, sizeof(r), (want & poll::WANT_REGISTER) && !res.keys_skipped, res.reg_status);
  fmtStatus(k, sizeof(k), ((want & poll::WANT_SHARED_KEY) && !res.keys_skipped) || res.shared_status != 0, res.shared_status);
  POLL_LOG("CoreScope answered: owners %s policy %s register %s shared-key %s%s", o, p, r, k,
           res.keys_skipped ? " (key requests held: mode not encrypted)" : "");
}

void RepeaterPoller::noteSkip(uint8_t idx, const poll::Decision& d, uint32_t now) {
  poll::TargetState& t = states_[idx];
  const uint8_t prev = skip_[idx];
  skip_[idx] = d.poll ? (uint8_t)poll::SKIP_NONE : d.reason;
  t.role = poll::mapRole(community_, owners_[idx]);
  t.standby_rank = t.role == poll::ROLE_STANDBY ? (owners_[idx].standby_rank ? owners_[idx].standby_rank : 1) : 0;
  char pfx[9];
  hex8(pfx, meta_[idx].pub);
  if (d.poll) {
    if (t.role == poll::ROLE_STANDBY) {
      POLL_LOG("%s standby#%u: newest reading is older than the window plus grace; stepping in", pfx,
               (unsigned)t.standby_rank);
    }
    return;
  }
  // The gate is logged once for every target; the others once per change.
  if (d.reason == prev || d.reason == poll::SKIP_GATE) return;
  if (d.until > now) {
    POLL_LOG("%s skip %s (%s), looks again in %lus", pfx, poll::skipReasonName(d.reason), poll::roleName(t.role),
             (unsigned long)(d.until - now));
  } else {
    POLL_LOG("%s skip %s (%s)", pfx, poll::skipReasonName(d.reason), poll::roleName(t.role));
  }
}

// poll_cycle.py: the targets whose slot is due, oldest attempt first, then skip_reason on each
// until one is chosen. One scheduled poll at a time.
void RepeaterPoller::scheduleTick(uint32_t now) {
  if (!ctl_pending_) ctl_note_ &= (uint8_t)~kNoteWaitCtl;
  bool due[kSlots];
  bool any = false;
  for (uint8_t i = 0; i < kSlots; i++) {
    due[i] = false;
    if (!meta_[i].used || meta_[i].orphan) continue;
    poll::TargetState& t = states_[i];
    if (t.admin_seen) {
      note(i, NOTE_ADMIN, 0);
      continue;
    }
    if (!directRoute(i, NULL)) {
      t.code = poll::CODE_NOT_DIRECT;
      note(i, NOTE_NOT_DIRECT, 0);
      continue;
    }
    if (!poll::isDue(t, meta_[i].interval_s, now, meta_[i].phase_s)) {
      note(i, NOTE_WAIT, 0);
      continue;
    }
    due[i] = true;
    any = true;
  }
  if (!any) return;
  // The decision is made on fresh data when a session is on its way; it is bounded, so this cannot hang.
  if (ctl_pending_ || ctl_deferred_) {
    if (!(ctl_note_ & kNoteWaitCtl)) POLL_LOG("a target is due: waiting for the CoreScope answer");
    ctl_note_ |= kNoteWaitCtl;
    return;
  }
  for (;;) {
    const int pick = poll::pickDue(states_, due, kSlots, now, last_scheduled_at_, kScheduledGapS);
    if (pick < 0) return;
    due[pick] = false;
    const poll::Decision d = poll::decideCycle(states_[pick], cycleInputFor((uint8_t)pick, false, now), now);
    noteSkip((uint8_t)pick, d, now);
    if (!d.poll) continue;
    if (key_in_use_ == poll::KEY_NONE) {
      // The plugin falls back to its companion path here; this firmware has none, so nothing is sent.
      if (!key_note_) POLL_LOG("a target is due but no polling key is in use (not registered with CoreScope, no shared key)");
      key_note_ = 1;
      return;
    }
    note((uint8_t)pick, NOTE_DUE, 0);
    startPoll((uint8_t)pick, true);
    return;
  }
}

void RepeaterPoller::fmtControl(char* out, size_t cap, uint32_t now) const {
  char reg[32], pol[40], own[24], ctl[48], clm[24], age[12];
  if (reg_.tried_at == 0) {
    strcpy(reg, "never");
  } else if (reg_.ok) {
    fmtAge(age, sizeof(age), poll::elapsedSec(now, reg_.ok_at));
    snprintf(reg, sizeof(reg), "ok %s%s", age, poll::chooseKey(reg_, false, now) == poll::KEY_OWN ? "" : " (lapsed)");
  } else if (reg_.unsupported) {
    strcpy(reg, "unsupported");
  } else {
    snprintf(reg, sizeof(reg), "failed %d", (int)reg_.last_status);
  }
  if (policy_.ever_fetched) {
    fmtAge(age, sizeof(age), poll::elapsedSec(now, policy_.fetched_at));
    snprintf(pol, sizeof(pol), "%s %s%s", policy_.version[0] ? policy_.version : "-", age,
             poll::policyStale(policy_, now) ? " stale" : "");
  } else {
    strcpy(pol, policy_answered_ ? "none (404)" : "-");
  }
  if (!community_) {
    strcpy(own, "off");
  } else if (owners_ok_at_) {
    fmtAge(own, sizeof(own), poll::elapsedSec(now, owners_ok_at_));
  } else {
    strcpy(own, "-");
  }
  fmtAge(age, sizeof(age), poll::elapsedSec(now, ctl_last_at_));
  if (ctl_pending_) {
    strcpy(ctl, "pending");
  } else if (ctl_last_ == CTL_NEVER) {
    strcpy(ctl, "-");
  } else if (ctl_last_ == CTL_NO_SESSION) {
    snprintf(ctl, sizeof(ctl), "no session %s", age);
  } else if (ctl_last_ == CTL_TIMEOUT) {
    snprintf(ctl, sizeof(ctl), "timeout %s", age);
  } else {
    char o[8], p[8], r[8], k[8];
    fmtStatus(o, sizeof(o), ctl_status_[0] != 0, ctl_status_[0]);
    fmtStatus(p, sizeof(p), ctl_status_[1] != 0, ctl_status_[1]);
    fmtStatus(r, sizeof(r), ctl_status_[2] != 0, ctl_status_[2]);
    fmtStatus(k, sizeof(k), ctl_status_[3] != 0, ctl_status_[3]);
    snprintf(ctl, sizeof(ctl), "%s/%s/%s/%s %s", o, p, r, k, age);
  }
  if (claim_pending_) {
    strcpy(clm, "pending");
  } else if (claim_sent_at_) {
    fmtAge(age, sizeof(age), poll::elapsedSec(now, claim_sent_at_));
    snprintf(clm, sizeof(clm), "%s%s", age, claim_.last_community ? "" : " (off)");
  } else {
    strcpy(clm, "-");
  }
  snprintf(out, cap, "> reg %s, policy %s, owners %s, ctl %s, claim %s", reg, pol, own, ctl, clm);
}

#endif  // WITH_REPEATER_POLL
