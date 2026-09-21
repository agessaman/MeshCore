#pragma once
#include <stdint.h>
#include <string.h>
#include "ObserverMailbox.h"

// One loop-task publisher, one worker consumer. Reconfigure requests coalesce
// with the complete configuration that caused them, never in separate flags.
template<class Prefs, class Metadata>
class ObserverConfigMailbox {
 public:
  void publish(const Prefs& prefs, const Metadata& metadata, uint32_t reconfigure = 0) {
    ObserverGuard guard(_lock);
    if (_revision && !reconfigure && memcmp(&_prefs, &prefs, sizeof(prefs)) == 0 &&
        memcmp(&_metadata, &metadata, sizeof(metadata)) == 0) return;
    _prefs = prefs;
    _metadata = metadata;
    _reconfigure |= reconfigure;
    if (++_revision == 0) ++_revision;
  }
  bool consume(Prefs& prefs, Metadata& metadata, uint32_t& revision, uint32_t& reconfigure) {
    ObserverGuard guard(_lock);
    if (!_revision || revision == _revision) return false;
    prefs = _prefs;
    metadata = _metadata;
    revision = _revision;
    reconfigure = _reconfigure;
    _reconfigure = 0;
    return true;
  }
  uint32_t revision() const {
    ObserverGuard guard(_lock);
    return _revision;
  }
 private:
  mutable ObserverLock _lock;
  Prefs _prefs{};
  Metadata _metadata{};
  uint32_t _revision = 0, _reconfigure = 0;
};
