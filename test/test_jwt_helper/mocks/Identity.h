#pragma once

// Host stand-in for src/Identity.h: same key layout, writeTo() and sign() as the device class;
// verify() uses the orlp library here, where the device uses rweather Ed25519::verify.
#include "Arduino.h"
#include "Utils.h"
#include "ed_25519.h"

namespace mesh {

class Identity {
public:
  uint8_t pub_key[PUB_KEY_SIZE];
  Identity() { memset(pub_key, 0, sizeof(pub_key)); }

  bool verify(const uint8_t* sig, const uint8_t* message, int msg_len) const {
    return ed25519_verify(sig, message, msg_len, pub_key) == 1;
  }
};

class LocalIdentity : public Identity {
  uint8_t prv_key[PRV_KEY_SIZE];
public:
  LocalIdentity() { memset(prv_key, 0, sizeof(prv_key)); }
  explicit LocalIdentity(const uint8_t seed[32]) { ed25519_create_keypair(pub_key, prv_key, seed); }

  void sign(uint8_t* sig, const uint8_t* message, int msg_len) const {
    ed25519_sign(sig, message, msg_len, pub_key, prv_key);
  }

  size_t writeTo(uint8_t* dest, size_t max_len) {
    if (max_len < PRV_KEY_SIZE) return 0;
    if (max_len < PRV_KEY_SIZE + PUB_KEY_SIZE) {
      memcpy(dest, prv_key, PRV_KEY_SIZE);
      return PRV_KEY_SIZE;
    }
    memcpy(dest, prv_key, PRV_KEY_SIZE);
    memcpy(&dest[PRV_KEY_SIZE], pub_key, PUB_KEY_SIZE);
    return PRV_KEY_SIZE + PUB_KEY_SIZE;
  }
};

}
