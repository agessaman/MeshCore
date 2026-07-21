#include "JWTHelper.h"
#include <ArduinoJson.h>
#include <SHA256.h>
#include <string.h>
#include <stdlib.h>
#include "ed_25519.h"
#include "mbedtls/base64.h"

// Base64 URL encoding table (without padding)
static const char base64url_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

bool JWTHelper::createAuthToken(
  const mesh::LocalIdentity& identity,
  const char* audience,
  unsigned long issuedAt,
  unsigned long expiresIn,
  char* token,
  size_t tokenSize,
  const char* owner,
  const char* client,
  const char* email
) {
  if (!audience || !token || tokenSize == 0) {
    return false;
  }
  
  // Use current time if not specified
  if (issuedAt == 0) {
    issuedAt = time(nullptr);
  }
  
  // Create header
  char header[256];
  size_t headerLen = createHeader(header, sizeof(header));
  if (headerLen == 0) {
    return false;
  }
  
  // Get public key as UPPERCASE HEX string
  char publicKeyHex[65];
  mesh::Utils::toHex(publicKeyHex, identity.pub_key, PUB_KEY_SIZE);
  for (int i = 0; publicKeyHex[i]; i++) {
    publicKeyHex[i] = toupper(publicKeyHex[i]);
  }
  
  // Create payload
  char payload[512];
  size_t payloadLen = createPayload(publicKeyHex, audience, issuedAt, expiresIn, payload, sizeof(payload), owner, client, email);
  if (payloadLen == 0) {
    return false;
  }
  
  // Create signing input: header.payload
  char signingInput[768];
  size_t signingInputLen = headerLen + 1 + payloadLen;
  if (signingInputLen >= sizeof(signingInput)) {
    return false;
  }
  
  memcpy(signingInput, header, headerLen);
  signingInput[headerLen] = '.';
  memcpy(signingInput + headerLen + 1, payload, payloadLen);
  
  // Sign the data using direct Ed25519 signing
  uint8_t signature[64];
  mesh::LocalIdentity identity_copy = identity;
  
  uint8_t export_buffer[96];
  size_t exported_size = identity_copy.writeTo(export_buffer, sizeof(export_buffer));
  
  if (exported_size != 96) {
    return false;
  }
  
  uint8_t* private_key = export_buffer;
  uint8_t* public_key = export_buffer + 64;
  
  ed25519_sign(signature, (const unsigned char*)signingInput, signingInputLen, public_key, private_key);
  
  // Verify the signature locally
  int verify_result = ed25519_verify(signature, (const unsigned char*)signingInput, signingInputLen, public_key);
  if (verify_result != 1) {
    if (Serial.availableForWrite() > 0) Serial.println("JWTHelper: Signature verification failed!");
    return false;
  }
  
  // Convert signature to hex
  char signatureHex[129];
  for (int i = 0; i < 64; i++) {
    sprintf(signatureHex + (i * 2), "%02X", signature[i]);
  }
  signatureHex[128] = '\0';
  
  // Create final token: header.payload.signatureHex (MeshCore Decoder format)
  size_t sigHexLen = strlen(signatureHex);
  size_t totalLen = headerLen + 1 + payloadLen + 1 + sigHexLen;
  if (totalLen >= tokenSize) {
    return false;
  }
  
  memcpy(token, header, headerLen);
  token[headerLen] = '.';
  memcpy(token + headerLen + 1, payload, payloadLen);
  token[headerLen + 1 + payloadLen] = '.';
  memcpy(token + headerLen + 1 + payloadLen + 1, signatureHex, sigHexLen);
  token[totalLen] = '\0';

  return true;
}

size_t JWTHelper::base64UrlEncode(const uint8_t* input, size_t inputLen, char* output, size_t outputSize) {
  if (!input || !output || outputSize == 0) {
    return 0;
  }
  
  size_t outlen = 0;
  int ret = mbedtls_base64_encode((unsigned char*)output, outputSize - 1, &outlen, input, inputLen);
  
  if (ret != 0) {
    return 0;
  }
  
  // Convert to base64 URL format in-place (replace + with -, / with _, remove padding =)
  for (size_t i = 0; i < outlen; i++) {
    if (output[i] == '+') {
      output[i] = '-';
    } else if (output[i] == '/') {
      output[i] = '_';
    }
  }
  
  // Remove padding '=' characters
  while (outlen > 0 && output[outlen-1] == '=') {
    outlen--;
  }
  output[outlen] = '\0';
  return outlen;
}

size_t JWTHelper::createHeader(char* output, size_t outputSize) {
  // Create JWT header: {"alg":"Ed25519","typ":"JWT"}
  DynamicJsonDocument doc(256);
  doc["alg"] = "Ed25519";
  doc["typ"] = "JWT";
  
  char jsonBuffer[256];
  size_t len = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
  if (len == 0 || len >= sizeof(jsonBuffer)) {
    return 0;
  }
  
  return base64UrlEncode((uint8_t*)jsonBuffer, len, output, outputSize);
}

size_t JWTHelper::createPayload(
  const char* publicKey,
  const char* audience,
  unsigned long issuedAt,
  unsigned long expiresIn,
  char* output,
  size_t outputSize,
  const char* owner,
  const char* client,
  const char* email
) {
  // Create JWT payload
  DynamicJsonDocument doc(512);
  doc["publicKey"] = publicKey;
  doc["aud"] = audience;
  doc["iat"] = issuedAt;
  
  if (expiresIn > 0) {
    doc["exp"] = issuedAt + expiresIn;
  }
  
  // Add optional owner field if provided
  if (owner && strlen(owner) > 0) {
    doc["owner"] = owner;
  }
  
  // Add optional client field if provided
  if (client && strlen(client) > 0) {
    doc["client"] = client;
  }
  
  // Add optional email field if provided
  if (email && strlen(email) > 0) {
    doc["email"] = email;
  }
  
  char jsonBuffer[512];
  size_t len = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
  if (len == 0 || len >= sizeof(jsonBuffer)) {
    return 0;
  }

  return base64UrlEncode((uint8_t*)jsonBuffer, len, output, outputSize);
}

size_t JWTHelper::base64UrlDecode(const char* input, uint8_t* output, size_t outputSize) {
  if (!input || !output || outputSize == 0) {
    return 0;
  }

  size_t inputLen = strlen(input);
  if (inputLen == 0) {
    return 0;
  }

  // base64url -> base64 (with padding) in a heap buffer; keeps the MQTT task stack small.
  char* b64 = (char*)malloc(inputLen + 4 + 1);
  if (!b64) {
    return 0;
  }
  for (size_t i = 0; i < inputLen; i++) {
    char c = input[i];
    b64[i] = (c == '-') ? '+' : (c == '_') ? '/' : c;
  }
  size_t padding = (4 - (inputLen % 4)) % 4;
  for (size_t i = 0; i < padding; i++) {
    b64[inputLen + i] = '=';
  }
  b64[inputLen + padding] = '\0';

  size_t outlen = 0;
  int ret = mbedtls_base64_decode(output, outputSize, &outlen,
                                  (const unsigned char*)b64, inputLen + padding);
  free(b64);
  return (ret != 0) ? 0 : outlen;
}

bool JWTHelper::verifyToken(
  const char* token,
  const uint8_t* expected_public_key,
  size_t key_len,
  char* extracted_public_key,
  size_t extracted_key_size,
  char* extracted_nonce,
  size_t nonce_size,
  unsigned long* issued_at,
  unsigned long* expires_at
) {
  if (!token || !extracted_public_key || extracted_key_size < 65) {
    return false;
  }

  // Split header.payload.signature
  const char* dot1 = strchr(token, '.');
  if (!dot1) return false;
  const char* dot2 = strchr(dot1 + 1, '.');
  if (!dot2) return false;

  size_t headerLen = dot1 - token;
  size_t payloadLen = dot2 - (dot1 + 1);
  size_t signatureLen = strlen(dot2 + 1);

  // Decode and parse the payload JSON (heap-allocated to spare the task stack).
  char* payload_b64 = (char*)malloc(payloadLen + 1);
  if (!payload_b64) return false;
  memcpy(payload_b64, dot1 + 1, payloadLen);
  payload_b64[payloadLen] = '\0';

  char* payload = (char*)malloc(512);
  if (!payload) { free(payload_b64); return false; }
  size_t payloadDecodedLen = base64UrlDecode(payload_b64, (uint8_t*)payload, 512);
  free(payload_b64);
  if (payloadDecodedLen == 0) { free(payload); return false; }
  payload[payloadDecodedLen] = '\0';

  DynamicJsonDocument* doc = new DynamicJsonDocument(512);
  if (!doc) { free(payload); return false; }
  DeserializationError error = deserializeJson(*doc, payload);
  free(payload);
  if (error) { delete doc; return false; }

  // publicKey claim (64 hex chars) is mandatory
  const char* pubkey_str = (*doc)["publicKey"];
  if (!pubkey_str || strlen(pubkey_str) != 64) { delete doc; return false; }
  strncpy(extracted_public_key, pubkey_str, extracted_key_size - 1);
  extracted_public_key[extracted_key_size - 1] = '\0';

  if (extracted_nonce && nonce_size > 0) {
    const char* nonce_str = (*doc)["nonce"];
    if (nonce_str) {
      strncpy(extracted_nonce, nonce_str, nonce_size - 1);
      extracted_nonce[nonce_size - 1] = '\0';
    } else {
      extracted_nonce[0] = '\0';
    }
  }

  unsigned long iat = doc->containsKey("iat") ? (*doc)["iat"].as<unsigned long>() : 0;
  unsigned long exp = doc->containsKey("exp") ? (*doc)["exp"].as<unsigned long>() : 0;
  if (issued_at) *issued_at = iat;
  if (expires_at) *expires_at = exp;
  delete doc;

  // Reject expired tokens when the clock is set and an exp claim is present.
  if (exp > 0) {
    unsigned long current_time = time(nullptr);
    if (current_time > 0 && current_time >= exp) {
      return false;
    }
  }

  uint8_t pubkey_bytes[PUB_KEY_SIZE];
  if (!mesh::Utils::fromHex(pubkey_bytes, PUB_KEY_SIZE, extracted_public_key)) {
    return false;
  }
  if (expected_public_key && key_len == PUB_KEY_SIZE) {
    if (memcmp(pubkey_bytes, expected_public_key, PUB_KEY_SIZE) != 0) {
      return false;
    }
  }

  // Decode the signature: hex (128 chars) or base64url.
  uint8_t signature[64];
  bool is_hex = (signatureLen == 128);
  if (is_hex) {
    for (size_t i = 0; i < signatureLen; i++) {
      char c = dot2[1 + i];
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
        is_hex = false;
        break;
      }
    }
  }
  if (is_hex) {
    if (!mesh::Utils::fromHex(signature, 64, dot2 + 1)) return false;
  } else {
    char* sig_b64 = (char*)malloc(signatureLen + 1);
    if (!sig_b64) return false;
    memcpy(sig_b64, dot2 + 1, signatureLen);
    sig_b64[signatureLen] = '\0';
    size_t sigDecodedLen = base64UrlDecode(sig_b64, signature, 64);
    free(sig_b64);
    if (sigDecodedLen != 64) return false;
  }

  // Signing input is the encoded header.payload (everything before the last dot).
  size_t signingInputLen = headerLen + 1 + payloadLen;
  if (signingInputLen >= 1024) return false;
  char* signingInput = (char*)malloc(signingInputLen + 1);
  if (!signingInput) return false;
  memcpy(signingInput, token, signingInputLen);
  signingInput[signingInputLen] = '\0';

#ifdef ESP_PLATFORM
  yield();  // feed the watchdog around the verify
#endif
  int verify_result = ed25519_verify(signature, (const unsigned char*)signingInput, signingInputLen, pubkey_bytes);
#ifdef ESP_PLATFORM
  yield();
#endif

  free(signingInput);
  return (verify_result == 1);
}

