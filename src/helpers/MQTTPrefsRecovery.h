#pragma once

#include <stdint.h>

// Pure recovery policy for the three MQTT preference transaction files. The
// writer first moves the old primary to .bak, then moves the verified .tmp to
// the primary name. On a reset, the loader uses this policy before decoding
// the primary. FutureUsable is syntactically valid but belongs to newer
// firmware. FutureClaimed and Indeterminate cannot be safely classified by
// this firmware, so recovery may run a known-good backup but must retain the
// uncertain image and hold further writes. Preserve is definitively invalid or
// unsupported and may be discarded only where the transaction policy permits.
//
// The filenames are the transaction state. A temp that exists while the primary
// name is empty says the commit had already passed the backup rename, and that
// is the only record of it — so an uncertain temp is answered with
// UseBackupHeld, which renames nothing. Publishing the backup instead would
// make the candidate look like an ordinary stale artifact to the next boot,
// which would delete it exactly when it finally became readable. When neither
// file can run this boot, RunDefaultsHeld keeps both names untouched for the
// same reason.
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
  // Run the backup where it lies, changing nothing on disk, and hold writes.
  // Every later boot re-runs this policy against the same three names until one
  // of them can classify the candidate.
  UseBackupHeld,
  // Neither the candidate nor the backup can run this boot. Change nothing,
  // come up on defaults, and hold writes until a boot that can classify them.
  RunDefaultsHeld,
};

inline bool uncertain(FileState state) {
  return state == FileState::FutureClaimed || state == FileState::Indeterminate;
}

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
  // cannot classify. A known-good backup can run this boot, but it must run
  // from its own name: promoting it would spend the empty primary name that
  // marks the candidate as mid-commit.
  //
  // A backup this firmware cannot run is still the previous committed image, so
  // the same argument applies to it: promoting the candidate would give it the
  // authoritative name, and the "any primary owns the name" rule above would
  // then keep it even on a boot that proves it corrupt. Spend the transaction
  // state recorded in the filenames only once the backup is definitively no
  // longer a usable fallback.
  if (uncertain(temp)) {
    if (backup == FileState::Usable) return Action::UseBackupHeld;
    if (backup == FileState::FutureUsable || uncertain(backup)) {
      return Action::RunDefaultsHeld;
    }
    return Action::PromoteTemp;
  }

  // No temp survived. The backup is the only recoverable image, even when it
  // is a newer layout that this firmware must preserve rather than decode.
  if (backup != FileState::Missing) return Action::PromoteBackup;
  return Action::None;
}

}  // namespace MQTTPrefsRecovery
