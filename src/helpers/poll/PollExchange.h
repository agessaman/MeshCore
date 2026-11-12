#pragma once

// One guest-key poll of a direct neighbor as an event-driven step function.
// Matches the guest-key poll of the CoreScope Observer Plus plugin (v0.11.1),
// restricted to direct routes (no alternative routes, no relay hops).
//
// Driving it: see the loop in PollTypes.h. The owner allocates tags, classifies
// received packets and feeds events; the exchange never looks at tags.
//   - After ACT_SEND_*, feed EV_TX_DONE or EV_TX_FAILED. Listen windows start at
//     EV_TX_DONE; EV_TX_FAILED skips the window as if it had timed out.
//   - After ACT_WAIT, feed EV_TIMER once wait_ms has elapsed, or a reply event.
//
// "No change" convention: a Step with act == ACT_WAIT and wait_ms == 0 (see
// isNoChange()) means the event was ignored or held. The owner keeps doing what
// it was doing: the previous timer keeps running, a pending TX report is still
// expected. Real waits are never 0 ms. Events after ACT_FINISH return no change.
//
// A reply that arrives while a TX report is pending is held and acted on at
// that report, so the machine never finishes with a packet unreported.
//
// TargetState fields written here: logged_in_at, login_checked_at, learned_pw,
// key_login_fails, flood_replies, route_sent_at, status_checked_at (set when a
// status request leaves the radio, whether or not it is answered). Everything
// else (code, no_reply_count, last_success, ...) belongs to PollSchedule and is
// only read, as it was before this poll.

#include "PollTypes.h"

namespace poll {

static const uint8_t  kForgetLoginAfter = 3;     // log in again only on every 3rd miss in a row
static const uint32_t kRouteRefreshS = kDay;     // re-teach a route this old before a bare request
static const uint32_t kAdminRecheckS = kDay;     // force a login this often to catch a quiet admin grant
static const uint16_t kDefaultStatusHours = 6;

// Why a poll ended without a data reply.
enum ExchangeReason : uint8_t {
  XR_NONE = 0,             // a data reply arrived
  XR_ENTRY_KEPT,           // fast path unanswered; ACL entry still trusted, no login this time
  XR_LOGIN_UNANSWERED,     // no login answer, so no request
  XR_REQUEST_UNANSWERED,   // login answered, the request was not
  XR_ADMIN,                // login answered as admin; run PollAdminDrill
  XR_NOTHING_SENT,         // every send failed; nothing left the radio
};

static inline const char* exchangeReasonName(uint8_t r) {
  switch (r) {
    case XR_NONE: return "replied";
    case XR_ENTRY_KEPT: return "no answer, ACL entry kept";
    case XR_LOGIN_UNANSWERED: return "login unanswered";
    case XR_REQUEST_UNANSWERED: return "no answer to the request after the login";
    case XR_ADMIN: return "answered as admin";
    case XR_NOTHING_SENT: return "nothing sent";
    default: return "unknown";
  }
}

struct ExchangeConfig {
  uint8_t  pw_mode;        // PasswordMode operator override
  bool     personal;       // true: per-observer polling key; false: public shared key
  uint16_t status_hours;   // status request at most this often; 0 = default (6)
  Timing   timing;
};

static inline ExchangeConfig defaultExchangeConfig() {
  ExchangeConfig c;
  c.pw_mode = PWMODE_AUTO;
  c.personal = false;
  c.status_hours = kDefaultStatusHours;
  c.timing = defaultTiming();
  return c;
}

struct ExchangeResult {
  bool    got_reply;       // a data reply arrived
  bool    reply_flooded;   // ...and it was flooded
  bool    admin;           // a login was answered as admin: owner must run PollAdminDrill
  uint8_t kind;            // ReqKind chosen for this poll
  bool    login_tried;     // the login path ran
  bool    login_answered;
  bool    login_flooded;   // the login answer came back flooded
  uint8_t logins, teaches, requests;   // packets that left the radio (EV_TX_DONE)
  uint8_t reason;          // ExchangeReason
};

static inline Step stepNoChange() {
  Step s;
  s.act = ACT_WAIT;
  s.pw = 0;
  s.kind = 0;
  s.wait_ms = 0;
  return s;
}

static inline bool isNoChange(const Step& s) { return s.act == ACT_WAIT && s.wait_ms == 0; }

// Seconds since `at`; an unset or future stamp reads as "long ago" so the check errs toward acting.
static inline uint32_t secondsSince(uint32_t now, uint32_t at) {
  return (at == 0 || at > now) ? 0xFFFFFFFFUL : now - at;
}

static inline bool statusDue(const TargetState& t, uint32_t now, uint16_t hours) {
  if (hours == 0) hours = kDefaultStatusHours;
  return secondsSince(now, t.status_checked_at) >= (uint32_t)hours * 3600UL;
}

// The repeater may have lost our return route, so teach it before a bare request.
static inline bool routeMayBeLost(const TargetState& t, uint32_t now) {
  return t.flood_replies > 0 || (t.code != CODE_OK && t.code != CODE_NO_BATTERY) ||
         secondsSince(now, t.route_sent_at) >= kRouteRefreshS;
}

// Passwords for one poll, one login each; returns the count (1 or 2).
static inline uint8_t passwordOrder(const TargetState& t, uint32_t now, uint8_t pw_mode, bool personal,
                                    uint8_t out[2]) {
  if (pw_mode == PWMODE_BLANK || pw_mode == PWMODE_HELLO) {
    out[0] = pw_mode == PWMODE_HELLO ? PW_HELLO : PW_BLANK;
    return 1;
  }
  // A blank success under the shared key may be another observer's ACL entry, so only a personal key trusts it.
  if (t.learned_pw == LEARNED_HELLO || (personal && t.learned_pw == LEARNED_BLANK)) {
    out[0] = t.learned_pw == LEARNED_HELLO ? PW_HELLO : PW_BLANK;
    return 1;
  }
  // Logins keep failing: one per poll, alternating by UTC day, to keep flooded answers rare.
  if (t.key_login_fails >= 2) {
    out[0] = ((now / kDay) % 2 == 0) ? PW_BLANK : PW_HELLO;
    return 1;
  }
  out[0] = PW_BLANK;
  out[1] = PW_HELLO;
  return 2;
}

class PollExchange {
 public:
  PollExchange() { clear(); }

  Step start(TargetState& t, const ExchangeConfig& cfg, uint32_t now) {
    clear();
    cfg_ = cfg;
    if (cfg_.status_hours == 0) cfg_.status_hours = kDefaultStatusHours;
    now_ = now;
    res_.kind = statusDue(t, now, cfg_.status_hours) ? REQ_STATUS : REQ_TELEMETRY;
    bool in_acl = t.logged_in_at != 0;
    bool recheck_due = secondsSince(now, t.login_checked_at) >= kAdminRecheckS;
    if (in_acl && !recheck_due) {
      // A lost packet is far likelier than a lost ACL entry, so ask without logging in.
      if (routeMayBeLost(t, now)) return sendTeach(PH_FAST_TEACH_TX);
      return sendRequest(PH_FAST_REQ_TX);
    }
    return beginLogin(t, in_acl);
  }

  Step onEvent(TargetState& t, uint8_t ev, bool flooded, uint32_t now) {
    if (!active()) return stepNoChange();
    now_ = now;
    switch (ev) {
      case EV_TX_DONE: return onTxReport(t, true);
      case EV_TX_FAILED: return onTxReport(t, false);
      case EV_TIMER: return onTimer(t);
      case EV_DATA_REPLY: return onDataReply(t, flooded);
      case EV_LOGIN_GUEST: return onLoginAnswer(t, false, flooded);
      case EV_LOGIN_ADMIN: return onLoginAnswer(t, true, flooded);
      default: return stepNoChange();
    }
  }

  bool active() const { return phase_ != PH_IDLE && phase_ != PH_DONE; }

  // True from the first ACT_SEND_LOGIN to the end of the poll, so an admin answer is never dropped.
  // A guest answer after one was already taken is ignored.
  bool expectsLogin() const { return active() && login_handed_; }

  // Any time after the first request, so a late reply during a gap or a later step still counts.
  bool expectsData() const { return active() && req_asked_ && !res_.got_reply; }

  const ExchangeResult& result() const { return res_; }

 private:
  enum Phase : uint8_t {
    PH_IDLE = 0,
    PH_FAST_TEACH_TX,
    PH_FAST_SETTLE,
    PH_FAST_REQ_TX,
    PH_FAST_LISTEN,
    PH_FAST_GAP,
    PH_PRE_TEACH_TX,   // admin recheck of a live entry: route first so the answer comes back direct
    PH_PRE_SETTLE,
    PH_LOGIN_GAP,
    PH_LOGIN_TX,
    PH_LOGIN_LISTEN,
    PH_FLOOD_CLEAR,
    PH_POST_TEACH_TX,
    PH_POST_SETTLE,
    PH_REQ_TX,
    PH_REQ_LISTEN,
    PH_DONE,
  };

  void clear() {
    memset(&res_, 0, sizeof(res_));
    cfg_ = defaultExchangeConfig();
    phase_ = PH_IDLE;
    now_ = 0;
    tried_ = 0;
    fast_reqs_ = 0;
    pw_count_ = 0;
    pw_next_ = 0;
    pws_[0] = pws_[1] = PW_BLANK;
    out_pw_ = PW_BLANK;
    handed_pw_ = PW_BLANK;
    answer_pw_ = PW_BLANK;
    login_handed_ = false;
    in_acl_ = false;
    req_asked_ = false;
    login_out_ = false;
    held_login_ = false;
    held_admin_ = false;
    held_flooded_ = false;
    held_data_ = false;
  }

  bool awaitingTx() const {
    return phase_ == PH_FAST_TEACH_TX || phase_ == PH_FAST_REQ_TX || phase_ == PH_PRE_TEACH_TX ||
           phase_ == PH_LOGIN_TX || phase_ == PH_POST_TEACH_TX || phase_ == PH_REQ_TX;
  }

  static uint8_t inc(uint8_t v) { return v == 0xFF ? v : (uint8_t)(v + 1); }

  Step wait(uint8_t phase, uint32_t ms) {
    phase_ = phase;
    Step s = stepNoChange();
    s.wait_ms = ms ? ms : 1;
    return s;
  }

  Step send(uint8_t phase, uint8_t act) {
    phase_ = phase;
    Step s = stepNoChange();
    s.act = act;
    return s;
  }

  Step sendTeach(uint8_t phase) { return send(phase, ACT_SEND_TEACH); }

  Step sendRequest(uint8_t phase) {
    req_asked_ = true;
    if (phase == PH_FAST_REQ_TX) {
      tried_++;
      fast_reqs_++;
    }
    Step s = send(phase, ACT_SEND_REQUEST);
    s.kind = res_.kind;
    return s;
  }

  Step sendLogin() {
    tried_++;
    Step s = send(PH_LOGIN_TX, ACT_SEND_LOGIN);
    s.pw = pws_[pw_next_++];
    handed_pw_ = s.pw;
    login_handed_ = true;
    return s;
  }

  Step beginLogin(TargetState& t, bool in_acl) {
    res_.login_tried = true;
    in_acl_ = in_acl;
    pw_count_ = passwordOrder(t, now_, cfg_.pw_mode, cfg_.personal, pws_);
    pw_next_ = 0;
    if (in_acl) return sendTeach(PH_PRE_TEACH_TX);
    return nextLogin(t);
  }

  Step nextLogin(TargetState& t) {
    if (pw_next_ >= pw_count_) {
      if (res_.logins > 0) t.key_login_fails = inc(t.key_login_fails);
      return finish(t, XR_LOGIN_UNANSWERED);
    }
    if (tried_ > 0) return wait(PH_LOGIN_GAP, cfg_.timing.retry_gap_ms);
    return sendLogin();
  }

  Step fastMiss(TargetState& t) {
    if (fast_reqs_ < 2) return wait(PH_FAST_GAP, cfg_.timing.retry_gap_ms);
    // Guests drop out only on a reboot or ACL churn, so a login is worth its airtime only now and then.
    unsigned misses = (unsigned)t.no_reply_count + 1;
    if (misses % kForgetLoginAfter != 0) return finish(t, XR_ENTRY_KEPT);
    return beginLogin(t, false);
  }

  Step onTxReport(TargetState& t, bool ok) {
    if (!awaitingTx()) return stepNoChange();
    uint8_t p = phase_;
    if (ok) {
      switch (p) {
        case PH_FAST_TEACH_TX:
        case PH_PRE_TEACH_TX:
        case PH_POST_TEACH_TX:
          res_.teaches = inc(res_.teaches);
          t.route_sent_at = now_;
          break;
        case PH_FAST_REQ_TX:
        case PH_REQ_TX:
          res_.requests = inc(res_.requests);
          if (res_.kind == REQ_STATUS) t.status_checked_at = now_;
          break;
        case PH_LOGIN_TX:
          res_.logins = inc(res_.logins);
          login_out_ = true;
          out_pw_ = pws_[pw_next_ - 1];
          break;
      }
    }
    // Held replies: an admin answer outranks everything, then the data reply.
    if (held_login_) {
      held_login_ = false;
      Step s = acceptLogin(t, held_admin_, held_flooded_);
      if (!active() || !held_data_) return s;
    }
    if (held_data_) return finish(t, XR_NONE);
    switch (p) {
      case PH_FAST_TEACH_TX: return ok ? wait(PH_FAST_SETTLE, cfg_.timing.settle_ms) : sendRequest(PH_FAST_REQ_TX);
      case PH_FAST_REQ_TX: return ok ? wait(PH_FAST_LISTEN, cfg_.timing.request_listen_ms) : fastMiss(t);
      case PH_PRE_TEACH_TX: return ok ? wait(PH_PRE_SETTLE, cfg_.timing.settle_ms) : nextLogin(t);
      case PH_LOGIN_TX: return ok ? wait(PH_LOGIN_LISTEN, cfg_.timing.login_listen_ms) : nextLogin(t);
      case PH_POST_TEACH_TX: return ok ? wait(PH_POST_SETTLE, cfg_.timing.settle_ms) : sendRequest(PH_REQ_TX);
      case PH_REQ_TX:
        return ok ? wait(PH_REQ_LISTEN, cfg_.timing.request_listen_ms) : finish(t, XR_REQUEST_UNANSWERED);
      default: return stepNoChange();
    }
  }

  Step onTimer(TargetState& t) {
    switch (phase_) {
      case PH_FAST_SETTLE:
      case PH_FAST_GAP: return sendRequest(PH_FAST_REQ_TX);
      case PH_FAST_LISTEN: return fastMiss(t);
      case PH_PRE_SETTLE:
      case PH_LOGIN_LISTEN: return nextLogin(t);
      case PH_LOGIN_GAP: return sendLogin();
      case PH_FLOOD_CLEAR: return sendTeach(PH_POST_TEACH_TX);
      case PH_POST_SETTLE: return sendRequest(PH_REQ_TX);
      case PH_REQ_LISTEN: return finish(t, XR_REQUEST_UNANSWERED);
      default: return stepNoChange();
    }
  }

  Step onDataReply(TargetState& t, bool flooded) {
    if (!expectsData()) return stepNoChange();
    res_.got_reply = true;
    res_.reply_flooded = flooded;
    if (awaitingTx()) {
      held_data_ = true;
      return stepNoChange();
    }
    return finish(t, XR_NONE);
  }

  Step onLoginAnswer(TargetState& t, bool admin, bool flooded) {
    if (!expectsLogin()) return stepNoChange();
    if (admin) {
      if (res_.admin || (held_login_ && held_admin_)) return stepNoChange();
    } else if (res_.login_answered || held_login_) {
      return stepNoChange();
    }
    if (!admin) {
      // With an earlier login on the air the answer is most likely to that one; otherwise to the unreported one.
      answer_pw_ = login_out_ ? out_pw_ : handed_pw_;
    }
    if (awaitingTx()) {
      // An admin answer replaces a held guest answer.
      held_login_ = true;
      held_admin_ = admin;
      held_flooded_ = flooded;
      return stepNoChange();
    }
    return acceptLogin(t, admin, flooded);
  }

  Step acceptLogin(TargetState& t, bool admin, bool flooded) {
    if (!res_.login_answered) res_.login_flooded = flooded;
    res_.login_answered = true;
    t.login_checked_at = now_;
    t.key_login_fails = 0;
    if (admin) {
      res_.admin = true;
      return finish(t, XR_ADMIN);
    }
    if (answer_pw_ == PW_HELLO || cfg_.personal) {
      t.learned_pw = answer_pw_ == PW_HELLO ? LEARNED_HELLO : LEARNED_BLANK;
    }
    if (!in_acl_ || flooded) {
      // A route sent into a flood still echoing around is lost; it only sticks once the key is in the ACL.
      if (flooded) return wait(PH_FLOOD_CLEAR, cfg_.timing.flood_clear_ms);
      return sendTeach(PH_POST_TEACH_TX);
    }
    return wait(PH_POST_SETTLE, cfg_.timing.settle_ms);
  }

  Step finish(TargetState& t, uint8_t reason) {
    phase_ = PH_DONE;
    if (res_.got_reply) {
      if (t.logged_in_at == 0) t.logged_in_at = now_;
      // Only data replies count: each follows a route teach, so a flooded one means the route did not take.
      t.flood_replies = res_.reply_flooded ? inc(t.flood_replies) : 0;
    }
    if (res_.admin) {
      reason = XR_ADMIN;   // outranks a data reply that arrived alongside it
    } else if (res_.got_reply) {
      reason = XR_NONE;
    } else if (res_.logins == 0 && res_.teaches == 0 && res_.requests == 0) {
      reason = XR_NOTHING_SENT;
    }
    res_.reason = reason;
    return send(PH_DONE, ACT_FINISH);
  }

  ExchangeConfig cfg_;
  ExchangeResult res_;
  uint8_t  phase_;
  uint32_t now_;
  uint8_t  tried_;       // requests and logins so far; a login after any of them waits the retry gap
  uint8_t  fast_reqs_;
  uint8_t  pws_[2];
  uint8_t  pw_count_;
  uint8_t  pw_next_;
  uint8_t  out_pw_;      // password of the last login that left the radio
  uint8_t  handed_pw_;   // password of the last login handed to the owner
  uint8_t  answer_pw_;
  bool     login_handed_;
  bool     in_acl_;
  bool     req_asked_;
  bool     login_out_;
  bool     held_login_, held_admin_, held_flooded_;
  bool     held_data_;
};

}  // namespace poll
