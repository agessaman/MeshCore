#pragma once

// The hand-off between the mesh loop, which decides when CoreScope has to be
// asked, and the bridge task, which owns the sockets. One request is out at a
// time; the answer is a fixed-size snapshot, so neither side shares pointers or
// takes a lock. Pure: no Arduino, no JSON, no clock reads.

#include "PollDecisions.h"
#include "PollPolicy.h"

namespace poll {

static const uint8_t  kControlUrlMax = 96;     // the size of the poll.url setting
static const uint16_t kRegBodyMax = 560;       // registration JSON with a 64-byte private key
static const uint16_t kClaimJsonMax = 2752;    // worst case 2725 bytes, rounded up to a multiple of 64

// The plugin release this firmware mirrors; CoreScope gates on it.
static const char kPluginVersion[] = "0.11.1";

// Requests one session may make, in this order.
enum ControlWant : uint8_t {
  WANT_OWNERS     = 1 << 0,
  WANT_POLICY     = 1 << 1,   // skipped when the owners reply carried a policy
  WANT_REGISTER   = 1 << 2,
  WANT_SHARED_KEY = 1 << 3,   // also fetched when the registration answers 404
};

struct ControlRequest {
  char     url[kControlUrlMax];    // base URL, http or https, no trailing slash
  char     client_version[24];     // firmware release in GET queries; "" = omit
  uint8_t  mode;                   // ReadingMode known to the loop; used when this session gets no policy
  uint8_t  observer_pub[kPubKeySize];
  uint8_t  want;                   // ControlWant bits
  uint8_t  target_count;
  uint8_t  request_capacity;   // 0 accepts no analyzer targets beyond targets[]
  uint8_t  targets[kMaxTargets][kPubKeySize];   // owners entries are read for these only
  uint16_t reg_body_len;           // 0 = nothing to register
  char     reg_body[kRegBodyMax];  // built and signed on the loop side
};

// An analyzer target that is not in the configured list.
struct RequestedEntry {
  uint8_t    pub[kPubKeySize];
  OwnerEntry entry;
};

// HTTP status fields: > 0 is the status, 0 means the request was not made, and
// < 0 is a transport failure (no connection, timeout, oversize or bad body).
struct ControlResult {
  bool     started;        // false: no session (link down, or the handshake guard never came free)
  uint32_t finished_at;    // RTC seconds when the session ended

  int16_t  owners_status;
  bool     owners_ok;      // 200 with a "nodes" object
  OwnerEntry owners[kMaxTargets];   // index-aligned with ControlRequest::targets
  uint8_t    requested_count;        // 0 unless the owners answer had a nodes object
  RequestedEntry requested[kMaxTargets];

  int16_t  policy_status;  // of the reply that carried the policy, or of the policy endpoint
  bool     policy_ok;      // policy holds the server's fields, not yet clamped
  Policy   policy;         // fetch bookkeeping (ever_fetched, fetched_at, last_status) is left zero

  int16_t  reg_status;
  bool     reg_body_ok;    // the reply said {"ok": true}
  bool     keys_skipped;   // registration and shared key withheld: JSON or unknown reading mode

  int16_t  shared_status;
  bool     shared_ok;      // shared_pub and shared_prv are valid
  uint8_t  shared_pub[kPubKeySize];
  uint8_t  shared_prv[64]; // expanded form, as LocalIdentity stores it
};

// Mailbox states. The loop task moves IDLE -> REQUESTED and DONE -> IDLE; the
// bridge task moves REQUESTED -> RUNNING -> DONE.
enum ControlMailbox : uint8_t {
  CONTROL_IDLE = 0,
  CONTROL_REQUESTED,
  CONTROL_RUNNING,
  CONTROL_DONE,
};

// Outcome of the last claim publication.
enum ClaimPublish : uint8_t {
  CLAIM_NONE = 0,     // nothing asked, or the answer was already taken
  CLAIM_PENDING,
  CLAIM_PUBLISHED,
  CLAIM_FAILED,       // slot down, not allowed on that slot, or the broker did not take it
  CLAIM_WRONG_HOST,   // automatic slot's broker is no longer the one that was matched
};

// Wrong host, a refusal, or a dropped answer: unsent, and a reading stays held.
static inline bool publishAttemptFailed(uint8_t result) {
  return result != CLAIM_PENDING && result != CLAIM_PUBLISHED;
}

}  // namespace poll
