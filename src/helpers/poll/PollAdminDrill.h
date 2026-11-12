#pragma once

// Removal of an admin grant on the polling key, as an event-driven step function.
// Sequence: "setperm <polling key> 0" (works because the key still holds the
// rights it gives up), settle, then up to three rounds of a blank login plus a
// route teach, each listening for a non-admin login answer that proves the grant
// is gone. Same driving rules and "no change" convention as PollExchange.h.

#include "PollExchange.h"

namespace poll {

static const uint32_t kDrillSettleMs = 10000;   // let the setperm take before the verifying login
static const uint32_t kDrillGapMs = 8000;       // between verifying rounds
static const uint32_t kDrillListenMs = 20000;   // for the verifying login's answer
static const uint8_t  kDrillRounds = 3;

class PollAdminDrill {
 public:
  PollAdminDrill() { clear(); }

  Step start(uint32_t now) {
    (void)now;
    clear();
    phase_ = PH_SETPERM_TX;
    return act(ACT_SEND_SETPERM);
  }

  Step onEvent(uint8_t ev, bool flooded, uint32_t now) {
    (void)flooded;
    (void)now;
    if (!active()) return stepNoChange();
    switch (ev) {
      case EV_TX_DONE: return onTxReport(true);
      case EV_TX_FAILED: return onTxReport(false);
      case EV_TIMER: return onTimer();
      case EV_LOGIN_GUEST: return onLoginAnswer(false);
      case EV_LOGIN_ADMIN: return onLoginAnswer(true);
      default: return stepNoChange();
    }
  }

  bool active() const { return phase_ != PH_IDLE && phase_ != PH_DONE; }
  bool removed() const { return removed_; }

  // True from the first verifying ACT_SEND_LOGIN, even before its TX report, to the end of the drill.
  bool expectsLogin() const { return active() && login_handed_; }

  uint8_t adminAnswers() const { return admin_answers_; }   // verifying logins still answered as admin
  uint8_t rounds() const { return round_; }
  uint8_t logins() const { return logins_; }
  uint8_t teaches() const { return teaches_; }
  bool setpermSent() const { return setperm_sent_; }

 private:
  enum Phase : uint8_t {
    PH_IDLE = 0,
    PH_SETPERM_TX,
    PH_SETTLE,
    PH_LOGIN_TX,
    PH_TEACH_TX,
    PH_LISTEN,
    PH_GAP,
    PH_DONE,
  };

  void clear() {
    phase_ = PH_IDLE;
    round_ = 0;
    logins_ = 0;
    teaches_ = 0;
    admin_answers_ = 0;
    removed_ = false;
    setperm_sent_ = false;
    login_handed_ = false;
    round_login_ok_ = false;
    held_guest_ = false;
  }

  bool awaitingTx() const { return phase_ == PH_SETPERM_TX || phase_ == PH_LOGIN_TX || phase_ == PH_TEACH_TX; }

  static Step act(uint8_t a) {
    Step s = stepNoChange();
    s.act = a;
    return s;
  }

  Step wait(uint8_t phase, uint32_t ms) {
    phase_ = phase;
    Step s = stepNoChange();
    s.wait_ms = ms;
    return s;
  }

  Step finish(bool removed) {
    removed_ = removed;
    phase_ = PH_DONE;
    return act(ACT_FINISH);
  }

  Step sendLogin() {
    phase_ = PH_LOGIN_TX;
    Step s = act(ACT_SEND_LOGIN);
    s.pw = PW_BLANK;
    login_handed_ = true;
    return s;
  }

  Step roundOver() {
    round_++;
    if (round_ >= kDrillRounds) return finish(false);
    return wait(PH_GAP, kDrillGapMs);
  }

  Step onTxReport(bool ok) {
    if (!awaitingTx()) return stepNoChange();
    switch (phase_) {
      case PH_SETPERM_TX:
        // Without the command on the air there is nothing to verify.
        if (!ok) return finish(false);
        setperm_sent_ = true;
        return wait(PH_SETTLE, kDrillSettleMs);
      case PH_LOGIN_TX:
        round_login_ok_ = ok;
        if (ok) logins_++;
        if (held_guest_) return finish(true);
        // setperm 0 deleted the ACL entry and its route, so teach again or the answer floods past us.
        phase_ = PH_TEACH_TX;
        return act(ACT_SEND_TEACH);
      case PH_TEACH_TX:
        if (ok) teaches_++;
        if (held_guest_) return finish(true);
        if (round_login_ok_) return wait(PH_LISTEN, kDrillListenMs);
        return roundOver();
      default: return stepNoChange();
    }
  }

  Step onTimer() {
    switch (phase_) {
      case PH_SETTLE:
      case PH_GAP: return sendLogin();
      case PH_LISTEN: return roundOver();
      default: return stepNoChange();
    }
  }

  Step onLoginAnswer(bool admin) {
    if (!expectsLogin()) return stepNoChange();
    if (admin) {
      // Still admin: keep listening, as a later answer may yet come back as guest.
      admin_answers_++;
      return stepNoChange();
    }
    if (held_guest_) return stepNoChange();
    if (awaitingTx()) {
      held_guest_ = true;
      return stepNoChange();
    }
    return finish(true);
  }

  uint8_t phase_;
  uint8_t round_;
  uint8_t logins_;
  uint8_t teaches_;
  uint8_t admin_answers_;
  bool    removed_;
  bool    setperm_sent_;
  bool    login_handed_;    // a verifying login was handed to the owner
  bool    round_login_ok_;  // this round's login left the radio
  bool    held_guest_;
};

}  // namespace poll
