#include "MeshcoreMapUploader.h"

#ifdef WITH_MQTT_BRIDGE

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Utils.h"
#include "helpers/AdvertDataHelpers.h"
#include "helpers/MQTTPresets.h"

#if defined(ARDUINO)
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#endif
#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

#if defined(MQTT_DEBUG) && defined(ARDUINO)
  #define MAP_DEBUG_PRINTLN(F, ...) \
    do { if (Serial.availableForWrite() > 0) { Serial.printf("MAP: " F "\n", ##__VA_ARGS__); } } while (0)
#else
  #define MAP_DEBUG_PRINTLN(...) {}
#endif

// The policy header redeclares the advert type nibbles so it can stay free of
// the mesh runtime headers. This is the one place that sees both, so it is where
// a drift between them is caught.
static_assert(MapUploadPolicy::kAdvTypeChat == ADV_TYPE_CHAT, "advert type drift: CHAT");
static_assert(MapUploadPolicy::kAdvTypeRepeater == ADV_TYPE_REPEATER, "advert type drift: REPEATER");
static_assert(MapUploadPolicy::kAdvTypeRoom == ADV_TYPE_ROOM, "advert type drift: ROOM");
static_assert(MapUploadPolicy::kAdvTypeSensor == ADV_TYPE_SENSOR, "advert type drift: SENSOR");
static_assert(MapUploadPolicy::kAdvTypeNone == ADV_TYPE_NONE, "advert type drift: NONE");

const char MeshcoreMapUploader::kApiUrl[] = "https://map.meshcore.io/api/v1/uploader/node";

// Prefer PSRAM, same policy as the bridge's own buffers: internal DRAM is what
// the TLS stack competes for, so keep the payload scratch out of it where we can.
static void* mapScratchAlloc(size_t size) {
#if defined(ESP_PLATFORM) && defined(BOARD_HAS_PSRAM)
  void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (p != nullptr) return p;
  return heap_caps_malloc(size, MALLOC_CAP_INTERNAL);
#else
  return malloc(size);
#endif
}

MeshcoreMapUploader::MeshcoreMapUploader()
    : _identity(nullptr),
      _enabled(false),
      _pending_len(0),
      _pending_staged_ms(0),
      _pending(false),
      _scratch(nullptr),
      _last_upload_ms(0),
      _any_upload_done(false),
      _last_memory_check_ms(0),
      _memory_pressure(false) {
  memset(&_stats, 0, sizeof(_stats));
  _last_error[0] = '\0';
}

void MeshcoreMapUploader::begin(const mesh::LocalIdentity* identity) {
  _identity = identity;
  _pending.store(false, std::memory_order_release);
  _pending_len = 0;
  _seen.clear();
  _last_upload_ms = 0;
  _any_upload_done = false;
  _memory_pressure = false;
  _last_memory_check_ms = 0;
  memset(&_stats, 0, sizeof(_stats));
  _last_error[0] = '\0';
}

void MeshcoreMapUploader::end() {
  _enabled.store(false, std::memory_order_release);
  _pending.store(false, std::memory_order_release);
  _pending_len = 0;
  _identity = nullptr;
  releaseScratch();
}

void MeshcoreMapUploader::setEnabled(bool enabled) {
  const bool was = _enabled.exchange(enabled, std::memory_order_acq_rel);
  if (was == enabled) return;
  if (!enabled) {
    // Drop anything staged; on re-enable the seen-table is deliberately kept, so
    // toggling the preset does not re-upload every node we already sent.
    _pending.store(false, std::memory_order_release);
    _pending_len = 0;
  }
  MAP_DEBUG_PRINTLN("uploader %s", enabled ? "enabled" : "disabled");
}

void MeshcoreMapUploader::offerAdvert(const uint8_t* frame, size_t len, uint32_t now_ms) {
  if (!isEnabled() || frame == nullptr) return;
  if (len == 0 || len > sizeof(_pending_frame)) return;

  // Core 1 does only the cheap work. The Ed25519 verify and the seen-table check
  // are Core 0's, so the radio path never pays for them.
  MapUploadPolicy::AdvertView view;
  if (!MapUploadPolicy::parseAdvertFrame(frame, len, &view)) return;
  if (!MapUploadPolicy::isMappableType(view.type)) return;

  if (_pending.load(std::memory_order_acquire)) {
    // One staging slot on purpose: an upload is at most one per
    // kMinUploadGapMs anyway, and adverts repeat. Core 0 releases the slot as
    // soon as it has *decided*, not when the POST finishes, so this is short.
    _stats.dropped_busy++;
    return;
  }

  memcpy(_pending_frame, frame, len);
  _pending_len = static_cast<uint8_t>(len);
  _pending_staged_ms = now_ms;
  _stats.staged++;
  _pending.store(true, std::memory_order_release);  // publishes the bytes above
}

void MeshcoreMapUploader::loop(const MapUploadRequest::RadioParams& radio, uint32_t now_ms) {
  if (!isEnabled() || _identity == nullptr) return;
  if (!_pending.load(std::memory_order_acquire)) return;

  // Everything below runs on Core 0 and owns the staged frame until it clears
  // _pending, so the buffer is stable without a lock.
  if (MapUploadPolicy::pendingExpired(now_ms, _pending_staged_ms)) {
    _stats.dropped_stale++;
    MAP_DEBUG_PRINTLN("dropping staged advert: waited too long");
    _pending.store(false, std::memory_order_release);
    return;
  }

#if defined(ARDUINO)
  if (WiFi.status() != WL_CONNECTED) return;  // keep it staged; staleness bounds the wait
#endif

  MapUploadPolicy::AdvertView view;
  if (!MapUploadPolicy::parseAdvertFrame(_pending_frame, _pending_len, &view)) {
    _pending.store(false, std::memory_order_release);
    return;
  }

  // Per-node rules first: they are free, and they release the staging slot for
  // another node without spending a TLS session.
  const uint32_t* last_ts = _seen.find(view.pub_key);
  const MapUploadPolicy::Verdict verdict = MapUploadPolicy::decideForAdvert(
      view.type, view.timestamp, last_ts != nullptr, last_ts != nullptr ? *last_ts : 0);
  if (verdict != MapUploadPolicy::Verdict::Upload) {
    _stats.rejected++;
    MAP_DEBUG_PRINTLN("skipping advert: %s", MapUploadPolicy::verdictName(verdict));
    _pending.store(false, std::memory_order_release);
    return;
  }

  if (!MapUploadRequest::paramsValid(radio)) {
    noteError("radio prefs not set");
    _pending.store(false, std::memory_order_release);
    return;
  }

  // From here the advert is worth sending, so hold the staging slot until it
  // either goes or is abandoned — pacing and heap are transient conditions.
  //
  // Both gates below come BEFORE the Ed25519 verify on purpose. This loop ticks
  // every 50 ms, so a deferred advert would otherwise be re-verified a few
  // hundred times while it waits out kMinUploadGapMs.
  if (!MapUploadPolicy::uploadGapElapsed(now_ms, _last_upload_ms, _any_upload_done)) return;

#if defined(ESP32)
  // Re-sample the heap at most once per pacing gap; the free-list walk is not
  // something to repeat per loop iteration.
  if (_last_memory_check_ms == 0 ||
      MapUploadPolicy::elapsedMs(now_ms, _last_memory_check_ms) > 5000) {
    _last_memory_check_ms = now_ms == 0 ? 1 : now_ms;
    _memory_pressure =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < kMinInternalBlockBytes;
  }
  if (_memory_pressure) {
    _stats.skipped_memory++;
    MAP_DEBUG_PRINTLN("deferring upload: internal heap below %u", (unsigned)kMinInternalBlockBytes);
    return;
  }
#endif

  // The advert carries its own signature; uploading one we have not verified
  // would let any transmitter put an arbitrary node on the map through us. The
  // reference tool checks this too. It runs here — past every gate, immediately
  // before the POST — so the cost is paid once per upload, not per loop tick,
  // and never on the radio path.
  {
    uint8_t message[PUB_KEY_SIZE + 4 + MAX_ADVERT_DATA_SIZE];
    const size_t msg_len = MapUploadPolicy::buildSignedAdvertMessage(view, message, sizeof(message));
    const mesh::Identity advertiser(view.pub_key);
    if (msg_len == 0 || !advertiser.verify(view.signature, message, static_cast<int>(msg_len))) {
      _stats.bad_signature++;
      MAP_DEBUG_PRINTLN("ignoring advert: signature verification failed");
      _pending.store(false, std::memory_order_release);
      return;
    }
  }

  if (!ensureScratch()) {
    _stats.skipped_memory++;
    return;  // transient: keep it staged, staleness bounds the wait
  }
  char* const data = _scratch;
  char* const body = _scratch + MapUploadRequest::kDataBufferSize;

  const int data_len = MapUploadRequest::buildSignedData(data, MapUploadRequest::kDataBufferSize,
                                                         radio, _pending_frame, _pending_len);
  if (data_len <= 0) {
    noteError("could not build upload payload");
    _pending.store(false, std::memory_order_release);
    return;
  }

  // Ed25519 over the SHA-256 digest of the data string — the reference tool signs
  // the digest, not the string, so the server verifies against the same 32 bytes.
  uint8_t digest[32];
  mesh::Utils::sha256(digest, sizeof(digest), reinterpret_cast<const uint8_t*>(data), data_len);
  uint8_t signature[SIGNATURE_SIZE];
  _identity->sign(signature, digest, sizeof(digest));

  const int body_len = MapUploadRequest::buildRequestBody(body, MapUploadRequest::kBodyBufferSize,
                                                          data, signature, sizeof(signature),
                                                          _identity->pub_key, PUB_KEY_SIZE);
  if (body_len <= 0) {
    noteError("could not build request body");
    _pending.store(false, std::memory_order_release);
    return;
  }

  const bool ok = postBody(body, static_cast<size_t>(body_len));

  // Pace off every attempt, not just the successes: a failing endpoint must not
  // turn into a tight loop of TLS handshakes.
  _last_upload_ms = now_ms;
  _any_upload_done = true;

  if (ok) {
    _stats.uploads_ok++;
    _last_error[0] = '\0';
    // Record only on success, so a failed upload is retried on the next advert
    // instead of being suppressed for an hour.
    _seen.record(view.pub_key, view.timestamp);
    _stats.tracked_nodes = static_cast<uint32_t>(_seen.size());
    MAP_DEBUG_PRINTLN("uploaded advert (type=%u, ts=%lu)", (unsigned)view.type,
                      (unsigned long)view.timestamp);
  } else {
    _stats.uploads_failed++;
  }

  _pending.store(false, std::memory_order_release);
}

bool MeshcoreMapUploader::ensureScratch() {
  if (_scratch != nullptr) return true;
  _scratch = static_cast<char*>(mapScratchAlloc(kScratchSize));
  if (_scratch == nullptr) {
    MAP_DEBUG_PRINTLN("payload scratch allocation failed (%u bytes)", (unsigned)kScratchSize);
    return false;
  }
  return true;
}

void MeshcoreMapUploader::releaseScratch() {
  if (_scratch == nullptr) return;
  free(_scratch);
  _scratch = nullptr;
}

bool MeshcoreMapUploader::postBody(const char* body, size_t body_len) {
#if defined(ARDUINO)
  WiFiClientSecure client;
  // A single pinned root, not the cert bundle: map.meshcore.io is Let's Encrypt
  // (YE1 -> Root YE -> ISRG Root X2 -> ISRG Root X1, cross-signed), and the
  // bundle verify is what exhausts internal heap next to live MQTT TLS sessions.
  client.setCACert(ISRG_ROOT_X1);
  client.setTimeout(kHttpTimeoutMs / 1000);

  HTTPClient http;
  if (!http.begin(client, kApiUrl)) {
    _stats.last_http_status = -1;
    noteError("connect failed");
    return false;
  }
  // Same reason as the OTA manifest fetch: HTTP/1.0 gets an unframed body
  // instead of a chunked stream this client would have to reassemble.
  http.useHTTP10(true);
  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);
  // The reference tool posts with fetch()'s default text/plain and the server
  // accepts it, so the endpoint parses the body regardless of this header;
  // application/json is simply the accurate description of what we send.
  http.addHeader("Content-Type", "application/json");

  const int code = http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(body)), body_len);
  _stats.last_http_status = code;

  const bool ok = (code >= 200 && code < 300);
  if (!ok) {
    if (code > 0) {
      // The API answers errors as JSON ({"error":...}); keep a bounded slice for
      // `get mqtt.status`. Only on failure, so the success path allocates nothing,
      // and only for a response small enough that reading it cannot itself be the
      // memory problem.
      const int len = http.getSize();
      if (len > 0 && len <= 256) {
        String payload = http.getString();
        noteError("HTTP %d: %.40s", code, payload.c_str());
      } else {
        noteError("HTTP %d", code);
      }
    } else {
      noteError("HTTP error %d", code);
    }
  }

  http.end();
  client.stop();
  return ok;
#else
  (void)body;
  (void)body_len;
  return false;
#endif
}

void MeshcoreMapUploader::noteError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(_last_error, sizeof(_last_error), fmt, args);
  va_end(args);
  MAP_DEBUG_PRINTLN("%s", _last_error);
}

void MeshcoreMapUploader::getStats(Stats* out) const {
  if (out == nullptr) return;
  *out = _stats;
  out->tracked_nodes = static_cast<uint32_t>(_seen.size());
}

void MeshcoreMapUploader::formatStatus(char* buf, size_t buf_size) const {
  if (buf == nullptr || buf_size == 0) return;
  if (!isEnabled()) {
    snprintf(buf, buf_size, "off");
    return;
  }
  // Surface the last error whenever there is one — a config problem (unset radio
  // prefs, payload build failure) never increments uploads_failed, and that is
  // exactly the case an operator needs told.
  if (_last_error[0] != '\0') {
    snprintf(buf, buf_size, "ok=%lu err=%lu nodes=%lu (%s)",
             (unsigned long)_stats.uploads_ok, (unsigned long)_stats.uploads_failed,
             (unsigned long)_seen.size(), _last_error);
    return;
  }
  snprintf(buf, buf_size, "ok=%lu err=%lu nodes=%lu",
           (unsigned long)_stats.uploads_ok, (unsigned long)_stats.uploads_failed,
           (unsigned long)_seen.size());
}

#endif  // WITH_MQTT_BRIDGE
