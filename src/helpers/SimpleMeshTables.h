#pragma once

#include <Mesh.h>
#if ARDUINO
  #include <Arduino.h>
#endif

#ifdef ESP32
  #include <FS.h>
#endif

#define MAX_PACKET_HASHES  (128+32)
#ifndef MESH_ENABLE_RECENT_REPEATERS
  #define MESH_ENABLE_RECENT_REPEATERS  0
#endif
#ifndef MAX_RECENT_REPEATERS
  // Recent repeater history is only needed by repeater firmware. Other roles
  // use unconditional retry decisions and skip this RAM-heavy cache.
  #if !MESH_ENABLE_RECENT_REPEATERS
    #define MAX_RECENT_REPEATERS  0
  #elif defined(ESP32) || defined(ESP32_PLATFORM)
    #define MAX_RECENT_REPEATERS  2048
  #elif defined(NRF52_PLATFORM)
    #define MAX_RECENT_REPEATERS  512
  #else
    #define MAX_RECENT_REPEATERS  64
  #endif
#endif
#define RECENT_REPEATER_STORAGE_SLOTS  (MAX_RECENT_REPEATERS > 0 ? MAX_RECENT_REPEATERS : 1)
#define MAX_ROUTE_HASH_BYTES   3

class SimpleMeshTables : public mesh::MeshTables {
public:
  struct RecentRepeaterInfo {
    // Identity and link quality for a next-hop path prefix.
    uint8_t prefix[MAX_ROUTE_HASH_BYTES];
    uint8_t prefix_len;
    int8_t snr_x4;
    uint32_t last_heard_millis;
  };

private:
  uint8_t _hashes[MAX_PACKET_HASHES*MAX_HASH_SIZE];
  int _next_idx;
  uint32_t _direct_dups, _flood_dups;
  RecentRepeaterInfo _recent_repeaters[RECENT_REPEATER_STORAGE_SLOTS];

  bool hasSeenHash(const uint8_t* hash) const {
    const uint8_t* sp = _hashes;
    for (int i = 0; i < MAX_PACKET_HASHES; i++, sp += MAX_HASH_SIZE) {
      if (memcmp(hash, sp, MAX_HASH_SIZE) == 0) {
        return true;
      }
    }
    return false;
  }

  void storeHash(const uint8_t* hash) {
    memcpy(&_hashes[_next_idx*MAX_HASH_SIZE], hash, MAX_HASH_SIZE);
    _next_idx = (_next_idx + 1) % MAX_PACKET_HASHES;
  }

  bool prefixesOverlap(const uint8_t* a, uint8_t a_len, const uint8_t* b, uint8_t b_len) const {
    uint8_t n = a_len < b_len ? a_len : b_len;
    return n > 0 && memcmp(a, b, n) == 0;
  }

  int8_t weightedSnrX4RoundUp(int8_t curr_snr_x4, int8_t new_snr_x4) const {
    // Keep existing SNR heavier than a single new sample: 75% existing + 25% new.
    int32_t weighted_sum = ((int32_t)curr_snr_x4 * 3) + (int32_t)new_snr_x4;
    int32_t blended = weighted_sum / 4;  // truncates toward zero
    // "Round up" means ceil(), which only differs from truncation for positive remainders.
    if (weighted_sum > 0 && (weighted_sum % 4) != 0) {
      blended++;
    }
    if (blended > 127) {
      blended = 127;
    } else if (blended < -128) {
      blended = -128;
    }
    return (int8_t)blended;
  }

  bool extractRecentRepeater(const mesh::Packet* packet, uint8_t* prefix, uint8_t& prefix_len) const {
    // Learn repeater prefixes only from packet shapes that expose a trustworthy repeater ID.
    // For flood traffic, the last path entry is the repeater we directly heard.
    if (packet->isRouteFlood() && packet->getPathHashCount() > 0) {
      prefix_len = packet->getPathHashSize();
      if (prefix_len > MAX_ROUTE_HASH_BYTES) {
        prefix_len = MAX_ROUTE_HASH_BYTES;
      }

      const uint8_t* last_hop = &packet->path[(packet->getPathHashCount() - 1) * packet->getPathHashSize()];
      memcpy(prefix, last_hop, prefix_len);
      return true;
    }

    // If there is no flood path to inspect, fall back to payload-derived identities.
    if (packet->getPayloadType() == PAYLOAD_TYPE_ADVERT && packet->payload_len >= PUB_KEY_SIZE) {
      memcpy(prefix, packet->payload, MAX_ROUTE_HASH_BYTES);
      prefix_len = MAX_ROUTE_HASH_BYTES;
      return true;
    }

    if (packet->getPayloadType() == PAYLOAD_TYPE_CONTROL
        && packet->isRouteDirect()
        && packet->getPathHashCount() == 0
        && packet->payload_len >= 6 + MAX_ROUTE_HASH_BYTES
        && (packet->payload[0] & 0xF0) == 0x90) {
      memcpy(prefix, &packet->payload[6], MAX_ROUTE_HASH_BYTES);
      prefix_len = MAX_ROUTE_HASH_BYTES;
      return true;
    }

    return false;
  }

  bool recentRepeaterComesBefore(const RecentRepeaterInfo& a, int a_idx,
                                 const RecentRepeaterInfo& b, int b_idx) const {
    if (a.prefix_len != b.prefix_len) {
      return a.prefix_len > b.prefix_len;  // 3-byte prefixes, then 2-byte, then 1-byte.
    }
    if (a.snr_x4 != b.snr_x4) {
      return a.snr_x4 > b.snr_x4;  // Highest SNR first within each prefix length.
    }
    int cmp = memcmp(a.prefix, b.prefix, a.prefix_len);
    if (cmp != 0) {
      return cmp < 0;
    }
    return a_idx < b_idx;
  }

  void recordRecentRepeater(const mesh::Packet* packet) {
    if (MAX_RECENT_REPEATERS == 0) {
      return;
    }

    uint8_t prefix[MAX_ROUTE_HASH_BYTES] = {0};
    uint8_t prefix_len = 0;
    if (!extractRecentRepeater(packet, prefix, prefix_len) || prefix_len == 0) {
      return;
    }
    setRecentRepeater(prefix, prefix_len, packet->_snr);
  }

public:
  SimpleMeshTables() { 
    memset(_hashes, 0, sizeof(_hashes));
    _next_idx = 0;
    _direct_dups = _flood_dups = 0;
    memset(_recent_repeaters, 0, sizeof(_recent_repeaters));
  }

#ifdef ESP32
  void restoreFrom(File f) {
    f.read(_hashes, sizeof(_hashes));
    f.read((uint8_t *) &_next_idx, sizeof(_next_idx));
    // Recent repeater entries are intentionally not restored across boots.
    // This avoids struct-layout migration issues and keeps stale path quality
    // stats from persisting indefinitely.
    memset(_recent_repeaters, 0, sizeof(_recent_repeaters));
  }
  void saveTo(File f) {
    f.write(_hashes, sizeof(_hashes));
    f.write((const uint8_t *) &_next_idx, sizeof(_next_idx));
  }
#endif

  bool wasSeen(const mesh::Packet* packet) override {
    uint8_t hash[MAX_HASH_SIZE];
    packet->calculatePacketHash(hash);

    if (hasSeenHash(hash)) {
      if (packet->isRouteDirect()) {
        _direct_dups++;   // keep some stats
      } else {
        _flood_dups++;
      }
      return true;
    }

    return false;
  }

  void markSeen(const mesh::Packet* packet) override {
    uint8_t hash[MAX_HASH_SIZE];
    packet->calculatePacketHash(hash);
    if (!hasSeenHash(hash)) {
      storeHash(hash);
      recordRecentRepeater(packet);
    }
  }

  void markSent(const mesh::Packet* packet) override {
    // Outbound packets must be marked as already-sent without teaching the recent-heard cache about ourselves.
    uint8_t hash[MAX_HASH_SIZE];
    packet->calculatePacketHash(hash);
    if (!hasSeenHash(hash)) {
      storeHash(hash);
    }
  }

  void clear(const mesh::Packet* packet) override {
    uint8_t hash[MAX_HASH_SIZE];
    packet->calculatePacketHash(hash);

    uint8_t* sp = _hashes;
    for (int i = 0; i < MAX_PACKET_HASHES; i++, sp += MAX_HASH_SIZE) {
      if (memcmp(hash, sp, MAX_HASH_SIZE) == 0) {
        memset(sp, 0, MAX_HASH_SIZE);
        break;
      }
    }
  }

  uint32_t getNumDirectDups() const { return _direct_dups; }
  uint32_t getNumFloodDups() const { return _flood_dups; }

  bool setRecentRepeater(const uint8_t* prefix, uint8_t prefix_len, int8_t snr_x4,
                         bool snr_locked = false, bool bypass_allow_filter = false) {
    (void)snr_locked;
    (void)bypass_allow_filter;
    if (MAX_RECENT_REPEATERS == 0) {
      return false;
    }
    if (prefix == NULL || prefix_len == 0) {
      return false;
    }

    if (prefix_len > MAX_ROUTE_HASH_BYTES) {
      prefix_len = MAX_ROUTE_HASH_BYTES;
    }

    // Keep exact prefixes distinct so a 1-byte path prefix does not collapse
    // independent 2/3-byte repeaters that share the same first byte.
    for (int i = 0; i < MAX_RECENT_REPEATERS; i++) {
      RecentRepeaterInfo& existing = _recent_repeaters[i];
      if (existing.prefix_len != prefix_len || memcmp(existing.prefix, prefix, prefix_len) != 0) {
        continue;
      }
      existing.snr_x4 = weightedSnrX4RoundUp(existing.snr_x4, snr_x4);
#if ARDUINO
      existing.last_heard_millis = millis();
#else
      existing.last_heard_millis = 0;
#endif
      return true;
    }

    int slot_idx = -1;
    for (int i = 0; i < MAX_RECENT_REPEATERS; i++) {
      if (_recent_repeaters[i].prefix_len == 0) {
        slot_idx = i;
        break;
      }
    }
    if (slot_idx < 0) {
      // Table is full: evict the oldest heard entry.
      slot_idx = 0;
#if ARDUINO
      uint32_t now = millis();
      uint32_t oldest_age = (uint32_t)(now - _recent_repeaters[0].last_heard_millis);
      for (int i = 1; i < MAX_RECENT_REPEATERS; i++) {
        uint32_t age = (uint32_t)(now - _recent_repeaters[i].last_heard_millis);
        if (age > oldest_age) {
          oldest_age = age;
          slot_idx = i;
        }
      }
#endif
    }

    RecentRepeaterInfo& slot = _recent_repeaters[slot_idx];
    memset(slot.prefix, 0, sizeof(slot.prefix));
    memcpy(slot.prefix, prefix, prefix_len);
    slot.prefix_len = prefix_len;
    slot.snr_x4 = snr_x4;
#if ARDUINO
    slot.last_heard_millis = millis();
#else
    slot.last_heard_millis = 0;
#endif
    return true;
  }
  bool decrementRecentRepeaterSnrX4(const uint8_t* prefix, uint8_t prefix_len, uint8_t amount_x4 = 1) {
    if (MAX_RECENT_REPEATERS == 0) {
      return false;
    }
    if (prefix == NULL || prefix_len == 0 || amount_x4 == 0) {
      return false;
    }
    if (prefix_len > MAX_ROUTE_HASH_BYTES) {
      prefix_len = MAX_ROUTE_HASH_BYTES;
    }

    for (int i = 0; i < MAX_RECENT_REPEATERS; i++) {
      RecentRepeaterInfo& existing = _recent_repeaters[i];
      if (existing.prefix_len != prefix_len || memcmp(existing.prefix, prefix, prefix_len) != 0) {
        continue;
      }
      int16_t lowered = (int16_t)existing.snr_x4 - (int16_t)amount_x4;
      if (lowered < -128) {
        lowered = -128;
      }
      existing.snr_x4 = (int8_t)lowered;
      return true;
    }
    return false;
  }
  int getRecentRepeaterCount() const {
    if (MAX_RECENT_REPEATERS == 0) {
      return 0;
    }
    int count = 0;
    for (int i = 0; i < MAX_RECENT_REPEATERS; i++) {
      if (_recent_repeaters[i].prefix_len > 0) {
        count++;
      }
    }
    return count;
  }
  const RecentRepeaterInfo* getRecentRepeaterBySortedIdx(int idx_wanted) const {
    if (MAX_RECENT_REPEATERS == 0) {
      return NULL;
    }
    if (idx_wanted < 0) {
      return NULL;
    }

    const RecentRepeaterInfo* last = NULL;
    int last_idx = -1;
    for (int rank = 0; rank <= idx_wanted; rank++) {
      const RecentRepeaterInfo* best = NULL;
      int best_idx = -1;
      for (int i = 0; i < MAX_RECENT_REPEATERS; i++) {
        const RecentRepeaterInfo* info = &_recent_repeaters[i];
        if (info->prefix_len == 0) {
          continue;
        }
        if (last != NULL && !recentRepeaterComesBefore(*last, last_idx, *info, i)) {
          continue;
        }
        if (best == NULL || recentRepeaterComesBefore(*info, i, *best, best_idx)) {
          best = info;
          best_idx = i;
        }
      }
      if (best == NULL) {
        return NULL;
      }
      last = best;
      last_idx = best_idx;
    }
    return last;
  }

  const RecentRepeaterInfo* findRecentRepeaterByHash(const uint8_t* hash, uint8_t hash_len) const {
    if (MAX_RECENT_REPEATERS == 0) {
      return NULL;
    }
    if (hash == NULL || hash_len == 0) {
      return NULL;
    }

    // Prefer exact matches. If none exists, fall back to the longest overlapping
    // prefix, using highest SNR to break ties.
    const RecentRepeaterInfo* best = NULL;
    for (int i = 0; i < MAX_RECENT_REPEATERS; i++) {
      const RecentRepeaterInfo* info = &_recent_repeaters[i];
      if (info->prefix_len == 0) {
        continue;
      }
      if (info->prefix_len == hash_len && memcmp(info->prefix, hash, hash_len) == 0) {
        return info;
      }
      if (prefixesOverlap(info->prefix, info->prefix_len, hash, hash_len)) {
        if (best == NULL || info->prefix_len > best->prefix_len
            || (info->prefix_len == best->prefix_len && info->snr_x4 > best->snr_x4)) {
          best = info;
        }
      }
    }
    return best;
  }
  void clearRecentRepeaters() {
    memset(_recent_repeaters, 0, sizeof(_recent_repeaters));
  }

  void resetStats() { _direct_dups = _flood_dups = 0; }
};
