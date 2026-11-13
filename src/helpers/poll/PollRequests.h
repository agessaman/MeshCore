#pragma once

// Requested targets and their passwords, held only in memory. The caller
// passes the owners snapshot. A dropped password is wiped.

#include "PollControlTypes.h"

#include <stdint.h>
#include <string.h>

namespace poll {

// One analyzer target. envelope is the newest copy; pw is the decrypted password.
struct RequestedTarget {
  bool       held;
  uint8_t    pub[kPubKeySize];
  PwEnvelope envelope;
  char       pw[16];
  bool       pw_ready;     // set by the caller after a successful decrypt
  uint32_t   pw_ver;
  uint32_t   failed_ver;   // envelope ver that login_failed refers to
  bool       login_failed;
  bool       pw_bad;
};

struct RequestTable {
  RequestedTarget targets[kMaxTargets];
  uint8_t count;
};

// Keys the last reconcile or dropAll added or removed, so the caller can log them.
struct RequestDelta {
  uint8_t added_count;
  uint8_t added[kMaxTargets][kPubKeySize];
  uint8_t dropped_count;
  uint8_t dropped[kMaxTargets][kPubKeySize];
};

// The owners snapshot reconcile reads. entries are already in rank order.
struct RequestOffer {
  bool owners_ok;
  uint8_t local_count;
  const uint8_t (*local)[kPubKeySize];
  uint8_t count;
  const RequestedEntry* entries;
};

// Both the opt-in and https are required before a requested target is accepted.
static inline bool requestsAllowed(bool opt_in, bool url_is_https) {
  return opt_in && url_is_https;
}

// Configured targets and held requested targets share the eight slots.
static inline uint8_t requestFreeSlots(uint8_t local_count, uint8_t held_count) {
  const unsigned used = (unsigned)local_count + (unsigned)held_count;
  if (used >= kMaxTargets) return 0;
  return (uint8_t)(kMaxTargets - used);
}

// True when the opt-in, the https URL, or the owners roles no longer hold.
static inline bool requestsMustDrop(bool opt_in, bool url_is_https, bool url_changed,
                                    uint32_t owners_ok_at, uint32_t now) {
  if (url_changed || !requestsAllowed(opt_in, url_is_https)) return true;
  return ownersLapsed(owners_ok_at, now);
}

// Skip a bad envelope, one not decrypted yet, or a refusal at the current ver.
static inline bool requestMayPoll(const RequestedTarget& target) {
  if (!target.held) return false;
  if (target.pw_bad || target.envelope.bad) return false;
  if (target.envelope.present && !target.pw_ready) return false;
  const uint32_t ver = target.envelope.present ? target.envelope.ver : 0;
  if (target.login_failed && target.failed_ver == ver) return false;
  return true;
}

static inline void wipeRequested(RequestedTarget& target) {
  volatile char* p = target.pw;
  for (uint8_t i = 0; i < (uint8_t)sizeof(target.pw); i++) p[i] = 0;
  memset(&target, 0, sizeof(target));
}

static inline bool sameEnvelope(const PwEnvelope& a, const PwEnvelope& b) {
  if (a.present != b.present || a.bad != b.bad || a.ver != b.ver) return false;
  if (!a.present) return true;
  return memcmp(a.epk, b.epk, sizeof(a.epk)) == 0 && memcmp(a.ct, b.ct, sizeof(a.ct)) == 0 &&
         memcmp(a.tag, b.tag, sizeof(a.tag)) == 0;
}

static inline bool sameKey(const uint8_t* a, const uint8_t* b) {
  return memcmp(a, b, kPubKeySize) == 0;
}

static inline bool keyListed(const uint8_t* key, const uint8_t (*keys)[kPubKeySize], uint8_t n) {
  if (!keys) return false;
  if (n > kMaxTargets) n = kMaxTargets;
  for (uint8_t i = 0; i < n; i++) {
    if (sameKey(keys[i], key)) return true;
  }
  return false;
}

static inline void noteKey(uint8_t* count, uint8_t (*keys)[kPubKeySize], const uint8_t* key) {
  if (*count >= kMaxTargets) return;
  memcpy(keys[*count], key, kPubKeySize);
  (*count)++;
}

static inline int findRequested(const RequestTable& table, const uint8_t* pub) {
  uint8_t n = table.count > kMaxTargets ? kMaxTargets : table.count;
  for (uint8_t i = 0; i < n; i++) {
    if (table.targets[i].held && sameKey(table.targets[i].pub, pub)) return (int)i;
  }
  return -1;
}

// A new envelope drops the decrypted password and the refusal, so it can be tried again.
static inline void replaceEnvelope(RequestedTarget& target, const PwEnvelope& envelope) {
  volatile char* p = target.pw;
  for (uint8_t i = 0; i < (uint8_t)sizeof(target.pw); i++) p[i] = 0;
  target.envelope = envelope;
  target.pw_bad = envelope.bad;
  target.pw_ready = false;
  target.pw_ver = 0;
  target.login_failed = false;
  target.failed_ver = 0;
}

static inline void removeRequested(RequestTable& table, uint8_t index, RequestDelta& delta) {
  if (index >= table.count || table.count > kMaxTargets) return;
  noteKey(&delta.dropped_count, delta.dropped, table.targets[index].pub);
  wipeRequested(table.targets[index]);
  for (uint8_t j = index; j + 1 < table.count; j++) table.targets[j] = table.targets[j + 1];
  table.count--;
  wipeRequested(table.targets[table.count]);
}

// Forget every requested target and wipe its password.
static inline void dropAll(RequestTable& table, RequestDelta& delta) {
  memset(&delta, 0, sizeof(delta));
  uint8_t n = table.count > kMaxTargets ? kMaxTargets : table.count;
  for (uint8_t i = 0; i < n; i++) noteKey(&delta.dropped_count, delta.dropped, table.targets[i].pub);
  for (uint8_t i = 0; i < kMaxTargets; i++) wipeRequested(table.targets[i]);
  table.count = 0;
}

// Apply one owners snapshot. now is unused here; requestsMustDrop reads the clock.
static inline void reconcile(RequestTable& table, const RequestOffer& offer, uint32_t now,
                             RequestDelta& delta) {
  (void)now;
  memset(&delta, 0, sizeof(delta));
  if (table.count > kMaxTargets) table.count = kMaxTargets;

  const uint8_t local_n = offer.local ? offer.local_count : 0;
  for (uint8_t i = 0; i < table.count;) {
    if (keyListed(table.targets[i].pub, offer.local, local_n)) removeRequested(table, i, delta);
    else i++;
  }
  if (!offer.owners_ok) return;

  const RequestedEntry* entries = offer.entries;
  uint8_t offered = entries ? offer.count : 0;
  if (offered > kMaxTargets) offered = kMaxTargets;

  for (uint8_t i = 0; i < table.count;) {
    bool listed = false;
    for (uint8_t j = 0; j < offered; j++) {
      if (entries[j].entry.requested && sameKey(table.targets[i].pub, entries[j].pub)) listed = true;
    }
    if (!listed) removeRequested(table, i, delta);
    else i++;
  }

  for (uint8_t j = 0; j < offered; j++) {
    if (!entries[j].entry.requested) continue;
    const int idx = findRequested(table, entries[j].pub);
    if (idx < 0) continue;
    RequestedTarget& target = table.targets[idx];
    if (!sameEnvelope(target.envelope, entries[j].entry.pw)) {
      replaceEnvelope(target, entries[j].entry.pw);
    }
  }

  for (uint8_t j = 0; j < offered; j++) {
    if (!entries[j].entry.requested) continue;
    if (keyListed(entries[j].pub, offer.local, local_n)) continue;
    if (findRequested(table, entries[j].pub) >= 0) continue;
    if (requestFreeSlots(local_n, table.count) == 0) break;
    RequestedTarget& target = table.targets[table.count];
    memset(&target, 0, sizeof(target));
    target.held = true;
    memcpy(target.pub, entries[j].pub, kPubKeySize);
    target.envelope = entries[j].entry.pw;
    target.pw_bad = entries[j].entry.pw.bad;
    table.count++;
    noteKey(&delta.added_count, delta.added, entries[j].pub);
  }
}

}  // namespace poll
