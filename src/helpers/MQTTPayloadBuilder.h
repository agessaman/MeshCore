#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

// Connection health of the MQTT slots themselves, reported inside a status
// publication. A slot that cannot reach its broker cannot report its own outage,
// so this travels on every *healthy* slot's status message and is how a fleet
// learns that a sibling slot is down. Publish-success/error counters cannot show
// this: a slot that never connects never attempts a publish, so its error count
// stays at zero for the entire outage.
//
// `heap_largest` is the leading indicator. Observed on non-PSRAM hardware: a slot
// needs one contiguous 16,384-byte block for its TLS record buffers, and measured
// immediately before an attempt this value predicted the outcome every time. Free
// heap alone does not — an 85 KB-free board sat unable to connect for hours.
// A low value with all slots up is normal, not a fault: established sessions
// already hold their buffers. It means only that the next slot to ask will fail.
// Sampled at status-publish time, not at the attempt, so it is a trend, not a
// per-attempt reading.
struct MQTTConnHealth {
  int slots_up = -1;             // -1 = not supplied, field omitted
  int slots_total = -1;          // enabled, fully configured slots, connected or not
  int worst_outage_secs = -1;    // longest timed outage; -1 = none timed (see below)
  int heap_largest = -1;         // largest free internal block, bytes
  // Attempts that ended without reaching onConnect, summed across all slots since
  // boot. A slot stuck retrying moves this while its publish counters stay frozen,
  // which is what separates "actively failing" from "idle and quiet".
  int connect_failures = -1;
  // Slots parked by the circuit breaker. Materially worse than a retrying slot:
  // the breaker only probes every 30 minutes, so recovery is far slower.
  int slots_breaker = -1;
};

// Mesh-independent JSON serialization core for MQTT publication payloads.
// MQTTMessageBuilder keeps the firmware-facing API and delegates these three
// deterministic contracts here so they can be exercised by native tests.

// RSSI sentinel for "no radio measurement": far outside the small negative ints
// a radio actually reports, so a real reading can never collide with it.
static const int MQTT_RSSI_UNKNOWN = INT16_MIN;

class MQTTPayloadBuilder {
public:
  static int buildStatusMessage(
    JsonDocument& doc,
    const char* origin,
    const char* origin_id,
    const char* model,
    const char* firmware_version,
    const char* radio,
    const char* client_version,
    const char* status,
    const char* timestamp,
    char* buffer,
    size_t buffer_size,
    int battery_mv = -1,
    int uptime_secs = -1,
    int errors = -1,
    int queue_len = -1,
    int noise_floor = -999,
    int tx_air_secs = -1,
    int rx_air_secs = -1,
    int recv_errors = -1,
    int internal_heap = -1,
    int packets_sent = -1,
    int packets_received = -1,
    const char* repeat = nullptr,
    const MQTTConnHealth& conn_health = MQTTConnHealth()
  );

  static int buildPacketMessage(
    JsonDocument& doc,
    const char* origin,
    const char* origin_id,
    const char* timestamp,
    const char* direction,
    const char* time,
    const char* date,
    int len,
    int packet_type,
    const char* route,
    int payload_len,
    const char* raw,
    float snr,
    int rssi,
    float score,
    const char* hash,
    const uint8_t* path_bytes,
    int path_hop_count,
    int path_hash_size,
    size_t max_path_bytes,
    char* buffer,
    size_t buffer_size
  );

  static int buildRawMessage(
    JsonDocument& doc,
    const char* origin,
    const char* origin_id,
    const char* timestamp,
    const char* raw,
    char* buffer,
    size_t buffer_size
  );

  struct NeighborsMessageEntry {
    const char* pubkey_hex;
    float snr;
    uint32_t heard_secs_ago;
    const char* scopes;
    const char* status;
    // True renders heard_secs_ago as JSON null, for a neighbour whose stored
    // stamp cannot yield an age. No default initializer: the struct stays an
    // aggregate for the device toolchain, and a zeroed tail means "age known",
    // so size-measurement callers keep reserving the widest numeric value.
    bool heard_unknown;
  };

  // Build neighbors-table JSON for the meshcore/{iata}/{device}/neighbors topic.
  // Callers order entries most- to least-useful; document growth is bounded to
  // buffer_size and the remaining tail is dropped once the next entry won't fit.
  static int buildNeighborsMessage(
    JsonDocument& doc,
    const char* origin,
    const char* origin_id,
    const char* timestamp,
    const char* self_scopes,
    const char* self_default_scope,
    const NeighborsMessageEntry* neighbors,
    int neighbor_count,
    char* buffer,
    size_t buffer_size,
    int total_neighbors = -1,
    int queried_neighbors = -1,
    bool truncated = false
  );

  // Exact serialized-size components used by paced neighbor discovery. The
  // base reserves the largest progress metadata values for this snapshot;
  // callers add each entry size plus one byte for commas after the first.
  static size_t measureNeighborsMessageBase(
    const char* origin,
    const char* origin_id,
    const char* timestamp,
    const char* self_scopes,
    const char* self_default_scope,
    int total_neighbors
  );
  static size_t measureNeighborsMessageEntry(const NeighborsMessageEntry& neighbor);
};
