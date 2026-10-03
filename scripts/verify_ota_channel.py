#!/usr/bin/env python3
"""Verify every built observer binary is baked for the expected OTA channel.

Observer builds carry three manifest bases (src/helpers/OtaChannel.h), each stored
behind a tag so it can be read back from the binary:

  ota-base-native:<url>   the channel the build OTAs from by default
  ota-base-stable:<url>   production, the target of `ota branch prod`
  ota-base-dev:<url>      beta, the target of `ota branch beta`

Every build contains both channel URLs, so checking for the presence or absence
of a URL can no longer tell production from beta. This reads the tags instead and
fails unless, in every .bin in out/ (the files that get published), each tag appears with exactly one value, the stable
and dev tags match the workflow's URLs, and the native tag is the URL of the
channel named by --expect.

  python3 scripts/verify_ota_channel.py --expect prod
  python3 scripts/verify_ota_channel.py --self-test

URLs default to OTA_MANIFEST_BASE_URL, OTA_MANIFEST_BASE_STABLE_URL and
OTA_MANIFEST_BASE_DEV_URL from the environment. Stdlib only.
"""
import argparse
import os
import re
import sys

TAG_RE = re.compile(rb"ota-base-(native|stable|dev):([\x21-\x7e]*)\x00")


def read_tags(data):
    tags = {"native": set(), "stable": set(), "dev": set()}
    for m in TAG_RE.finditer(data):
        tags[m.group(1).decode()].add(m.group(2).decode())
    return tags


def check(data, expect, native_url, stable_url, dev_url):
    """Return a list of problems with one binary's tags (empty when it passes)."""
    problems = []
    if stable_url == dev_url:
        problems.append("stable and dev URLs are identical (%s)" % stable_url)
    expected_native = stable_url if expect == "prod" else dev_url
    if native_url != expected_native:
        problems.append("OTA_MANIFEST_BASE_URL %s is not the %s URL %s"
                        % (native_url, expect, expected_native))
    tags = read_tags(data)
    for name, want in (("native", expected_native), ("stable", stable_url), ("dev", dev_url)):
        found = sorted(tags[name])
        if found != [want]:
            problems.append("ota-base-%s: expected [%s], found %s" % (name, want, found or "nothing"))
    return problems


def find_bins(root):
    for name in os.listdir(root):
        if name.endswith(".bin"):
            yield os.path.join(root, name)


def self_test():
    P, B = "https://h/v", "https://h/beta/v"

    def blob(native, stable=P, dev=B):
        return b"\x00junk\x00ota-base-native:%s\x00ota-base-stable:%s\x00ota-base-dev:%s\x00" % (
            native.encode(), stable.encode(), dev.encode())

    cases = [
        ("prod build passes", blob(P), "prod", P, True),
        ("beta build passes", blob(B), "beta", B, True),
        ("beta build fails prod check", blob(B), "prod", P, False),
        ("prod build fails beta check", blob(P), "beta", B, False),
        ("missing tags fail", b"\x00https://h/v\x00https://h/beta/v\x00", "prod", P, False),
        ("wrong dev URL fails", blob(P, dev="https://other/v"), "prod", P, False),
        ("conflicting native tags fail", blob(P) + blob(B), "prod", P, False),
        ("workflow URL off-channel fails", blob(P), "prod", B, False),
    ]
    failed = 0
    for name, data, expect, native, ok in cases:
        got = not check(data, expect, native, P, B)
        if got != ok:
            failed += 1
            print("FAIL: %s" % name)
    print("self-test: %d/%d passed" % (len(cases) - failed, len(cases)))
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--expect", choices=("prod", "beta"))
    ap.add_argument("--bin-dir", default="out")
    ap.add_argument("--native-url", default=os.environ.get("OTA_MANIFEST_BASE_URL"))
    ap.add_argument("--stable-url", default=os.environ.get("OTA_MANIFEST_BASE_STABLE_URL"))
    ap.add_argument("--dev-url", default=os.environ.get("OTA_MANIFEST_BASE_DEV_URL"))
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.expect:
        ap.error("--expect is required")
    for opt in ("native_url", "stable_url", "dev_url"):
        if not getattr(args, opt):
            ap.error("--%s (or its environment variable) is required" % opt.replace("_", "-"))

    bins = sorted(find_bins(args.bin_dir)) if os.path.isdir(args.bin_dir) else []
    if not bins:
        print("ERROR: no .bin files in %s" % args.bin_dir, file=sys.stderr)
        return 1
    bad = 0
    for path in bins:
        with open(path, "rb") as f:
            problems = check(f.read(), args.expect, args.native_url, args.stable_url, args.dev_url)
        for p in problems:
            print("ERROR: %s: %s" % (path, p), file=sys.stderr)
        bad += bool(problems)
    if bad:
        print("ERROR: %d of %d builds are not baked for %s" % (bad, len(bins), args.expect), file=sys.stderr)
        return 1
    print("OK: %d builds default to %s (%s) and carry both channels" % (len(bins), args.expect, args.native_url))
    return 0


if __name__ == "__main__":
    sys.exit(main())
