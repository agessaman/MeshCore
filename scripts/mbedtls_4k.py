"""Link the reduced-TLS mbedTLS archives, and prove they were actually linked.

Opt in per build with MESHCORE_REDUCED_TLS=1. Off by default, so an ordinary build
needs no 6 MB artifact and behaves exactly as before.

    MESHCORE_REDUCED_TLS=1 pio run -e Heltec_v3_repeater_observer_mqtt

The archives lower the mbedTLS outbound record buffer from 16 KiB to 4 KiB, saving
~12 KiB of internal DRAM per TLS connection on non-PSRAM observers. The inbound
buffer stays at 16 KiB, so the contiguous allocation a handshake needs is unchanged
— this buys headroom, it does not move that floor. See docs/mbedtls-tls-footprint.md.

Three failure modes this guards against, each of which produces a firmware that looks
fine and is silently wrong:

  - a -L pointing at a missing or partial directory. The linker ignores an
    unusable search path and quietly resolves mbedTLS from the framework instead.
  - archives that do not match the manifest, e.g. left over from an earlier
    platform version.
  - a manifest and archives that agree with each other but not with the installed
    framework, which is what a platform bump without a rebuild leaves behind. That
    one links cleanly and drifts on struct layout at runtime.

So the opt-in path verifies every archive by sha256 before the build, checks the
framework's own mbedTLS archives still fingerprint as the ones these were built
against, and after the link re-reads firmware.map to confirm every libmbed*.a came
from our directory.
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
    stock = {}
    platform_id = ""
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) == 2 and parts[0] == "platform":
                platform_id = parts[1]
            elif len(parts) == 3 and parts[0] == "stock:" + arch:
                stock[parts[2]] = parts[1]
            elif len(parts) == 3 and parts[0] == arch:
                wanted[parts[2]] = parts[1]
    return wanted, stock, platform_id


# Where each espressif32 generation keeps the archives we displace. First directory
# holding all four wins, so this resolves without knowing which platform is in play.
FRAMEWORK_LIB_DIRS = (
    ("framework-arduinoespressif32", "tools/sdk/%s/lib"),
    ("framework-arduinoespressif32-libs", "%s/lib"),
    ("framework-arduinoespressif32", "tools/esp32-arduino-libs/%s/lib"),
)


def _framework_lib_dir(platform, arch):
    for package, layout in FRAMEWORK_LIB_DIRS:
        try:
            base = platform.get_package_dir(package)
        except Exception:
            base = None
        if not base:
            continue
        path = os.path.join(base, *(layout % arch).split("/"))
        if all(os.path.isfile(os.path.join(path, name)) for name in REQUIRED):
            return path
    return None


if os.environ.get("MESHCORE_REDUCED_TLS", "") not in ("1", "true", "yes"):
    Return()

project_dir = env.subst("$PROJECT_DIR")
arch = env.BoardConfig().get("build.mcu", "")
if not arch:
    _fail("could not determine board MCU, so cannot pick an archive set")

staged = os.path.join(project_dir, ".mbedtls-4k", arch)
if not os.path.isdir(staged):
    _fail("no archives for %s at %s" % (arch, staged))

wanted, stock, manifest_platform = _manifest(project_dir, arch)
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

# Bind the staged archives to the framework they were built against. The hashes above
# only prove the staged files are the ones the manifest names; they say nothing about
# whether the manifest is still current. Bump the platform without rebuilding, and
# every check above still passes while the link takes mbedTLS built against a
# different IDF — a struct-layout drift that corrupts silently at runtime. So
# fingerprint the framework's own archives, the ones being displaced: if those moved,
# the staged pair is stale by construction.
platform = env.PioPlatform()
platform_id = "%s@%s" % (platform.name, platform.version)
stock_dir = _framework_lib_dir(platform, arch)
if stock_dir is None:
    _fail("cannot locate the framework's own mbedTLS archives for %s, so the staged "
          "ones cannot be tied to a framework version" % arch)
if not stock:
    _fail("manifest has no stock:%s fingerprints — it predates the framework binding.\n"
          "Add these lines for the framework now installed (%s):\n%s"
          % (arch, platform_id,
             "\n".join("stock:%s %s %s" % (arch, _sha256(os.path.join(stock_dir, n)), n)
                       for n in REQUIRED)))

for name in REQUIRED:
    actual = _sha256(os.path.join(stock_dir, name))
    if name not in stock:
        _fail("manifest has no stock:%s entry for %s" % (arch, name))
    if actual != stock[name]:
        _fail(
            "the framework's mbedTLS archives are not the ones these were built against.\n"
            "  %s\n  manifest %s\n  installed %s\n"
            "Manifest records %s; installed is %s.\n"
            "Rebuild the reduced-TLS archives against this framework "
            "(docs/mbedtls-tls-footprint.md), then update every hash in the manifest."
            % (os.path.join(stock_dir, name), stock[name], actual,
               manifest_platform or "no platform", platform_id)
        )

if manifest_platform and manifest_platform != platform_id:
    # Hashes are the check; the version string is orientation. Identical archives
    # under a renamed platform are not a compatibility problem.
    print("reduced-TLS: manifest says %s, installed is %s — archives match, so this is "
          "only a stale label" % (manifest_platform, platform_id))

# Prepend so these satisfy mbedTLS symbols ahead of the framework's own copies:
# the linker takes each archive member from the first archive that resolves it.
env.Prepend(LIBPATH=[staged])
print("reduced-TLS: linking mbedTLS from %s (verified)" % staged)


def _verify_map(source, target, env):
    """Confirm every mbedTLS archive in the link came from our directory.

    Fails closed. Anything that stops this from *proving* the link — no map, an
    unparsable map, a short archive list — is a failure, not a warning. A warning
    here would leave exactly the hole the check exists to close: an opt-in build
    that succeeds while silently linking the framework's 16 KiB buffers.
    """
    # Derive the map name from PROGNAME rather than hardcoding firmware.map, so a
    # renamed program cannot leave us inspecting a stale or absent file.
    map_path = os.path.join(env.subst("$BUILD_DIR"),
                            env.subst("${PROGNAME}") + ".map")
    if not os.path.isfile(map_path):
        legacy = os.path.join(env.subst("$BUILD_DIR"), "firmware.map")
        map_path = legacy if os.path.isfile(legacy) else map_path
    if not os.path.isfile(map_path):
        print("\n*** reduced-TLS: no linker map at %s ***" % map_path,
              file=sys.stderr)
        print("Cannot prove the reduced-TLS archives were linked. Ensure the env "
              "emits a map (-Wl,-Map), or unset MESHCORE_REDUCED_TLS.",
              file=sys.stderr)
        env.Exit(1)
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
        return
    # Every required archive must appear. Seeing only some of them means the rest
    # resolved somewhere this parse did not recognise, which is not proof of anything.
    missing = [name for name in REQUIRED if name not in seen]
    if missing:
        print("\n*** reduced-TLS: %s names only %d of %d archives ***"
              % (os.path.basename(map_path), len(seen), len(REQUIRED)),
              file=sys.stderr)
        print("  missing: " + ", ".join(missing), file=sys.stderr)
        print("Either the map format changed or mbedTLS was resolved elsewhere; "
              "the reduced buffers cannot be assumed.", file=sys.stderr)
        env.Exit(1)
        return
    print("reduced-TLS: confirmed all %d archives linked from %s"
          % (len(REQUIRED), staged))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", _verify_map)
