#pragma once

// Shared vocabulary for the repeater poller (guest-key polling of neighbor
// repeaters). Everything under src/helpers/poll/Poll*.h is pure: no Arduino, no
// mesh types, no heap, no clock reads. Times are passed in.
//
// Units: *_at fields and "now" are RTC epoch seconds; *_ms fields are durations
// in milliseconds; *_s fields are durations in seconds.

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace poll {

static const uint8_t  kPubKeySize = 32;
static const uint8_t  kMaxTargets = 8;
static const uint8_t  kRecentTags = 16;   // own request tags never read as a login answer
static const uint8_t  kSendHistory = 4;   // send times kept for the per-window request cap
static const uint32_t kDay = 24UL * 3600UL;

// MeshCore request types and reply codes the poller uses.
static const uint8_t kReqStatus = 0x01;
static const uint8_t kReqTelemetry = 0x03;
static const uint8_t kRespServerLoginOk = 0x00;
static const uint8_t kTxtTypeCliData = 0x01;

// Outcome of one poll, kept per target as the last code.
enum Code : uint8_t {
  CODE_NONE = 0,     // never polled
  CODE_OK,           // reading with a battery voltage
  CODE_NO_BATTERY,   // status answered with 0 mV
  CODE_NO_REPLY,     // nothing usable came back
  CODE_NOT_DIRECT,   // no direct route: nothing was sent
  CODE_ERROR,        // local failure (no packet, no key)
  CODE_ADMIN_STOP,   // the polling key was answered as admin
};

static inline const char* codeName(uint8_t code) {
  switch (code) {
    case CODE_OK: return "ok";
    case CODE_NO_BATTERY: return "no_battery";
    case CODE_NO_REPLY: return "no_reply";
    case CODE_NOT_DIRECT: return "not_direct";
    case CODE_ERROR: return "error";
    case CODE_ADMIN_STOP: return "admin_stop";
    default: return "none";
  }
}

enum ReqKind : uint8_t { REQ_TELEMETRY = 0, REQ_STATUS = 1 };

// The only two passwords the polling key may ever present.
enum Password : uint8_t { PW_BLANK = 0, PW_HELLO = 1 };

static inline const char* passwordText(uint8_t pw) { return pw == PW_HELLO ? "hello" : ""; }

// Operator override per target.
enum PasswordMode : uint8_t { PWMODE_AUTO = 0, PWMODE_BLANK = 1, PWMODE_HELLO = 2 };

// What a target last accepted.
enum LearnedPassword : uint8_t { LEARNED_NONE = 0, LEARNED_BLANK = 1, LEARNED_HELLO = 2 };

// Who polls a target, as CoreScope assigns it.
enum Role : uint8_t {
  ROLE_POLLER = 0,   // this observer polls (also: no CoreScope, community off, no entry)
  ROLE_STANDBY,      // another observer polls; step in when its readings stop
  ROLE_EXCLUDED,
  ROLE_PAUSED,
  ROLE_DISABLED,
};

static inline const char* roleName(uint8_t role) {
  switch (role) {
    case ROLE_STANDBY: return "standby";
    case ROLE_EXCLUDED: return "excluded";
    case ROLE_PAUSED: return "paused";
    case ROLE_DISABLED: return "disabled";
    default: return "poller";
  }
}

// Per-target state. RAM only, except admin_seen and learned_pw (see
// PollStateStore). Zero-initialized means "never polled".
struct TargetState {
  // Last outcome
  uint8_t  code;               // Code
  uint8_t  no_reply_count;     // misses in a row
  uint8_t  flood_replies;      // polls in a row whose data reply came back flooded
  uint8_t  key_login_fails;    // polls in a row whose login went unanswered
  uint8_t  learned_pw;         // LearnedPassword
  uint8_t  role;               // Role
  uint8_t  standby_rank;       // >= 1 when role is ROLE_STANDBY
  bool     admin_seen;         // permanent stop until the operator re-enables
  bool     admin_removed;      // the drill saw a non-admin login afterwards
  bool     retried;            // the last attempt was a quick retry

  uint32_t last_attempt;       // a poll started (packets may have gone out)
  uint32_t last_success;       // CODE_OK or CODE_NO_BATTERY
  uint32_t unheard_since;      // first miss of the current silent run, 0 if heard
  uint32_t logged_in_at;       // believed to be in the target's ACL since; 0 = not
  uint32_t login_checked_at;   // last guest (non-admin) login answer
  uint32_t route_sent_at;      // last route teach
  uint32_t status_checked_at;  // last status reply
  uint32_t next_due_at;        // do not poll before this (cap, fresh, standby); 0 = none
  uint32_t standby_since;      // when this observer became standby; 0 = not standby

  // Last reading (local display only; CoreScope decodes its own copy)
  uint16_t battery_mv;
  int16_t  temperature_c10;    // tenths of a degree C; INT16_MIN = none
  uint32_t uptime_secs;        // from the last status reply; 0 = none
  int16_t  noise_floor;        // from the last status reply

  uint32_t sends[kSendHistory];        // poll start times, newest last, 0 = empty
  uint32_t recent_tags[kRecentTags];   // ring of own request and login tags
  uint8_t  recent_tag_next;
};

static inline void rememberTag(TargetState& t, uint32_t tag) {
  t.recent_tags[t.recent_tag_next % kRecentTags] = tag;
  t.recent_tag_next = (uint8_t)((t.recent_tag_next + 1) % kRecentTags);
}

static inline bool isRecentTag(const TargetState& t, uint32_t tag) {
  if (tag == 0) return false;
  for (uint8_t i = 0; i < kRecentTags; i++) {
    if (t.recent_tags[i] == tag) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Step-function vocabulary shared by PollExchange and PollAdminDrill.
//
// The owner (RepeaterPoller) runs a loop like this:
//   Step s = machine.start(...);
//   for (;;) {
//     switch (s.act) {
//       case ACT_SEND_*: queue the packet; feed EV_TX_DONE or EV_TX_FAILED when
//                        the radio reports it;
//       case ACT_WAIT:   feed EV_TIMER after s.wait_ms, or a reply event if one
//                        arrives first;
//       case ACT_FINISH: read machine.result() and stop.
//     }
//   }
// A machine never asks for a send while a previous send is unreported.
// ---------------------------------------------------------------------------

enum Act : uint8_t {
  ACT_WAIT = 0,       // nothing to send; wait_ms until EV_TIMER
  ACT_SEND_LOGIN,     // anonymous login with Step::pw
  ACT_SEND_TEACH,     // direct PATH packet that teaches the target the route back
  ACT_SEND_REQUEST,   // Step::kind request
  ACT_SEND_SETPERM,   // CLI text "setperm <polling key> 0" (admin drill only)
  ACT_FINISH,
};

struct Step {
  uint8_t  act;       // Act
  uint8_t  pw;        // Password, for ACT_SEND_LOGIN
  uint8_t  kind;      // ReqKind, for ACT_SEND_REQUEST
  uint32_t wait_ms;   // for ACT_WAIT
};

enum Event : uint8_t {
  EV_TX_DONE = 0,     // the packet asked for by the last ACT_SEND_* left the radio
  EV_TX_FAILED,       // it could not be queued or the radio gave up on it
  EV_TIMER,           // wait_ms elapsed
  EV_LOGIN_GUEST,     // a login answer that is not admin
  EV_LOGIN_ADMIN,     // a login answer with the admin flag
  EV_DATA_REPLY,      // a reply whose tag matches a request sent in this poll
};

// Gaps and listen windows. The owner fills request_listen_ms from airtime.
struct Timing {
  uint32_t login_listen_ms;     // after a login leaves the radio
  uint32_t request_listen_ms;   // after a request leaves the radio
  uint32_t retry_gap_ms;        // before repeating a request or trying the next password
  uint32_t settle_ms;           // after a route teach, and between a login answer and the request
  uint32_t flood_clear_ms;      // after a flooded login answer, before the route teach
};

static inline Timing defaultTiming() {
  Timing t;
  t.login_listen_ms = 10000;
  t.request_listen_ms = 10000;
  t.retry_gap_ms = 8000;
  t.settle_ms = 1000;
  t.flood_clear_ms = 15000;
  return t;
}

}  // namespace poll
