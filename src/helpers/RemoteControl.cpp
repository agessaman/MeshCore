#include "RemoteControl.h"

#include <ctype.h>

namespace {

// Case-insensitive equality (ASCII). Avoids depending on <strings.h>.
bool ciEquals(const char* a, const char* b) {
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    a++; b++;
  }
  return *a == '\0' && *b == '\0';
}

// Parse exactly 64 hex chars into a RC_PUB_KEY_SIZE-byte key. false otherwise.
bool hexToKey(const char* hex, uint8_t* out) {
  if (!hex || strlen(hex) != RC_PUB_KEY_SIZE * 2) return false;
  for (int i = 0; i < RC_PUB_KEY_SIZE; i++) {
    int hi = -1, lo = -1;
    char c = hex[i * 2];
    char d = hex[i * 2 + 1];
    hi = (c >= '0' && c <= '9') ? c - '0'
       : (c >= 'a' && c <= 'f') ? c - 'a' + 10
       : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    lo = (d >= '0' && d <= '9') ? d - '0'
       : (d >= 'a' && d <= 'f') ? d - 'a' + 10
       : (d >= 'A' && d <= 'F') ? d - 'A' + 10 : -1;
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

}  // namespace

RemoteControl::RemoteControl(RemoteControlCrypto* crypto,
                             RemoteControlAuthorizer* authorizer,
                             RemoteControlExecutor* executor,
                             RemoteControlClock* clock)
    : _crypto(crypto), _authorizer(authorizer), _executor(executor), _clock(clock) {}

RemoteControl::Outcome RemoteControl::error(const char* device_id, const char* request_id,
                                            const char* message, char* out_jwt,
                                            size_t out_jwt_size) {
  unsigned long iat = _clock->unixNow();
  if (iat == 0) iat = _clock->millisNow() / 1000;  // fallback before NTP sync

  RemoteCommandResponse resp;
  resp.device_id = device_id;
  resp.command = "";
  resp.request_id = request_id ? request_id : "";
  resp.success = false;
  resp.response = message;
  resp.iat = iat;
  resp.exp = iat + RESPONSE_TTL_SEC;

  if (!_crypto->signResponse(resp, out_jwt, out_jwt_size)) {
    return Outcome::SignFailed;
  }
  return Outcome::ResponseReady;
}

RemoteControl::Outcome RemoteControl::process(const char* token, const char* device_id,
                                              char* out_jwt, size_t out_jwt_size) {
  if (!token || !device_id || !out_jwt || out_jwt_size == 0) {
    return Outcome::SilentIgnore;
  }

  RemoteCommandRequest req;
  memset(&req, 0, sizeof(req));
  if (!_crypto->parseRequest(token, req)) {
    return error(device_id, "", "Invalid or unparseable command token", out_jwt, out_jwt_size);
  }

  // Target filtering: silently ignore commands addressed to another device.
  if (req.target[0] != '\0' && !ciEquals(req.target, device_id)) {
    return Outcome::SilentIgnore;
  }

  if (req.command[0] == '\0') {
    return error(device_id, req.nonce, "Invalid command", out_jwt, out_jwt_size);
  }

  // Early replay check (before the expensive signature verify).
  if (req.nonce[0] != '\0' && _nonces.isUsed(req.nonce)) {
    return error(device_id, req.nonce, "Nonce already used - possible replay", out_jwt, out_jwt_size);
  }

  // Early rate-limit check keyed on the claimed public key.
  uint8_t claimed_key[RC_PUB_KEY_SIZE];
  bool have_claimed_key = hexToKey(req.public_key, claimed_key);
  if (have_claimed_key && _rate_limiter.isRateLimited(claimed_key, _clock->millisNow())) {
    return error(device_id, req.nonce, "Rate limit exceeded - too many commands", out_jwt, out_jwt_size);
  }

  // Verify the signature and recover the actual signing key.
  char signer_hex[65];
  signer_hex[0] = '\0';
  if (!_crypto->verifySignature(token, signer_hex, sizeof(signer_hex))) {
    return error(device_id, req.nonce, "Invalid token signature", out_jwt, out_jwt_size);
  }

  // The claimed key (if present) must match the key that actually signed.
  if (req.public_key[0] != '\0' && !ciEquals(req.public_key, signer_hex)) {
    return error(device_id, req.nonce, "Public key mismatch in token", out_jwt, out_jwt_size);
  }

  uint8_t signer_key[RC_PUB_KEY_SIZE];
  if (!hexToKey(signer_hex, signer_key)) {
    return error(device_id, req.nonce, "Invalid public key format in token", out_jwt, out_jwt_size);
  }

  // Policy checks.
  if (_blacklist.isBlacklisted(req.command)) {
    return error(device_id, req.nonce, "Command not allowed via remote execution", out_jwt, out_jwt_size);
  }
  if (strncmp(req.command, "reboot", 6) == 0) {
    return error(device_id, req.nonce, "Reboot not allowed via remote execution", out_jwt, out_jwt_size);
  }
  if (!_authorizer->authorize(signer_key, RC_PUB_KEY_SIZE)) {
    return error(device_id, req.nonce,
                 _authorizer->useACL() ? "Unauthorized: public key not in ACL admin list"
                                       : "Unauthorized: public key mismatch",
                 out_jwt, out_jwt_size);
  }

  // Authorized: record the nonce so it cannot be replayed.
  if (req.nonce[0] != '\0') {
    _nonces.add(req.nonce);
  }

  // Execute with a wall-clock timeout guard.
  char reply[256];
  reply[0] = '\0';
  unsigned long start = _clock->millisNow();
  _executor->execute(req.command, reply, sizeof(reply));
  if ((_clock->millisNow() - start) > COMMAND_TIMEOUT_MS) {
    return error(device_id, req.nonce, "Command execution timeout", out_jwt, out_jwt_size);
  }

  unsigned long iat = _clock->unixNow();
  if (iat == 0) iat = _clock->millisNow() / 1000;

  RemoteCommandResponse resp;
  resp.device_id = device_id;
  resp.command = req.command;
  resp.request_id = req.nonce;
  resp.success = true;
  resp.response = reply;
  resp.iat = iat;
  resp.exp = iat + RESPONSE_TTL_SEC;

  if (!_crypto->signResponse(resp, out_jwt, out_jwt_size)) {
    return Outcome::SignFailed;
  }
  return Outcome::ResponseReady;
}
