#!/usr/bin/env bash
# Fetch the reduced-TLS mbedTLS archives into .mbedtls-4k/<arch>/.
#
# These archives are built from the shipped sdkconfig plus three lines
# (CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y, IN_CONTENT_LEN 16384,
# OUT_CONTENT_LEN 4096) and save ~12 KiB of internal DRAM per TLS connection.
# See docs/mbedtls-tls-footprint.md for the rationale and the build recipe.
#
# They are not committed: ~6 MB per architecture, and they must be rebuilt for
# every platform bump, so they are published as a release asset keyed on the
# espressif32 platform version instead.
#
#   scripts/fetch_mbedtls_4k.sh [arch]        # default: esp32s3
#
# Set MBEDTLS_4K_LOCAL to skip the download and copy from a local build tree:
#   MBEDTLS_4K_LOCAL=~/mbedtls-4k-esp32s3/staged scripts/fetch_mbedtls_4k.sh
set -euo pipefail

ARCH="${1:-esp32s3}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_ROOT/.mbedtls-4k/$ARCH"
MANIFEST="$REPO_ROOT/scripts/mbedtls_4k_manifest.txt"
BASE_URL="${MBEDTLS_4K_BASE_URL:-https://github.com/agessaman/MeshCore/releases/download/mbedtls-4k}"

if [ ! -f "$MANIFEST" ]; then
  echo "error: missing $MANIFEST" >&2
  exit 1
fi

# Manifest lines: <arch> <sha256> <filename>. Blank lines and # comments ignored.
expected="$(awk -v a="$ARCH" '$1 == a && $0 !~ /^#/ {print $2"  "$3}' "$MANIFEST")"
if [ -z "$expected" ]; then
  echo "error: no manifest entries for arch '$ARCH'" >&2
  echo "known arches: $(awk '$0 !~ /^#/ && NF {print $1}' "$MANIFEST" | sort -u | tr '\n' ' ')" >&2
  exit 1
fi

mkdir -p "$DEST"

if [ -n "${MBEDTLS_4K_LOCAL:-}" ]; then
  echo "copying from $MBEDTLS_4K_LOCAL"
  while read -r _sha name; do
    cp "$MBEDTLS_4K_LOCAL/$name" "$DEST/$name"
  done <<< "$expected"
else
  TARBALL="mbedtls-4k-$ARCH.tar.gz"
  echo "downloading $BASE_URL/$TARBALL"
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT
  curl -fsSL "$BASE_URL/$TARBALL" -o "$tmp/$TARBALL"
  tar -xzf "$tmp/$TARBALL" -C "$tmp"
  while read -r _sha name; do
    # Accept the archive whether or not the tarball has a leading directory.
    found="$(find "$tmp" -name "$name" -type f | head -1)"
    if [ -z "$found" ]; then
      echo "error: $name missing from $TARBALL" >&2
      exit 1
    fi
    cp "$found" "$DEST/$name"
  done <<< "$expected"
fi

# Verify every archive against the manifest. A wrong or truncated archive would
# otherwise link silently and produce a firmware without the reduced buffers.
cd "$DEST"
if command -v shasum >/dev/null 2>&1; then
  echo "$expected" | shasum -a 256 -c -
else
  echo "$expected" | sha256sum -c -
fi

echo "ok: $ARCH archives verified in $DEST"
