"""Check observer defaults, caller overrides, and fetch failures in build.sh."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

BUILD = Path(__file__).resolve().parents[2] / "build.sh"


class ReleaseBuildTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "scripts").mkdir()
        (self.root / "bin").mkdir()
        shutil.copy(BUILD, self.root / "build.sh")
        for path, source in {
            "bin/git": '#!/usr/bin/env bash\necho abcdef0\n',
            "bin/pio": '''#!/usr/bin/env bash
if [ "$1" = project ]; then
  echo '[]'
  exit 0
fi
printf '%s TLS=%s\\n' "$*" "${MESHCORE_REDUCED_TLS-unset}" >> "$BUILD_TEST_LOG"
''',
            "scripts/fetch_mbedtls_4k.sh": '''#!/usr/bin/env bash
printf 'fetch %s\\n' "$*" >> "$BUILD_TEST_LOG"
exit "${BUILD_TEST_FETCH_EXIT:-0}"
''',
        }.items():
            script = self.root / path
            script.write_text(source)
            script.chmod(0o755)
        self.variables = {key: value for key, value in os.environ.items()
                          if key not in ("MESHCORE_REDUCED_TLS", "PLATFORMIO_BUILD_FLAGS")}
        self.variables.update(PATH=str(self.root / "bin") + os.pathsep + os.environ["PATH"],
                              BUILD_TEST_LOG=str(self.root / "build.log"), FIRMWARE_VERSION="v0.0.0-test")

    def build(self, *envs, **variables):
        return subprocess.run(["bash", "build.sh", "build-firmware", *envs], cwd=self.root,
                              env={**self.variables, **variables}, text=True, capture_output=True)

    def test_observer_default_does_not_leak_to_next_target(self):
        result = self.build("observer_fixture", "repeater_fixture")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "build.log").read_text().splitlines(), [
            "fetch --all", "run -e observer_fixture TLS=auto",
            "run -t mergebin -e observer_fixture TLS=auto", "run -e repeater_fixture TLS=unset",
            "run -t mergebin -e repeater_fixture TLS=unset"])

    def test_caller_values_survive_for_both_build_steps(self):
        for mode in ("0", "", "1", "auto"):
            with self.subTest(mode=mode):
                result = self.build("observer_fixture", MESHCORE_REDUCED_TLS=mode)
                self.assertEqual(result.returncode, 0, result.stderr)
                lines = (self.root / "build.log").read_text().splitlines()[-3:]
                self.assertEqual(lines, ["fetch --all", f"run -e observer_fixture TLS={mode}",
                                        f"run -t mergebin -e observer_fixture TLS={mode}"])

    def test_fetch_failure_prevents_build(self):
        result = self.build("observer_fixture", BUILD_TEST_FETCH_EXIT="23")
        self.assertEqual(result.returncode, 23)
        self.assertEqual((self.root / "build.log").read_text(), "fetch --all\n")

    def test_non_observer_does_not_fetch_or_set_mode(self):
        result = self.build("repeater_fixture", BUILD_TEST_FETCH_EXIT="23")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "build.log").read_text().splitlines(), [
            "run -e repeater_fixture TLS=unset", "run -t mergebin -e repeater_fixture TLS=unset"])


if __name__ == "__main__":
    unittest.main()
