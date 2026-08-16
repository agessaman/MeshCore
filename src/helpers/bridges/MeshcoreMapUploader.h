#pragma once

#ifdef WITH_MQTT_BRIDGE

#include <stddef.h>
#include <stdint.h>
#include <atomic>

#include "Identity.h"
#include "MeshCore.h"
#include "helpers/MapUploadPolicy.h"
#include "helpers/MapUploadRequest.h"

/**
 * @brief Uploads heard adverts to the meshcore.io map.
 *
 * Selected per slot with `set mqttN.preset meshcore-map`. Unlike every other
 * preset this one is not an MQTT broker: it POSTs to
 * https://map.meshcore.io/api/v1/uploader/node over HTTPS, signing each upload
 * with the node's own Ed25519 identity. The wire format and the accept/reject
 * rules come from recrof/map.meshcore.io-uploader, the reference tool the map
 * operator documents; see MapUploadRequest.h for the request shape and
 * MapUploadPolicy.h for the rules.
 *
 * Deliberately NOT wired into MQTTSlot. A slot carries a PsychicMqttClient, a
 * JWT, a keepalive, a topic, and a reconnect ladder, none of which mean anything
 * for a stateless POST — threading a second transport through that machinery
 * would put ~50 new branches on the fleet's MQTT path to save one small object.
 * The slot preset name only flips this object on.
 *
 * Threading mirrors the bridge's existing staging idiom:
 * - offerAdvert() runs on Core 1 from the radio path. It does the cheap frame
 *   decode and stages at most one advert.
 * - loop() runs on Core 0 from the MQTT task and owns everything expensive: the
 *   seen-table, the Ed25519 verify of the advert, the signing, and the POST.
 * The handoff is a single release/acquire flag, like _neighbors_publish_pending.
 */
class MeshcoreMapUploader {
public:
  static const char kApiUrl[];

  // How many nodes we remember having uploaded. Each entry is ~20 bytes, and the
  // only cost of forgetting one is a duplicate upload the server will collapse,
  // so this is sized to cover a busy metro mesh on PSRAM and to stay small on
  // boards where internal DRAM is contended.
#if defined(BOARD_HAS_PSRAM)
  static const size_t kSeenNodes = 64;
#else
  static const size_t kSeenNodes = 24;
#endif

  // A POST opens a fresh TLS session (mbedTLS record buffers + socket), and the
  // handshake needs a contiguous internal block. Below this we skip rather than
  // compete with the live MQTT TLS sessions for the last of the internal heap —
  // ESP32Board's OTA path documents that same collapse on a no-PSRAM board.
  static const size_t kMinInternalBlockBytes = 42000;

  static const uint32_t kHttpTimeoutMs = 15000;

  MeshcoreMapUploader();

  // identity signs the uploads; it must outlive this object (the bridge owns it).
  void begin(const mesh::LocalIdentity* identity);
  void end();

  void setEnabled(bool enabled);
  bool isEnabled() const { return _enabled.load(std::memory_order_acquire); }

  /** Offer a raw received frame. Core 1 only. Cheap: decodes the frame and
   *  stages it only if it is an advert of a type the map plots. */
  void offerAdvert(const uint8_t* frame, size_t len, uint32_t now_ms);

  /** Do any pending upload. Core 0 (MQTT task) only — performs blocking network
   *  I/O. radio carries the live prefs values that describe this node's mesh. */
  void loop(const MapUploadRequest::RadioParams& radio, uint32_t now_ms);

  struct Stats {
    uint32_t uploads_ok;
    uint32_t uploads_failed;
    uint32_t staged;         // adverts accepted by the Core 1 pre-filter
    uint32_t dropped_busy;   // an upload was already staged
    uint32_t dropped_stale;  // waited past kMaxPendingAgeMs for WiFi/heap
    uint32_t rejected;       // seen-table/replay/too-soon
    uint32_t bad_signature;  // advert failed its own Ed25519 verify
    uint32_t skipped_memory; // deferred on low internal heap
    int last_http_status;    // <0 = HTTPClient/transport error
    uint32_t tracked_nodes;
  };
  void getStats(Stats* out) const;

  /** One-line summary for `get mqtt.status`. Never writes more than buf_size. */
  void formatStatus(char* buf, size_t buf_size) const;

  /** Last failure text (empty when none). Core 0 writes, CLI reads; a torn read
   *  of a diagnostic string is harmless. */
  const char* lastError() const { return _last_error; }

  // Payload scratch: the signed data string followed by the request body.
  static const size_t kScratchSize =
      MapUploadRequest::kDataBufferSize + MapUploadRequest::kBodyBufferSize;

private:
  // Returns true on a 2xx. Sets _last_http_status and, on failure, _last_error.
  bool postBody(const char* body, size_t body_len);
  void noteError(const char* fmt, ...);
  // Allocate _scratch on first use. False when the heap could not spare it (the
  // upload is then deferred, not failed).
  bool ensureScratch();
  void releaseScratch();

  const mesh::LocalIdentity* _identity;
  std::atomic<bool> _enabled;

  // Core 1 stages here, Core 0 consumes. _pending is the release/acquire fence:
  // the frame bytes are written before it is set and read after it is seen.
  uint8_t _pending_frame[MapUploadRequest::kMaxFrameBytes];
  uint8_t _pending_len;
  uint32_t _pending_staged_ms;
  std::atomic<bool> _pending;

  // Core 0 only. Allocated on the first upload and held until end(): 1.7 KB is
  // too much to put on the 8 KB MQTT task stack next to an mbedTLS handshake,
  // and allocating it per upload would churn the very heap the TLS session needs.
  // PSRAM where the board has it, like the bridge's own publish buffer.
  char* _scratch;

  MapUploadPolicy::SeenTable<kSeenNodes> _seen;
  uint32_t _last_upload_ms;
  bool _any_upload_done;
  uint32_t _last_memory_check_ms;
  bool _memory_pressure;

  Stats _stats;
  char _last_error[80];
};

#endif  // WITH_MQTT_BRIDGE
