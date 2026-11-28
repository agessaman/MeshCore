#!/usr/bin/env python3
"""Beacon-side password envelopes, from the written steps alone.

Expands an Ed25519 seed with SHA-512 and clamps the first 32 bytes. Turns the
observer's Edwards y (bit 255 cleared, then mod 2^255-19) into Montgomery u
with (1+y)/(1-y). The shared secret is X25519 of that clamped scalar and u.
The MAC input is ASCII, lowercase hex, and the version in decimal with no
leading zeros and no trailing NUL. One AES-128 block, then HMAC-SHA256 over
the raw ciphertext bytes.
"""

import hashlib
import hmac
import sys

P = 2**255 - 19


def expand_seed(seed: bytes) -> bytes:
    exp = bytearray(hashlib.sha512(seed).digest())
    exp[0] &= 248
    exp[31] &= 63
    exp[31] |= 64
    return bytes(exp)


def ed25519_public(seed: bytes) -> bytes:
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    except ImportError:
        sys.exit("Ed25519 public keys need the cryptography package")
    key = Ed25519PrivateKey.from_private_bytes(seed)
    return key.public_key().public_bytes_raw()


def montgomery_u(ed_pub: bytes) -> bytes:
    y = int.from_bytes(ed_pub, "little") & ((1 << 255) - 1)
    y %= P
    inv = pow((1 - y) % P, -1, P)
    u = ((1 + y) * inv) % P
    return u.to_bytes(32, "little")


def x25519_pure(scalar: bytes, u_bytes: bytes) -> bytes:
    k = bytearray(scalar)
    k[0] &= 248
    k[31] &= 63
    k[31] |= 64
    e = int.from_bytes(k, "little")
    x1 = int.from_bytes(u_bytes, "little") & ((1 << 255) - 1)
    x2, z2 = 1, 0
    x3, z3 = x1, 1
    swap = 0
    for t in range(254, -1, -1):
        kt = (e >> t) & 1
        swap ^= kt
        if swap:
            x2, x3 = x3, x2
            z2, z3 = z3, z2
        swap = kt
        a = (x2 + z2) % P
        aa = (a * a) % P
        b = (x2 - z2) % P
        bb = (b * b) % P
        e_diff = (aa - bb) % P
        c = (x3 + z3) % P
        d = (x3 - z3) % P
        da = (d * a) % P
        cb = (c * b) % P
        x3 = pow((da + cb) % P, 2, P)
        z3 = (x1 * pow((da - cb) % P, 2, P)) % P
        x2 = (aa * bb) % P
        z2 = (e_diff * ((aa + 121665 * e_diff) % P)) % P
    if swap:
        x2, z2 = x3, z3
    return ((x2 * pow(z2, -1, P)) % P).to_bytes(32, "little")


def x25519(scalar: bytes, u_bytes: bytes) -> bytes:
    pure = x25519_pure(scalar, u_bytes)
    try:
        from cryptography.hazmat.primitives.asymmetric.x25519 import (
            X25519PrivateKey,
            X25519PublicKey,
        )
    except ImportError:
        return pure
    lib = X25519PrivateKey.from_private_bytes(scalar).exchange(
        X25519PublicKey.from_public_bytes(u_bytes)
    )
    if lib != pure:
        sys.exit("X25519 fallback disagreed with cryptography")
    return lib


def xtime(a: int) -> int:
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else (a << 1) & 0xFF


def gmul(a: int, b: int) -> int:
    r = 0
    while b:
        if b & 1:
            r ^= a
        a = xtime(a)
        b >>= 1
    return r & 0xFF


def gf_inv(a: int) -> int:
    if a == 0:
        return 0
    r = 1
    base = a
    e = 254
    while e:
        if e & 1:
            r = gmul(r, base)
        base = gmul(base, base)
        e >>= 1
    return r


def sbox_byte(a: int) -> int:
    x = gf_inv(a)
    y = 0
    for i in range(8):
        bit = (
            (x >> i)
            ^ (x >> ((i + 4) % 8))
            ^ (x >> ((i + 5) % 8))
            ^ (x >> ((i + 6) % 8))
            ^ (x >> ((i + 7) % 8))
            ^ (0x63 >> i)
        ) & 1
        y |= bit << i
    return y


SBOX = [sbox_byte(i) for i in range(256)]
RCON = [0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36]


def aes128_encrypt_block(key: bytes, block: bytes) -> bytes:
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    except ImportError:
        return aes128_encrypt_pure(key, block)
    enc = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    out = enc.update(block) + enc.finalize()
    if out != aes128_encrypt_pure(key, block):
        sys.exit("AES fallback disagreed with cryptography")
    return out


def aes128_encrypt_pure(key: bytes, block: bytes) -> bytes:
    rk = bytearray(key)
    rcon_i = 1
    for i in range(4, 44):
        temp = bytearray(rk[(i - 1) * 4 : i * 4])
        if i % 4 == 0:
            t = temp[0]
            temp[0] = SBOX[temp[1]] ^ RCON[rcon_i]
            temp[1] = SBOX[temp[2]]
            temp[2] = SBOX[temp[3]]
            temp[3] = SBOX[t]
            rcon_i += 1
        for j in range(4):
            rk.append(rk[(i - 4) * 4 + j] ^ temp[j])
    s = bytearray(block)
    for i in range(16):
        s[i] ^= rk[i]
    for rnd in range(1, 11):
        s = bytearray(SBOX[b] for b in s)
        s[1], s[5], s[9], s[13] = s[5], s[9], s[13], s[1]
        s[2], s[10] = s[10], s[2]
        s[6], s[14] = s[14], s[6]
        s[15], s[11], s[7], s[3] = s[11], s[7], s[3], s[15]
        if rnd != 10:
            mixed = bytearray(16)
            for c in range(4):
                a0, a1, a2, a3 = s[4 * c : 4 * c + 4]
                mixed[4 * c] = xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3
                mixed[4 * c + 1] = a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3
                mixed[4 * c + 2] = a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3
                mixed[4 * c + 3] = xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3)
            s = mixed
        off = rnd * 16
        for i in range(16):
            s[i] ^= rk[off + i]
    return bytes(s)


def shared_secret(one_time_expanded: bytes, observer_pub: bytes) -> bytes:
    return x25519(one_time_expanded[:32], montgomery_u(observer_pub))


def derive(secret: bytes, observer: bytes, target: bytes, epk: bytes, ver: int) -> bytes:
    data = (
        b"beacon-poll-pw/v1|"
        + observer.hex().encode()
        + b"|"
        + target.hex().encode()
        + b"|"
        + epk.hex().encode()
        + b"|"
        + str(ver).encode()
    )
    return hmac.new(secret, data, hashlib.sha256).digest()


def seal_block(secret: bytes, observer: bytes, target: bytes, epk: bytes, ver: int, block: bytes):
    k = derive(secret, observer, target, epk, ver)
    ct = aes128_encrypt_block(k[:16], block)
    tag = hmac.new(k[16:], ct, hashlib.sha256).digest()
    return ct, tag


def c_bytes(data: bytes) -> str:
    rows = []
    for i in range(0, len(data), 16):
        chunk = ", ".join("0x%02x" % b for b in data[i : i + 16])
        rows.append("    " + chunk + ",")
    return "\n".join(rows)


def emit(name: str, data: bytes) -> None:
    print("static const uint8_t %s[%d] = {" % (name, len(data)))
    print(c_bytes(data))
    print("};")


def make_keys(n: int):
    obs_seed = bytes((n * 3 + i * 17) & 0xFF for i in range(32))
    eph_seed = bytes((n * 5 + 9 + i * 13) & 0xFF for i in range(32))
    tgt_seed = bytes((n * 7 + 21 + i * 11) & 0xFF for i in range(32))
    obs_prv = expand_seed(obs_seed)
    eph_prv = expand_seed(eph_seed)
    obs_pub = ed25519_public(obs_seed)
    epk = ed25519_public(eph_seed)
    target = ed25519_public(tgt_seed)
    secret = shared_secret(eph_prv, obs_pub)
    return obs_prv, obs_pub, target, epk, secret


def main() -> None:
    rfc_k = bytes.fromhex(
        "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"
    )
    rfc_u = bytes.fromhex(
        "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"
    )
    rfc = x25519(rfc_k, rfc_u)
    if rfc.hex() != "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742":
        sys.exit("X25519 self-check failed")
    nist_ct = aes128_encrypt_block(
        bytes(range(16)), bytes.fromhex("00112233445566778899aabbccddeeff")
    )
    if nist_ct.hex() != "69c4e0d86a7b0430d8cdb78070b4c55a":
        sys.exit("AES self-check failed")

    goods = [
        (1, b"x", 0),
        (2, b"0123456789abcde", 10),
        (3, b"hi\xff", 4294967295),
    ]
    print("// Generated by gen_vectors.py. Independent of the C++ envelope.")
    for n, pw, ver in goods:
        obs_prv, obs_pub, target, epk, secret = make_keys(n)
        block = pw + bytes(16 - len(pw))
        ct, tag = seal_block(secret, obs_pub, target, epk, ver, block)
        prefix = "kV%d" % n
        print("// %d-byte password, version %d" % (len(pw), ver))
        emit(prefix + "ObsPrv", obs_prv)
        emit(prefix + "ObsPub", obs_pub)
        emit(prefix + "Target", target)
        emit(prefix + "Epk", epk)
        emit(prefix + "Secret", secret)
        emit(prefix + "Ct", ct)
        emit(prefix + "Tag", tag)
        emit(prefix + "Pw", pw)
        print("static const uint32_t %sVer = %du;" % (prefix, ver))
        print("static const size_t %sPwLen = %d;" % (prefix, len(pw)))
        print()

    bads = [
        (4, bytes(16), 1, "empty"),
        (5, bytes([0x61]) * 16, 2, "sixteen"),
        (6, b"ab\x00c" + bytes(12), 3, "trail"),
    ]
    for n, block, ver, label in bads:
        obs_prv, obs_pub, target, epk, secret = make_keys(n)
        ct, tag = seal_block(secret, obs_pub, target, epk, ver, block)
        prefix = "kBad" + label[0].upper() + label[1:]
        print("// rejected plaintext: %s" % label)
        emit(prefix + "ObsPub", obs_pub)
        emit(prefix + "Target", target)
        emit(prefix + "Epk", epk)
        emit(prefix + "Secret", secret)
        emit(prefix + "Ct", ct)
        emit(prefix + "Tag", tag)
        print("static const uint32_t %sVer = %du;" % (prefix, ver))
        print()


if __name__ == "__main__":
    main()
