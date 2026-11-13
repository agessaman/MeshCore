// ObserverConfig.cpp — /mqtt.json load, recovery and atomic save. Moved verbatim
// from CommonCLI.cpp so every observer host (repeater, room server, companion)
// shares one file format and one recovery policy.
#include <Arduino.h>
#include "ObserverConfig.h"

#ifdef WITH_MQTT_BRIDGE
#include "MQTTDefaults.h"
#include "MQTTPrefsAtomicStore.h"
#include "MQTTPrefsCodec.h"
#include "MQTTPrefsRecovery.h"
#include "MQTTPrefsSerializer.h"
#include <new>
#endif

#ifdef WITH_MQTT_BRIDGE
static const uint32_t MQTT_JSON_FNV1A_OFFSET_BASIS = 2166136261u;
static const uint32_t MQTT_JSON_FNV1A_PRIME = 16777619u;

// Set default values for MQTT preferences (used when file doesn't exist or is corrupted)
static void setMQTTPrefsDefaults(MQTTPrefs* prefs) {
  applyMQTTDefaults(prefs);
}

static File openMqttPrefsRead(FILESYSTEM* fs, const char* path = "/mqtt_prefs") {
#if defined(RP2040_PLATFORM)
  return fs->open(path, "r");
#else
  return fs->open(path);
#endif
}

enum class JsonPrefsLoadResult : uint8_t {
  Loaded,
  LoadedWithRepairs,
  UnsupportedVersion,
  FutureClaimed,
  Invalid,
  NoMemory,
};

static bool jsonPrefsLoaded(JsonPrefsLoadResult result) {
  return result == JsonPrefsLoadResult::Loaded ||
         result == JsonPrefsLoadResult::LoadedWithRepairs;
}

static JsonPrefsLoadResult loadMqttJsonFile(FILESYSTEM* fs, const char* path,
                                             MQTTPrefs* output) {
  if (output == nullptr) return JsonPrefsLoadResult::Invalid;
  applyMQTTDefaults(output);

  // Probe the root version without applying the v1 schema. A future version
  // may legitimately change an existing field's type, and must still be held
  // opaquely rather than misclassified as corrupt and rolled back.
  File version_file = openMqttPrefsRead(fs, path);
  if (!version_file) return JsonPrefsLoadResult::Invalid;
  MQTTPrefsVersionProbe version_probe;
  const bool version_parsed = version_probe.loadSerial(version_file);
  version_file.close();
  if (version_probe.hasFutureVersion()) {
    return version_parsed ? JsonPrefsLoadResult::UnsupportedVersion
                          : JsonPrefsLoadResult::FutureClaimed;
  }

  File file = openMqttPrefsRead(fs, path);
  if (!file) return JsonPrefsLoadResult::Invalid;
  MQTTPrefsSerializer serializer(output);
  const bool parsed = serializer.loadSerial(file);
  file.close();
  if (!parsed) return JsonPrefsLoadResult::Invalid;
  if (serializer.hasFutureVersion()) return JsonPrefsLoadResult::UnsupportedVersion;
  bool repaired = false;
  if (!serializer.apply(&repaired)) return JsonPrefsLoadResult::Invalid;
  return repaired ? JsonPrefsLoadResult::LoadedWithRepairs : JsonPrefsLoadResult::Loaded;
}

// Parse `path` through a heap scratch object and publish it over `dest` only
// once the whole file is known good, so a late failure cannot leave the live
// preferences half-loaded.
static JsonPrefsLoadResult adoptMqttJsonFile(FILESYSTEM* fs, const char* path,
                                              MQTTPrefs* dest) {
  MQTTPrefs* scratch = new (std::nothrow) MQTTPrefs;
  if (scratch == nullptr) return JsonPrefsLoadResult::NoMemory;
  const JsonPrefsLoadResult result = loadMqttJsonFile(fs, path, scratch);
  if (jsonPrefsLoaded(result)) memcpy(dest, scratch, sizeof(*dest));
  delete scratch;
  return result;
}

static MQTTPrefsRecovery::FileState mqttJsonFileState(FILESYSTEM* fs, const char* path) {
  if (!fs->exists(path)) return MQTTPrefsRecovery::FileState::Missing;
  MQTTPrefs* scratch = new (std::nothrow) MQTTPrefs;
  if (scratch == nullptr) return MQTTPrefsRecovery::FileState::Indeterminate;
  const JsonPrefsLoadResult result = loadMqttJsonFile(fs, path, scratch);
  delete scratch;
  if (result == JsonPrefsLoadResult::UnsupportedVersion) {
    // Syntax and the mandatory version field were valid, so this can be a
    // fully verified transaction written by newer firmware. Keep it distinct
    // from a torn/corrupt temp during recovery.
    return MQTTPrefsRecovery::FileState::FutureUsable;
  }
  if (result == JsonPrefsLoadResult::FutureClaimed) {
    return MQTTPrefsRecovery::FileState::FutureClaimed;
  }
  return result == JsonPrefsLoadResult::Loaded ||
         result == JsonPrefsLoadResult::LoadedWithRepairs
      ? MQTTPrefsRecovery::FileState::Usable
      : MQTTPrefsRecovery::FileState::Preserve;
}

// hold: observer writes must not replace whatever was left on disk.
// run_from_backup: nothing was renamed and /mqtt.json does not exist; this boot
// reads the last committed image straight out of /mqtt.json.bak.
struct MqttJsonRecovery {
  bool hold;
  bool run_from_backup;
};

static MqttJsonRecovery recoverMqttJsonFiles(FILESYSTEM* fs) {
  const MQTTPrefsRecovery::FileState primary = mqttJsonFileState(fs, "/mqtt.json");
  const MQTTPrefsRecovery::FileState temp = mqttJsonFileState(fs, "/mqtt.json.tmp");
  const MQTTPrefsRecovery::FileState backup = mqttJsonFileState(fs, "/mqtt.json.bak");
  const MQTTPrefsRecovery::Action action = MQTTPrefsRecovery::select(primary, temp, backup);

  if (action == MQTTPrefsRecovery::Action::KeepPrimary) {
    if (primary == MQTTPrefsRecovery::FileState::Usable) {
      if (!MQTTPrefsRecovery::uncertain(temp) &&
          temp != MQTTPrefsRecovery::FileState::Missing) {
        fs->remove("/mqtt.json.tmp");
      }
      if (!MQTTPrefsRecovery::uncertain(backup) &&
          backup != MQTTPrefsRecovery::FileState::Missing) {
        fs->remove("/mqtt.json.bak");
      }
    }
    return {MQTTPrefsRecovery::uncertain(primary) ||
                MQTTPrefsRecovery::uncertain(temp) ||
                MQTTPrefsRecovery::uncertain(backup),
            false};
  }
  if (action == MQTTPrefsRecovery::Action::DiscardTemp) {
    if (fs->remove("/mqtt.json.tmp")) {
      MESH_DEBUG_PRINTLN("MQTT: discarded incomplete first-migration JSON temp");
      return {false, false};
    }
    MESH_DEBUG_PRINTLN("MQTT: could not discard incomplete /mqtt.json temp; source held");
    return {true, false};
  }
  if (action == MQTTPrefsRecovery::Action::PromoteTemp) {
    if (fs->rename("/mqtt.json.tmp", "/mqtt.json")) {
      if (temp == MQTTPrefsRecovery::FileState::Usable &&
          backup != MQTTPrefsRecovery::FileState::Missing) {
        fs->remove("/mqtt.json.bak");
      }
      MESH_DEBUG_PRINTLN("MQTT: recovered /mqtt.json from transaction temp");
      return {MQTTPrefsRecovery::uncertain(temp) ||
                  MQTTPrefsRecovery::uncertain(backup),
              false};
    }
    MESH_DEBUG_PRINTLN("MQTT: could not recover /mqtt.json temp; files preserved");
    return {true, false};
  }
  if (action == MQTTPrefsRecovery::Action::UseBackupHeld) {
    // Deliberately rename nothing. The empty primary name is what records that
    // the interrupted commit had already moved the old image aside, and a boot
    // that cannot classify the candidate must not spend that record: a later
    // boot with more heap, or firmware that understands the candidate, promotes
    // it through the ordinary rule instead of deleting it as a stale artifact.
    MESH_DEBUG_PRINTLN("MQTT: unresolved /mqtt.json candidate; running the backup in place and holding writes");
    return {true, true};
  }
  if (action == MQTTPrefsRecovery::Action::RunDefaultsHeld) {
    // Same reasoning, with no image this firmware can run: renaming either file
    // would spend transaction state that a later boot still needs.
    MESH_DEBUG_PRINTLN("MQTT: unresolved /mqtt.json candidate and no runnable backup; "
                       "using defaults (files preserved)");
    return {true, false};
  }
  if (action == MQTTPrefsRecovery::Action::PromoteBackup) {
    if (fs->rename("/mqtt.json.bak", "/mqtt.json")) {
      if (backup == MQTTPrefsRecovery::FileState::Usable &&
          !MQTTPrefsRecovery::uncertain(temp) &&
          temp != MQTTPrefsRecovery::FileState::Missing) {
        fs->remove("/mqtt.json.tmp");
      }
      MESH_DEBUG_PRINTLN("MQTT: recovered /mqtt.json from transaction backup");
      return {MQTTPrefsRecovery::uncertain(temp) ||
                  MQTTPrefsRecovery::uncertain(backup),
              false};
    }
    MESH_DEBUG_PRINTLN("MQTT: could not recover /mqtt.json backup; files preserved");
    return {true, false};
  }
  return {false, false};
}

static MQTTPrefsRecovery::FileState mqttPrefsFileState(FILESYSTEM* fs, const char* path) {
  if (!fs->exists(path)) return MQTTPrefsRecovery::FileState::Missing;
  File file = openMqttPrefsRead(fs, path);
  if (!file) return MQTTPrefsRecovery::FileState::Preserve;
  const size_t file_size = file.size();
  uint8_t prefix[sizeof(MQTTPrefsHeader)] = {};
  const size_t prefix_size = file_size < sizeof(prefix) ? file_size : sizeof(prefix);
  const size_t prefix_read = file.read(prefix, prefix_size);
  file.close();
  return MQTTPrefsCodec::classify(prefix, prefix_read, file_size).preserve_file
      ? MQTTPrefsRecovery::FileState::Preserve
      : MQTTPrefsRecovery::FileState::Usable;
}

// Restore the only usable image before the normal loader inspects /mqtt_prefs.
// SPIFFS cannot rename over an existing destination, so publishing moves the
// old primary to .bak before moving the verified temp into the empty name.
// The decision helper deliberately treats unsupported/corrupt files as opaque:
// no recovery path overwrites one with an older layout.
static bool recoverMqttPrefsFiles(FILESYSTEM* fs) {
  const MQTTPrefsRecovery::FileState primary = mqttPrefsFileState(fs, "/mqtt_prefs");
  const MQTTPrefsRecovery::FileState temp = mqttPrefsFileState(fs, "/mqtt_prefs.tmp");
  const MQTTPrefsRecovery::FileState backup = mqttPrefsFileState(fs, "/mqtt_prefs.bak");
  const MQTTPrefsRecovery::Action action = MQTTPrefsRecovery::select(primary, temp, backup);

  if (action == MQTTPrefsRecovery::Action::KeepPrimary) {
    // A current/known legacy primary has already published. Every transaction
    // artifact is therefore unpublished or stale, including a partial temp
    // left by a reset during write(), and can be discarded. Preserve artifacts
    // only when the primary itself is opaque (the branch above still keeps it).
    if (primary == MQTTPrefsRecovery::FileState::Usable) {
      if (temp != MQTTPrefsRecovery::FileState::Missing) fs->remove("/mqtt_prefs.tmp");
      if (backup != MQTTPrefsRecovery::FileState::Missing) fs->remove("/mqtt_prefs.bak");
    }
    return false;
  }
  if (action == MQTTPrefsRecovery::Action::DiscardTemp) {
    if (fs->remove("/mqtt_prefs.tmp")) {
      MESH_DEBUG_PRINTLN("MQTT: discarded incomplete legacy transaction temp");
      return false;
    }
    MESH_DEBUG_PRINTLN("MQTT: could not discard incomplete /mqtt_prefs temp; source held");
    return true;
  }
  if (action == MQTTPrefsRecovery::Action::PromoteTemp) {
    if (fs->rename("/mqtt_prefs.tmp", "/mqtt_prefs")) {
      // A usable temp is now the committed primary. Its backup is necessarily
      // a stale transaction artifact, even if this firmware cannot decode it.
      if (temp == MQTTPrefsRecovery::FileState::Usable &&
          backup != MQTTPrefsRecovery::FileState::Missing) {
        fs->remove("/mqtt_prefs.bak");
      }
      MESH_DEBUG_PRINTLN("MQTT: recovered /mqtt_prefs from transaction temp");
      return false;
    }
    MESH_DEBUG_PRINTLN("MQTT: could not recover /mqtt_prefs temp; files preserved");
    return true;
  }
  if (action == MQTTPrefsRecovery::Action::UseBackupHeld ||
      action == MQTTPrefsRecovery::Action::RunDefaultsHeld) {
    // Unreachable for the binary format, whose classifier never reports an
    // uncertain state. Hold rather than fall through to "nothing to do" if a
    // later classifier change makes it reachable.
    return true;
  }
  if (action == MQTTPrefsRecovery::Action::PromoteBackup) {
    if (fs->rename("/mqtt_prefs.bak", "/mqtt_prefs")) {
      // Symmetric case: a usable backup is now primary, so any interrupted
      // temp is no longer authoritative and must not block a later save.
      if (backup == MQTTPrefsRecovery::FileState::Usable &&
          temp != MQTTPrefsRecovery::FileState::Missing) {
        fs->remove("/mqtt_prefs.tmp");
      }
      MESH_DEBUG_PRINTLN("MQTT: recovered /mqtt_prefs from transaction backup");
      return false;
    }
    MESH_DEBUG_PRINTLN("MQTT: could not recover /mqtt_prefs backup; files preserved");
    return true;
  }
  return false;
}

// Filesystem adapter for the ConfigSerializer image. It writes to
// /mqtt.json.tmp and verifies its size and checksum. Publishing is a recoverable SPIFFS
// transaction: primary -> .bak, then tmp -> primary, then best-effort backup
// cleanup. A power loss at every boundary leaves at least one recoverable file.
class MQTTPrefsJsonFileStore {
public:
  explicit MQTTPrefsJsonFileStore(FILESYSTEM* fs) : _fs(fs) {}

  bool begin() {
    _finished = false;
    _open = false;
    _owns_temp = false;
    _owns_backup = false;
    _bytes_written = 0;
    _expected_crc = MQTT_JSON_FNV1A_OFFSET_BASIS;
    // Recovery owns stale artifacts. Do not delete them here: a power cut, or a
    // failed commit that could not be rolled back, may have moved the old
    // primary to .bak and left a verified temp that the next boot must choose
    // between. Refusing the save is safer than erasing an image this firmware
    // cannot decode.
    if (_fs->exists("/mqtt.json.tmp") || _fs->exists("/mqtt.json.bak")) return false;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
    _file = _fs->open("/mqtt.json.tmp", FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
    _file = _fs->open("/mqtt.json.tmp", "w");
#else
    _file = _fs->open("/mqtt.json.tmp", "w", true);
#endif
    _open = _file;
    _owns_temp = _open;
    return _open;
  }

  size_t write(const uint8_t* bytes, size_t size) {
    if (!_open) return 0;
    const size_t written = _file.write(bytes, size);
    for (size_t i = 0; i < written; ++i) {
      _expected_crc = (_expected_crc ^ bytes[i]) * MQTT_JSON_FNV1A_PRIME;
    }
    _bytes_written += written;
    return written;
  }

  bool finish() {
    if (!_open) return false;
    _file.close();
    _open = false;
#if defined(RP2040_PLATFORM)
    File verify = _fs->open("/mqtt.json.tmp", "r");
#else
    File verify = _fs->open("/mqtt.json.tmp");
#endif
    if (!verify) return false;
    uint32_t actual_crc = MQTT_JSON_FNV1A_OFFSET_BASIS;
    size_t actual_size = 0;
    bool read_failed = false;
    uint8_t buf[64];
    while (verify.available() > 0) {
      // Arduino File implementations normally return a byte count, but some
      // Stream implementations use -1 for a read error. Keep that sentinel
      // signed so it cannot become a huge size_t and overrun this buffer.
      const int count = static_cast<int>(verify.read(buf, sizeof(buf)));
      if (count <= 0) {
        read_failed = count < 0;
        break;
      }
      actual_size += static_cast<size_t>(count);
      for (int i = 0; i < count; ++i) {
        actual_crc = (actual_crc ^ buf[i]) * MQTT_JSON_FNV1A_PRIME;
      }
    }
    const bool complete = !read_failed &&
                          verify.size() == _bytes_written &&
                          actual_size == _bytes_written &&
                          actual_crc == _expected_crc;
    verify.close();
    if (!complete) return false;
    _finished = true;
    return true;
  }

  bool commit() {
    if (!_finished) return false;
    // SPIFFS refuses rename(tmp, existing_dest). Move the existing image to a
    // recoverable backup first, then publish temp into the now-empty primary.
    // A power cut at either boundary is resolved by boot recovery; a failure
    // that returns here is undone by rollbackFailedCommit().
    if (_fs->exists("/mqtt.json.bak")) return false;
    if (_fs->exists("/mqtt.json")) {
      if (!_fs->rename("/mqtt.json", "/mqtt.json.bak")) return false;
      _owns_backup = true;
    }
    if (!_fs->rename("/mqtt.json.tmp", "/mqtt.json")) return false;
    // Cleanup failure is non-fatal: the new primary is published and recovery
    // will remove a known-good stale backup on a later boot.
    if (_fs->exists("/mqtt.json.bak")) _fs->remove("/mqtt.json.bak");
    return true;
  }

  // Undo a commit that failed midway. Without this, a failed publish can leave
  // the old primary parked in .bak and the verified temp still holding the new
  // image, which boot recovery then promotes — so a setter that told the
  // operator the change was rolled back would be wrong after the next reset.
  //
  // Republishing the backup is the operation that matters: once the primary
  // name is occupied, recovery keeps it and treats the temp as stale, so the
  // temp removal below is only housekeeping. Returns false when the
  // pre-transaction state could not be restored and the outcome of the next
  // boot is therefore uncertain.
  bool rollbackFailedCommit() {
    // Only ever restore the backup this transaction made. Anything else under
    // that name predates the transaction and is recovery's to resolve.
    if (_owns_backup && !_fs->exists("/mqtt.json") &&
        !_fs->rename("/mqtt.json.bak", "/mqtt.json")) {
      return false;
    }
    _owns_backup = false;
    if (_owns_temp && _fs->exists("/mqtt.json.tmp") && !_fs->remove("/mqtt.json.tmp")) {
      // A verified temp still outranks a missing primary during recovery, so
      // this is only harmless if the primary name is occupied again.
      return _fs->exists("/mqtt.json");
    }
    return true;
  }

  // Drop a temp that was written and byte-verified but then rejected. Returns
  // false when it is still on flash and no primary outranks it: recovery
  // promotes a lone temp, so the caller cannot claim the change was undone.
  bool discardFinishedTemp() {
    if (_open) _file.close();
    _open = false;
    bool removed = true;
    if (_owns_temp && _fs->exists("/mqtt.json.tmp")) {
      removed = _fs->remove("/mqtt.json.tmp");
    }
    _finished = false;
    _owns_temp = false;
    return removed || _fs->exists("/mqtt.json");
  }

  // Same disposition contract as discardFinishedTemp(). Only unfinished staging
  // is disposable here: once finish() has verified the temp,
  // rollbackFailedCommit() has already decided its fate, and a temp that
  // survived that is one recovery must resolve after reset.
  bool abort() {
    if (_open) _file.close();
    _open = false;
    bool removed = true;
    if (_owns_temp && !_finished && _fs->exists("/mqtt.json.tmp")) {
      removed = _fs->remove("/mqtt.json.tmp");
    }
    _finished = false;
    _owns_temp = false;
    _owns_backup = false;
    return removed || _fs->exists("/mqtt.json");
  }

private:
  FILESYSTEM* _fs;
  File _file;
  bool _open = false;
  bool _finished = false;
  bool _owns_temp = false;
  // This transaction moved the old primary to /mqtt.json.bak, so a failed
  // publish may put it back. Never true for a backup it did not create.
  bool _owns_backup = false;
  size_t _bytes_written = 0;
  uint32_t _expected_crc = MQTT_JSON_FNV1A_OFFSET_BASIS;
};

class MQTTPrefsStoreStream : public Stream {
public:
  explicit MQTTPrefsStoreStream(MQTTPrefsJsonFileStore* store) : _store(store) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* buffer, size_t size) override {
    if (!_ok || _store == nullptr) return 0;
    const size_t written = _store->write(buffer, size);
    if (written != size) _ok = false;
    return written;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  bool ok() const { return _ok; }

private:
  MQTTPrefsJsonFileStore* _store;
  bool _ok = true;
};

#endif  // WITH_MQTT_BRIDGE

#ifdef WITH_MQTT_BRIDGE

void ObserverConfig::loadMQTTPrefs(
    FILESYSTEM* fs, MQTTPrefsAtomicStore::LegacyUpgradeGate* legacy_upgrade) {
  setMQTTPrefsDefaults(&_mqtt_prefs);
  const MqttJsonRecovery recovery = recoverMqttJsonFiles(fs);
  _mqtt_prefs_hold = recovery.hold;

  // An interrupted commit left a candidate this boot cannot classify, so
  // recovery renamed nothing. Read the last committed image out of the backup
  // rather than publishing it: the transaction filenames must survive this boot
  // intact for a later one to promote the candidate.
  if (recovery.run_from_backup) {
    _legacy_tail.valid = false;
    const JsonPrefsLoadResult backup_result =
        adoptMqttJsonFile(fs, "/mqtt.json.bak", &_mqtt_prefs);
    if (jsonPrefsLoaded(backup_result)) {
      MESH_DEBUG_PRINTLN("MQTT: running the /mqtt.json backup; candidate preserved and writes held");
    } else {
      setMQTTPrefsDefaults(&_mqtt_prefs);
      MESH_DEBUG_PRINTLN("MQTT: /mqtt.json backup became unreadable; using defaults (files preserved)");
    }
    return;
  }

  if (_mqtt_prefs_hold && !fs->exists("/mqtt.json") &&
      (fs->exists("/mqtt.json.tmp") || fs->exists("/mqtt.json.bak"))) {
    _legacy_tail.valid = false;
    MESH_DEBUG_PRINTLN("MQTT: unresolved /mqtt.json recovery files; using defaults (files preserved)");
    return;
  }

  // The JSON file is authoritative once it exists. Never fall back to the
  // stale binary snapshot when JSON is corrupt, unreadable, or from a future
  // schema: preserve it and run defaults until an operator resolves it.
  if (fs->exists("/mqtt.json")) {
    const JsonPrefsLoadResult json_result =
        adoptMqttJsonFile(fs, "/mqtt.json", &_mqtt_prefs);
    if (jsonPrefsLoaded(json_result)) {
      _legacy_tail.valid = false;
      if (json_result == JsonPrefsLoadResult::LoadedWithRepairs) {
        MESH_DEBUG_PRINTLN("MQTT: repaired out-of-range values in /mqtt.json");
        if (!_mqtt_prefs_hold && !saveMQTTPrefs(fs)) {
          _mqtt_prefs_hold = true;
          MESH_DEBUG_PRINTLN("MQTT: could not persist /mqtt.json repairs; source held");
        }
      }
      return;
    }
    _mqtt_prefs_hold = true;
    _legacy_tail.valid = false;
    if (json_result == JsonPrefsLoadResult::NoMemory) {
      MESH_DEBUG_PRINTLN("MQTT: no memory to validate /mqtt.json; source preserved");
    } else if (json_result == JsonPrefsLoadResult::UnsupportedVersion) {
      MESH_DEBUG_PRINTLN("MQTT: /mqtt.json uses a future version; using defaults (file preserved)");
    } else if (json_result == JsonPrefsLoadResult::FutureClaimed) {
      MESH_DEBUG_PRINTLN(
          "MQTT: /mqtt.json claims a future version but uses unknown grammar; "
          "using defaults (file preserved)");
    } else {
      MESH_DEBUG_PRINTLN("MQTT: /mqtt.json is invalid or unreadable; using defaults (file preserved)");
    }
    return;
  }

  // Complete or preserve an interrupted SPIFFS transaction before decoding.
  // A failed recovery leaves the artifacts untouched and blocks this boot from
  // replacing them with defaults through a later CLI save.
  _mqtt_prefs_hold = _mqtt_prefs_hold || recoverMqttPrefsFiles(fs);
  bool has_observer_fields = false;
  bool mqtt_rewrite_pending = false;
  bool migrated_legacy_mqtt = false;

  if (fs->exists("/mqtt_prefs")) {
    File file = openMqttPrefsRead(fs);
    if (file) {
      const size_t file_size = file.size();
      uint8_t prefix[sizeof(MQTTPrefsHeader)] = {};
      const size_t prefix_size = file_size < sizeof(prefix) ? file_size : sizeof(prefix);
      const size_t prefix_read = file.read(prefix, prefix_size);
      file.close();

      const MQTTPrefsCodec::DecodePlan plan =
          MQTTPrefsCodec::classify(prefix, prefix_read, file_size);
      if (plan.preserve_file) {
        _mqtt_prefs_hold = true;
        MESH_DEBUG_PRINTLN("MQTT: /mqtt_prefs is unsupported or corrupt, using defaults (file preserved)");
      } else if (plan.source == MQTTPrefsCodec::Source::Current) {
        file = openMqttPrefsRead(fs);
        MQTTPrefsHeader header;
        LegacyV1MQTTPrefs* old_prefs = new (std::nothrow) LegacyV1MQTTPrefs;
        if (old_prefs) memset(old_prefs, 0, sizeof(*old_prefs));
        if (!old_prefs || !file || file.read((uint8_t *)&header, sizeof(header)) != sizeof(header) ||
            file.read((uint8_t *)old_prefs, plan.payload_len) != plan.payload_len) {
          setMQTTPrefsDefaults(&_mqtt_prefs);
          _mqtt_prefs_hold = true;
          MESH_DEBUG_PRINTLN("MQTT: /mqtt_prefs read failed, using defaults (file preserved)");
        } else if (!MQTTPrefsCodec::isPlausibleV1(*old_prefs, plan.payload_len)) {
          setMQTTPrefsDefaults(&_mqtt_prefs);
          _mqtt_prefs_hold = true;
          MESH_DEBUG_PRINTLN("MQTT: /mqtt_prefs v1 content failed plausibility checks; source preserved");
        } else {
          MQTTPrefsCodec::migrateV1(*old_prefs, plan.payload_len, &_mqtt_prefs);
          has_observer_fields = plan.observer_fields_present;
          mqtt_rewrite_pending = true;
          migrated_legacy_mqtt = true;
          // Written by a later build with appended fields. Everything this
          // binary knows loaded normally; say so, because the next `set` will
          // rewrite the file at this length and drop the newer settings.
          if (file_size - sizeof(MQTTPrefsHeader) > plan.payload_len) {
            MESH_DEBUG_PRINTLN(
                "MQTT: /mqtt_prefs written by newer firmware (%u > %u bytes); "
                "known settings loaded; newer binary fields remain in the rollback snapshot",
                (unsigned)(file_size - sizeof(MQTTPrefsHeader)),
                (unsigned)plan.payload_len);
          }
        }
        delete old_prefs;
        if (file) file.close();
      } else if (plan.rewrite_legacy) {
        bool migrated = false;
        file = openMqttPrefsRead(fs);
        if (file) {
          switch (plan.source) {
            case MQTTPrefsCodec::Source::LegacyPreSlot: {
              union {
                OldMQTTPrefs post_wifi_power;
                PreWifiPowerOldMQTTPrefs pre_wifi_power;
              } old_prefs = {};
              if (file.read((uint8_t *)&old_prefs, sizeof(old_prefs)) == sizeof(old_prefs)) {
                if (MQTTPrefsCodec::isPlausibleLegacy(plan.source,
                                                       (const uint8_t *)&old_prefs, sizeof(old_prefs))) {
                  if (MQTTPrefsCodec::looksLikePreWifiPower((uint8_t *)&old_prefs, sizeof(old_prefs))) {
                    MQTTPrefsCodec::migratePreWifiPower(old_prefs.pre_wifi_power, &_mqtt_prefs);
                  } else {
                    MQTTPrefsCodec::migratePreSlot(old_prefs.post_wifi_power, &_mqtt_prefs);
                  }
                  migrated = true;
                } else {
                  MESH_DEBUG_PRINTLN("MQTT: /mqtt_prefs legacy content failed plausibility checks");
                }
              }
              break;
            }
            case MQTTPrefsCodec::Source::LegacyThreeSlotBase: {
              ThreeSlotBaseMQTTPrefs old_prefs = {};
              if (file.read((uint8_t *)&old_prefs, sizeof(old_prefs)) == sizeof(old_prefs)) {
                if (MQTTPrefsCodec::isPlausibleLegacy(plan.source,
                                                       (const uint8_t *)&old_prefs, sizeof(old_prefs))) {
                  MQTTPrefsCodec::migrateThreeSlot(old_prefs, &_mqtt_prefs);
                  migrated = true;
                } else {
                  MESH_DEBUG_PRINTLN("MQTT: /mqtt_prefs legacy content failed plausibility checks");
                }
              }
              break;
            }
            case MQTTPrefsCodec::Source::LegacyThreeSlot: {
              ThreeSlotMQTTPrefs old_prefs = {};
              if (file.read((uint8_t *)&old_prefs, sizeof(old_prefs)) == sizeof(old_prefs)) {
                if (MQTTPrefsCodec::isPlausibleLegacy(plan.source,
                                                       (const uint8_t *)&old_prefs, sizeof(old_prefs))) {
                  MQTTPrefsCodec::migrateThreeSlot(old_prefs, &_mqtt_prefs);
                  migrated = true;
                } else {
                  MESH_DEBUG_PRINTLN("MQTT: /mqtt_prefs legacy content failed plausibility checks");
                }
              }
              break;
            }
            case MQTTPrefsCodec::Source::LegacySixSlotBase:
            case MQTTPrefsCodec::Source::LegacySixSlotAudience:
            case MQTTPrefsCodec::Source::LegacySixSlotAudienceRx:
            case MQTTPrefsCodec::Source::LegacySixSlot: {
              Legacy6SlotMQTTPrefs* old_prefs = new (std::nothrow) Legacy6SlotMQTTPrefs;
              if (old_prefs) memset(old_prefs, 0, sizeof(*old_prefs));
              if (old_prefs && file.read((uint8_t *)old_prefs, plan.payload_len) == plan.payload_len) {
                if (MQTTPrefsCodec::isPlausibleLegacy(plan.source,
                                                       (const uint8_t *)old_prefs, plan.payload_len)) {
                  MQTTPrefsCodec::migrateLegacySixSlot(*old_prefs, plan.source, &_mqtt_prefs);
                  migrated = true;
                } else {
                  MESH_DEBUG_PRINTLN("MQTT: /mqtt_prefs legacy content failed plausibility checks");
                }
              }
              delete old_prefs;
              break;
            }
            default:
              break;
          }
          file.close();
        }
        if (migrated) {
          // Do not save yet: a legacy /com_prefs observer tail may still need
          // to be overlaid below. Publish the complete v1 image once, after it.
          mqtt_rewrite_pending = true;
          migrated_legacy_mqtt = true;
        } else {
          setMQTTPrefsDefaults(&_mqtt_prefs);
          _mqtt_prefs_hold = true;
          MESH_DEBUG_PRINTLN("MQTT: /mqtt_prefs legacy read failed, using defaults (file preserved)");
        }
      }
    } else {
      _mqtt_prefs_hold = true;
      MESH_DEBUG_PRINTLN("MQTT: /mqtt_prefs could not be opened, using defaults (file preserved)");
    }
  }

  if (_legacy_tail.valid && !has_observer_fields) {
    _mqtt_prefs.snmp_enabled = _legacy_tail.snmp_enabled;
    memcpy(_mqtt_prefs.snmp_community, _legacy_tail.snmp_community, sizeof(_mqtt_prefs.snmp_community));
    _mqtt_prefs.radio_watchdog_minutes = _legacy_tail.radio_watchdog_minutes;
    _mqtt_prefs.alert_enabled = _legacy_tail.alert_enabled;
    memcpy(_mqtt_prefs.alert_psk_hex, _legacy_tail.alert_psk_hex, sizeof(_mqtt_prefs.alert_psk_hex));
    _mqtt_prefs.alert_wifi_minutes = _legacy_tail.alert_wifi_minutes;
    _mqtt_prefs.alert_mqtt_minutes = _legacy_tail.alert_mqtt_minutes;
    _mqtt_prefs.alert_min_interval_min = _legacy_tail.alert_min_interval_min;
    memcpy(_mqtt_prefs.alert_hashtag, _legacy_tail.alert_hashtag, sizeof(_mqtt_prefs.alert_hashtag));
    memcpy(_mqtt_prefs.alert_region, _legacy_tail.alert_region, sizeof(_mqtt_prefs.alert_region));
    mqtt_rewrite_pending = true;
    MESH_DEBUG_PRINTLN("MQTT: Migrated observer settings from legacy /com_prefs trailing block");
  }

  // Keep persisted values inside the signed-delta millis() scheduling window.
  // This also repairs any manually-written or experimental value from firmware
  // that briefly accepted intervals longer than the supported two-week cap.
  if (_mqtt_prefs.mqtt_neighbors_interval < MQTT_NEIGHBORS_MIN_INTERVAL_MS ||
      _mqtt_prefs.mqtt_neighbors_interval > MQTT_NEIGHBORS_MAX_INTERVAL_MS) {
    _mqtt_prefs.mqtt_neighbors_interval = MQTT_NEIGHBORS_DEFAULT_INTERVAL_MS;
    // Persist the repair so a corrupt flash value is not re-clamped every boot.
    // Skip when hold is set so we never overwrite a deliberately preserved file.
    if (!_mqtt_prefs_hold) {
      mqtt_rewrite_pending = true;
    }
    MESH_DEBUG_PRINTLN("MQTT: invalid neighbors interval reset to %u hours",
                       (unsigned)MQTT_NEIGHBORS_DEFAULT_INTERVAL_HOURS);
  }
  _legacy_tail.valid = false;

  if (mqtt_rewrite_pending) {
    legacy_upgrade->requireMqttRewrite();
    if (migrated_legacy_mqtt) {
      MESH_DEBUG_PRINTLN("MQTT: Migrating binary /mqtt_prefs to /mqtt.json");
    } else {
      MESH_DEBUG_PRINTLN("MQTT: Persisting observer tail into /mqtt.json before /com_prefs compaction");
    }
    if (saveMQTTPrefs(fs)) {
      legacy_upgrade->recordMqttSave(true);
    } else {
      // The legacy source(s) remain intact because the failed transaction never
      // published its temp file. Hold this boot so loadPrefs leaves /com_prefs
      // untouched; the next boot can recover the tail and retry the transaction.
      _mqtt_prefs_hold = true;
      legacy_upgrade->recordMqttSave(false);
      MESH_DEBUG_PRINTLN("MQTT: /mqtt.json migration save failed; legacy files preserved and held");
    }
  }
}

bool ObserverConfig::saveMQTTPrefs(FILESYSTEM* fs) {
  if (_mqtt_prefs_hold) {
    // Loading deliberately preserved the source file. Do not replace it with this
    // boot's defaults after an unsupported, corrupt, or temporarily failed read.
    MESH_DEBUG_PRINTLN("MQTT: observer preference source held, not overwriting /mqtt.json");
    return false;
  }

  MQTTPrefs* repair_defaults = new (std::nothrow) MQTTPrefs;
  if (repair_defaults == nullptr) {
    MESH_DEBUG_PRINTLN("MQTT: no memory to normalize observer settings before save");
    return false;
  }
  setMQTTPrefsDefaults(repair_defaults);
  MQTTPrefsSerializer serializer(&_mqtt_prefs, repair_defaults);
  // The serializer hierarchy copies the default values it needs. Release this
  // large temporary before file I/O and the independent verification scratch.
  delete repair_defaults;
  bool repaired = false;
  if (!serializer.normalize(&repaired)) return false;
  if (repaired) MESH_DEBUG_PRINTLN("MQTT: normalized out-of-range observer settings before save");

  MQTTPrefsJsonFileStore store(fs);
  bool verify_oom = false;
  const MQTTPrefsAtomicStore::VerifiedImageResult result =
      MQTTPrefsAtomicStore::writeVerifiedImage(
          store,
          [&]() -> bool {
            MQTTPrefsStoreStream stream(&store);
            return serializer.saveSerial(stream) && stream.ok();
          },
          [&]() -> bool {
            MQTTPrefs* verify = new (std::nothrow) MQTTPrefs;
            if (verify == nullptr) {
              verify_oom = true;
              return false;
            }
            const JsonPrefsLoadResult verify_result =
                loadMqttJsonFile(fs, "/mqtt.json.tmp", verify);
            delete verify;
            // saveSerial() emits already-normalized values. Needing another
            // repair here means save/load is not idempotent.
            return verify_result == JsonPrefsLoadResult::Loaded;
          });

  // A recovery file this firmware may still promote is holding the new image,
  // so the change cannot be reported as rolled back. No further save can start
  // until it is resolved: begin() refuses while either the temp or the backup
  // exists, which is exactly the state that gets us here.
  if (MQTTPrefsAtomicStore::saveOutcomeUnresolved(result)) _observer_save_indeterminate = true;

  switch (result) {
    case MQTTPrefsAtomicStore::VerifiedImageResult::Committed:
      return true;
    case MQTTPrefsAtomicStore::VerifiedImageResult::BeginFailed:
      MESH_DEBUG_PRINTLN("MQTT: atomic /mqtt.json save failed at begin; source preserved");
      break;
    case MQTTPrefsAtomicStore::VerifiedImageResult::WriteFailed:
      MESH_DEBUG_PRINTLN("MQTT: atomic /mqtt.json save failed during write; source preserved");
      break;
    case MQTTPrefsAtomicStore::VerifiedImageResult::FinishFailed:
      MESH_DEBUG_PRINTLN("MQTT: atomic /mqtt.json save failed during checksum verification; source preserved");
      break;
    case MQTTPrefsAtomicStore::VerifiedImageResult::VerifyFailed:
      if (verify_oom) {
        MESH_DEBUG_PRINTLN("MQTT: no memory to validate /mqtt.json temp; source preserved");
      } else {
        MESH_DEBUG_PRINTLN("MQTT: generated /mqtt.json temp failed schema validation; source preserved");
      }
      break;
    case MQTTPrefsAtomicStore::VerifiedImageResult::CleanupIndeterminate:
      MESH_DEBUG_PRINTLN("MQTT: rejected /mqtt.json temp could not be removed and no primary outranks it; "
                         "recovery files preserved");
      break;
    case MQTTPrefsAtomicStore::VerifiedImageResult::CommitFailed:
      MESH_DEBUG_PRINTLN("MQTT: atomic /mqtt.json save failed during rename; transaction rolled back");
      break;
    case MQTTPrefsAtomicStore::VerifiedImageResult::CommitIndeterminate:
      MESH_DEBUG_PRINTLN("MQTT: atomic /mqtt.json save failed during rename and could not be rolled back; "
                         "recovery files preserved");
      break;
  }
  return false;
}

#endif
