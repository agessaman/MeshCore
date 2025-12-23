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

  /**
   * Verify a JWT token and extract information
   * 
   * @param token JWT token string (header.payload.signature format)
   * @param expected_public_key Expected public key for verification (NULL if checking against ACL)
   * @param key_len Length of expected public key
   * @param extracted_public_key Output buffer for extracted public key from token
   * @param extracted_key_size Size of extracted_public_key buffer
   * @param extracted_nonce Output buffer for extracted nonce from token
   * @param nonce_size Size of extracted_nonce buffer
   * @param issued_at Output: issued at timestamp
   * @param expires_at Output: expiration timestamp
   * @return true if token is valid and signature verifies
   */
  static bool verifyToken(
    const char* token,
    const uint8_t* expected_public_key,
    size_t key_len,
    char* extracted_public_key,
    size_t extracted_key_size,
    char* extracted_nonce,
    size_t nonce_size,
    unsigned long* issued_at,
    unsigned long* expires_at
  );

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
   * Base64 URL decode data
   * 
   * @param input Input base64url encoded string
   * @param output Output buffer
   * @param outputSize Size of output buffer
   * @return Length of decoded data, or 0 on error
   */
  static size_t base64UrlDecode(const char* input, uint8_t* output, size_t outputSize);

private:
  
  /**
   * Create JWT header
   * 
   * @param output Output buffer
   * @param outputSize Size of output buffer
   * @return Length of header, or 0 on error
   */
  static size_t createHeader(char* output, size_t outputSize);
  
  /**
   * Create JWT payload
   * 
   * @param publicKey Public key in hex format
   * @param audience Audience string
   * @param issuedAt Issued at timestamp
   * @param expiresIn Expiration time in seconds (0 for no expiration)
   * @param output Output buffer
   * @param outputSize Size of output buffer
   * @param owner Optional owner public key in hex format (nullptr if not set)
   * @param client Optional client string (nullptr if not set)
   * @param email Optional email address (nullptr if not set)
   * @return Length of payload, or 0 on error
   */
  static size_t createPayload(
    const char* publicKey,
    const char* audience,
    unsigned long issuedAt,
    unsigned long expiresIn,
    char* output,
    size_t outputSize,
    const char* owner = nullptr,
    const char* client = nullptr,
    const char* email = nullptr
  );
  
};
