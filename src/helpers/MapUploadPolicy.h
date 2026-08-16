#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "Packet.h"  // packet header bit layout + PAYLOAD_TYPE_ADVERT

// Pure decision logic for the "meshcore-map" preset, which uploads the adverts
// this node hears to https://map.meshcore.io/api/v1/uploader/node.
//
// The rules mirror recrof/map.meshcore.io-uploader (the reference Node.js tool
// the map operator documents): decode the advert, skip CHAT nodes, drop
// non-advancing timestamps as replays, and re-upload a node only once its
// advert timestamp has moved on by an hour. The two additions here are the ones
// a repeater needs and a desktop script does not — a bounded seen-table and a
// pacing gate, because every upload costs a TLS handshake.
//
// Nothing in this file touches Arduino, WiFi, or the mesh runtime, so the whole
// contract is exercised by test/test_map_upload_policy/.
namespace MapUploadPolicy {

// Advert app_data type nibble. Mirrors ADV_TYPE_* in helpers/AdvertDataHelpers.h,
// redeclared here so this header stays free of the mesh runtime headers. The
// binding static_asserts the two against each other.
static const uint8_t kAdvTypeNone     = 0;
static const uint8_t kAdvTypeChat     = 1;
static const uint8_t kAdvTypeRepeater = 2;
static const uint8_t kAdvTypeRoom     = 3;
static const uint8_t kAdvTypeSensor   = 4;

// Fixed part of an advert payload: pub_key | timestamp | signature, followed by
// the variable-length app_data.
static const size_t kAdvertHeaderLen = PUB_KEY_SIZE + 4 + SIGNATURE_SIZE;

// A node is re-uploaded only once its advert timestamp has advanced a full hour.
// The reference tool rejects anything sooner as "timestamp too new to reupload";
// the map only needs one fix per node per hour.
static const uint32_t kMinReuploadSecs = 3600;

// Minimum wall-clock gap between two uploads, regardless of which node they are
// for. Each upload is a fresh HTTPS connection (~40 KB of internal DRAM for the
// mbedTLS record buffers on a non-PSRAM board), so a mesh that floods a dozen
// adverts at once must not turn into a dozen back-to-back handshakes.
static const uint32_t kMinUploadGapMs = 15000;

// A staged advert older than this is dropped rather than uploaded. Bounds how
// stale a fix can be when WiFi has been down or the heap has been tight.
static const uint32_t kMaxPendingAgeMs = 300000;  // 5 minutes

// A decoded advert. Pointers borrow the caller's frame buffer and are valid only
// as long as it is.
struct AdvertView {
  const uint8_t* pub_key;    // PUB_KEY_SIZE bytes: the advertiser's identity
  const uint8_t* signature;  // SIGNATURE_SIZE bytes, over pub_key|timestamp|app_data
  const uint8_t* app_data;   // may be null when app_data_len == 0
  size_t app_data_len;
  uint32_t timestamp;        // the advert's own timestamp, little-endian on the wire
  uint8_t type;              // kAdvType*
};

// Decode an over-the-air MeshCore frame as an advert. Layout matches
// Dispatcher::tryParsePacket():
//   header | [transport_codes] | path_len | path | payload
// and the advert payload itself matches Mesh::onRecvPacket()'s ADVERT case.
// Returns false for anything that is not a well-formed advert; a caller can
// treat that as "not interesting" without distinguishing corrupt from unrelated.
inline bool parseAdvertFrame(const uint8_t* frame, size_t len, AdvertView* out) {
  if (frame == nullptr || out == nullptr || len < 1) return false;

  size_t i = 0;
  const uint8_t header = frame[i++];

  // Only version 1 packets have the layout decoded below.
  if (((header >> PH_VER_SHIFT) & PH_VER_MASK) > PAYLOAD_VER_1) return false;
  if (((header >> PH_TYPE_SHIFT) & PH_TYPE_MASK) != PAYLOAD_TYPE_ADVERT) return false;

  const uint8_t route = header & PH_ROUTE_MASK;
  if (route == ROUTE_TYPE_TRANSPORT_FLOOD || route == ROUTE_TYPE_TRANSPORT_DIRECT) {
    i += 4;  // two uint16 transport codes
  }
  if (i >= len) return false;

  const uint8_t path_len_byte = frame[i++];
  const uint8_t path_mode = static_cast<uint8_t>(path_len_byte >> 6);
  if (path_mode == 3) return false;  // reserved upstream

  const size_t path_bytes =
      static_cast<size_t>(path_len_byte & 63) * static_cast<size_t>(path_mode + 1);
  if (path_bytes > MAX_PATH_SIZE) return false;
  if (i + path_bytes > len) return false;
  i += path_bytes;

  const size_t payload_len = len - i;
  if (payload_len < kAdvertHeaderLen) return false;

  const uint8_t* payload = frame + i;
  out->pub_key = payload;
  // Read the timestamp explicitly little-endian rather than memcpy'ing into a
  // uint32_t: it is a wire field, and the host running the tests need not share
  // the target's byte order.
  out->timestamp = static_cast<uint32_t>(payload[PUB_KEY_SIZE]) |
                   (static_cast<uint32_t>(payload[PUB_KEY_SIZE + 1]) << 8) |
                   (static_cast<uint32_t>(payload[PUB_KEY_SIZE + 2]) << 16) |
                   (static_cast<uint32_t>(payload[PUB_KEY_SIZE + 3]) << 24);
  out->signature = payload + PUB_KEY_SIZE + 4;

  size_t app_data_len = payload_len - kAdvertHeaderLen;
  if (app_data_len > MAX_ADVERT_DATA_SIZE) app_data_len = MAX_ADVERT_DATA_SIZE;
  out->app_data = app_data_len > 0 ? payload + kAdvertHeaderLen : nullptr;
  out->app_data_len = app_data_len;
  out->type = app_data_len > 0 ? static_cast<uint8_t>(out->app_data[0] & 0x0F) : kAdvTypeNone;
  return true;
}

// Length of the message an advert's Ed25519 signature covers:
// pub_key | timestamp | app_data (the signature itself is excluded).
inline size_t buildSignedAdvertMessage(const AdvertView& advert, uint8_t* out, size_t out_size) {
  const size_t needed = PUB_KEY_SIZE + 4 + advert.app_data_len;
  if (out == nullptr || out_size < needed) return 0;

  size_t n = 0;
  memcpy(out + n, advert.pub_key, PUB_KEY_SIZE); n += PUB_KEY_SIZE;
  // Same little-endian encoding the advertiser signed.
  out[n++] = static_cast<uint8_t>(advert.timestamp & 0xFF);
  out[n++] = static_cast<uint8_t>((advert.timestamp >> 8) & 0xFF);
  out[n++] = static_cast<uint8_t>((advert.timestamp >> 16) & 0xFF);
  out[n++] = static_cast<uint8_t>((advert.timestamp >> 24) & 0xFF);
  if (advert.app_data_len > 0) {
    memcpy(out + n, advert.app_data, advert.app_data_len); n += advert.app_data_len;
  }
  return n;
}

// True for the advert types the map actually plots. CHAT nodes are companion
// clients and are deliberately excluded, matching the reference tool.
inline bool isMappableType(uint8_t adv_type) {
  return adv_type == kAdvTypeRepeater || adv_type == kAdvTypeRoom || adv_type == kAdvTypeSensor;
}

enum class Verdict : uint8_t {
  Upload,       // send it
  NotMappable,  // CHAT node, or an advert carrying no type at all
  Replay,       // timestamp did not advance past what we last uploaded
  TooSoon,      // advanced, but by less than kMinReuploadSecs
};

// The per-node rule, split out from the seen-table so it can be tested directly.
// seen_before/last_uploaded_ts describe what this node last had accepted.
inline Verdict decideForAdvert(uint8_t adv_type, uint32_t advert_ts,
                               bool seen_before, uint32_t last_uploaded_ts) {
  if (!isMappableType(adv_type)) return Verdict::NotMappable;
  if (!seen_before) return Verdict::Upload;
  if (advert_ts <= last_uploaded_ts) return Verdict::Replay;
  // Unsigned subtraction is safe: the branch above established advert_ts > last.
  if (advert_ts - last_uploaded_ts < kMinReuploadSecs) return Verdict::TooSoon;
  return Verdict::Upload;
}

inline const char* verdictName(Verdict v) {
  switch (v) {
    case Verdict::Upload:      return "upload";
    case Verdict::NotMappable: return "not-mappable";
    case Verdict::Replay:      return "replay";
    case Verdict::TooSoon:     return "too-soon";
  }
  return "?";
}

// Wrap-safe millis() comparisons, matching the idiom the rest of the MQTT
// observer uses (see MQTTPacketQueuePolicy). Both arguments are uint32_t so the
// subtraction wraps correctly across the 49-day rollover.
inline uint32_t elapsedMs(uint32_t now_ms, uint32_t since_ms) {
  return now_ms - since_ms;
}

// The global pacing gate. any_upload_done is false until the first upload, so a
// freshly booted node does not have to wait out the gap.
inline bool uploadGapElapsed(uint32_t now_ms, uint32_t last_upload_ms, bool any_upload_done) {
  if (!any_upload_done) return true;
  return elapsedMs(now_ms, last_upload_ms) >= kMinUploadGapMs;
}

// True once a staged advert has waited too long to still be worth sending.
inline bool pendingExpired(uint32_t now_ms, uint32_t staged_ms) {
  return elapsedMs(now_ms, staged_ms) >= kMaxPendingAgeMs;
}

// Bounded record of "what have we already uploaded for this node".
//
// Keyed on a prefix of the public key rather than the whole 32 bytes: the mesh
// already treats key prefixes as node identity (Identity::isHashMatch), and at
// 8 bytes a collision would cost nothing worse than one suppressed upload for
// one hour. Eviction is least-recently-used, so a node that keeps adverting
// keeps its entry and a node that has gone quiet loses it first.
static const size_t kSeenKeyBytes = 8;

template <size_t N>
class SeenTable {
public:
  SeenTable() { clear(); }

  void clear() {
    memset(_entries, 0, sizeof(_entries));
    _count = 0;
    _tick = 0;
  }

  size_t capacity() const { return N; }
  size_t size() const { return _count; }

  // Last uploaded advert timestamp for this node, or nullptr if never uploaded.
  // Does NOT count as a use for LRU purposes — only a recorded upload does, so a
  // node whose adverts are all being rejected cannot keep its entry alive at the
  // expense of a node we are actually publishing.
  const uint32_t* find(const uint8_t* pub_key) const {
    const int idx = indexOf(pub_key);
    return idx < 0 ? nullptr : &_entries[idx].last_ts;
  }

  // Note an accepted upload. Inserts, updates, or evicts the least recently
  // recorded entry when full.
  void record(const uint8_t* pub_key, uint32_t advert_ts) {
    if (pub_key == nullptr) return;
    int idx = indexOf(pub_key);
    if (idx < 0) {
      idx = freeOrEvictedIndex();
      memcpy(_entries[idx].key, pub_key, kSeenKeyBytes);
      if (!_entries[idx].used) {
        _entries[idx].used = true;
        _count++;
      }
    }
    _entries[idx].last_ts = advert_ts;
    _entries[idx].used_at = ++_tick;
  }

private:
  struct Entry {
    uint8_t key[kSeenKeyBytes];
    uint32_t last_ts;
    uint32_t used_at;  // monotonic LRU stamp; 0 = never
    bool used;
  };

  int indexOf(const uint8_t* pub_key) const {
    if (pub_key == nullptr) return -1;
    for (size_t i = 0; i < N; i++) {
      if (_entries[i].used && memcmp(_entries[i].key, pub_key, kSeenKeyBytes) == 0) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  int freeOrEvictedIndex() {
    for (size_t i = 0; i < N; i++) {
      if (!_entries[i].used) return static_cast<int>(i);
    }
    // Full: evict the least recently recorded. _tick is monotonic and only ever
    // increments once per accepted upload, so it cannot realistically wrap.
    size_t oldest = 0;
    for (size_t i = 1; i < N; i++) {
      if (_entries[i].used_at < _entries[oldest].used_at) oldest = i;
    }
    return static_cast<int>(oldest);
  }

  Entry _entries[N];
  size_t _count;
  uint32_t _tick;
};

}  // namespace MapUploadPolicy
