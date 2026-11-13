#pragma once

// One-block password envelope. Stack buffers only, for firmware and host tests.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace poll {

namespace detail {

static inline void wipe(void* p, size_t n) {
  volatile uint8_t* v = (volatile uint8_t*)p;
  while (n--) *v++ = 0;
}

static inline uint32_t rotr32(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

struct Sha256 {
  uint32_t h[8];
  uint8_t block[64];
  uint64_t bits;
  size_t n;
};

static inline void sha256Init(Sha256* s) {
  s->h[0] = 0x6a09e667u;
  s->h[1] = 0xbb67ae85u;
  s->h[2] = 0x3c6ef372u;
  s->h[3] = 0xa54ff53au;
  s->h[4] = 0x510e527fu;
  s->h[5] = 0x9b05688cu;
  s->h[6] = 0x1f83d9abu;
  s->h[7] = 0x5be0cd19u;
  s->bits = 0;
  s->n = 0;
}

static inline void sha256Block(Sha256* s, const uint8_t block[64]) {
  static const uint32_t k[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
      0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
      0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
      0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
      0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
      0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
      0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
      0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
      0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
  }
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
  uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + S1 + ch + k[i] + w[i];
    uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  s->h[0] += a;
  s->h[1] += b;
  s->h[2] += c;
  s->h[3] += d;
  s->h[4] += e;
  s->h[5] += f;
  s->h[6] += g;
  s->h[7] += h;
  wipe(w, sizeof(w));
}

static inline void sha256Update(Sha256* s, const uint8_t* data, size_t len) {
  s->bits += (uint64_t)len * 8u;
  while (len--) {
    s->block[s->n++] = *data++;
    if (s->n == 64) {
      sha256Block(s, s->block);
      s->n = 0;
    }
  }
}

static inline void sha256Final(Sha256* s, uint8_t out[32]) {
  s->block[s->n++] = 0x80;
  if (s->n > 56) {
    while (s->n < 64) s->block[s->n++] = 0;
    sha256Block(s, s->block);
    s->n = 0;
  }
  while (s->n < 56) s->block[s->n++] = 0;
  for (int i = 0; i < 8; i++) s->block[63 - i] = (uint8_t)(s->bits >> (8 * i));
  sha256Block(s, s->block);
  for (int i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(s->h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)s->h[i];
  }
  wipe(s, sizeof(*s));
}

static inline void sha256(uint8_t out[32], const uint8_t* data, size_t len) {
  Sha256 s;
  sha256Init(&s);
  sha256Update(&s, data, len);
  sha256Final(&s, out);
}

// HMAC-SHA256. Keys longer than one block are hashed down first.
static inline void hmacSha256(uint8_t out[32], const uint8_t* key, size_t key_len,
                              const uint8_t* data, size_t data_len) {
  uint8_t k[64];
  memset(k, 0, sizeof(k));
  if (key_len > 64) {
    sha256(k, key, key_len);
  } else if (key_len) {
    memcpy(k, key, key_len);
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; i++) {
    ipad[i] = (uint8_t)(k[i] ^ 0x36);
    opad[i] = (uint8_t)(k[i] ^ 0x5c);
  }
  Sha256 inner;
  sha256Init(&inner);
  sha256Update(&inner, ipad, 64);
  sha256Update(&inner, data, data_len);
  uint8_t inner_hash[32];
  sha256Final(&inner, inner_hash);
  Sha256 outer;
  sha256Init(&outer);
  sha256Update(&outer, opad, 64);
  sha256Update(&outer, inner_hash, 32);
  sha256Final(&outer, out);
  wipe(k, sizeof(k));
  wipe(ipad, sizeof(ipad));
  wipe(opad, sizeof(opad));
  wipe(inner_hash, sizeof(inner_hash));
}

static const uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static const uint8_t kInvSbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};

static inline uint8_t xtime(uint8_t x) {
  return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static inline uint8_t gmul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  while (b) {
    if (b & 1) r ^= a;
    a = xtime(a);
    b = (uint8_t)(b >> 1);
  }
  return r;
}

static inline void aesExpand(uint8_t rk[176], const uint8_t key[16]) {
  static const uint8_t rcon[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
  memcpy(rk, key, 16);
  int rcon_i = 1;
  for (int i = 4; i < 44; i++) {
    uint8_t temp[4];
    temp[0] = rk[(i - 1) * 4];
    temp[1] = rk[(i - 1) * 4 + 1];
    temp[2] = rk[(i - 1) * 4 + 2];
    temp[3] = rk[(i - 1) * 4 + 3];
    if ((i % 4) == 0) {
      uint8_t t = temp[0];
      temp[0] = (uint8_t)(kSbox[temp[1]] ^ rcon[rcon_i++]);
      temp[1] = kSbox[temp[2]];
      temp[2] = kSbox[temp[3]];
      temp[3] = kSbox[t];
    }
    rk[i * 4] = (uint8_t)(rk[(i - 4) * 4] ^ temp[0]);
    rk[i * 4 + 1] = (uint8_t)(rk[(i - 4) * 4 + 1] ^ temp[1]);
    rk[i * 4 + 2] = (uint8_t)(rk[(i - 4) * 4 + 2] ^ temp[2]);
    rk[i * 4 + 3] = (uint8_t)(rk[(i - 4) * 4 + 3] ^ temp[3]);
    wipe(temp, sizeof(temp));
  }
}

static inline void addRoundKey(uint8_t s[16], const uint8_t* rk) {
  for (int i = 0; i < 16; i++) s[i] ^= rk[i];
}

static inline void subBytes(uint8_t s[16]) {
  for (int i = 0; i < 16; i++) s[i] = kSbox[s[i]];
}

static inline void invSubBytes(uint8_t s[16]) {
  for (int i = 0; i < 16; i++) s[i] = kInvSbox[s[i]];
}

static inline void shiftRows(uint8_t s[16]) {
  uint8_t t = s[1];
  s[1] = s[5];
  s[5] = s[9];
  s[9] = s[13];
  s[13] = t;
  t = s[2];
  s[2] = s[10];
  s[10] = t;
  t = s[6];
  s[6] = s[14];
  s[14] = t;
  t = s[15];
  s[15] = s[11];
  s[11] = s[7];
  s[7] = s[3];
  s[3] = t;
}

static inline void invShiftRows(uint8_t s[16]) {
  uint8_t t = s[13];
  s[13] = s[9];
  s[9] = s[5];
  s[5] = s[1];
  s[1] = t;
  t = s[2];
  s[2] = s[10];
  s[10] = t;
  t = s[6];
  s[6] = s[14];
  s[14] = t;
  t = s[3];
  s[3] = s[7];
  s[7] = s[11];
  s[11] = s[15];
  s[15] = t;
}

static inline void mixColumns(uint8_t s[16]) {
  for (int c = 0; c < 4; c++) {
    uint8_t* p = s + 4 * c;
    uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
    p[0] = (uint8_t)(xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3);
    p[1] = (uint8_t)(a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3);
    p[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3);
    p[3] = (uint8_t)(xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3));
  }
}

static inline void invMixColumns(uint8_t s[16]) {
  for (int c = 0; c < 4; c++) {
    uint8_t* p = s + 4 * c;
    uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
    p[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
    p[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
    p[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
    p[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
  }
}

// Encrypts one AES-128 block.
static inline void aesEncryptBlock(const uint8_t key[16], uint8_t out[16], const uint8_t in[16]) {
  uint8_t rk[176];
  uint8_t s[16];
  aesExpand(rk, key);
  memcpy(s, in, 16);
  addRoundKey(s, rk);
  for (int round = 1; round <= 9; round++) {
    subBytes(s);
    shiftRows(s);
    mixColumns(s);
    addRoundKey(s, rk + round * 16);
  }
  subBytes(s);
  shiftRows(s);
  addRoundKey(s, rk + 160);
  memcpy(out, s, 16);
  wipe(rk, sizeof(rk));
  wipe(s, sizeof(s));
}

// Decrypts one AES-128 block.
static inline void aesDecryptBlock(const uint8_t key[16], uint8_t out[16], const uint8_t in[16]) {
  uint8_t rk[176];
  uint8_t s[16];
  aesExpand(rk, key);
  memcpy(s, in, 16);
  addRoundKey(s, rk + 160);
  for (int round = 9; round >= 1; round--) {
    invShiftRows(s);
    invSubBytes(s);
    addRoundKey(s, rk + round * 16);
    invMixColumns(s);
  }
  invShiftRows(s);
  invSubBytes(s);
  addRoundKey(s, rk);
  memcpy(out, s, 16);
  wipe(rk, sizeof(rk));
  wipe(s, sizeof(s));
}

static inline void appendHex(uint8_t* dest, const uint8_t src[32]) {
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    dest[i * 2] = (uint8_t)hex[src[i] >> 4];
    dest[i * 2 + 1] = (uint8_t)hex[src[i] & 0x0f];
  }
}

static inline size_t writeDec(uint8_t* dest, uint32_t ver) {
  char tmp[10];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + (ver % 10u));
    ver /= 10u;
  } while (ver != 0);
  for (int i = 0; i < n; i++) dest[i] = (uint8_t)tmp[n - 1 - i];
  return (size_t)n;
}

// Binds the key to the two node ids, the one-time public key, and the version.
static inline void deriveKey(uint8_t key[32], const uint8_t secret[32], const uint8_t observer_pub[32],
                             const uint8_t target_pub[32], const uint8_t epk[32], uint32_t ver) {
  static const char label[] = "beacon-poll-pw/v1|";
  uint8_t info[sizeof(label) - 1 + 64 + 1 + 64 + 1 + 64 + 1 + 10];
  size_t n = sizeof(label) - 1;
  memcpy(info, label, n);
  appendHex(info + n, observer_pub);
  n += 64;
  info[n++] = '|';
  appendHex(info + n, target_pub);
  n += 64;
  info[n++] = '|';
  appendHex(info + n, epk);
  n += 64;
  info[n++] = '|';
  n += writeDec(info + n, ver);
  hmacSha256(key, secret, 32, info, n);
  wipe(info, sizeof(info));
}

// Full-tag compare with no early exit.
static inline uint8_t tagDiff(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d = (uint8_t)(d | (a[i] ^ b[i]));
  return d;
}

// Password is 1..15 non-zero bytes and the rest of the block is zero.
static inline bool passwordBlockOk(const uint8_t block[16]) {
  size_t i = 0;
  while (i < 16 && block[i] != 0) i++;
  if (i < 1 || i > 15) return false;
  for (size_t j = i; j < 16; j++) {
    if (block[j] != 0) return false;
  }
  return true;
}

}  // namespace detail

// Checks the tag, then decrypts. On failure out_pw is 16 zero bytes.
inline bool openPwEnvelope(const uint8_t secret[32], const uint8_t observer_pub[32],
                           const uint8_t target_pub[32], const uint8_t epk[32], const uint8_t ct[16],
                           const uint8_t tag[32], uint32_t ver, char out_pw[16]) {
  memset(out_pw, 0, 16);
  uint8_t key[32];
  detail::deriveKey(key, secret, observer_pub, target_pub, epk, ver);
  uint8_t expect[32];
  detail::hmacSha256(expect, key + 16, 16, ct, 16);
  uint8_t mismatch = detail::tagDiff(expect, tag, 32);
  detail::wipe(expect, sizeof(expect));
  if (mismatch) {
    detail::wipe(key, sizeof(key));
    return false;
  }
  uint8_t plain[16];
  detail::aesDecryptBlock(key, plain, ct);
  detail::wipe(key, sizeof(key));
  bool ok = detail::passwordBlockOk(plain);
  if (ok) memcpy(out_pw, plain, 16);
  detail::wipe(plain, sizeof(plain));
  return ok;
}

inline bool sealPwEnvelope(const uint8_t secret[32], const uint8_t observer_pub[32],
                           const uint8_t target_pub[32], const uint8_t epk[32], uint32_t ver,
                           const uint8_t* pw, size_t pw_len, uint8_t ct[16], uint8_t tag[32]) {
  memset(ct, 0, 16);
  memset(tag, 0, 32);
  if (!pw || pw_len < 1 || pw_len > 15) return false;
  for (size_t i = 0; i < pw_len; i++) {
    if (pw[i] == 0) return false;
  }
  uint8_t plain[16];
  memset(plain, 0, sizeof(plain));
  memcpy(plain, pw, pw_len);
  uint8_t key[32];
  detail::deriveKey(key, secret, observer_pub, target_pub, epk, ver);
  detail::aesEncryptBlock(key, ct, plain);
  detail::hmacSha256(tag, key + 16, 16, ct, 16);
  detail::wipe(plain, sizeof(plain));
  detail::wipe(key, sizeof(key));
  return true;
}

}  // namespace poll
