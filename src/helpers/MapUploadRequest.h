#pragma once

#include <stddef.h>
#include <stdint.h>

#include "MeshCore.h"  // MAX_TRANS_UNIT, PUB_KEY_SIZE, SIGNATURE_SIZE

// Wire-format builder for the meshcore.io map uploader API.
//
// The request the map expects (see recrof/map.meshcore.io-uploader) is:
//
//   POST https://map.meshcore.io/api/v1/uploader/node
//   {"data":"<json>","signature":"<hex>","publicKey":"<hex>"}
//
// where <json> is itself a JSON *string* — the exact bytes that were hashed and
// signed — carrying the receiving node's radio parameters and the advert it
// heard, verbatim, as a hex "meshcore://" link:
//
//   {"params":{"freq":910.525,"cr":5,"sf":10,"bw":250},
//    "links":["meshcore://<raw advert frame in hex>"]}
//
// The signature is Ed25519 over the SHA-256 digest of that string, made with the
// *receiving* node's identity key — it attests "this node heard this advert",
// not anything about the advert itself (the advert carries its own signature,
// which the server can check independently).
//
// Because the signature covers the exact bytes of the data string, this class
// builds that string once and hands the same buffer to both the hasher and the
// request body. Everything here is deterministic and free of Arduino, WiFi, and
// mesh runtime dependencies, so it is exercised by test/test_map_upload_request/.
class MapUploadRequest {
public:
  // Radio parameters as the API wants them. Units match NodePrefs exactly, so
  // the binding passes prefs fields straight through: the reference tool derives
  // the same two values by dividing the companion protocol's kHz/Hz integers.
  struct RadioParams {
    float freq_mhz;  // NodePrefs::freq, e.g. 910.525
    float bw_khz;    // NodePrefs::bw, e.g. 250
    uint8_t sf;      // NodePrefs::sf
    uint8_t cr;      // NodePrefs::cr
  };

  // Largest advert frame we will ever hex-encode (one LoRa transmission unit).
  static const size_t kMaxFrameBytes = MAX_TRANS_UNIT;

  // Buffer sizes callers should use. Both are checked against the worst case by
  // static_asserts in the .cpp, so a wire-format change cannot silently start
  // truncating.
  static const size_t kDataBufferSize = 640;
  static const size_t kBodyBufferSize = 1024;

  // False when the radio prefs cannot produce a meaningful map fix (unset or
  // nonsensical). An upload with freq 0 would place the node on a mesh that does
  // not exist, so the binding refuses rather than sending it.
  static bool paramsValid(const RadioParams& params);

  // Build the exact string that gets hashed, signed, and embedded as "data".
  // Field order matches the reference tool's object literal; the server hashes
  // whatever we send, so order is not load-bearing for verification, but keeping
  // it identical means our payload is byte-comparable with the operator's own
  // tool when debugging a rejection.
  // Returns the length written, or 0 on failure (output is emptied).
  static int buildSignedData(char* out, size_t out_size, const RadioParams& params,
                             const uint8_t* frame, size_t frame_len);

  // Wrap a built data string plus its signature into the POST body.
  // Returns the length written, or 0 on failure (output is emptied).
  static int buildRequestBody(char* out, size_t out_size, const char* data_json,
                              const uint8_t* signature, size_t signature_len,
                              const uint8_t* public_key, size_t public_key_len);

  // --- Pieces, exposed for direct testing -----------------------------------

  // JSON number formatting that matches JavaScript's: no trailing zeros, no
  // trailing point, so 250.0 renders "250" and 910.525 renders "910.525".
  // Returns the length written, or 0 if it did not fit or the value is not
  // finite.
  static size_t formatNumber(char* out, size_t out_size, float value);

  // Lowercase hex, matching the reference tool's BufferUtils.bytesToHex().
  // Returns the length written (2 * len), or 0 if it did not fit.
  static size_t toHexLower(char* out, size_t out_size, const uint8_t* bytes, size_t len);

  // Escape a string for embedding as a JSON string value. Our own generated JSON
  // only ever needs the quote rule, but control characters and backslashes are
  // handled too so this cannot emit invalid JSON for an unexpected input.
  // Returns the length written, or 0 if it did not fit.
  static size_t escapeJsonString(char* out, size_t out_size, const char* in);
};
