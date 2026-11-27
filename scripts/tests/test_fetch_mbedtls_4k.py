"""Check fetching without network access, including future manifest architectures."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "fetch_mbedtls_4k.sh"
ARCHIVES = ("libmbedcrypto.a", "libmbedtls_2.a", "libmbedtls.a", "libmbedx509.a")


class FetchTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "scripts").mkdir()
        (self.root / "bin").mkdir()
        shutil.copy(SCRIPT, self.root / "scripts/fetch_mbedtls_4k.sh")
        lines = ["release platform-6.11.0", "platform espressif32@6.11.0"]
        for arch in ("esp32s3", "esp32"):
            source = self.root / arch
            source.mkdir()
            with tarfile.open(self.root / f"mbedtls-4k-{arch}.tar.gz", "w:gz") as tar:
                for name in ARCHIVES:
                    content = (arch + name).encode()
                    (source / name).write_bytes(content)
                    tar.add(source / name, arcname=f"staged/{name}")
                    lines.append(f"{arch} {hashlib.sha256(content).hexdigest()} {name}")
                    lines.append(f"stock:{arch} unused {name}")
        (self.root / "scripts/mbedtls_4k_manifest.txt").write_text("\n".join(lines))
        curl = self.root / "bin/curl"
        curl.write_text(f"#!{sys.executable}\n" + '''import os, pathlib, shutil, sys
with open(os.environ["FETCH_CURL_LOG"], "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\\n")
if os.environ.get("FETCH_CURL_FAIL"):
    sys.exit(1)
shutil.copyfile(pathlib.Path(os.environ["FETCH_ASSETS"]) / sys.argv[-3].rsplit("/", 1)[-1], sys.argv[-1])
''')
        curl.chmod(0o755)
        self.variables = {key: value for key, value in os.environ.items()
                          if not key.startswith("MBEDTLS_4K_")}
        self.variables.update(PATH=str(self.root / "bin") + os.pathsep + os.environ["PATH"],
                              FETCH_CURL_LOG=str(self.root / "curl.log"), FETCH_ASSETS=str(self.root))

    def fetch(self, arch, **variables):
        return subprocess.run(["bash", "scripts/fetch_mbedtls_4k.sh", arch], cwd=self.root,
                              env={**self.variables, **variables}, text=True, capture_output=True)

    def test_all_uses_release_tag_retries_and_skips_verified_archives(self):
        first = self.fetch("--all")
        self.assertEqual(first.returncode, 0, first.stderr)
        calls = (self.root / "curl.log").read_text().splitlines()
        self.assertEqual(len(calls), 2)
        for arch, call in zip(("esp32", "esp32s3"), calls):
            self.assertIn(f"/platform-6.11.0/mbedtls-4k-{arch}.tar.gz", call)
            self.assertIn("--retry 3", call)
            for name in ARCHIVES:
                self.assertEqual((self.root / f".mbedtls-4k/{arch}/{name}").read_bytes(),
                                 (self.root / arch / name).read_bytes())
        second = self.fetch("--all", FETCH_CURL_FAIL="1")
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(second.stdout.count("no download"), 2)
        self.assertEqual((self.root / "curl.log").read_text().splitlines(), calls)

    def test_url_override_and_corrupt_archive_repair(self):
        first = self.fetch("esp32s3", MBEDTLS_4K_BASE_URL="https://override.invalid/assets")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertIn("https://override.invalid/assets/mbedtls-4k-esp32s3.tar.gz",
                      (self.root / "curl.log").read_text())
        damaged = self.root / ".mbedtls-4k/esp32s3/libmbedtls.a"
        damaged.write_bytes(b"wrong")
        repaired = self.fetch("esp32s3", MBEDTLS_4K_LOCAL=str(self.root / "esp32s3"))
        self.assertEqual(repaired.returncode, 0, repaired.stderr)
        self.assertEqual(damaged.read_bytes(), (self.root / "esp32s3/libmbedtls.a").read_bytes())

    def test_invalid_local_source_does_not_install_archives(self):
        (self.root / "esp32s3/libmbedtls.a").write_bytes(b"wrong")
        result = self.fetch("esp32s3", MBEDTLS_4K_LOCAL=str(self.root / "esp32s3"))
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / ".mbedtls-4k/esp32s3").exists())

    def test_unknown_arch_fails_without_download(self):
        result = self.fetch("unknown")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "curl.log").exists())


if __name__ == "__main__":
    unittest.main()
