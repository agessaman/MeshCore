#pragma once

#include "PollRequests.h"
#include "PollPwEnvelope.h"
#include "PollClaims.h"
#include "PollPacketCodec.h"

namespace poll {

// Reserve local seats before asking the analyzer again.
static inline void reserveLocalRequests(RequestTable& table, const uint8_t (*local)[kPubKeySize],
                                         uint8_t local_count, uint8_t* roles, RequestDelta& delta) {
  memset(&delta, 0, sizeof(delta));
  for (uint8_t r = 0; r < table.count;) {
    if (!keyListed(table.targets[r].pub, local, local_count)) { r++; continue; }
    removeRequested(table, r, delta);
    for (uint8_t j = r; j < table.count; j++) roles[j] = roles[j + 1];
  }
  while (table.count > requestFreeSlots(local_count, 0)) {
    const uint8_t r = (uint8_t)requestToEvict(table, roles);
    removeRequested(table, r, delta);
    for (uint8_t j = r; j < table.count; j++) roles[j] = roles[j + 1];
  }
}

static inline bool requestedLoginMustReset(const PwEnvelope& before, const PwEnvelope& after) {
  return before.present != after.present || before.ver != after.ver;
}

static inline bool requestedRunAllowed(const RequestedTarget& target, const OwnerBrief& owner) {
  const uint8_t role = requestedRole(owner);
  return requestMayPoll(target) && role != ROLE_DISABLED && role != ROLE_PAUSED && role != ROLE_EXCLUDED;
}

static inline uint8_t requestedClaimKey(const RequestedTarget& target) {
  return target.pw_ready && target.pw[0] ? CLAIM_KEY_PASSWORD : CLAIM_KEY_OWN;
}

static inline uint8_t requestedClaimState(const TargetState& state, const RequestedTarget& target) {
  if (state.admin_seen) return CLAIM_STATE_ADMIN_STOP;
  if (target.login_failed) return CLAIM_STATE_LOGIN_FAILED;
  return claimStateFor(state, target.envelope.present);
}

// Envelopes carry bytes; the printable operator-password restriction does not apply.
static inline size_t buildEnvelopeLoginPlain(uint8_t* out, size_t cap, uint32_t timestamp, const char* pw) {
  if (!out || !pw) return 0;
  size_t n = 0;
  while (n < 16 && pw[n]) n++;
  if (n == 0 || n > 15 || cap < 4 + n) return 0;
  writeLe32(out, timestamp);
  memcpy(out + 4, pw, n);
  return 4 + n;
}

// Consumes the node shared secret and wipes every temporary plaintext copy.
static inline void openRequestedEnvelope(RequestedTarget& target, uint8_t secret[32],
                                          const uint8_t observer[32]) {
  uint8_t any = 0;
  for (uint8_t i = 0; i < 32; i++) any |= secret[i];
  char pw[16] = {};
  const PwEnvelope& e = target.envelope;
  const bool ok = any != 0 && openPwEnvelope(secret, observer, target.pub, e.epk, e.ct, e.tag, e.ver, pw);
  envelopeOpened(target, ok, pw);
  detail::wipe(secret, 32);
  detail::wipe(pw, sizeof(pw));
}

}  // namespace poll
