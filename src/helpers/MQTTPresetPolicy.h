#pragma once

#include "MQTTPresets.h"

#include <string.h>

// Policy that keys off a preset rather than describing one.
//
// MQTTPresets.h is data: it is the broker table, and it is kept byte-identical
// between the observer-firmware and observer-firmware-dev channels so a node
// that rolls back cannot meet a preset name its firmware does not know. An
// unknown name is not merely ignored — MQTTPrefsSerializer repairs it to "none"
// and the repaired /mqtt.json is written back, so the operator's slot is gone
// for good. Channel-specific behaviour therefore lives here instead, where the
// two channels are free to differ.

// True when the broker tears down a live session once its JWT passes exp, so the
// renewal must proactively bounce the connection to present a fresh token.
//
// Default true, because getting this wrong the safe way costs a re-handshake and
// getting it wrong the unsafe way costs an outage. waev is the exception: its
// operator confirmed (2026-08-11) that their servers do not disconnect on expiry,
// so a live session there needs only its credentials refreshed for the next
// reconnect. waev is also the only preset with a short token_lifetime, so it was
// the only one bouncing often — every ~47 min, and each bounce's re-handshake can
// cost ~10 KB of contiguous internal DRAM on a non-PSRAM board.
//
// Keyed by name rather than a struct field on purpose: adding a field would mean
// re-ordering a dozen positional initialisers in the table, where a mistake is
// silent.
static inline bool mqttPresetEnforcesTokenExp(const MQTTPresetDef* preset) {
  if (!preset || !preset->name) return true;   // custom/audience slots: assume enforced
  return strcmp(preset->name, "waev") != 0;
}
