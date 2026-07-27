#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Remote command execution policy engine.
//
// This class owns the security-critical *decisions* for JWT-authenticated
// remote serial commands (replay protection, rate limiting, command blacklist,
// target filtering, authorization ordering) while delegating all I/O, JSON,
// base64 and Ed25519 work to injected seams. That split lets the whole pipeline
// be unit-tested on the host without MQTT, ArduinoJson or crypto — see
// test/test_remote_control. The firmware wires the seams to JWTHelper,
// LocalIdentity, the ACL and the CLI in MQTTBridge.

// Public-key length in bytes. Kept independent of mesh headers so this unit
// compiles standalone on the host; MQTTBridge static_asserts it == PUB_KEY_SIZE.
static const int RC_PUB_KEY_SIZE = 32;

// Fields parsed from an *unverified* JWT payload, used for the cheap early
// checks that run before signature verification.
struct RemoteCommandRequest {
  char command[256];
  char target[65];       // intended device id (hex pubkey); empty = broadcast
  char nonce[48];        // UUID-style request id; empty = no replay protection
  char public_key[65];   // signer pubkey claimed in the payload (hex); empty = absent
};

// Fields the engine hands to the crypto seam to build a signed response token.
struct RemoteCommandResponse {
  const char* device_id;   // our public key (hex) — the response signer
  const char* command;     // echoed command ("" for errors)
  const char* request_id;  // the originating nonce ("" if none)
  bool success;
  const char* response;    // command reply, or error message
  unsigned long iat;       // issued-at (unix seconds)
  unsigned long exp;       // expiry (unix seconds)
};

// Seam: JSON / base64 / Ed25519. Firmware impl wraps JWTHelper + LocalIdentity.
class RemoteControlCrypto {
public:
  virtual ~RemoteControlCrypto() {}
  // Decode the payload segment and extract fields. false if the token is
  // malformed or the payload cannot be parsed. No signature check here.
  virtual bool parseRequest(const char* token, RemoteCommandRequest& out) = 0;
  // Verify the token signature and extract the signing key (hex) into
  // out_pubkey_hex (>= 65 bytes). false if the signature does not verify.
  virtual bool verifySignature(const char* token, char* out_pubkey_hex, size_t out_size) = 0;
  // Serialize + sign the response into out_jwt. false on failure.
  virtual bool signResponse(const RemoteCommandResponse& resp, char* out_jwt, size_t out_size) = 0;
};

// Seam: authorization. Firmware impl combines the ACL admin list with the
// explicit admin-key preference, selected by the mqtt.useacl flag.
class RemoteControlAuthorizer {
public:
  virtual ~RemoteControlAuthorizer() {}
  virtual bool useACL() = 0;                                    // for error-message wording
  virtual bool authorize(const uint8_t* pubkey, size_t len) = 0;
};

// Seam: command execution. Firmware impl calls CommonCLI::handleCommand.
class RemoteControlExecutor {
public:
  virtual ~RemoteControlExecutor() {}
  virtual void execute(const char* command, char* reply, size_t reply_size) = 0;
};

// Seam: time. Firmware impl uses millis() and time(nullptr).
class RemoteControlClock {
public:
  virtual ~RemoteControlClock() {}
  virtual unsigned long millisNow() = 0;
  virtual unsigned long unixNow() = 0;   // unix seconds, 0 if the clock is unset
};

// Tracks recently-seen nonces to reject replays. Fixed-size circular buffer.
class RCNonceTracker {
public:
  static const int MAX_NONCES = 10;
  RCNonceTracker() : _index(0) { memset(_nonces, 0, sizeof(_nonces)); }
  bool isUsed(const char* nonce) const {
    for (int i = 0; i < MAX_NONCES; i++) {
      if (_nonces[i][0] != '\0' && strcmp(_nonces[i], nonce) == 0) return true;
    }
    return false;
  }
  void add(const char* nonce) {
    strncpy(_nonces[_index], nonce, sizeof(_nonces[_index]) - 1);
    _nonces[_index][sizeof(_nonces[_index]) - 1] = '\0';
    _index = (_index + 1) % MAX_NONCES;
  }
private:
  char _nonces[MAX_NONCES][48];
  uint8_t _index;
};

// Per-public-key minimum interval between commands. Evicts the oldest key when
// full. `now` is supplied by the caller so this stays pure/testable.
class RCRateLimiter {
public:
  static const int MAX_TRACKED_KEYS = 20;
  static const unsigned long MIN_INTERVAL_MS = 1000;
  RCRateLimiter() : _num_tracked(0) {
    memset(_last_ms, 0, sizeof(_last_ms));
    memset(_keys, 0, sizeof(_keys));
  }
  bool isRateLimited(const uint8_t* pubkey, unsigned long now) {
    int idx = findKey(pubkey);
    if (idx < 0) idx = addKey(pubkey);
    if (idx < 0) return false;
    if (_last_ms[idx] != 0 && (now - _last_ms[idx]) < MIN_INTERVAL_MS) return true;
    _last_ms[idx] = now;
    return false;
  }
private:
  int findKey(const uint8_t* pubkey) const {
    for (int i = 0; i < _num_tracked; i++) {
      if (memcmp(_keys[i], pubkey, RC_PUB_KEY_SIZE) == 0) return i;
    }
    return -1;
  }
  int addKey(const uint8_t* pubkey) {
    int idx;
    if (_num_tracked >= MAX_TRACKED_KEYS) {
      idx = 0;
      for (int i = 1; i < MAX_TRACKED_KEYS; i++) {
        if (_last_ms[i] < _last_ms[idx]) idx = i;
      }
    } else {
      idx = _num_tracked++;
    }
    memcpy(_keys[idx], pubkey, RC_PUB_KEY_SIZE);
    _last_ms[idx] = 0;
    return idx;
  }
  unsigned long _last_ms[MAX_TRACKED_KEYS];
  uint8_t _keys[MAX_TRACKED_KEYS][RC_PUB_KEY_SIZE];
  uint8_t _num_tracked;
};

// Commands that may never run remotely, matched by prefix.
class RCCommandBlacklist {
public:
  RCCommandBlacklist() : _count(0) {
    add("get wifi.pwd");     // Wi-Fi password
    add("set mqtt.admin");   // admin key (security-critical)
  }
  bool add(const char* prefix) {
    if (_count >= MAX_ENTRIES) return false;
    _entries[_count++] = prefix;
    return true;
  }
  bool isBlacklisted(const char* command) const {
    for (int i = 0; i < _count; i++) {
      if (strncmp(command, _entries[i], strlen(_entries[i])) == 0) return true;
    }
    return false;
  }
private:
  static const int MAX_ENTRIES = 20;
  const char* _entries[MAX_ENTRIES];
  int _count;
};

class RemoteControl {
public:
  enum class Outcome {
    SilentIgnore,    // not addressed to us / nothing to send
    ResponseReady,   // out_jwt holds a signed response (success or error) to publish
    SignFailed,      // response could not be built (log only)
  };

  RemoteControl(RemoteControlCrypto* crypto,
                RemoteControlAuthorizer* authorizer,
                RemoteControlExecutor* executor,
                RemoteControlClock* clock);

  // Run the full pipeline for one inbound command token. `device_id` is our
  // public key in hex (used for target matching and as the response signer).
  Outcome process(const char* token, const char* device_id, char* out_jwt, size_t out_jwt_size);

  // Longest a remote command may run before its reply is rejected.
  static const unsigned long COMMAND_TIMEOUT_MS = 5000;
  // Response token lifetime.
  static const unsigned long RESPONSE_TTL_SEC = 60;

  RCCommandBlacklist& blacklist() { return _blacklist; }

private:
  Outcome error(const char* device_id, const char* request_id, const char* message,
                char* out_jwt, size_t out_jwt_size);

  RemoteControlCrypto* _crypto;
  RemoteControlAuthorizer* _authorizer;
  RemoteControlExecutor* _executor;
  RemoteControlClock* _clock;

  RCNonceTracker _nonces;
  RCRateLimiter _rate_limiter;
  RCCommandBlacklist _blacklist;
};
