#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "helpers/MQTTPrefsAtomicStore.h"
#include "helpers/MQTTPrefsRecovery.h"

namespace AtomicStore = MQTTPrefsAtomicStore;
namespace Recovery = MQTTPrefsRecovery;

namespace {

enum class FailurePoint {
  None,
  Begin,
  HeaderWrite,
  PayloadWrite,
  ImageWrite,
  Finish,
  Verify,
  Commit,
};

class InMemoryStore {
public:
  explicit InMemoryStore(FailurePoint failure, bool preexisting_recovery_temp = false)
      : _failure(failure), _preexisting_recovery_temp(preexisting_recovery_temp) {
    _files["/mqtt_prefs"] = {'o', 'l', 'd', '-', 'p', 'r', 'e', 'f', 's'};
    if (_preexisting_recovery_temp) _files["/mqtt_prefs.tmp"] = {'r', 'e', 'c', 'o', 'v', 'e', 'r'};
  }

  bool begin() {
    ++begin_calls;
    if (_preexisting_recovery_temp) return false;
    _files.erase("/mqtt_prefs.tmp");
    _open = _failure != FailurePoint::Begin;
    _owns_temp = _open;
    return _open;
  }

  size_t write(const uint8_t* bytes, size_t size) {
    ++write_calls;
    if (!_open) return 0;
    const bool should_fail = (write_calls == 1 && _failure == FailurePoint::HeaderWrite) ||
        (write_calls == 2 && _failure == FailurePoint::PayloadWrite) ||
        _failure == FailurePoint::ImageWrite;
    const size_t written = should_fail && size > 0 ? size - 1 : size;
    _staging.insert(_staging.end(), bytes, bytes + written);
    return written;
  }

  bool finish() {
    ++finish_calls;
    _open = false;
    if (_failure == FailurePoint::Finish) return false;
    _files["/mqtt_prefs.tmp"] = _staging;
    _finished = true;
    return true;
  }

  bool commit() {
    ++commit_calls;
    if (_failure == FailurePoint::Commit) return false;
    _files["/mqtt_prefs"] = _files["/mqtt_prefs.tmp"];
    _files.erase("/mqtt_prefs.tmp");
    _finished = false;
    return true;
  }

  void abort() {
    ++abort_calls;
    _open = false;
    _staging.clear();
    // Mirrors MQTTPrefsFileStore: after finish(), a failed commit may already
    // have moved the old primary to .bak, so the verified temp is recovery
    // data rather than disposable staging.
    if (_owns_temp && !_finished) _files.erase("/mqtt_prefs.tmp");
    _finished = false;
    _owns_temp = false;
  }

  const std::vector<uint8_t>& source() const { return _files.at("/mqtt_prefs"); }
  bool tempExists() const { return _files.count("/mqtt_prefs.tmp") != 0; }

  int begin_calls = 0;
  int write_calls = 0;
  int finish_calls = 0;
  int commit_calls = 0;
  int abort_calls = 0;

private:
  FailurePoint _failure;
  bool _preexisting_recovery_temp = false;
  bool _open = false;
  bool _finished = false;
  bool _owns_temp = false;
  std::vector<uint8_t> _staging;
  std::map<std::string, std::vector<uint8_t>> _files;
};

AtomicStore::Result run(InMemoryStore* store) {
  const uint8_t header[] = {0xf5, 'M', 'Q', 'P', 1, 0, 0x09, 0x00};
  const uint8_t payload[] = {'n', 'e', 'w', '-', 'p', 'r', 'e', 'f', 's'};
  return AtomicStore::write(*store, header, sizeof(header), payload, sizeof(payload));
}

AtomicStore::Result runWithObserverTail(InMemoryStore* store) {
  const uint8_t header[] = {0xf5, 'M', 'Q', 'P', 1, 0, 0x0a, 0x00};
  // The final three bytes stand in for the observer tail transferred from a
  // legacy /com_prefs file. They must be committed before that source is compacted.
  const uint8_t payload[] = {'m', 'i', 'g', 'r', 'a', 't', 'e', 0x91, 0x7e, 0xa5};
  return AtomicStore::write(*store, header, sizeof(header), payload, sizeof(payload));
}

// Models MQTTPrefsJsonFileStore under the SPIFFS rule it was written for:
// rename() refuses an existing destination, so publishing must move the old
// primary to .bak before the verified temp can take its name. Both halves of
// that publish, and the rollback that undoes a half-done one, are injectable.
class InMemoryJsonStore {
public:
  struct Options {
    FailurePoint failure = FailurePoint::None;
    bool rollback_rename_fails = false;
    bool rollback_remove_fails = false;
    bool has_primary = true;
  };

  explicit InMemoryJsonStore(FailurePoint failure) : _opts{failure, false, false, true} {
    _files["/mqtt.json"] = oldImage();
  }
  explicit InMemoryJsonStore(Options opts) : _opts(opts) {
    if (_opts.has_primary) _files["/mqtt.json"] = oldImage();
  }

  bool begin() {
    ++begin_calls;
    // Production refuses to open a new transaction while recovery still owns
    // an artifact, so a rollback that left one behind blocks the retry.
    if (has("/mqtt.json.tmp") || has("/mqtt.json.bak")) return false;
    _staging.clear();
    _open = _opts.failure != FailurePoint::Begin;
    return _open;
  }

  size_t write(const uint8_t* bytes, size_t size) {
    ++write_calls;
    if (!_open) return 0;
    const size_t written =
        _opts.failure == FailurePoint::ImageWrite && size > 0 ? size - 1 : size;
    _staging.insert(_staging.end(), bytes, bytes + written);
    return written;
  }

  bool finish() {
    ++finish_calls;
    _open = false;
    if (_opts.failure == FailurePoint::Finish) return false;
    _files["/mqtt.json.tmp"] = _staging;
    _finished = true;
    return true;
  }

  bool verify() {
    ++verify_calls;
    return _opts.failure != FailurePoint::Verify;
  }

  bool commit() {
    ++commit_calls;
    if (!_finished) return false;
    if (has("/mqtt.json.bak")) return false;
    if (has("/mqtt.json") && !rename("/mqtt.json", "/mqtt.json.bak")) return false;
    // Fail the publish rename, the boundary that leaves the old primary parked
    // in .bak with the verified new image still sitting in the temp.
    if (_opts.failure == FailurePoint::Commit) return false;
    if (!rename("/mqtt.json.tmp", "/mqtt.json")) return false;
    _files.erase("/mqtt.json.bak");
    _finished = false;
    return true;
  }

  bool rollbackFailedCommit() {
    ++rollback_calls;
    if (!has("/mqtt.json") && has("/mqtt.json.bak")) {
      if (_opts.rollback_rename_fails || !rename("/mqtt.json.bak", "/mqtt.json")) {
        return false;
      }
    }
    if (has("/mqtt.json.tmp")) {
      if (_opts.rollback_remove_fails) return has("/mqtt.json");
      _files.erase("/mqtt.json.tmp");
    }
    return true;
  }

  void discardFinishedTemp() {
    ++discard_calls;
    _files.erase("/mqtt.json.tmp");
    _finished = false;
  }

  void abort() {
    ++abort_calls;
    _open = false;
    _staging.clear();
    if (!_finished) _files.erase("/mqtt.json.tmp");
    _finished = false;
  }

  // Apply the boot-recovery policy to whatever the transaction left behind and
  // report the image the node would come up on. Every file present in these
  // scenarios is a complete image, so each maps to Usable.
  std::vector<uint8_t> imageAfterReboot() {
    const auto state = [this](const char* path) {
      return has(path) ? Recovery::FileState::Usable : Recovery::FileState::Missing;
    };
    switch (Recovery::select(state("/mqtt.json"), state("/mqtt.json.tmp"),
                             state("/mqtt.json.bak"))) {
      case Recovery::Action::PromoteTemp:
        rename("/mqtt.json.tmp", "/mqtt.json");
        break;
      case Recovery::Action::PromoteBackup:
        rename("/mqtt.json.bak", "/mqtt.json");
        break;
      case Recovery::Action::DiscardTemp:
        _files.erase("/mqtt.json.tmp");
        break;
      default:
        break;
    }
    return has("/mqtt.json") ? _files.at("/mqtt.json") : std::vector<uint8_t>();
  }

  bool has(const char* path) const { return _files.count(path) != 0; }
  bool canStartSave() const { return !has("/mqtt.json.tmp") && !has("/mqtt.json.bak"); }
  const std::vector<uint8_t>& source() const { return _files.at("/mqtt.json"); }
  static std::vector<uint8_t> oldImage() {
    const char* text = "{version:1,wifi:{ssid:\"old\"}}";
    return std::vector<uint8_t>(text, text + strlen(text));
  }
  static std::vector<uint8_t> newImage() {
    const char* text = "{version:1,wifi:{ssid:\"mesh\"}}";
    return std::vector<uint8_t>(text, text + strlen(text));
  }

  int begin_calls = 0;
  int write_calls = 0;
  int finish_calls = 0;
  int commit_calls = 0;
  int abort_calls = 0;
  int verify_calls = 0;
  int discard_calls = 0;
  int rollback_calls = 0;

private:
  bool rename(const char* from, const char* to) {
    if (_files.count(from) == 0 || _files.count(to) != 0) return false;
    _files[to] = _files[from];
    _files.erase(from);
    return true;
  }

  Options _opts;
  bool _open = false;
  bool _finished = false;
  std::vector<uint8_t> _staging;
  std::map<std::string, std::vector<uint8_t>> _files;
};

AtomicStore::VerifiedImageResult runVerifiedJson(InMemoryJsonStore* store) {
  const std::vector<uint8_t> json = InMemoryJsonStore::newImage();
  return AtomicStore::writeVerifiedImage(
      *store,
      [store, &json]() {
        return store->write(json.data(), json.size()) == json.size();
      },
      [store]() { return store->verify(); });
}

class LegacyComPrefs {
public:
  LegacyComPrefs() : bytes({'l', 'e', 'g', 'a', 'c', 'y', '-', 'c', 'o', 'm'}) {}

  void compactAfterMqttCommit(const std::vector<uint8_t>& mqtt_bytes) {
    const std::vector<uint8_t> observer_tail = {0x91, 0x7e, 0xa5};
    mqtt_tail_present_before_compaction = mqtt_bytes.size() >= observer_tail.size() &&
        std::equal(observer_tail.rbegin(), observer_tail.rend(), mqtt_bytes.rbegin());
    bytes = {'c', 'o', 'm', 'p', 'a', 'c', 't'};
    ++compact_calls;
  }

  std::vector<uint8_t> bytes;
  bool mqtt_tail_present_before_compaction = false;
  int compact_calls = 0;
};

class LegacyNodePrefs {
public:
  LegacyNodePrefs() : bytes({'l', 'e', 'g', 'a', 'c', 'y', '-', 'n', 'o', 'd', 'e'}) {}

  void migrateAfterMqttCommit(const std::vector<uint8_t>& mqtt_bytes) {
    const std::vector<uint8_t> observer_tail = {0x91, 0x7e, 0xa5};
    mqtt_tail_present_before_removal = mqtt_bytes.size() >= observer_tail.size() &&
        std::equal(observer_tail.rbegin(), observer_tail.rend(), mqtt_bytes.rbegin());
    bytes.clear();  // model removal after current-layout /com_prefs is written
    ++migration_calls;
  }

  std::vector<uint8_t> bytes;
  bool mqtt_tail_present_before_removal = false;
  int migration_calls = 0;
};

// Models the final old-name migration separately from the MQTT transaction:
// /com_prefs is absent while /node_prefs is authoritative. A failed temp write
// or rename must leave that source as the only usable preference image.
class InMemoryCommonPrefsStore {
public:
  explicit InMemoryCommonPrefsStore(FailurePoint failure) : _failure(failure) {
    _files["/node_prefs"] = {'l', 'e', 'g', 'a', 'c', 'y', '-', 'n', 'o', 'd', 'e'};
  }

  bool begin() {
    ++begin_calls;
    _files.erase("/com_prefs.tmp");
    _staging.clear();
    _open = _failure != FailurePoint::Begin;
    return _open;
  }

  size_t write(const uint8_t* bytes, size_t size) {
    ++write_calls;
    if (!_open) return 0;
    const bool should_fail = _failure == FailurePoint::ImageWrite && write_calls == 2;
    const size_t written = should_fail && size > 0 ? size - 1 : size;
    _staging.insert(_staging.end(), bytes, bytes + written);
    return written;
  }

  bool finish() {
    ++finish_calls;
    _open = false;
    if (_failure == FailurePoint::Finish) return false;
    _files["/com_prefs.tmp"] = _staging;
    return true;
  }

  bool commit() {
    ++commit_calls;
    if (_failure == FailurePoint::Commit) return false;
    _files["/com_prefs"] = _files["/com_prefs.tmp"];
    _files.erase("/com_prefs.tmp");
    return true;
  }

  void abort() {
    ++abort_calls;
    _open = false;
    _staging.clear();
    _files.erase("/com_prefs.tmp");
  }

  void removeNodeSource() { _files.erase("/node_prefs"); }
  const std::vector<uint8_t>& nodeSource() const { return _files.at("/node_prefs"); }
  const std::vector<uint8_t>& destination() const { return _files.at("/com_prefs"); }
  bool destinationExists() const { return _files.count("/com_prefs") != 0; }
  bool tempExists() const { return _files.count("/com_prefs.tmp") != 0; }
  bool nodeSourceIsPreferred() const {
    return _files.count("/node_prefs") != 0 && _files.count("/com_prefs") == 0;
  }
  bool nodeSourceExists() const { return _files.count("/node_prefs") != 0; }

  int begin_calls = 0;
  int write_calls = 0;
  int finish_calls = 0;
  int commit_calls = 0;
  int abort_calls = 0;

private:
  FailurePoint _failure;
  bool _open = false;
  std::vector<uint8_t> _staging;
  std::map<std::string, std::vector<uint8_t>> _files;
};

AtomicStore::ImageResult runCommonPrefsImage(InMemoryCommonPrefsStore* store) {
  const uint8_t core[] = {'c', 'o', 'm', '-', 'p', 'r', 'e', 'f', 's'};
  const uint8_t tail[] = {0x19, 0xa4, 0x7e};
  return AtomicStore::writeImage(*store, [&core, &tail](InMemoryCommonPrefsStore& target) {
    return target.write(core, sizeof(core)) == sizeof(core) &&
        target.write(tail, sizeof(tail)) == sizeof(tail);
  });
}

// Models the exact SPIFFS transaction used by MQTTPrefsFileStore. Files only
// move by rename: SPIFFS rejects a destination that already exists, so the old
// primary must remain available as .bak until the new temp owns the primary.
class SpiffsMqttTransaction {
public:
  enum class Boundary {
    BeforeBackupRename,
    AfterBackupRename,
    AfterPrimaryRename,
    AfterBackupCleanup,
  };

  SpiffsMqttTransaction() {
    _files["/mqtt_prefs"] = oldImage();
  }

  void writeVerifiedTemp() { _files["/mqtt_prefs.tmp"] = newImage(); }
  void cutDuringTempWrite() { _files["/mqtt_prefs.tmp"] = {'n'}; }

  void cutAt(Boundary boundary) {
    writeVerifiedTemp();
    if (boundary == Boundary::BeforeBackupRename) return;
    rename("/mqtt_prefs", "/mqtt_prefs.bak");
    if (boundary == Boundary::AfterBackupRename) return;
    rename("/mqtt_prefs.tmp", "/mqtt_prefs");
    if (boundary == Boundary::AfterPrimaryRename) return;
    _files.erase("/mqtt_prefs.bak");
  }

  // Inject ordinary operation failures (as distinct from a power cut). A
  // failed temp rename leaves both the verified temp and old backup intact,
  // which is the state rollbackFailedPublish() then undoes.
  bool publish(bool fail_backup_rename, bool fail_temp_rename, bool fail_cleanup) {
    writeVerifiedTemp();
    if (fail_backup_rename || !rename("/mqtt_prefs", "/mqtt_prefs.bak")) return false;
    if (fail_temp_rename || !rename("/mqtt_prefs.tmp", "/mqtt_prefs")) return false;
    if (!fail_cleanup) _files.erase("/mqtt_prefs.bak");
    return true;  // backup cleanup is intentionally non-fatal after publish
  }

  bool rollbackFailedPublish() {
    if (!has("/mqtt_prefs") && has("/mqtt_prefs.bak") &&
        !rename("/mqtt_prefs.bak", "/mqtt_prefs")) {
      return false;
    }
    _files.erase("/mqtt_prefs.tmp");
    return true;
  }

  void recover(Recovery::FileState primary = Recovery::FileState::Usable,
               Recovery::FileState temp = Recovery::FileState::Usable,
               Recovery::FileState backup = Recovery::FileState::Usable) {
    const bool had_primary = _files.count("/mqtt_prefs") != 0;
    const auto stateFor = [&](const char* path, Recovery::FileState readable) {
      return _files.count(path) == 0 ? Recovery::FileState::Missing : readable;
    };
    const Recovery::Action action = Recovery::select(
        stateFor("/mqtt_prefs", primary), stateFor("/mqtt_prefs.tmp", temp),
        stateFor("/mqtt_prefs.bak", backup));
    if (action == Recovery::Action::PromoteTemp) {
      rename("/mqtt_prefs.tmp", "/mqtt_prefs");
      // Match production: once a usable temp becomes primary, every backup is
      // stale and is cleared so a second save can start this boot.
      if (temp == Recovery::FileState::Usable && backup != Recovery::FileState::Missing) {
        _files.erase("/mqtt_prefs.bak");
      }
      return;
    }
    if (action == Recovery::Action::DiscardTemp) {
      _files.erase("/mqtt_prefs.tmp");
      return;
    }
    if (action == Recovery::Action::UseBackupHeld) {
      return;  // production renames nothing and runs the backup where it lies
    }
    if (action == Recovery::Action::PromoteBackup) {
      rename("/mqtt_prefs.bak", "/mqtt_prefs");
      if (backup == Recovery::FileState::Usable && !Recovery::uncertain(temp) &&
          temp != Recovery::FileState::Missing) {
        _files.erase("/mqtt_prefs.tmp");
      }
      return;
    }

    // A usable primary is authoritative, so production cleans every stale or
    // incomplete transaction artifact. It only preserves artifacts when the
    // primary itself is opaque.
    if (had_primary && primary == Recovery::FileState::Usable) {
      if (!Recovery::uncertain(temp)) _files.erase("/mqtt_prefs.tmp");
      if (!Recovery::uncertain(backup)) _files.erase("/mqtt_prefs.bak");
    }
  }

  bool has(const char* path) const { return _files.count(path) != 0; }
  void removePrimary() { _files.erase("/mqtt_prefs"); }
  bool canStartSave() const { return !has("/mqtt_prefs.tmp") && !has("/mqtt_prefs.bak"); }
  const std::vector<uint8_t>& primary() const { return _files.at("/mqtt_prefs"); }
  static std::vector<uint8_t> oldImage() { return {'o', 'l', 'd'}; }
  static std::vector<uint8_t> newImage() { return {'n', 'e', 'w'}; }

private:
  bool rename(const char* from, const char* to) {
    if (_files.count(from) == 0 || _files.count(to) != 0) return false;
    _files[to] = _files[from];
    _files.erase(from);
    return true;
  }

  std::map<std::string, std::vector<uint8_t>> _files;
};

}  // namespace

TEST(MQTTPrefsAtomicStore, CommitPublishesExactHeaderThenPayload) {
  InMemoryStore store(FailurePoint::None);

  EXPECT_EQ(AtomicStore::Result::Committed, run(&store));
  EXPECT_EQ((std::vector<uint8_t>{0xf5, 'M', 'Q', 'P', 1, 0, 0x09, 0x00,
                                  'n', 'e', 'w', '-', 'p', 'r', 'e', 'f', 's'}),
            store.source());
  EXPECT_FALSE(store.tempExists());
  EXPECT_EQ(1, store.begin_calls);
  EXPECT_EQ(2, store.write_calls);
  EXPECT_EQ(1, store.finish_calls);
  EXPECT_EQ(1, store.commit_calls);
  EXPECT_EQ(0, store.abort_calls);
}

TEST(MQTTPrefsAtomicStore, ProductionJsonPolicyCoversEveryVerificationBoundary) {
  const struct {
    FailurePoint point;
    AtomicStore::VerifiedImageResult expected;
    int writes;
    int finishes;
    int verifies;
    int commits;
    int aborts;
    int discards;
    int rollbacks;
  } cases[] = {
      {FailurePoint::None, AtomicStore::VerifiedImageResult::Committed,
       1, 1, 1, 1, 0, 0, 0},
      {FailurePoint::Begin, AtomicStore::VerifiedImageResult::BeginFailed,
       0, 0, 0, 0, 1, 0, 0},
      {FailurePoint::ImageWrite, AtomicStore::VerifiedImageResult::WriteFailed,
       1, 0, 0, 0, 1, 0, 0},
      {FailurePoint::Finish, AtomicStore::VerifiedImageResult::FinishFailed,
       1, 1, 0, 0, 1, 0, 0},
      {FailurePoint::Verify, AtomicStore::VerifiedImageResult::VerifyFailed,
       1, 1, 1, 0, 0, 1, 0},
      {FailurePoint::Commit, AtomicStore::VerifiedImageResult::CommitFailed,
       1, 1, 1, 1, 1, 0, 1},
  };

  for (const auto& test_case : cases) {
    InMemoryJsonStore store(test_case.point);
    EXPECT_EQ(test_case.expected, runVerifiedJson(&store));
    EXPECT_EQ(test_case.writes, store.write_calls);
    EXPECT_EQ(test_case.finishes, store.finish_calls);
    EXPECT_EQ(test_case.verifies, store.verify_calls);
    EXPECT_EQ(test_case.commits, store.commit_calls);
    EXPECT_EQ(test_case.aborts, store.abort_calls);
    EXPECT_EQ(test_case.discards, store.discard_calls);
    EXPECT_EQ(test_case.rollbacks, store.rollback_calls);
    // Every failure leaves the previous image published and no artifact behind,
    // so the next save can start immediately.
    if (test_case.point != FailurePoint::None) {
      EXPECT_EQ(InMemoryJsonStore::oldImage(), store.source());
      EXPECT_TRUE(store.canStartSave());
    }
  }
}

TEST(MQTTPrefsAtomicStore, FailedPublishIsUndoneSoTheRefusedValueCannotReturnAtBoot) {
  InMemoryJsonStore store(FailurePoint::Commit);

  // The publish moved the old primary to .bak before failing to rename the
  // verified temp into place. Reporting the change as rolled back is only
  // truthful because that half-done transaction is undone here.
  ASSERT_EQ(AtomicStore::VerifiedImageResult::CommitFailed, runVerifiedJson(&store));
  EXPECT_EQ(InMemoryJsonStore::oldImage(), store.source());
  EXPECT_FALSE(store.has("/mqtt.json.tmp"));
  EXPECT_FALSE(store.has("/mqtt.json.bak"));
  EXPECT_EQ(InMemoryJsonStore::oldImage(), store.imageAfterReboot());
}

TEST(MQTTPrefsAtomicStore, UnrestorablePublishFailureIsReportedAsIndeterminate) {
  // Rollback cannot republish the backup: the primary name stays empty and the
  // verified temp still wins recovery, so the refused value does come back at
  // the next boot. The caller must say so rather than claim a rollback.
  InMemoryJsonStore backup_stuck(InMemoryJsonStore::Options{
      FailurePoint::Commit, /*rollback_rename_fails=*/true, false, true});
  ASSERT_EQ(AtomicStore::VerifiedImageResult::CommitIndeterminate,
            runVerifiedJson(&backup_stuck));
  EXPECT_TRUE(backup_stuck.has("/mqtt.json.tmp"));
  EXPECT_TRUE(backup_stuck.has("/mqtt.json.bak"));
  EXPECT_FALSE(backup_stuck.canStartSave());  // retries fail until this resolves
  EXPECT_EQ(InMemoryJsonStore::newImage(), backup_stuck.imageAfterReboot());

  // Same verdict for the first-ever save, where there is no backup to restore
  // and the undeletable temp is the only image the next boot can find.
  InMemoryJsonStore first_save(InMemoryJsonStore::Options{
      FailurePoint::Commit, false, /*rollback_remove_fails=*/true,
      /*has_primary=*/false});
  ASSERT_EQ(AtomicStore::VerifiedImageResult::CommitIndeterminate,
            runVerifiedJson(&first_save));
  EXPECT_FALSE(first_save.has("/mqtt.json"));
  EXPECT_EQ(InMemoryJsonStore::newImage(), first_save.imageAfterReboot());
}

TEST(MQTTPrefsAtomicStore, FirstSavePublishFailureLeavesNoImageToPromote) {
  InMemoryJsonStore store(InMemoryJsonStore::Options{
      FailurePoint::Commit, false, false, /*has_primary=*/false});

  // No prior /mqtt.json exists, so rollback only has to discard the temp. The
  // next boot re-runs migration from the legacy source instead of adopting the
  // value the CLI just refused.
  ASSERT_EQ(AtomicStore::VerifiedImageResult::CommitFailed, runVerifiedJson(&store));
  EXPECT_FALSE(store.has("/mqtt.json.tmp"));
  EXPECT_TRUE(store.imageAfterReboot().empty());
  EXPECT_TRUE(store.canStartSave());
}

TEST(MQTTPrefsAtomicStore, AnyFailureAbortsAndPreservesExistingSource) {
  const std::vector<uint8_t> source = {'o', 'l', 'd', '-', 'p', 'r', 'e', 'f', 's'};
  const struct {
    FailurePoint point;
    AtomicStore::Result expected;
    int writes;
    int finishes;
    int commits;
  } cases[] = {
      {FailurePoint::Begin, AtomicStore::Result::BeginFailed, 0, 0, 0},
      {FailurePoint::HeaderWrite, AtomicStore::Result::HeaderWriteFailed, 1, 0, 0},
      {FailurePoint::PayloadWrite, AtomicStore::Result::PayloadWriteFailed, 2, 0, 0},
      {FailurePoint::Finish, AtomicStore::Result::FinishFailed, 2, 1, 0},
      {FailurePoint::Commit, AtomicStore::Result::CommitFailed, 2, 1, 1},
  };

  for (const auto& test_case : cases) {
    InMemoryStore store(test_case.point);
    EXPECT_EQ(test_case.expected, run(&store));
    EXPECT_EQ(source, store.source());
    EXPECT_EQ(test_case.point == FailurePoint::Commit, store.tempExists());
    EXPECT_EQ(1, store.begin_calls);
    EXPECT_EQ(test_case.writes, store.write_calls);
    EXPECT_EQ(test_case.finishes, store.finish_calls);
    EXPECT_EQ(test_case.commits, store.commit_calls);
    EXPECT_EQ(1, store.abort_calls);
  }
}

TEST(MQTTPrefsAtomicStore, BeginFailureDoesNotErasePreexistingRecoveryTemp) {
  InMemoryStore store(FailurePoint::Begin, true);

  EXPECT_EQ(AtomicStore::Result::BeginFailed, run(&store));
  EXPECT_TRUE(store.tempExists());
  EXPECT_EQ(1, store.abort_calls);
}

TEST(MQTTPrefsAtomicStore, LegacyCrossFileUpgradeCommitsTailBeforeCompactingComPrefs) {
  InMemoryStore mqtt_store(FailurePoint::None);
  LegacyComPrefs com_prefs;
  AtomicStore::LegacyUpgradeGate gate(true);
  gate.requireMqttRewrite();

  const AtomicStore::Result result = runWithObserverTail(&mqtt_store);
  gate.recordMqttSave(AtomicStore::committed(result));
  ASSERT_TRUE(gate.mayRewriteComPrefs());

  com_prefs.compactAfterMqttCommit(mqtt_store.source());
  gate.recordComPrefsRewrite();

  EXPECT_TRUE(com_prefs.mqtt_tail_present_before_compaction);
  EXPECT_EQ(1, com_prefs.compact_calls);
  EXPECT_EQ((std::vector<uint8_t>{'c', 'o', 'm', 'p', 'a', 'c', 't'}), com_prefs.bytes);
  EXPECT_FALSE(gate.mayRewriteComPrefs());
}

TEST(MQTTPrefsAtomicStore, LegacyCrossFilePowerCutPreservesBothSources) {
  const std::vector<uint8_t> legacy_mqtt = {'o', 'l', 'd', '-', 'p', 'r', 'e', 'f', 's'};
  const std::vector<uint8_t> legacy_com = {'l', 'e', 'g', 'a', 'c', 'y', '-', 'c', 'o', 'm'};
  for (const FailurePoint point : {FailurePoint::Begin, FailurePoint::HeaderWrite,
                                   FailurePoint::PayloadWrite, FailurePoint::Finish,
                                   FailurePoint::Commit}) {
    InMemoryStore mqtt_store(point);
    LegacyComPrefs com_prefs;
    AtomicStore::LegacyUpgradeGate gate(true);
    gate.requireMqttRewrite();

    const AtomicStore::Result result = runWithObserverTail(&mqtt_store);
    gate.recordMqttSave(AtomicStore::committed(result));
    if (gate.mayRewriteComPrefs()) {
      com_prefs.compactAfterMqttCommit(mqtt_store.source());
      gate.recordComPrefsRewrite();
    }

    EXPECT_FALSE(AtomicStore::committed(result));
    EXPECT_EQ(legacy_mqtt, mqtt_store.source());
    EXPECT_EQ(legacy_com, com_prefs.bytes);
    EXPECT_EQ(0, com_prefs.compact_calls);
    EXPECT_TRUE(gate.blocksComPrefsRewrite());
  }
}

TEST(MQTTPrefsAtomicStore, LegacyNodePrefsMigrationWaitsForObserverTailCommit) {
  InMemoryStore mqtt_store(FailurePoint::None);
  LegacyNodePrefs node_prefs;
  AtomicStore::LegacyUpgradeGate gate(true);
  gate.requireMqttRewrite();

  const AtomicStore::Result result = runWithObserverTail(&mqtt_store);
  gate.recordMqttSave(AtomicStore::committed(result));
  ASSERT_TRUE(gate.mayRewriteComPrefs());

  node_prefs.migrateAfterMqttCommit(mqtt_store.source());
  gate.recordComPrefsRewrite();

  EXPECT_TRUE(node_prefs.mqtt_tail_present_before_removal);
  EXPECT_EQ(1, node_prefs.migration_calls);
  EXPECT_TRUE(node_prefs.bytes.empty());
}

TEST(MQTTPrefsAtomicStore, LegacyNodePrefsPowerCutPreservesSource) {
  const std::vector<uint8_t> legacy_mqtt = {'o', 'l', 'd', '-', 'p', 'r', 'e', 'f', 's'};
  const std::vector<uint8_t> legacy_node = {
      'l', 'e', 'g', 'a', 'c', 'y', '-', 'n', 'o', 'd', 'e'};
  for (const FailurePoint point : {FailurePoint::Begin, FailurePoint::HeaderWrite,
                                   FailurePoint::PayloadWrite, FailurePoint::Finish,
                                   FailurePoint::Commit}) {
    InMemoryStore mqtt_store(point);
    LegacyNodePrefs node_prefs;
    AtomicStore::LegacyUpgradeGate gate(true);
    gate.requireMqttRewrite();

    const AtomicStore::Result result = runWithObserverTail(&mqtt_store);
    gate.recordMqttSave(AtomicStore::committed(result));
    if (gate.mayRewriteComPrefs()) {
      node_prefs.migrateAfterMqttCommit(mqtt_store.source());
      gate.recordComPrefsRewrite();
    }

    EXPECT_FALSE(AtomicStore::committed(result));
    EXPECT_EQ(legacy_mqtt, mqtt_store.source());
    EXPECT_EQ(legacy_node, node_prefs.bytes);
    EXPECT_EQ(0, node_prefs.migration_calls);
    EXPECT_TRUE(gate.blocksComPrefsRewrite());
  }
}

TEST(MQTTPrefsAtomicStore, NodePrefsMigrationPublishesComPrefsBeforeRemovingSource) {
  InMemoryCommonPrefsStore store(FailurePoint::None);

  ASSERT_EQ(AtomicStore::ImageResult::Committed, runCommonPrefsImage(&store));
  EXPECT_TRUE(store.nodeSourceExists());  // caller removes it only after commit
  EXPECT_EQ((std::vector<uint8_t>{'c', 'o', 'm', '-', 'p', 'r', 'e', 'f', 's', 0x19, 0xa4, 0x7e}),
            store.destination());
  EXPECT_FALSE(store.tempExists());

  store.removeNodeSource();
  EXPECT_FALSE(store.nodeSourceExists());
  EXPECT_TRUE(store.destinationExists());
}

TEST(MQTTPrefsAtomicStore, NodePrefsMigrationFailurePreservesSourceAndNeverPrefersPartialDestination) {
  const std::vector<uint8_t> legacy_node = {
      'l', 'e', 'g', 'a', 'c', 'y', '-', 'n', 'o', 'd', 'e'};
  const struct {
    FailurePoint point;
    AtomicStore::ImageResult expected;
    int writes;
    int finishes;
    int commits;
  } cases[] = {
      {FailurePoint::Begin, AtomicStore::ImageResult::BeginFailed, 0, 0, 0},
      {FailurePoint::ImageWrite, AtomicStore::ImageResult::WriteFailed, 2, 0, 0},
      {FailurePoint::Finish, AtomicStore::ImageResult::FinishFailed, 2, 1, 0},
      {FailurePoint::Commit, AtomicStore::ImageResult::CommitFailed, 2, 1, 1},
  };

  for (const auto& test_case : cases) {
    InMemoryCommonPrefsStore store(test_case.point);
    EXPECT_EQ(test_case.expected, runCommonPrefsImage(&store));
    EXPECT_EQ(legacy_node, store.nodeSource());
    EXPECT_TRUE(store.nodeSourceIsPreferred());
    EXPECT_FALSE(store.destinationExists());
    EXPECT_FALSE(store.tempExists());
    EXPECT_EQ(1, store.begin_calls);
    EXPECT_EQ(test_case.writes, store.write_calls);
    EXPECT_EQ(test_case.finishes, store.finish_calls);
    EXPECT_EQ(test_case.commits, store.commit_calls);
    EXPECT_EQ(1, store.abort_calls);
  }
}

TEST(MQTTPrefsAtomicStore, SpiffsPowerCutsAtEveryPublishBoundaryLeaveRecoverableImage) {
  const struct {
    SpiffsMqttTransaction::Boundary boundary;
    std::vector<uint8_t> expected_after_reboot;
  } cases[] = {
      // Temp has not become the committed image yet, so the old primary wins.
      {SpiffsMqttTransaction::Boundary::BeforeBackupRename, SpiffsMqttTransaction::oldImage()},
      // Old primary is .bak and verified new temp wins the recovery race.
      {SpiffsMqttTransaction::Boundary::AfterBackupRename, SpiffsMqttTransaction::newImage()},
      {SpiffsMqttTransaction::Boundary::AfterPrimaryRename, SpiffsMqttTransaction::newImage()},
      {SpiffsMqttTransaction::Boundary::AfterBackupCleanup, SpiffsMqttTransaction::newImage()},
  };

  for (const auto& test_case : cases) {
    SpiffsMqttTransaction store;
    store.cutAt(test_case.boundary);
    store.recover();
    ASSERT_TRUE(store.has("/mqtt_prefs"));
    EXPECT_EQ(test_case.expected_after_reboot, store.primary());
    EXPECT_FALSE(store.has("/mqtt_prefs.tmp"));
    EXPECT_FALSE(store.has("/mqtt_prefs.bak"));
  }
}

TEST(MQTTPrefsAtomicStore, PowerCutDuringTempWriteKeepsPrimaryAndAllowsNextSave) {
  SpiffsMqttTransaction store;
  store.cutDuringTempWrite();

  // The partial temp is opaque to the codec, but the existing primary is the
  // only committed image. Recovery discards the incomplete transaction rather
  // than blocking every later config save behind /mqtt_prefs.tmp.
  store.recover(Recovery::FileState::Usable, Recovery::FileState::Preserve);
  EXPECT_EQ(SpiffsMqttTransaction::oldImage(), store.primary());
  EXPECT_FALSE(store.has("/mqtt_prefs.tmp"));
  EXPECT_TRUE(store.canStartSave());
}

TEST(MQTTPrefsAtomicStore, TornFirstMigrationTempIsDiscardedSoLegacyCanRetry) {
  SpiffsMqttTransaction store;
  store.removePrimary();
  store.cutDuringTempWrite();

  EXPECT_EQ(Recovery::Action::DiscardTemp,
            Recovery::select(Recovery::FileState::Missing,
                             Recovery::FileState::Preserve,
                             Recovery::FileState::Missing));
  store.recover(Recovery::FileState::Missing,
                Recovery::FileState::Preserve,
                Recovery::FileState::Missing);
  EXPECT_FALSE(store.has("/mqtt_prefs"));
  EXPECT_FALSE(store.has("/mqtt_prefs.tmp"));
  EXPECT_TRUE(store.canStartSave());
}

TEST(MQTTPrefsAtomicStore, RecoveredUsablePrimaryClearsOpaqueTransactionArtifacts) {
  {
    SpiffsMqttTransaction store;
    store.cutAt(SpiffsMqttTransaction::Boundary::AfterBackupRename);
    // A current-format temp wins; the old backup need not be decodable to be
    // stale once that usable temp owns the primary name.
    store.recover(Recovery::FileState::Usable, Recovery::FileState::Usable,
                  Recovery::FileState::Preserve);
    EXPECT_EQ(SpiffsMqttTransaction::newImage(), store.primary());
    EXPECT_TRUE(store.canStartSave());
  }
  {
    SpiffsMqttTransaction store;
    store.cutAt(SpiffsMqttTransaction::Boundary::AfterBackupRename);
    // Conversely, when the usable backup becomes primary, an opaque temp was
    // never published and must not leave saves permanently blocked.
    store.recover(Recovery::FileState::Usable, Recovery::FileState::Preserve,
                  Recovery::FileState::Usable);
    EXPECT_EQ(SpiffsMqttTransaction::oldImage(), store.primary());
    EXPECT_TRUE(store.canStartSave());
  }
}

TEST(MQTTPrefsAtomicStore, SpiffsRenameAndCleanupFailuresRemainRecoverable) {
  {
    SpiffsMqttTransaction store;
    EXPECT_FALSE(store.publish(true, false, false));
    store.recover();
    EXPECT_EQ(SpiffsMqttTransaction::oldImage(), store.primary());
    EXPECT_FALSE(store.has("/mqtt_prefs.tmp"));
    EXPECT_FALSE(store.has("/mqtt_prefs.bak"));
  }
  {
    SpiffsMqttTransaction store;
    EXPECT_FALSE(store.publish(false, true, false));
    EXPECT_FALSE(store.has("/mqtt_prefs"));
    EXPECT_TRUE(store.has("/mqtt_prefs.tmp"));
    EXPECT_TRUE(store.has("/mqtt_prefs.bak"));
    // Unlike a power cut at this boundary, a returned failure is answered to
    // the caller, so the transaction is undone before recovery ever sees it.
    EXPECT_TRUE(store.rollbackFailedPublish());
    store.recover();
    EXPECT_EQ(SpiffsMqttTransaction::oldImage(), store.primary());
    EXPECT_FALSE(store.has("/mqtt_prefs.tmp"));
    EXPECT_FALSE(store.has("/mqtt_prefs.bak"));
  }
  {
    SpiffsMqttTransaction store;
    EXPECT_TRUE(store.publish(false, false, true));
    EXPECT_EQ(SpiffsMqttTransaction::newImage(), store.primary());
    EXPECT_TRUE(store.has("/mqtt_prefs.bak"));
    store.recover();
    EXPECT_FALSE(store.has("/mqtt_prefs.bak"));
  }
}

TEST(MQTTPrefsAtomicStore, RecoveryNeverOverwritesOpaqueNewerLayout) {
  // An unreadable primary owns its name, even if an older usable backup and a
  // verified temp exist. This is the downgrade-preservation invariant.
  EXPECT_EQ(Recovery::Action::KeepPrimary,
            Recovery::select(Recovery::FileState::Preserve, Recovery::FileState::Usable,
                             Recovery::FileState::Usable));
  // A syntactically valid future temp may already have passed verification and
  // reached the rename phase. It wins over the stale supported backup and is
  // promoted into the authoritative name, where older firmware will hold it.
  EXPECT_EQ(Recovery::Action::PromoteTemp,
            Recovery::select(Recovery::FileState::Missing, Recovery::FileState::FutureUsable,
                             Recovery::FileState::Usable));
  // A corrupt or incomplete temp is different: the supported backup remains
  // the last known committed image.
  EXPECT_EQ(Recovery::Action::PromoteBackup,
            Recovery::select(Recovery::FileState::Missing, Recovery::FileState::Preserve,
                             Recovery::FileState::Usable));
  // With no other image, an opaque backup is renamed into the empty primary
  // name so CommonCLI will hold it rather than silently replace it with defaults.
  EXPECT_EQ(Recovery::Action::PromoteBackup,
            Recovery::select(Recovery::FileState::Missing, Recovery::FileState::Missing,
                             Recovery::FileState::Preserve));
}

TEST(MQTTPrefsAtomicStore, AmbiguousFutureOrOomTempIsNeverDeleted) {
  for (const Recovery::FileState uncertain : {
           Recovery::FileState::FutureClaimed,
           Recovery::FileState::Indeterminate}) {
    SpiffsMqttTransaction store;
    store.cutAt(SpiffsMqttTransaction::Boundary::AfterBackupRename);
    store.recover(Recovery::FileState::Missing, uncertain,
                  Recovery::FileState::Usable);

    // The supported backup runs this boot from its own name. Nothing is
    // renamed, so the empty primary name still records that the candidate had
    // reached the publish phase. Its presence also blocks a new transaction.
    EXPECT_FALSE(store.has("/mqtt_prefs"));
    EXPECT_TRUE(store.has("/mqtt_prefs.tmp"));
    EXPECT_TRUE(store.has("/mqtt_prefs.bak"));
    EXPECT_FALSE(store.canStartSave());
  }
}

TEST(MQTTPrefsAtomicStore, PreservedCandidateIsPromotedByTheBootThatCanReadIt) {
  // The whole point of keeping an uncertain temp is that a later boot can act
  // on it. Publishing the backup on the first boot would defeat that: the
  // candidate would then look like a stale artifact next to a usable primary,
  // and get deleted exactly when it finally became readable.
  const struct {
    Recovery::FileState first_boot;
    Recovery::FileState second_boot;
    // A promoted current-format image ends the transaction, so its backup goes
    // too. A future-format one keeps the last readable image and stays held.
    bool clears_backup;
  } cases[] = {
      // Classification scratch could not be allocated, then heap recovered.
      {Recovery::FileState::Indeterminate, Recovery::FileState::Usable, true},
      // Downgraded firmware could not parse it, then the node rolled forward.
      {Recovery::FileState::FutureClaimed, Recovery::FileState::Usable, true},
      // Still a newer schema on the second boot, but now classifiable.
      {Recovery::FileState::FutureClaimed, Recovery::FileState::FutureUsable, false},
  };

  for (const auto& test_case : cases) {
    SpiffsMqttTransaction store;
    store.cutAt(SpiffsMqttTransaction::Boundary::AfterBackupRename);

    store.recover(Recovery::FileState::Missing, test_case.first_boot,
                  Recovery::FileState::Usable);
    ASSERT_TRUE(store.has("/mqtt_prefs.tmp"));
    ASSERT_FALSE(store.has("/mqtt_prefs"));

    store.recover(Recovery::FileState::Missing, test_case.second_boot,
                  Recovery::FileState::Usable);
    EXPECT_EQ(SpiffsMqttTransaction::newImage(), store.primary());
    EXPECT_FALSE(store.has("/mqtt_prefs.tmp"));
    EXPECT_EQ(test_case.clears_backup, !store.has("/mqtt_prefs.bak"));
    EXPECT_EQ(test_case.clears_backup, store.canStartSave());
  }
}

TEST(MQTTPrefsAtomicStore, CandidateThatProvesInvalidYieldsToTheHeldBackup) {
  SpiffsMqttTransaction store;
  store.cutAt(SpiffsMqttTransaction::Boundary::AfterBackupRename);
  store.recover(Recovery::FileState::Missing, Recovery::FileState::Indeterminate,
                Recovery::FileState::Usable);

  // The second boot can classify it and finds it definitively corrupt, so the
  // last committed image is published and the candidate is dropped. The node
  // leaves the held state without ever having discarded an unread candidate.
  store.recover(Recovery::FileState::Missing, Recovery::FileState::Preserve,
                Recovery::FileState::Usable);
  EXPECT_EQ(SpiffsMqttTransaction::oldImage(), store.primary());
  EXPECT_FALSE(store.has("/mqtt_prefs.tmp"));
  EXPECT_TRUE(store.canStartSave());
}

TEST(MQTTPrefsAtomicStore, UncertainTempWithNoUsableBackupStillOwnsThePrimaryName) {
  // With no image that can run this boot, the candidate is the only thing left
  // to protect, so it takes the authoritative name and CommonCLI holds it.
  EXPECT_EQ(Recovery::Action::PromoteTemp,
            Recovery::select(Recovery::FileState::Missing,
                             Recovery::FileState::Indeterminate,
                             Recovery::FileState::Missing));
  EXPECT_EQ(Recovery::Action::PromoteTemp,
            Recovery::select(Recovery::FileState::Missing,
                             Recovery::FileState::FutureClaimed,
                             Recovery::FileState::Preserve));
}

TEST(MQTTPrefsAtomicStore, UsablePrimaryDoesNotCleanIndeterminateArtifact) {
  SpiffsMqttTransaction store;
  store.cutDuringTempWrite();
  store.recover(Recovery::FileState::Usable,
                Recovery::FileState::Indeterminate);
  EXPECT_EQ(SpiffsMqttTransaction::oldImage(), store.primary());
  EXPECT_TRUE(store.has("/mqtt_prefs.tmp"));
  EXPECT_FALSE(store.canStartSave());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
