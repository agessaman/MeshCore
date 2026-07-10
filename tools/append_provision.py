#!/usr/bin/env python3
"""Append (or replace) a MeshCore provision trailer on an ESP32 app .bin.

The trailer is appended verbatim to the end of the built image:

    b"MCPV1\\x00" + uint16-LE payload length + payload + uint32-LE CRC32(payload)

esptool flashes the whole file, so the trailer lands in the app partition right
after the image. On first boot (no /provision, no /provision_done marker) the
firmware validates the trailer and copies the payload to /provision, then the
normal boot auto-apply runs it. See PROVISIONING.md.

ESP32-only: nRF52 DFU zips are signed against the exact image and RP2040 UF2
has no equivalent side channel.

Usage:
    tools/append_provision.py firmware.bin region_defaults.txt              # in place
    tools/append_provision.py firmware.bin region_defaults.txt -o out.bin
"""

import argparse
import struct
import sys
import zlib

MAGIC = b"MCPV1\x00"
MAX_SIZE = 4096
MAX_LINE = 159
HEADER_PREFIX = b"#meshcore-provision v"
ESP_IMAGE_MAGIC = 0xE9


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def validate_provision(data: bytes, name: str):
    if len(data) == 0:
        die(f"{name} is empty")
    if len(data) > MAX_SIZE:
        die(f"{name} is {len(data)} bytes; the firmware caps /provision at {MAX_SIZE}")
    lines = data.split(b"\n")
    first = next((l.strip() for l in lines if l.strip()), None)
    if first is None or not first.startswith(HEADER_PREFIX):
        die(f"{name}: first non-blank line must start with '{HEADER_PREFIX.decode()}1'")
    ver = first[len(HEADER_PREFIX):].split()[0] if len(first) > len(HEADER_PREFIX) else b""
    if not ver.isdigit() or int(ver) != 1:
        die(f"{name}: unsupported provision version '{ver.decode(errors='replace')}' (expected 1)")
    for i, l in enumerate(lines, 1):
        if len(l.rstrip(b"\r")) > MAX_LINE:
            die(f"{name}: line {i} is {len(l.rstrip(b'\r'))} chars; the firmware caps lines at {MAX_LINE}")


def parse_trailer_at_end(blob: bytes):
    """Return the trailer's start offset if blob ends with a valid trailer, else None."""
    # magic(6) + len(2) + payload + crc(4); search the tail window for candidates
    window_start = max(0, len(blob) - (len(MAGIC) + 2 + MAX_SIZE + 4))
    idx = blob.rfind(MAGIC, window_start)
    while idx != -1:
        cand = blob[idx:]
        if len(cand) >= len(MAGIC) + 2 + 4:
            (plen,) = struct.unpack_from("<H", cand, len(MAGIC))
            if len(cand) == len(MAGIC) + 2 + plen + 4:
                payload = cand[len(MAGIC) + 2:len(MAGIC) + 2 + plen]
                (crc,) = struct.unpack_from("<I", cand, len(MAGIC) + 2 + plen)
                if zlib.crc32(payload) & 0xFFFFFFFF == crc:
                    return idx
        idx = blob.rfind(MAGIC, window_start, idx)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n".join(__doc__.split("\n")[1:]))
    ap.add_argument("bin", help="built ESP32 app image (.bin)")
    ap.add_argument("provision", help="provision text file (#meshcore-provision v1 header)")
    ap.add_argument("-o", "--output", help="write result here instead of modifying BIN in place")
    ap.add_argument("--force", action="store_true",
                    help="skip the ESP32 image magic-byte sanity check")
    args = ap.parse_args()

    with open(args.bin, "rb") as f:
        blob = f.read()
    with open(args.provision, "rb") as f:
        payload = f.read()

    if not args.force and (len(blob) == 0 or blob[0] != ESP_IMAGE_MAGIC):
        die(f"{args.bin} does not look like an ESP32 app image "
            f"(first byte is not 0x{ESP_IMAGE_MAGIC:02X}); use --force to override")

    validate_provision(payload, args.provision)

    existing = parse_trailer_at_end(blob)
    if existing is not None:
        print(f"replacing existing trailer at offset {existing}")
        blob = blob[:existing]

    trailer = MAGIC + struct.pack("<H", len(payload)) + payload + \
        struct.pack("<I", zlib.crc32(payload) & 0xFFFFFFFF)
    out_path = args.output or args.bin
    with open(out_path, "wb") as f:
        f.write(blob + trailer)
    print(f"wrote {out_path}: image {len(blob)} bytes + trailer {len(trailer)} bytes "
          f"(payload {len(payload)} bytes)")


if __name__ == "__main__":
    main()
