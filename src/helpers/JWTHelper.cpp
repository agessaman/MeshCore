// MQTT-only translation unit. 22 variants re-glob helpers/*.cpp past the
// arduino_base exclusion, so the contents are guarded here rather than in
// the build filter — same idiom as helpers/esp32/WebConfigServer.cpp.
#ifdef WITH_MQTT_BRIDGE

#include "JWTHelper.h"
#include <ArduinoJson.h>
#include <string.h>
#include "mbedtls/base64.h"

// base64url of {"alg":"Ed25519","typ":"JWT"}, the fixed JWT header.
static const char kJwtHeader[] = "eyJhbGciOiJFZDI1NTE5IiwidHlwIjoiSldUIn0";
static const size_t kJwtHeaderLen = sizeof(kJwtHeader) - 1;
static const size_t kSignatureHexLen = SIGNATURE_SIZE * 2;
// Encoded payload cap including NUL, which limits the payload JSON to 381 bytes.
static const size_t kPayloadOutputMax = 512;

// Builds header.payload.signatureHex in place; size checks run before the first write.
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

  const size_t fixedLen = kJwtHeaderLen + 1 + 1 + kSignatureHexLen + 1;
  if (tokenSize <= fixedLen) {
    return false;
  }
  // The encoder briefly needs padding and a NUL past the payload; the signature overwrites them.
  size_t encodeCap = tokenSize - (kJwtHeaderLen + 1);
  if (encodeCap > kPayloadOutputMax) {
    encodeCap = kPayloadOutputMax;
  }

  char* payload = token + kJwtHeaderLen + 1;
  size_t payloadLen = createPayload(identity.pub_key, audience, issuedAt, expiresIn,
                                    payload, encodeCap, tokenSize - fixedLen,
                                    owner, client, email);
  if (payloadLen == 0) {
    return false;
  }

  memcpy(token, kJwtHeader, kJwtHeaderLen);
  token[kJwtHeaderLen] = '.';
  const size_t signingLen = kJwtHeaderLen + 1 + payloadLen;

  uint8_t signature[SIGNATURE_SIZE];
  identity.sign(signature, (const uint8_t*)token, (int)signingLen);

  // Signature as uppercase hex (MeshCore Decoder format); toHex() writes the NUL.
  token[signingLen] = '.';
  mesh::Utils::toHex(token + signingLen + 1, signature, SIGNATURE_SIZE);

  if (!verifyTokenSignature(identity, token, signingLen)) {
    memset(token, 0, signingLen + 1 + kSignatureHexLen + 1);
    if (Serial.availableForWrite() > 0) Serial.println("JWTHelper: Signature verification failed!");
    return false;
  }

  return true;
}

// Kept out of line so the verifier's deep stack never sits under createAuthToken's locals.
__attribute__((noinline)) bool JWTHelper::verifyTokenSignature(
  const mesh::Identity& identity,
  const char* token,
  size_t signingLen
) {
  uint8_t signature[SIGNATURE_SIZE];
  if (!mesh::Utils::fromHex(signature, SIGNATURE_SIZE, token + signingLen + 1)) {
    return false;
  }
  return identity.verify(signature, (const uint8_t*)token, (int)signingLen);
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

size_t JWTHelper::createPayload(
  const uint8_t* publicKey,
  const char* audience,
  unsigned long issuedAt,
  unsigned long expiresIn,
  char* output,
  size_t outputSize,
  size_t maxLen,
  const char* owner,
  const char* client,
  const char* email
) {
  char publicKeyHex[PUB_KEY_SIZE * 2 + 1];
  mesh::Utils::toHex(publicKeyHex, publicKey, PUB_KEY_SIZE);

  // Create JWT payload
  DynamicJsonDocument doc(512);
  doc["publicKey"] = publicKeyHex;
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
  
  // Unpadded base64url length, checked before the encoder writes anything.
  if ((len * 4 + 2) / 3 > maxLen) {
    return 0;
  }

  return base64UrlEncode((uint8_t*)jsonBuffer, len, output, outputSize);
}

#endif  // WITH_MQTT_BRIDGE
