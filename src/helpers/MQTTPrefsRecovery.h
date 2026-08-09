#pragma once

#include <stdint.h>

// Pure recovery policy for the three MQTT preference transaction files. The
// writer first moves the old primary to .bak, then moves the verified .tmp to
// the primary name. On a reset, the loader uses this policy before decoding
// the primary. FutureUsable is syntactically valid but belongs to newer
// firmware. FutureClaimed and Indeterminate cannot be safely classified by
// this firmware, so recovery may use a known-good backup but must retain the
// uncertain image and hold further writes. Preserve is definitively invalid or
// unsupported and may be discarded only where the transaction policy permits.
namespace MQTTPrefsRecovery {

enum class FileState : uint8_t {
  Missing,
  Usable,
  FutureUsable,
  FutureClaimed,
  Indeterminate,
  Preserve,
};

enum class Action : uint8_t {
  None,
  KeepPrimary,
  DiscardTemp,
  PromoteTemp,
  PromoteBackup,
};

inline Action select(FileState primary, FileState temp, FileState backup) {
  // A primary of any kind owns the name. In particular, do not roll a newer
  // or corrupt primary back to an older backup just because it cannot be read
  // by this firmware.
  if (primary != FileState::Missing) return Action::KeepPrimary;

  // A completed temp is the new image and wins over the old backup.
  if (temp == FileState::Usable || temp == FileState::FutureUsable) {
    return Action::PromoteTemp;
  }

  // Preserve is definitively invalid, so it was never a committed image. If
  // no backup exists, discard the interrupted temp and leave the primary name
  // absent; this is what lets a first JSON migration retry from /mqtt_prefs.
  // If a backup exists, it is the prior committed image and wins regardless
  // of whether this firmware understands its schema.
  if (temp == FileState::Preserve) {
    return backup == FileState::Missing ? Action::DiscardTemp : Action::PromoteBackup;
  }

  // FutureClaimed and Indeterminate may be a completed image this firmware
  // cannot classify. A known-good backup can run this boot, but without one
  // preserve the only candidate under the authoritative name and hold writes.
  if (temp == FileState::FutureClaimed || temp == FileState::Indeterminate) {
    return backup == FileState::Usable ? Action::PromoteBackup : Action::PromoteTemp;
  }

  // No temp survived. The backup is the only recoverable image, even when it
  // is a newer layout that this firmware must preserve rather than decode.
  if (backup != FileState::Missing) return Action::PromoteBackup;
  return Action::None;
}

inline bool uncertain(FileState state) {
  return state == FileState::FutureClaimed || state == FileState::Indeterminate;
}

}  // namespace MQTTPrefsRecovery
