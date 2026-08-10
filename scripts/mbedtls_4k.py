"""Link the reduced-TLS mbedTLS archives, and prove they were actually linked.

Opt in per build with MESHCORE_REDUCED_TLS=1. Off by default, so an ordinary build
needs no 6 MB artifact and behaves exactly as before.

    MESHCORE_REDUCED_TLS=1 pio run -e Heltec_v3_repeater_observer_mqtt

The archives lower the mbedTLS outbound record buffer from 16 KiB to 4 KiB, saving
~12 KiB of internal DRAM per TLS connection on non-PSRAM observers. The inbound
buffer stays at 16 KiB, so the contiguous allocation a handshake needs is unchanged
— this buys headroom, it does not move that floor. See docs/mbedtls-tls-footprint.md.

Two failure modes this guards against, both of which produce a firmware that looks
fine and silently lacks the change:

  - a -L pointing at a missing or partial directory. The linker ignores an
    unusable search path and quietly resolves mbedTLS from the framework instead.
  - archives that do not match the manifest, e.g. left over from an earlier
    platform version.

So the opt-in path verifies every archive by sha256 before the build, and after the
link re-reads firmware.map to confirm every libmbed*.a came from our directory.
"""
Import("env")

import hashlib
import os
import sys

REQUIRED = ("libmbedcrypto.a", "libmbedtls_2.a", "libmbedtls.a", "libmbedx509.a")


def _fail(msg):
    print("\n*** reduced-TLS build failed ***", file=sys.stderr)
    print(msg, file=sys.stderr)
    print(
        "\nFetch the archives with:  scripts/fetch_mbedtls_4k.sh <arch>"
        "\nOr build without them by unsetting MESHCORE_REDUCED_TLS.",
        file=sys.stderr,
    )
    env.Exit(1)


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _manifest(project_dir, arch):
    path = os.path.join(project_dir, "scripts", "mbedtls_4k_manifest.txt")
    if not os.path.isfile(path):
        _fail("missing scripts/mbedtls_4k_manifest.txt")
    wanted = {}
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) == 3 and parts[0] == arch:
                wanted[parts[2]] = parts[1]
    return wanted


if os.environ.get("MESHCORE_REDUCED_TLS", "") not in ("1", "true", "yes"):
    Return()

project_dir = env.subst("$PROJECT_DIR")
arch = env.BoardConfig().get("build.mcu", "")
if not arch:
    _fail("could not determine board MCU, so cannot pick an archive set")

staged = os.path.join(project_dir, ".mbedtls-4k", arch)
if not os.path.isdir(staged):
    _fail("no archives for %s at %s" % (arch, staged))

wanted = _manifest(project_dir, arch)
if not wanted:
    _fail("manifest has no entries for arch '%s'" % arch)

for name in REQUIRED:
    archive = os.path.join(staged, name)
    if not os.path.isfile(archive):
        _fail("missing %s" % archive)
    if name not in wanted:
        _fail("%s is not in the manifest for %s" % (name, arch))
    actual = _sha256(archive)
    if actual != wanted[name]:
        _fail(
            "%s does not match the manifest\n  expected %s\n  actual   %s\n"
            "Rebuild it for this platform version, or re-run the fetch script."
            % (archive, wanted[name], actual)
        )

# Prepend so these satisfy mbedTLS symbols ahead of the framework's own copies:
# the linker takes each archive member from the first archive that resolves it.
env.Prepend(LIBPATH=[staged])
print("reduced-TLS: linking mbedTLS from %s (verified)" % staged)


def _verify_map(source, target, env):
    """Confirm every mbedTLS archive in the link came from our directory."""
    map_path = os.path.join(env.subst("$BUILD_DIR"), "firmware.map")
    if not os.path.isfile(map_path):
        print("reduced-TLS: WARNING no firmware.map, cannot confirm the link",
              file=sys.stderr)
        return
    # The map records whatever the linker was given, which for a -L hit is a path
    # relative to the linker's cwd (the project dir). Resolve before comparing, or
    # every one of our own archives reads as stray.
    staged_real = os.path.realpath(staged)
    stray = set()
    seen = set()
    with open(map_path, errors="replace") as fh:
        for line in fh:
            for token in line.split():
                if "libmbed" not in token or ".a" not in token:
                    continue
                path = token.split("(")[0]
                base = os.path.basename(path)
                if not base.startswith("libmbed") or not base.endswith(".a"):
                    continue
                seen.add(base)
                resolved = os.path.realpath(os.path.join(project_dir, path))
                if os.path.dirname(resolved) != staged_real:
                    stray.add(path)
    if stray:
        print("\n*** reduced-TLS: archives linked from the WRONG place ***",
              file=sys.stderr)
        for path in sorted(stray):
            print("  " + path, file=sys.stderr)
        env.Exit(1)
    if not seen:
        print("reduced-TLS: WARNING firmware.map names no mbedTLS archive",
              file=sys.stderr)
        return
    print("reduced-TLS: confirmed %d archives linked from %s"
          % (len(seen), staged))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", _verify_map)
