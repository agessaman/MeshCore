#include <gtest/gtest.h>
#include <stdint.h>

#include <algorithm>
#include <functional>
#include <set>
#include <vector>

#include "helpers/poll/PollAdminDrill.h"
#include "helpers/poll/PollExchange.h"

using namespace poll;

namespace {

const uint32_t kNow = 2000000;  // an even UTC day boundary is kNow - kNow % kDay

// A direct neighbor repeater as the firmware behaves towards an unknown key.
struct FakeRepeater {
  bool accepts_blank = true;
  bool accepts_hello = true;
  bool admin = false;
  bool stubborn = false;        // setperm never takes
  bool answer_requests = true;
  bool require_login = false;   // bare requests from a key outside the ACL are ignored
  bool lose_paths = false;      // route teaches never take, so replies keep flooding
  bool in_acl = false;
  bool route_known = false;     // no stored route: answers flood
  int ignore_logins = 0;        // the next N logins are not heard at all
  uint32_t login_delay_ms = 300;
  uint32_t reply_delay_ms = 500;
  std::vector<uint32_t> reply_delays;   // per request, overrides reply_delay_ms while it lasts

  int logins = 0, teaches = 0, requests = 0, setperms = 0, paths_taught = 0;
  std::vector<uint8_t> login_pws;
  std::vector<uint8_t> request_kinds;
};

struct Pending {
  uint64_t at;
  uint8_t ev;
  bool flooded;
};

// Runs a machine against the fake repeater on a fake clock.
struct Sim {
  FakeRepeater rep;
  TargetState t;
  ExchangeConfig cfg = defaultExchangeConfig();
  uint64_t clock_ms = 0;
  std::vector<Pending> air;
  std::vector<uint8_t> tx;            // every ACT_SEND_* in order, failed or not
  std::set<size_t> fail_tx;           // indices into tx reported as EV_TX_FAILED
  std::vector<uint32_t> waits;
  PollExchange x;
  PollAdminDrill drill;

  Sim() { memset(&t, 0, sizeof(t)); }

  uint32_t now() const { return kNow + (uint32_t)(clock_ms / 1000); }

  void schedule(uint32_t delay, uint8_t ev, bool flooded) {
    air.push_back(Pending{clock_ms + delay, ev, flooded});
    std::stable_sort(air.begin(), air.end(), [](const Pending& a, const Pending& b) { return a.at < b.at; });
  }

  void hear(const Step& s) {
    switch (s.act) {
      case ACT_SEND_LOGIN: {
        rep.logins++;
        rep.login_pws.push_back(s.pw);
        if (rep.ignore_logins > 0) {
          rep.ignore_logins--;
          return;
        }
        bool ok = s.pw == PW_HELLO ? rep.accepts_hello : rep.accepts_blank;
        if (!ok) return;
        rep.in_acl = true;
        schedule(rep.login_delay_ms, rep.admin ? EV_LOGIN_ADMIN : EV_LOGIN_GUEST, !rep.route_known);
        return;
      }
      case ACT_SEND_TEACH:
        rep.teaches++;
        if (rep.in_acl && !rep.lose_paths) {
          rep.route_known = true;
          rep.paths_taught++;
        }
        return;
      case ACT_SEND_REQUEST: {
        rep.requests++;
        rep.request_kinds.push_back(s.kind);
        uint32_t delay = rep.reply_delay_ms;
        if (!rep.reply_delays.empty()) {
          delay = rep.reply_delays.front();
          rep.reply_delays.erase(rep.reply_delays.begin());
        }
        if (rep.require_login && !rep.in_acl) return;
        if (!rep.answer_requests) return;
        schedule(delay, EV_DATA_REPLY, !rep.route_known);
        return;
      }
      case ACT_SEND_SETPERM:
        rep.setperms++;
        if (!rep.stubborn) rep.admin = false;
        rep.in_acl = false;  // setperm 0 deletes the entry and its route
        rep.route_known = false;
        return;
    }
  }

  void drive(Step s, const std::function<Step(uint8_t, bool)>& feed) {
    for (int guard = 0; guard < 200; guard++) {
      if (s.act == ACT_FINISH) return;
      if (s.act != ACT_WAIT) {
        size_t idx = tx.size();
        tx.push_back(s.act);
        bool fail = fail_tx.count(idx) != 0;
        if (!fail) hear(s);
        clock_ms += 50;
        s = feed(fail ? EV_TX_FAILED : EV_TX_DONE, false);
        ASSERT_FALSE(isNoChange(s)) << "a TX report must move the machine on";
        continue;
      }
      ASSERT_FALSE(isNoChange(s)) << "no-change returned where a step was due";
      waits.push_back(s.wait_ms);
      uint64_t deadline = clock_ms + s.wait_ms;
      for (;;) {
        if (!air.empty() && air.front().at <= deadline) {
          Pending p = air.front();
          air.erase(air.begin());
          clock_ms = std::max(clock_ms, p.at);
          Step r = feed(p.ev, p.flooded);
          if (isNoChange(r)) continue;
          s = r;
          break;
        }
        clock_ms = deadline;
        s = feed(EV_TIMER, false);
        break;
      }
    }
    FAIL() << "machine did not finish";
  }

  const ExchangeResult& poll() {
    tx.clear();
    waits.clear();
    air.clear();
    Step s = x.start(t, cfg, now());
    drive(s, [this](uint8_t ev, bool fl) { return x.onEvent(t, ev, fl, now()); });
    EXPECT_FALSE(x.active());
    return x.result();
  }

  bool runDrill() {
    tx.clear();
    waits.clear();
    air.clear();
    Step s = drill.start(now());
    drive(s, [this](uint8_t ev, bool fl) { return drill.onEvent(ev, fl, now()); });
    EXPECT_FALSE(drill.active());
    return drill.removed();
  }

  int count(uint8_t act) const { return (int)std::count(tx.begin(), tx.end(), act); }

  // Believed in the ACL with a fresh route and a fresh admin check.
  void steadyState() {
    t.logged_in_at = kNow - 60;
    t.login_checked_at = kNow - 60;
    t.route_sent_at = kNow - 3600;
    t.status_checked_at = kNow;
    t.code = CODE_OK;
    rep.in_acl = rep.route_known = true;
  }
};

std::vector<uint8_t> acts(std::initializer_list<uint8_t> l) { return std::vector<uint8_t>(l); }

}  // namespace

// ---- first contact ---------------------------------------------------------

TEST(PollExchange, FirstContactIsLoginFloodClearTeachRequest) {
  Sim s;
  s.t.status_checked_at = kNow;  // telemetry
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_LOGIN, ACT_SEND_TEACH, ACT_SEND_REQUEST}), s.tx);
  EXPECT_TRUE(std::find(s.waits.begin(), s.waits.end(), 15000u) != s.waits.end());
  EXPECT_TRUE(r.got_reply);
  EXPECT_FALSE(r.reply_flooded);
  EXPECT_TRUE(r.login_tried && r.login_answered && r.login_flooded);
  EXPECT_FALSE(r.admin);
  EXPECT_EQ(REQ_TELEMETRY, r.kind);
  EXPECT_EQ(1, r.logins);
  EXPECT_EQ(1, r.teaches);
  EXPECT_EQ(1, r.requests);
  EXPECT_EQ(XR_NONE, r.reason);
  EXPECT_TRUE(s.rep.route_known);
  EXPECT_EQ(0, s.t.flood_replies);   // a new key's flooded login answer is not held against it
  EXPECT_NE(0u, s.t.logged_in_at);
  EXPECT_NE(0u, s.t.login_checked_at);
  EXPECT_NE(0u, s.t.route_sent_at);
  EXPECT_EQ(LEARNED_NONE, s.t.learned_pw);   // blank under the shared key proves nothing
  EXPECT_EQ(0, s.t.key_login_fails);
}

TEST(PollExchange, FirstContactWaitsOutTheFloodBeforeTheTeach) {
  Sim s;
  s.t.status_checked_at = kNow;
  s.poll();
  // login listen ends at the answer, then 15 s flood clear, settle after the teach, request listen
  EXPECT_EQ((std::vector<uint32_t>{10000, 15000, 1000, 10000}), s.waits);
}

TEST(PollExchange, FirstContactAsksForStatusWhenDue) {
  Sim s;
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(REQ_STATUS, r.kind);
  ASSERT_EQ(1u, s.rep.request_kinds.size());
  EXPECT_EQ(REQ_STATUS, s.rep.request_kinds[0]);
  EXPECT_GE(s.t.status_checked_at, kNow);
}

TEST(PollExchange, TelemetryBetweenStatusReads) {
  Sim s;
  s.t.status_checked_at = kNow - 3600;
  EXPECT_EQ(REQ_TELEMETRY, s.poll().kind);
  EXPECT_EQ(kNow - 3600, s.t.status_checked_at);

  Sim s2;
  s2.t.status_checked_at = kNow - 7 * 3600;
  EXPECT_EQ(REQ_STATUS, s2.poll().kind);
  EXPECT_GE(s2.t.status_checked_at, kNow);

  Sim s3;  // a 1 h setting: status due after 2 h
  s3.cfg.status_hours = 1;
  s3.t.status_checked_at = kNow - 2 * 3600;
  EXPECT_EQ(REQ_STATUS, s3.poll().kind);
}

TEST(PollExchange, SilentRepeaterOnFirstContactGetsTwoLoginsAndNoRequest) {
  Sim s;
  s.rep.accepts_blank = s.rep.accepts_hello = false;
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_LOGIN, ACT_SEND_LOGIN}), s.tx);
  EXPECT_EQ((std::vector<uint8_t>{PW_BLANK, PW_HELLO}), s.rep.login_pws);
  EXPECT_EQ((std::vector<uint32_t>{10000, 8000, 10000}), s.waits);
  EXPECT_FALSE(r.got_reply);
  EXPECT_TRUE(r.login_tried);
  EXPECT_FALSE(r.login_answered);
  EXPECT_EQ(XR_LOGIN_UNANSWERED, r.reason);
  EXPECT_EQ(0u, s.t.login_checked_at);   // admin recheck stays due
  EXPECT_EQ(0u, s.t.logged_in_at);
  EXPECT_EQ(1, s.t.key_login_fails);
}

TEST(PollExchange, AdminAnswerStopsBeforeAnyRequest) {
  Sim s;
  s.rep.admin = true;
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_LOGIN}), s.tx);
  EXPECT_TRUE(r.admin);
  EXPECT_EQ(XR_ADMIN, r.reason);
  EXPECT_FALSE(r.got_reply);
  EXPECT_EQ(0, s.rep.requests);
  EXPECT_EQ(0u, s.t.logged_in_at);
}

// ---- steady state ------------------------------------------------------------

TEST(PollExchange, SteadyStateWithFreshRouteIsOneRequest) {
  Sim s;
  s.steadyState();
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_REQUEST}), s.tx);
  EXPECT_TRUE(r.got_reply);
  EXPECT_FALSE(r.login_tried);
  EXPECT_EQ(0, s.rep.logins);
  EXPECT_EQ(0, s.rep.paths_taught);
  EXPECT_EQ(kNow - 60, s.t.logged_in_at);   // kept, not refreshed
}

TEST(PollExchange, RouteRetaughtOnceADay) {
  Sim s;
  s.steadyState();
  s.t.route_sent_at = kNow - 25 * 3600;
  s.poll();
  EXPECT_EQ(acts({ACT_SEND_TEACH, ACT_SEND_REQUEST}), s.tx);
  EXPECT_EQ(1000u, s.waits[0]);   // settle after the teach
  EXPECT_GE(s.t.route_sent_at, kNow);
  EXPECT_EQ(0, s.rep.logins);
}

TEST(PollExchange, TeachBeforeRequestWhenRepliesFlooded) {
  Sim s;
  s.steadyState();
  s.t.flood_replies = 1;
  s.poll();
  EXPECT_EQ(acts({ACT_SEND_TEACH, ACT_SEND_REQUEST}), s.tx);
  EXPECT_EQ(0, s.t.flood_replies);   // a direct reply clears it
}

TEST(PollExchange, TeachBeforeRequestWhenLastPollFailed) {
  const uint8_t codes[] = {CODE_NO_REPLY, CODE_NONE, CODE_ERROR};
  for (uint8_t code : codes) {
    Sim s;
    s.steadyState();
    s.t.code = code;
    s.poll();
    EXPECT_EQ(acts({ACT_SEND_TEACH, ACT_SEND_REQUEST}), s.tx) << codeName(code);
  }
  Sim nb;
  nb.steadyState();
  nb.t.code = CODE_NO_BATTERY;
  nb.poll();
  EXPECT_EQ(acts({ACT_SEND_REQUEST}), nb.tx);
}

TEST(PollExchange, SilentRepeaterOnFastPathGetsTwoRequestsAndNoLogin) {
  for (uint8_t misses = 0; misses < 2; misses++) {
    Sim s;
    s.steadyState();
    s.rep.answer_requests = false;
    s.rep.accepts_blank = s.rep.accepts_hello = false;
    s.t.no_reply_count = misses;
    const ExchangeResult& r = s.poll();
    EXPECT_EQ(acts({ACT_SEND_REQUEST, ACT_SEND_REQUEST}), s.tx);
    EXPECT_EQ((std::vector<uint32_t>{10000, 8000, 10000}), s.waits);
    EXPECT_FALSE(r.login_tried);
    EXPECT_EQ(XR_ENTRY_KEPT, r.reason);
    EXPECT_EQ(kNow - 60, s.t.logged_in_at);   // one lost reply does not cost the entry
    EXPECT_EQ(0, s.t.key_login_fails);
  }
}

TEST(PollExchange, EveryThirdMissLogsIn) {
  Sim s;
  s.steadyState();
  s.rep.answer_requests = false;
  s.rep.accepts_blank = s.rep.accepts_hello = false;
  s.t.no_reply_count = kForgetLoginAfter - 1;
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_REQUEST, ACT_SEND_REQUEST, ACT_SEND_LOGIN, ACT_SEND_LOGIN}), s.tx);
  // the first login waits the retry gap after the requests
  EXPECT_EQ((std::vector<uint32_t>{10000, 8000, 10000, 8000, 10000, 8000, 10000}), s.waits);
  EXPECT_TRUE(r.login_tried);
  EXPECT_EQ(XR_LOGIN_UNANSWERED, r.reason);
  EXPECT_EQ(1, s.t.key_login_fails);

  Sim s5;  // miss 6 is a 3rd miss as well
  s5.steadyState();
  s5.rep.answer_requests = false;
  s5.t.no_reply_count = 5;
  s5.rep.accepts_blank = s5.rep.accepts_hello = false;
  s5.poll();
  EXPECT_EQ(2, s5.count(ACT_SEND_LOGIN));
}

TEST(PollExchange, LapsedAclEntryRecoversOnTheThirdMissWithATeach) {
  Sim s;
  s.steadyState();
  s.rep.in_acl = s.rep.route_known = false;   // rebooted: guests are not saved
  s.rep.require_login = true;
  s.t.no_reply_count = kForgetLoginAfter - 1;
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_REQUEST, ACT_SEND_REQUEST, ACT_SEND_LOGIN, ACT_SEND_TEACH, ACT_SEND_REQUEST}), s.tx);
  EXPECT_TRUE(r.got_reply);
  EXPECT_TRUE(r.login_flooded);
  EXPECT_FALSE(r.reply_flooded);
  EXPECT_EQ(1, s.rep.paths_taught);
  EXPECT_EQ(0, s.t.flood_replies);
}

// ---- admin recheck -------------------------------------------------------------

TEST(PollExchange, ForcedLoginAfterADayTeachesFirst) {
  Sim s;
  s.steadyState();
  s.t.login_checked_at = kNow - 25 * 3600;
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_TEACH, ACT_SEND_LOGIN, ACT_SEND_REQUEST}), s.tx);
  EXPECT_FALSE(r.login_flooded);   // the route went first, so the answer came back direct
  EXPECT_TRUE(r.got_reply);
  EXPECT_GE(s.t.login_checked_at, kNow);
  // teach settle, login listen (cut by the answer), settle, request listen
  EXPECT_EQ((std::vector<uint32_t>{1000, 10000, 1000, 10000}), s.waits);
}

TEST(PollExchange, ForcedLoginCatchesAQuietAdminGrant) {
  Sim s;
  s.steadyState();
  s.rep.admin = true;
  s.t.login_checked_at = kNow - 25 * 3600;
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_TEACH, ACT_SEND_LOGIN}), s.tx);
  EXPECT_TRUE(r.admin);
  EXPECT_EQ(0, s.rep.requests);
}

TEST(PollExchange, NeverCheckedMeansRecheckDue) {
  Sim s;
  s.steadyState();
  s.t.login_checked_at = 0;
  s.poll();
  EXPECT_EQ(1, s.count(ACT_SEND_LOGIN));
}

TEST(PollExchange, FloodedAnswerOnRecheckWaitsAndTeachesAgain) {
  Sim s;
  s.steadyState();
  s.rep.lose_paths = true;
  s.rep.route_known = false;
  s.t.login_checked_at = kNow - 25 * 3600;
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_TEACH, ACT_SEND_LOGIN, ACT_SEND_TEACH, ACT_SEND_REQUEST}), s.tx);
  EXPECT_TRUE(r.login_flooded);
  EXPECT_TRUE(r.reply_flooded);
  EXPECT_EQ(1, s.t.flood_replies);
}

// ---- passwords -----------------------------------------------------------------

TEST(PollExchange, BlankSuccessRememberedOnlyUnderAPersonalKey) {
  Sim shared;
  shared.poll();
  EXPECT_EQ(LEARNED_NONE, shared.t.learned_pw);

  Sim own;
  own.cfg.personal = true;
  own.poll();
  EXPECT_EQ(LEARNED_BLANK, own.t.learned_pw);
}

TEST(PollExchange, HelloTriedAfterBlankUnansweredAndRemembered) {
  Sim s;
  s.rep.accepts_blank = false;
  s.rep.require_login = true;
  const ExchangeResult& r = s.poll();
  EXPECT_EQ((std::vector<uint8_t>{PW_BLANK, PW_HELLO}), s.rep.login_pws);
  EXPECT_TRUE(r.got_reply);
  EXPECT_EQ(LEARNED_HELLO, s.t.learned_pw);
  EXPECT_EQ(0, s.t.key_login_fails);
}

TEST(PollExchange, KnownHelloIsSentAlone) {
  Sim s;
  s.t.learned_pw = LEARNED_HELLO;
  s.poll();
  EXPECT_EQ((std::vector<uint8_t>{PW_HELLO}), s.rep.login_pws);
}

TEST(PollExchange, OperatorOverrideSendsOnlyThatPassword) {
  Sim s;
  s.cfg.pw_mode = PWMODE_HELLO;
  s.cfg.personal = true;
  s.t.learned_pw = LEARNED_BLANK;
  s.poll();
  EXPECT_EQ((std::vector<uint8_t>{PW_HELLO}), s.rep.login_pws);

  Sim b;
  b.cfg.pw_mode = PWMODE_BLANK;
  b.t.learned_pw = LEARNED_HELLO;
  b.rep.accepts_blank = b.rep.accepts_hello = false;
  b.poll();
  EXPECT_EQ((std::vector<uint8_t>{PW_BLANK}), b.rep.login_pws);
}

TEST(PollExchange, PasswordOrderTable) {
  uint8_t pw[2];
  TargetState t;
  memset(&t, 0, sizeof(t));
  t.learned_pw = LEARNED_BLANK;
  ASSERT_EQ(1, passwordOrder(t, 0, PWMODE_AUTO, true, pw));
  EXPECT_EQ(PW_BLANK, pw[0]);
  ASSERT_EQ(2, passwordOrder(t, 0, PWMODE_AUTO, false, pw));   // the shared key never trusts blank
  EXPECT_EQ(PW_BLANK, pw[0]);
  EXPECT_EQ(PW_HELLO, pw[1]);

  memset(&t, 0, sizeof(t));
  t.key_login_fails = 2;
  ASSERT_EQ(1, passwordOrder(t, 0, PWMODE_AUTO, false, pw));
  EXPECT_EQ(PW_BLANK, pw[0]);
  ASSERT_EQ(1, passwordOrder(t, 86400, PWMODE_AUTO, false, pw));
  EXPECT_EQ(PW_HELLO, pw[0]);
  ASSERT_EQ(1, passwordOrder(t, 2 * 86400 + 5, PWMODE_AUTO, false, pw));
  EXPECT_EQ(PW_BLANK, pw[0]);

  memset(&t, 0, sizeof(t));
  t.learned_pw = LEARNED_HELLO;
  t.key_login_fails = 9;
  ASSERT_EQ(1, passwordOrder(t, 86400, PWMODE_AUTO, false, pw));
  EXPECT_EQ(PW_HELLO, pw[0]);

  t.learned_pw = LEARNED_NONE;
  t.key_login_fails = 1;
  EXPECT_EQ(2, passwordOrder(t, 86400, PWMODE_AUTO, false, pw));
}

TEST(PollExchange, FailingKeyAlternatesOneLoginPerPollByDay) {
  const uint32_t day_start = kNow - kNow % kDay;
  uint8_t day = (uint8_t)((kNow / kDay) % 2);
  Sim s;
  s.t.key_login_fails = 2;
  s.rep.accepts_blank = s.rep.accepts_hello = false;
  s.poll();
  ASSERT_EQ(1u, s.rep.login_pws.size());
  EXPECT_EQ(day == 0 ? PW_BLANK : PW_HELLO, s.rep.login_pws[0]);
  EXPECT_EQ(3, s.t.key_login_fails);
  EXPECT_EQ(1u, s.waits.size());   // one login listen, no gap

  // The next day the other password goes.
  Sim n;
  n.t.key_login_fails = 2;
  n.clock_ms = (uint64_t)(day_start + kDay - kNow) * 1000;
  n.poll();
  ASSERT_EQ(1u, n.rep.login_pws.size());
  EXPECT_EQ(day == 0 ? PW_HELLO : PW_BLANK, n.rep.login_pws[0]);
  EXPECT_EQ(0, n.t.key_login_fails);   // answered
}

// ---- flood accounting ----------------------------------------------------------

TEST(PollExchange, FloodedDataRepliesCountFloodedLoginAnswersDoNot) {
  Sim s;
  s.rep.lose_paths = true;   // the route never takes
  s.t.status_checked_at = kNow;
  const ExchangeResult& r = s.poll();
  EXPECT_TRUE(r.login_flooded);
  EXPECT_TRUE(r.reply_flooded);
  EXPECT_EQ(1, s.t.flood_replies);

  // Second poll: in the ACL, flood_replies > 0 so the route goes first; still floods.
  s.t.code = CODE_OK;
  s.poll();
  EXPECT_EQ(acts({ACT_SEND_TEACH, ACT_SEND_REQUEST}), s.tx);
  EXPECT_EQ(2, s.t.flood_replies);

  // Route takes again: a direct reply clears the count.
  s.rep.lose_paths = false;
  s.poll();
  EXPECT_EQ(0, s.t.flood_replies);
}

TEST(PollExchange, NoDataReplyLeavesFloodRepliesAlone) {
  Sim s;
  s.steadyState();
  s.t.flood_replies = 1;
  s.rep.answer_requests = false;
  s.poll();
  EXPECT_EQ(1, s.t.flood_replies);
}

// ---- late answers --------------------------------------------------------------

TEST(PollExchange, LateReplyDuringTheRetryGapCounts) {
  Sim s;
  s.steadyState();
  s.rep.reply_delays = {12000};   // after the 10 s listen, inside the 8 s gap
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_REQUEST}), s.tx);
  EXPECT_TRUE(r.got_reply);
  EXPECT_EQ(1, r.requests);
}

TEST(PollExchange, LateReplyAfterTheRepeatRequestCounts) {
  Sim s;
  s.steadyState();
  s.rep.reply_delays = {19000, 60000};   // first request's reply lands in the repeat's listen
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_REQUEST, ACT_SEND_REQUEST}), s.tx);
  EXPECT_TRUE(r.got_reply);
}

TEST(PollExchange, LateReplyDuringTheLoginsOfAThirdMissCounts) {
  Sim s;
  s.steadyState();
  s.rep.accepts_blank = s.rep.accepts_hello = false;
  s.rep.reply_delays = {40000, 60000};   // the first reply lands during the blank login's listen
  s.t.no_reply_count = 2;
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_REQUEST, ACT_SEND_REQUEST, ACT_SEND_LOGIN}), s.tx);
  EXPECT_TRUE(r.got_reply);
  EXPECT_TRUE(r.login_tried);
  EXPECT_EQ(0, s.t.key_login_fails);   // cut short, not a failed login
}

TEST(PollExchange, LateLoginAnswerInTheGapStopsMoreLogins) {
  Sim s;
  s.t.status_checked_at = kNow;
  s.rep.login_delay_ms = 12000;   // after the 10 s listen, inside the gap before hello
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_LOGIN, ACT_SEND_TEACH, ACT_SEND_REQUEST}), s.tx);
  EXPECT_TRUE(r.login_answered);
  EXPECT_TRUE(r.got_reply);
}

// ---- TX failures ---------------------------------------------------------------

TEST(PollExchange, FailedFastRequestsSkipTheListen) {
  Sim s;
  s.steadyState();
  s.fail_tx = {0, 1};
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_REQUEST, ACT_SEND_REQUEST}), s.tx);
  EXPECT_EQ((std::vector<uint32_t>{8000}), s.waits);   // the gap only
  EXPECT_EQ(0, r.requests);
  EXPECT_EQ(XR_NOTHING_SENT, r.reason);
}

TEST(PollExchange, FailedFirstRequestStillRepeats) {
  Sim s;
  s.steadyState();
  s.fail_tx = {0};
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_REQUEST, ACT_SEND_REQUEST}), s.tx);
  EXPECT_TRUE(r.got_reply);
  EXPECT_EQ(1, r.requests);
}

TEST(PollExchange, FailedLoginMovesToTheNextPassword) {
  Sim s;
  s.t.status_checked_at = kNow;
  s.fail_tx = {0};
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_LOGIN, ACT_SEND_LOGIN, ACT_SEND_TEACH, ACT_SEND_REQUEST}), s.tx);
  EXPECT_EQ(PW_HELLO, s.rep.login_pws[0]);   // only hello reached the repeater
  EXPECT_EQ(8000u, s.waits[0]);
  EXPECT_EQ(1, r.logins);
  EXPECT_EQ(LEARNED_HELLO, s.t.learned_pw);
}

TEST(PollExchange, AllLoginsFailedIsNotALoginFailure) {
  Sim s;
  s.fail_tx = {0, 1};
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(XR_NOTHING_SENT, r.reason);
  EXPECT_EQ(0, s.t.key_login_fails);
}

TEST(PollExchange, FailedTeachGoesStraightToTheRequest) {
  Sim s;
  s.t.status_checked_at = kNow;
  s.fail_tx = {1};
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(acts({ACT_SEND_LOGIN, ACT_SEND_TEACH, ACT_SEND_REQUEST}), s.tx);
  EXPECT_EQ((std::vector<uint32_t>{10000, 15000, 10000}), s.waits);   // no settle
  EXPECT_EQ(0, r.teaches);
  EXPECT_EQ(0u, s.t.route_sent_at);
  EXPECT_TRUE(r.reply_flooded);   // the repeater never learned the route
}

TEST(PollExchange, FailedRequestAfterLoginEndsThePoll) {
  Sim s;
  s.fail_tx = {2};
  const ExchangeResult& r = s.poll();
  EXPECT_EQ(XR_REQUEST_UNANSWERED, r.reason);
  EXPECT_EQ(0, r.requests);
  EXPECT_EQ(0u, s.t.status_checked_at);   // no status request left the radio
}

// ---- event handling ------------------------------------------------------------

TEST(PollExchange, UnexpectedEventsAreIgnored) {
  TargetState t;
  memset(&t, 0, sizeof(t));
  t.logged_in_at = t.login_checked_at = kNow - 60;
  t.route_sent_at = kNow - 60;
  t.code = CODE_OK;
  PollExchange x;
  ExchangeConfig cfg = defaultExchangeConfig();
  Step s = x.start(t, cfg, kNow);
  ASSERT_EQ(ACT_SEND_REQUEST, s.act);
  EXPECT_TRUE(x.expectsData());
  EXPECT_FALSE(x.expectsLogin());
  EXPECT_TRUE(isNoChange(x.onEvent(t, EV_TIMER, false, kNow)));          // TX report pending
  EXPECT_TRUE(isNoChange(x.onEvent(t, EV_LOGIN_ADMIN, false, kNow)));    // no login sent
  s = x.onEvent(t, EV_TX_DONE, false, kNow);
  ASSERT_EQ(ACT_WAIT, s.act);
  EXPECT_EQ(10000u, s.wait_ms);
  EXPECT_TRUE(isNoChange(x.onEvent(t, EV_TX_DONE, false, kNow)));        // nothing in flight
  EXPECT_TRUE(isNoChange(x.onEvent(t, EV_LOGIN_GUEST, false, kNow)));
  s = x.onEvent(t, EV_DATA_REPLY, false, kNow + 1);
  EXPECT_EQ(ACT_FINISH, s.act);
  EXPECT_FALSE(x.active());
  EXPECT_TRUE(isNoChange(x.onEvent(t, EV_DATA_REPLY, false, kNow + 2)));
  EXPECT_TRUE(isNoChange(x.onEvent(t, EV_TIMER, false, kNow + 2)));
}

TEST(PollExchange, DataReplyBeforeAnyRequestIsIgnored) {
  TargetState t;
  memset(&t, 0, sizeof(t));
  PollExchange x;
  Step s = x.start(t, defaultExchangeConfig(), kNow);
  ASSERT_EQ(ACT_SEND_LOGIN, s.act);
  EXPECT_FALSE(x.expectsData());
  EXPECT_FALSE(x.expectsLogin());   // the login has not left yet
  EXPECT_TRUE(isNoChange(x.onEvent(t, EV_DATA_REPLY, false, kNow)));
  x.onEvent(t, EV_TX_DONE, false, kNow);
  EXPECT_TRUE(x.expectsLogin());
}

TEST(PollExchange, ReplyHeldWhileTheRepeatRequestIsQueued) {
  TargetState t;
  memset(&t, 0, sizeof(t));
  t.logged_in_at = t.login_checked_at = t.route_sent_at = kNow - 60;
  t.code = CODE_OK;
  PollExchange x;
  Step s = x.start(t, defaultExchangeConfig(), kNow);
  ASSERT_EQ(ACT_SEND_REQUEST, s.act);
  s = x.onEvent(t, EV_TX_DONE, false, kNow);
  s = x.onEvent(t, EV_TIMER, false, kNow + 10);
  ASSERT_EQ(8000u, s.wait_ms);
  s = x.onEvent(t, EV_TIMER, false, kNow + 18);
  ASSERT_EQ(ACT_SEND_REQUEST, s.act);
  EXPECT_TRUE(isNoChange(x.onEvent(t, EV_DATA_REPLY, true, kNow + 19)));
  EXPECT_TRUE(x.active());
  s = x.onEvent(t, EV_TX_DONE, false, kNow + 19);
  EXPECT_EQ(ACT_FINISH, s.act);
  EXPECT_TRUE(x.result().got_reply);
  EXPECT_TRUE(x.result().reply_flooded);
  EXPECT_EQ(2, x.result().requests);
  EXPECT_EQ(1, t.flood_replies);
}

TEST(PollExchange, LoginAnswerHeldWhileTheNextLoginIsQueuedBelongsToTheEarlierPassword) {
  TargetState t;
  memset(&t, 0, sizeof(t));
  PollExchange x;
  ExchangeConfig cfg = defaultExchangeConfig();
  cfg.personal = true;
  Step s = x.start(t, cfg, kNow);
  ASSERT_EQ(ACT_SEND_LOGIN, s.act);
  ASSERT_EQ(PW_BLANK, s.pw);
  s = x.onEvent(t, EV_TX_DONE, false, kNow);
  s = x.onEvent(t, EV_TIMER, false, kNow + 10);   // listen over
  ASSERT_EQ(8000u, s.wait_ms);
  s = x.onEvent(t, EV_TIMER, false, kNow + 18);
  ASSERT_EQ(ACT_SEND_LOGIN, s.act);
  ASSERT_EQ(PW_HELLO, s.pw);
  EXPECT_TRUE(x.expectsLogin());
  EXPECT_TRUE(isNoChange(x.onEvent(t, EV_LOGIN_GUEST, true, kNow + 18)));
  EXPECT_FALSE(x.expectsLogin());
  s = x.onEvent(t, EV_TX_DONE, false, kNow + 19);
  ASSERT_EQ(ACT_WAIT, s.act);
  EXPECT_EQ(15000u, s.wait_ms);   // flood clear
  EXPECT_EQ(LEARNED_BLANK, t.learned_pw);
  EXPECT_EQ(2, x.result().logins);
  // A second answer (to hello) is not expected any more.
  EXPECT_TRUE(isNoChange(x.onEvent(t, EV_LOGIN_GUEST, false, kNow + 20)));
}

TEST(PollExchange, HeldAdminAnswerOutranksAHeldDataReply) {
  TargetState t;
  memset(&t, 0, sizeof(t));
  t.logged_in_at = t.login_checked_at = t.route_sent_at = kNow - 60;
  t.code = CODE_OK;
  t.no_reply_count = 2;
  PollExchange x;
  Step s = x.start(t, defaultExchangeConfig(), kNow);
  s = x.onEvent(t, EV_TX_DONE, false, kNow);       // request 1
  s = x.onEvent(t, EV_TIMER, false, kNow);         // listen over
  s = x.onEvent(t, EV_TIMER, false, kNow);         // gap over
  s = x.onEvent(t, EV_TX_DONE, false, kNow);       // request 2
  s = x.onEvent(t, EV_TIMER, false, kNow);         // listen over: 3rd miss
  ASSERT_EQ(8000u, s.wait_ms);
  s = x.onEvent(t, EV_TIMER, false, kNow);
  ASSERT_EQ(ACT_SEND_LOGIN, s.act);
  s = x.onEvent(t, EV_TX_DONE, false, kNow);
  s = x.onEvent(t, EV_TIMER, false, kNow);         // blank unanswered
  s = x.onEvent(t, EV_TIMER, false, kNow);         // gap over
  ASSERT_EQ(ACT_SEND_LOGIN, s.act);
  EXPECT_TRUE(isNoChange(x.onEvent(t, EV_LOGIN_ADMIN, false, kNow)));
  EXPECT_TRUE(isNoChange(x.onEvent(t, EV_DATA_REPLY, false, kNow)));
  s = x.onEvent(t, EV_TX_DONE, false, kNow);
  EXPECT_EQ(ACT_FINISH, s.act);
  EXPECT_TRUE(x.result().admin);
  EXPECT_EQ(XR_ADMIN, x.result().reason);
}

TEST(PollExchange, ZeroTimingNeverLooksLikeNoChange) {
  TargetState t;
  memset(&t, 0, sizeof(t));
  ExchangeConfig cfg = defaultExchangeConfig();
  memset(&cfg.timing, 0, sizeof(cfg.timing));
  PollExchange x;
  x.start(t, cfg, kNow);
  Step s = x.onEvent(t, EV_TX_DONE, false, kNow);
  EXPECT_EQ(ACT_WAIT, s.act);
  EXPECT_FALSE(isNoChange(s));
}

TEST(PollExchange, ReasonNames) {
  for (uint8_t r = XR_NONE; r <= XR_NOTHING_SENT; r++) EXPECT_STRNE("unknown", exchangeReasonName(r));
}

// ---- admin drill ---------------------------------------------------------------

TEST(PollAdminDrill, SetpermThenGuestLoginConfirmsRemoval) {
  Sim s;
  s.rep.admin = true;
  s.rep.in_acl = true;
  EXPECT_TRUE(s.runDrill());
  EXPECT_EQ(acts({ACT_SEND_SETPERM, ACT_SEND_LOGIN, ACT_SEND_TEACH}), s.tx);
  EXPECT_EQ((std::vector<uint32_t>{10000, 20000}), s.waits);
  EXPECT_EQ((std::vector<uint8_t>{PW_BLANK}), s.rep.login_pws);
  EXPECT_EQ(1, s.rep.setperms);
  EXPECT_EQ(0, s.drill.rounds());   // finished inside the first round
}

TEST(PollAdminDrill, GivesUpAfterThreeSilentRounds) {
  Sim s;
  s.rep.accepts_blank = s.rep.accepts_hello = false;
  EXPECT_FALSE(s.runDrill());
  EXPECT_EQ(acts({ACT_SEND_SETPERM, ACT_SEND_LOGIN, ACT_SEND_TEACH, ACT_SEND_LOGIN, ACT_SEND_TEACH,
                  ACT_SEND_LOGIN, ACT_SEND_TEACH}),
            s.tx);
  EXPECT_EQ((std::vector<uint32_t>{10000, 20000, 8000, 20000, 8000, 20000}), s.waits);
  EXPECT_EQ(3, s.drill.rounds());
}

TEST(PollAdminDrill, StillAdminAnswersAreNotARemoval) {
  Sim s;
  s.rep.admin = true;
  s.rep.stubborn = true;
  EXPECT_FALSE(s.runDrill());
  EXPECT_EQ(3, s.drill.adminAnswers());
  EXPECT_EQ(3, s.rep.logins);
}

TEST(PollAdminDrill, SecondRoundSucceeds) {
  Sim s;
  s.rep.admin = true;
  s.rep.ignore_logins = 1;
  EXPECT_TRUE(s.runDrill());
  EXPECT_EQ(2, s.rep.logins);
  EXPECT_EQ((std::vector<uint32_t>{10000, 20000, 8000, 20000}), s.waits);
}

TEST(PollAdminDrill, FailedSetpermEndsTheDrill) {
  Sim s;
  s.fail_tx = {0};
  EXPECT_FALSE(s.runDrill());
  EXPECT_EQ(acts({ACT_SEND_SETPERM}), s.tx);
  EXPECT_FALSE(s.drill.setpermSent());
}

TEST(PollAdminDrill, FailedLoginSkipsThatRoundsListen) {
  Sim s;
  s.rep.admin = true;
  s.fail_tx = {1};
  EXPECT_TRUE(s.runDrill());
  EXPECT_EQ((std::vector<uint32_t>{10000, 8000, 20000}), s.waits);
}

TEST(PollAdminDrill, GuestAnswerDuringTheTeachIsHeld) {
  PollAdminDrill d;
  Step s = d.start(kNow);
  ASSERT_EQ(ACT_SEND_SETPERM, s.act);
  EXPECT_FALSE(d.expectsLogin());
  s = d.onEvent(EV_TX_DONE, false, kNow);
  ASSERT_EQ(10000u, s.wait_ms);
  EXPECT_TRUE(isNoChange(d.onEvent(EV_LOGIN_GUEST, false, kNow)));   // no verifying login yet
  s = d.onEvent(EV_TIMER, false, kNow + 10);
  ASSERT_EQ(ACT_SEND_LOGIN, s.act);
  EXPECT_EQ(PW_BLANK, s.pw);
  s = d.onEvent(EV_TX_DONE, false, kNow + 10);
  ASSERT_EQ(ACT_SEND_TEACH, s.act);
  EXPECT_TRUE(isNoChange(d.onEvent(EV_LOGIN_GUEST, true, kNow + 10)));
  EXPECT_TRUE(d.active());
  s = d.onEvent(EV_TX_DONE, false, kNow + 11);
  EXPECT_EQ(ACT_FINISH, s.act);
  EXPECT_TRUE(d.removed());
  EXPECT_TRUE(isNoChange(d.onEvent(EV_LOGIN_GUEST, false, kNow + 12)));
}

TEST(PollAdminDrill, LateGuestAnswerInTheGapCounts) {
  Sim s;
  s.rep.admin = true;
  s.rep.login_delay_ms = 24000;   // after the 20 s listen, inside the 8 s gap
  EXPECT_TRUE(s.runDrill());
  EXPECT_EQ(1, s.rep.logins);
}

TEST(PollAdminDrill, DataRepliesAreIgnored) {
  PollAdminDrill d;
  d.start(kNow);
  d.onEvent(EV_TX_DONE, false, kNow);
  EXPECT_TRUE(isNoChange(d.onEvent(EV_DATA_REPLY, false, kNow)));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
