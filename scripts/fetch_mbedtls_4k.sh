#!/usr/bin/env bash
# Fetch and verify reduced-TLS archives; valid local copies need no download.
# Usage: scripts/fetch_mbedtls_4k.sh [arch|--all] (default: esp32s3).
# MBEDTLS_4K_LOCAL copies a local archive tree instead of downloading.
set -euo pipefail

ARCH="${1:-esp32s3}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$REPO_ROOT/scripts/mbedtls_4k_manifest.txt"

if [ ! -f "$MANIFEST" ]; then
  echo "error: missing $MANIFEST" >&2
  exit 1
fi

arches="$(awk 'NF == 3 && $1 !~ /^(#|stock:)/ {print $1}' "$MANIFEST" | sort -u)"
if [ "$ARCH" = "--all" ]; then
  if [ -z "$arches" ]; then
    echo "error: no architectures in $MANIFEST" >&2
    exit 1
  fi
  for arch in $arches; do
    bash "$REPO_ROOT/scripts/fetch_mbedtls_4k.sh" "$arch"
  done
  exit 0
fi

release="$(awk '$1 == "release" && NF == 2 {print $2}' "$MANIFEST")"
if [ -z "$release" ]; then
  echo "error: no release tag in $MANIFEST" >&2
  exit 1
fi
BASE_URL="${MBEDTLS_4K_BASE_URL:-https://github.com/agessaman/MeshCore/releases/download/$release}"
DEST="$REPO_ROOT/.mbedtls-4k/$ARCH"
expected="$(awk -v a="$ARCH" '$1 == a && NF == 3 {print $2"  "$3}' "$MANIFEST")"
if [ -z "$expected" ]; then
  echo "error: no manifest entries for arch '$ARCH'" >&2
  echo "known arches: $arches" >&2
  exit 1
fi

verify_archives() {
  (
    cd "$1"
    if command -v shasum >/dev/null 2>&1; then
      echo "$expected" | shasum -a 256 -c -
    else
      echo "$expected" | sha256sum -c -
    fi
  )
}

if [ -d "$DEST" ] && verify_archives "$DEST" >/dev/null 2>&1; then
  echo "ok: $ARCH archives already verified in $DEST (no download)"
  exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/staged"
if [ -n "${MBEDTLS_4K_LOCAL:-}" ]; then
  echo "copying from $MBEDTLS_4K_LOCAL"
  while read -r _sha name; do
    cp "$MBEDTLS_4K_LOCAL/$name" "$tmp/staged/$name"
  done <<< "$expected"
else
  TARBALL="mbedtls-4k-$ARCH.tar.gz"
  echo "downloading $BASE_URL/$TARBALL"
  curl -fsSL --retry 3 --retry-delay 2 --retry-connrefused "$BASE_URL/$TARBALL" -o "$tmp/$TARBALL"
  mkdir -p "$tmp/unpacked"
  tar -xzf "$tmp/$TARBALL" -C "$tmp/unpacked"
  while read -r _sha name; do
    # Accept tarballs with or without a leading directory.
    found="$(find "$tmp/unpacked" -name "$name" -type f | head -1)"
    if [ -z "$found" ]; then
      echo "error: $name missing from $TARBALL" >&2
      exit 1
    fi
    cp "$found" "$tmp/staged/$name"
  done <<< "$expected"
fi

verify_archives "$tmp/staged"
mkdir -p "$DEST"
while read -r _sha name; do
  cp "$tmp/staged/$name" "$DEST/$name"
done <<< "$expected"
echo "ok: $ARCH archives verified in $DEST"
