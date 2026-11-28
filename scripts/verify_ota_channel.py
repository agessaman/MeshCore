#!/usr/bin/env python3
"""Verify every built observer binary is baked for the expected OTA channel.

Observer builds carry four manifest bases (src/helpers/OtaChannel.h), each stored
behind a tag so it can be read back from the binary (plus an ota-compat tag, which a
channel switch reads from the downloaded image before booting it):

  ota-base-native:<url>   the channel the build OTAs from by default
  ota-base-stable:<url>   production, the target of `ota branch prod`
  ota-base-dev:<url>      beta, the target of `ota branch beta`
  ota-base-poll:<url>     repeater polling, the target of `ota branch poll`

Each tag must appear with exactly one value. The named tags must match their
workflow URLs, and the native tag must match the channel named by --expect.

  python3 scripts/verify_ota_channel.py --expect prod
  python3 scripts/verify_ota_channel.py --expect poll
  python3 scripts/verify_ota_channel.py --self-test

URLs default to the OTA_MANIFEST_BASE*_URL workflow environment variables.
"""
import argparse
import os
import re
import sys

TAG_RE = re.compile(rb"ota-base-(native|stable|dev|poll):([\x21-\x7e]*)\x00")
COMPAT_RE = re.compile(rb"ota-compat:([0-9]+(?:\+[a-z]+)*)\x00")


def read_tags(data):
    tags = {"native": set(), "stable": set(), "dev": set(), "poll": set()}
    for m in TAG_RE.finditer(data):
        tags[m.group(1).decode()].add(m.group(2).decode())
    return tags


def check(data, expect, native_url, stable_url, dev_url, poll_url):
    """Return a list of problems with one binary's tags (empty when it passes)."""
    problems = []
    named = {"prod": stable_url, "beta": dev_url, "poll": poll_url}
    if len(set(named.values())) != len(named):
        problems.append("prod, beta and poll URLs must be distinct")
    expected_native = named.get(expect, native_url)
    if expect == "custom" and native_url in named.values():
        problems.append("OTA_MANIFEST_BASE_URL %s is a named channel URL" % native_url)
    if native_url != expected_native:
        problems.append("OTA_MANIFEST_BASE_URL %s is not the %s URL %s"
                        % (native_url, expect, expected_native))
    tags = read_tags(data)
    for name, want in (("native", expected_native), ("stable", stable_url), ("dev", dev_url), ("poll", poll_url)):
        found = sorted(tags[name])
        if found != [want]:
            problems.append("ota-base-%s: expected [%s], found %s" % (name, want, found or "nothing"))
    # Without its compat tag a build cannot be the target of a channel switch.
    compat = sorted({m.group(1).decode() for m in COMPAT_RE.finditer(data)})
    if len(compat) != 1:
        problems.append("ota-compat: expected one value, found %s" % (compat or "nothing"))
    return problems


def find_bins(root):
    for name in os.listdir(root):
        if name.endswith(".bin"):
            yield os.path.join(root, name)


def self_test():
    P = "https://h/v"
    B = "https://h/beta/v"
    T = "https://h/poll/v"
    C = "https://other/v"

    def blob(native, stable=P, dev=B, poll=T):
        return (b"\x00junk\x00ota-base-native:%s\x00ota-base-stable:%s"
                b"\x00ota-base-dev:%s\x00ota-base-poll:%s\x00ota-compat:2\x00") % (
                    native.encode(), stable.encode(), dev.encode(), poll.encode())

    cases = [
        ("prod build passes", blob(P), "prod", P, True),
        ("beta build passes", blob(B), "beta", B, True),
        ("poll build passes", blob(T), "poll", T, True),
        ("custom build passes", blob(C), "custom", C, True),
        ("beta build fails prod check", blob(B), "prod", P, False),
        ("prod build fails beta check", blob(P), "beta", B, False),
        ("beta build fails poll check", blob(B), "poll", T, False),
        ("poll build fails custom check", blob(T), "custom", T, False),
        ("missing tags fail", b"\x00https://h/v\x00https://h/beta/v\x00", "prod", P, False),
        ("missing poll tag fails", blob(P).replace(b"ota-base-poll:", b"poll:"), "prod", P, False),
        ("wrong poll URL fails", blob(P, poll=C), "prod", P, False),
        ("wrong dev URL fails", blob(P, dev=C), "prod", P, False),
        ("conflicting native tags fail", blob(P) + blob(B), "prod", P, False),
        ("workflow URL off-channel fails", blob(P), "prod", B, False),
        ("missing compat tag fails", blob(P).replace(b"ota-compat:2", b"ota-compat:"), "prod", P, False),
        ("conflicting compat tags fail", blob(P) + b"ota-compat:2+eth\x00", "prod", P, False),
    ]
    failed = 0
    for name, data, expect, native, ok in cases:
        got = not check(data, expect, native, P, B, T)
        if got != ok:
            failed += 1
            print("FAIL: %s" % name)
    print("self-test: %d/%d passed" % (len(cases) - failed, len(cases)))
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--expect", choices=("prod", "beta", "poll", "custom"))
    ap.add_argument("--bin-dir", default="out")
    ap.add_argument("--native-url", default=os.environ.get("OTA_MANIFEST_BASE_URL"))
    ap.add_argument("--stable-url", default=os.environ.get("OTA_MANIFEST_BASE_STABLE_URL"))
    ap.add_argument("--dev-url", default=os.environ.get("OTA_MANIFEST_BASE_DEV_URL"))
    ap.add_argument("--poll-url", default=os.environ.get("OTA_MANIFEST_BASE_POLL_URL"))
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.expect:
        ap.error("--expect is required")
    for opt in ("native_url", "stable_url", "dev_url", "poll_url"):
        if not getattr(args, opt):
            ap.error("--%s (or its environment variable) is required" % opt.replace("_", "-"))

    bins = sorted(find_bins(args.bin_dir)) if os.path.isdir(args.bin_dir) else []
    if not bins:
        print("ERROR: no .bin files in %s" % args.bin_dir, file=sys.stderr)
        return 1
    bad = 0
    for path in bins:
        with open(path, "rb") as f:
            problems = check(f.read(), args.expect, args.native_url, args.stable_url, args.dev_url, args.poll_url)
        for p in problems:
            print("ERROR: %s: %s" % (path, p), file=sys.stderr)
        bad += bool(problems)
    if bad:
        print("ERROR: %d of %d builds are not baked for %s" % (bad, len(bins), args.expect), file=sys.stderr)
        return 1
    print("OK: %d builds default to %s (%s) and carry all named channels" % (len(bins), args.expect, args.native_url))
    return 0


if __name__ == "__main__":
    sys.exit(main())
