#include "JWTHelper.h"
#include <ArduinoJson.h>
#include <SHA256.h>
#include <string.h>
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
  size_t tokenSize
) {
  Serial.printf("JWTHelper: Starting JWT creation for audience: %s\n", audience);
  
  if (!audience || !token || tokenSize == 0) {
    Serial.printf("JWTHelper: Invalid parameters - audience: %p, token: %p, tokenSize: %d\n", audience, token, (int)tokenSize);
    return false;
  }
  
  // Use current time if not specified
  if (issuedAt == 0) {
    issuedAt = time(nullptr);
  }
  Serial.printf("JWTHelper: Using issuedAt: %lu\n", issuedAt);
  
  // Create header
  char header[256];
  size_t headerLen = createHeader(header, sizeof(header));
  if (headerLen == 0) {
    Serial.printf("JWTHelper: Failed to create header\n");
    return false;
  }
  Serial.printf("JWTHelper: Header created, length: %d\n", (int)headerLen);
  Serial.printf("JWTHelper: Header: %s\n", header);
  
  // Get public key as UPPERCASE HEX string (not base64!)
  char publicKeyHex[65]; // 32 bytes * 2 + null terminator
  mesh::Utils::toHex(publicKeyHex, identity.pub_key, PUB_KEY_SIZE);
  
  // Convert to uppercase
  for (int i = 0; publicKeyHex[i]; i++) {
    publicKeyHex[i] = toupper(publicKeyHex[i]);
  }
  
  Serial.printf("JWTHelper: Public key hex: %s (length: %d)\n", publicKeyHex, (int)strlen(publicKeyHex));
  
  // Create payload with HEX public key (not base64!)
  char payload[512];
  size_t payloadLen = createPayload(publicKeyHex, audience, issuedAt, expiresIn, payload, sizeof(payload));
  if (payloadLen == 0) {
    Serial.printf("JWTHelper: Failed to create payload\n");
    return false;
  }
  Serial.printf("JWTHelper: Payload created, length: %d\n", (int)payloadLen);
  Serial.printf("JWTHelper: Payload: %s\n", payload);
  
  // Create signing input: header.payload
  char signingInput[768];
  size_t signingInputLen = headerLen + 1 + payloadLen;
  if (signingInputLen >= sizeof(signingInput)) {
    Serial.printf("JWTHelper: Signing input too large: %d >= %d\n", (int)signingInputLen, (int)sizeof(signingInput));
    return false;
  }
  
  memcpy(signingInput, header, headerLen);
  signingInput[headerLen] = '.';
  memcpy(signingInput + headerLen + 1, payload, payloadLen);
  Serial.printf("JWTHelper: Signing input created, length: %d\n", (int)signingInputLen);
  
  // Sign the data using direct Ed25519 signing
  uint8_t signature[64];
  
  // Create a non-const copy of the identity to access writeTo method
  mesh::LocalIdentity identity_copy = identity;
  
  // Export the private and public keys using the writeTo method
  uint8_t export_buffer[96]; // PRV_KEY_SIZE + PUB_KEY_SIZE = 64 + 32 = 96 bytes
  size_t exported_size = identity_copy.writeTo(export_buffer, sizeof(export_buffer));
  
  if (exported_size != 96) {
    Serial.printf("JWTHelper: Failed to export keys, got %d bytes instead of 96\n", (int)exported_size);
    return false;
  }
  
  // The first 64 bytes are the private key, next 32 bytes are the public key
  uint8_t* private_key = export_buffer;        // First 64 bytes
  uint8_t* public_key = export_buffer + 64;    // Next 32 bytes
  
  Serial.printf("JWTHelper: Using direct Ed25519 signing\n");
  Serial.printf("JWTHelper: Private key length: %d, Public key length: %d\n", 64, 32);
  
  // Use direct Ed25519 signing
  ed25519_sign(signature, (const unsigned char*)signingInput, signingInputLen, public_key, private_key);
  Serial.printf("JWTHelper: Signature created using direct Ed25519\n");
  
  // Verify the signature locally
  int verify_result = ed25519_verify(signature, (const unsigned char*)signingInput, signingInputLen, public_key);
  Serial.printf("JWTHelper: Signature verification result: %d (should be 1 for valid)\n", verify_result);
  
  if (verify_result != 1) {
    Serial.println("JWTHelper: ERROR - Signature verification failed!");
    return false;
  }
  
  // Log the exact signing input
  Serial.printf("JWTHelper: Signing input: %s\n", signingInput);
  Serial.printf("JWTHelper: Signing input hex: ");
  for (size_t i = 0; i < signingInputLen; i++) {
    Serial.printf("%02x", signingInput[i]);
  }
  Serial.println();
  
  // Log the signature
  Serial.printf("JWTHelper: Signature hex: ");
  for (int i = 0; i < 64; i++) {
    Serial.printf("%02x", signature[i]);
  }
  Serial.println();
  
  // Convert signature to hex (MeshCore Decoder expects hex, not base64url)
  char signatureHex[129]; // 64 bytes * 2 + null terminator
  for (int i = 0; i < 64; i++) {
    sprintf(signatureHex + (i * 2), "%02X", signature[i]);
  }
  signatureHex[128] = '\0';
  
  Serial.printf("JWTHelper: Signature converted to hex, length: %d\n", (int)strlen(signatureHex));
  Serial.printf("JWTHelper: Signature Hex: %s\n", signatureHex);
  
  // Create final token: header.payload.signatureHex (MeshCore Decoder format)
  size_t sigHexLen = strlen(signatureHex);
  size_t totalLen = headerLen + 1 + payloadLen + 1 + sigHexLen;
  if (totalLen >= tokenSize) {
    Serial.printf("JWTHelper: Token too large: %d >= %d\n", (int)totalLen, (int)tokenSize);
    return false;
  }
  
  memcpy(token, header, headerLen);
  token[headerLen] = '.';
  memcpy(token + headerLen + 1, payload, payloadLen);
  token[headerLen + 1 + payloadLen] = '.';
  memcpy(token + headerLen + 1 + payloadLen + 1, signatureHex, sigHexLen);
  token[totalLen] = '\0';

  Serial.printf("JWTHelper: JWT token created successfully, total length: %d\n", (int)totalLen);
  Serial.printf("JWTHelper: JWT Token: %s\n", token);
  return true;
}

size_t JWTHelper::base64UrlEncode(const uint8_t* input, size_t inputLen, char* output, size_t outputSize) {
  Serial.printf("JWTHelper: base64UrlEncode called with inputLen: %d, outputSize: %d\n", (int)inputLen, (int)outputSize);
  
  if (!input || !output || outputSize == 0) {
    Serial.printf("JWTHelper: base64UrlEncode invalid parameters\n");
    return 0;
  }
  
  // Use ESP32's built-in mbedTLS base64 encoding
  size_t outlen = 0;
  int ret = mbedtls_base64_encode((unsigned char*)output, outputSize - 1, &outlen, input, inputLen);
  
  if (ret != 0) {
    Serial.printf("JWTHelper: mbedtls_base64_encode failed with error: %d\n", ret);
    return 0;
  }
  
  Serial.printf("JWTHelper: mbedtls_base64_encode result: %s (outlen: %d)\n", output, (int)outlen);
  
  // Convert to base64 URL format (replace + with -, / with _, remove padding =)
  String encoded(output);
  encoded.replace('+', '-');
  encoded.replace('/', '_');
  encoded.replace("=", "");
  
  // Copy back to output buffer
  size_t len = encoded.length();
  if (len >= outputSize) {
    Serial.printf("JWTHelper: base64UrlEncode output too large: %d >= %d\n", (int)len, (int)outputSize);
    return 0;
  }
  
  strcpy(output, encoded.c_str());
  Serial.printf("JWTHelper: base64UrlEncode completed, outputLen: %d\n", (int)len);
  return len;
}

size_t JWTHelper::createHeader(char* output, size_t outputSize) {
  Serial.printf("JWTHelper: createHeader called with outputSize: %d\n", (int)outputSize);
  
  // Create JWT header: {"alg":"Ed25519","typ":"JWT"}
  DynamicJsonDocument doc(256);
  doc["alg"] = "Ed25519";
  doc["typ"] = "JWT";
  
  // Use temporary buffer for JSON
  char jsonBuffer[256];
  size_t len = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
  Serial.printf("JWTHelper: JSON serialized, length: %d\n", (int)len);
  if (len == 0 || len >= sizeof(jsonBuffer)) {
    Serial.printf("JWTHelper: JSON serialization failed or too large\n");
    return 0;
  }
  
  // Base64 URL encode from temporary buffer to output
  size_t encodedLen = base64UrlEncode((uint8_t*)jsonBuffer, len, output, outputSize);
  Serial.printf("JWTHelper: Header base64 encoded, length: %d\n", (int)encodedLen);
  return encodedLen;
}

size_t JWTHelper::createPayload(
  const char* publicKey,
  const char* audience,
  unsigned long issuedAt,
  unsigned long expiresIn,
  char* output,
  size_t outputSize
) {
  Serial.printf("JWTHelper: createPayload called with outputSize: %d\n", (int)outputSize);
  Serial.printf("JWTHelper: publicKey: %s, audience: %s, issuedAt: %lu, expiresIn: %lu\n", 
                publicKey, audience, issuedAt, expiresIn);
  
  // Create JWT payload
  DynamicJsonDocument doc(512);
  doc["publicKey"] = publicKey;
  doc["aud"] = audience;
  doc["iat"] = issuedAt;
  
  if (expiresIn > 0) {
    doc["exp"] = issuedAt + expiresIn;
  }
  
  // Use temporary buffer for JSON
  char jsonBuffer[512];
  size_t len = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
  Serial.printf("JWTHelper: Payload JSON serialized, length: %d\n", (int)len);
  if (len == 0 || len >= sizeof(jsonBuffer)) {
    Serial.printf("JWTHelper: Payload JSON serialization failed or too large\n");
    return 0;
  }
  
  // Base64 URL encode from temporary buffer to output
  size_t encodedLen = base64UrlEncode((uint8_t*)jsonBuffer, len, output, outputSize);
  Serial.printf("JWTHelper: Payload base64 encoded, length: %d\n", (int)encodedLen);
  return encodedLen;
}

