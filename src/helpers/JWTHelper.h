#pragma once

#include "MeshCore.h"
#include "Identity.h"

/**
 * JWT Helper for creating authentication tokens
 * 
 * This class provides functionality to create JWT-style authentication tokens
 * signed with Ed25519 private keys for MQTT authentication.
 */
class JWTHelper {
public:
  /**
   * Create an authentication token for MQTT authentication
   * 
   * @param identity LocalIdentity instance for signing
   * @param audience Audience string (e.g., "mqtt-us-v1.letsmesh.net")
   * @param issuedAt Unix timestamp (0 for current time)
   * @param expiresIn Expiration time in seconds (0 for no expiration)
   * @param token Buffer to store the resulting token
   * @param tokenSize Size of the token buffer
   * @param owner Optional owner public key in hex format (nullptr if not set)
   * @param client Optional client string (nullptr if not set)
   * @param email Optional email address (nullptr if not set)
   * @return true if token was created successfully
   */
  static bool createAuthToken(
    const mesh::LocalIdentity& identity,
    const char* audience,
    unsigned long issuedAt = 0,
    unsigned long expiresIn = 0,
    char* token = nullptr,
    size_t tokenSize = 0,
    const char* owner = nullptr,
    const char* client = nullptr,
    const char* email = nullptr
  );

private:
  /**
   * Base64 URL encode data
   * 
   * @param input Input data
   * @param inputLen Length of input data
   * @param output Output buffer
   * @param outputSize Size of output buffer
   * @return Length of encoded data, or 0 on error
   */
  static size_t base64UrlEncode(const uint8_t* input, size_t inputLen, char* output, size_t outputSize);
  
  /**
   * Check the signature hex that follows the header.payload prefix in a finished token
   *
   * @param identity Identity whose public key signed the token
   * @param token Token holding header.payload.signatureHex
   * @param signingLen Length of the header.payload prefix
   * @return true if the signature verifies
   */
  static bool verifyTokenSignature(const mesh::Identity& identity, const char* token, size_t signingLen);
  
  /**
   * Create JWT payload
   * 
   * @param publicKey Raw public key (PUB_KEY_SIZE bytes), encoded as uppercase hex
   * @param audience Audience string
   * @param issuedAt Issued at timestamp
   * @param expiresIn Expiration time in seconds (0 for no expiration)
   * @param output Output buffer
   * @param outputSize Size of output buffer
   * @param maxLen Longest encoded payload the caller can accept (output is left untouched if exceeded)
   * @param owner Optional owner public key in hex format (nullptr if not set)
   * @param client Optional client string (nullptr if not set)
   * @param email Optional email address (nullptr if not set)
   * @return Length of payload, or 0 on error
   */
  static size_t createPayload(
    const uint8_t* publicKey,
    const char* audience,
    unsigned long issuedAt,
    unsigned long expiresIn,
    char* output,
    size_t outputSize,
    size_t maxLen,
    const char* owner = nullptr,
    const char* client = nullptr,
    const char* email = nullptr
  );
  
};
