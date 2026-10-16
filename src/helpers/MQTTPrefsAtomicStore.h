#pragma once

#include <stddef.h>
#include <stdint.h>

// Transactional writer policy for preferences. The Store interface is
// intentionally narrow so host tests can exercise every failure boundary
// without an Arduino filesystem.
namespace MQTTPrefsAtomicStore {

// Production /mqtt.json flow. finish() verifies the exact bytes written,
// verify_image reparses the finished temp through the schema, and only then is
// commit() allowed to publish it. A schema-verification failure discards the
// finished temp; a commit failure is undone by rollbackFailedCommit().
//
// A failed save that is really gone and one that may still surface at the next
// boot are distinct outcomes, not shades of the same one. Publishing moves the
// old primary aside before the verified temp takes its name, so a half-done
// commit leaves an image that boot recovery would promote; CommitFailed means
// that was undone, CommitIndeterminate means it could not be. The same applies
// before the commit: a rejected temp that cleanup could not delete is still an
// image recovery may promote when no primary outranks it, which is
// CleanupIndeterminate. Callers must not report these as a rollback.
enum class VerifiedImageResult : uint8_t {
  Committed,
  BeginFailed,
  WriteFailed,
  FinishFailed,
  VerifyFailed,
  CleanupIndeterminate,
  CommitFailed,
  CommitIndeterminate,
};

// The change may still be on flash at the next boot, so the caller must not
// tell the operator it was rolled back.
inline bool saveOutcomeUnresolved(VerifiedImageResult result) {
  return result == VerifiedImageResult::CleanupIndeterminate ||
         result == VerifiedImageResult::CommitIndeterminate;
}

template <typename Store, typename ImageWriter, typename ImageVerifier>
inline VerifiedImageResult writeVerifiedImage(Store& store,
                                               ImageWriter write_image,
                                               ImageVerifier verify_image) {
  if (!store.begin()) {
    store.abort();
    return VerifiedImageResult::BeginFailed;
  }
  if (!write_image()) {
    // A short write leaves a structurally incomplete image that recovery
    // classifies as invalid, so a temp that survives cleanup here cannot be
    // promoted and the change really is gone.
    store.abort();
    return VerifiedImageResult::WriteFailed;
  }
  if (!store.finish()) {
    // Unlike a short write, this does not prove the bytes on disk are
    // unusable: finish() also fails when a complete temp cannot be read back.
    return store.abort() ? VerifiedImageResult::FinishFailed
                         : VerifiedImageResult::CleanupIndeterminate;
  }
  if (!verify_image()) {
    // The temp is complete and byte-verified, only rejected by the schema. If
    // it cannot be deleted and no primary outranks it, boot recovery promotes
    // the very image this call is about to report as not saved.
    return store.discardFinishedTemp() ? VerifiedImageResult::VerifyFailed
                                       : VerifiedImageResult::CleanupIndeterminate;
  }
  if (!store.commit()) {
    // Undo the partial publish before answering. Only the store knows whether
    // the pre-transaction state was actually restored, so take its word for
    // which failure this was.
    const bool rolled_back = store.rollbackFailedCommit();
    store.abort();
    return rolled_back ? VerifiedImageResult::CommitFailed
                       : VerifiedImageResult::CommitIndeterminate;
  }
  return VerifiedImageResult::Committed;
}

enum class Result : uint8_t {
  Committed,
  BeginFailed,
  HeaderWriteFailed,
  PayloadWriteFailed,
  FinishFailed,
  CommitFailed,
};

inline bool committed(Result result) {
  return result == Result::Committed;
}

// Generic streaming transaction for structured images such as /com_prefs.
// ImageWriter writes its fields directly to Store and returns false on any
// short write, so no contiguous staging allocation is required.
enum class ImageResult : uint8_t {
  Committed,
  BeginFailed,
  WriteFailed,
  FinishFailed,
  CommitFailed,
};

inline bool imageCommitted(ImageResult result) {
  return result == ImageResult::Committed;
}

template <typename Store, typename ImageWriter>
inline ImageResult writeImage(Store& store, ImageWriter write_image) {
  if (!store.begin()) {
    store.abort();
    return ImageResult::BeginFailed;
  }
  if (!write_image(store)) {
    store.abort();
    return ImageResult::WriteFailed;
  }
  if (!store.finish()) {
    store.abort();
    return ImageResult::FinishFailed;
  }
  if (!store.commit()) {
    store.abort();
    return ImageResult::CommitFailed;
  }
  return ImageResult::Committed;
}

// Coordinates a two-file legacy upgrade. /com_prefs must not be compacted
// until the observer tail it carries has been published into /mqtt.json.
// Keeping this state in a tiny pure helper lets host tests cover power-cut
// boundaries without an Arduino filesystem.
class LegacyUpgradeGate {
public:
  explicit LegacyUpgradeGate(bool com_prefs_rewrite_pending)
      : _com_prefs_rewrite_pending(com_prefs_rewrite_pending) {}

  void requireMqttRewrite() { _mqtt_rewrite_pending = true; }

  void recordMqttSave(bool did_commit) {
    if (did_commit) {
      _mqtt_rewrite_pending = false;
      _mqtt_source_held = false;
    } else {
      _mqtt_source_held = true;
    }
  }

  void holdMqttSource() { _mqtt_source_held = true; }

  bool mqttRewritePending() const { return _mqtt_rewrite_pending; }
  bool blocksComPrefsRewrite() const {
    return _com_prefs_rewrite_pending && (_mqtt_rewrite_pending || _mqtt_source_held);
  }
  bool mayRewriteComPrefs() const {
    return _com_prefs_rewrite_pending && !blocksComPrefsRewrite();
  }

  void recordComPrefsRewrite() {
    if (mayRewriteComPrefs()) _com_prefs_rewrite_pending = false;
  }

private:
  bool _com_prefs_rewrite_pending;
  bool _mqtt_rewrite_pending = false;
  bool _mqtt_source_held = false;
};

template <typename Store>
inline Result write(Store& store, const uint8_t* header, size_t header_size,
                    const uint8_t* payload, size_t payload_size) {
  if (!store.begin()) {
    store.abort();
    return Result::BeginFailed;
  }
  if (store.write(header, header_size) != header_size) {
    store.abort();
    return Result::HeaderWriteFailed;
  }
  if (store.write(payload, payload_size) != payload_size) {
    store.abort();
    return Result::PayloadWriteFailed;
  }
  if (!store.finish()) {
    store.abort();
    return Result::FinishFailed;
  }
  if (!store.commit()) {
    store.abort();
    return Result::CommitFailed;
  }
  return Result::Committed;
}

}  // namespace MQTTPrefsAtomicStore
