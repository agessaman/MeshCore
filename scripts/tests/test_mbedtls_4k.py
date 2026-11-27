"""Exercise reduced-TLS selection and fail-closed archive/map validation."""
import contextlib
import hashlib
import io
import os
from pathlib import Path
import shlex
import tempfile
import unittest
from unittest.mock import patch

HOOK = Path(__file__).resolve().parents[1] / "mbedtls_4k.py"
ARCHIVES = ("libmbedcrypto.a", "libmbedtls_2.a", "libmbedtls.a", "libmbedx509.a")


class ScriptReturn(Exception):
    pass


class BuildEnv(dict):
    def __init__(self, root, arch, flags, board_flags):
        super().__init__(CPPDEFINES=[])
        self.root = root
        self.board = {"build.mcu": arch, "build.extra_flags": board_flags}
        self.flags = flags
        self.actions = []
        self.alias = None

    def subst(self, value):
        return {"$PROJECT_DIR": str(self.root), "$PIOENV": "fixture",
                "$BUILD_DIR": str(self.root / "build"), "${PROGNAME}": self.get("PROGNAME", "firmware")}[value]

    def BoardConfig(self):
        return self.board

    def GetProjectOption(self, name, default):
        assert name == "build_flags"
        return self.flags

    def ParseFlags(self, flags):
        if isinstance(flags, list):
            return {"CPPDEFINES": [d for f in flags for d in self.ParseFlags(f)["CPPDEFINES"]]}
        tokens = iter(shlex.split(flags))
        defines = []
        for token in tokens:
            if token == "-D":
                token += next(tokens)
            if token.startswith("-D"):
                defines.append(token[2:].split("=", 1) if "=" in token else token[2:])
        return {"CPPDEFINES": defines}

    def PioPlatform(self):
        root = self.root

        class Platform:
            name = "espressif32"
            version = "6.11.0"

            def get_package_dir(self, package):
                return str(root / "framework")

        return Platform()

    def Prepend(self, **kwargs):
        self.update(kwargs)

    def AppendUnique(self, **kwargs):
        for key, value in kwargs.items():
            self.setdefault(key, []).extend(value)

    def AddPostAction(self, target, action):
        self.actions.append(action)

    def Alias(self, name, target=None, action=None):
        self.alias = (name, target, action)
        return name

    def AlwaysBuild(self, target):
        self.always = target

    def Depends(self, target, dependency):
        self.dependency = (target, dependency)

    def Default(self, target):
        self.default = target

    def Exit(self, code):
        raise SystemExit(code)


class ReducedTlsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "scripts").mkdir()
        (self.root / "build").mkdir()
        staged = self.root / ".mbedtls-4k/esp32s3"
        stock = self.root / "framework/tools/sdk/esp32s3/lib"
        staged.mkdir(parents=True)
        stock.mkdir(parents=True)
        lines = ["release mbedtls-4k", "platform espressif32@6.11.0"]
        for arch, directory, content in (("esp32s3", staged, b"reduced"),
                                         ("stock:esp32s3", stock, b"stock")):
            for name in ARCHIVES:
                (directory / name).write_bytes(content)
                lines.append(f"{arch} {hashlib.sha256(content).hexdigest()} {name}")
        (self.root / "scripts/mbedtls_4k_manifest.txt").write_text("\n".join(lines))
        self.env = BuildEnv(self.root, "esp32s3", ["-D WITH_MQTT_BRIDGE=1"], [])
        self.output = io.StringIO()

    def run_hook(self, mode="auto"):
        variables = {} if mode is None else {"MESHCORE_REDUCED_TLS": mode}

        def script_return():
            raise ScriptReturn()

        namespace = {"Import": lambda name: None, "Return": script_return, "env": self.env}
        with patch.dict(os.environ, variables, clear=True), contextlib.redirect_stdout(self.output), \
                patch("atexit.register") as register:
            try:
                exec(compile(HOOK.read_text(), str(HOOK), "exec"), namespace)
            except ScriptReturn:
                pass
        self.exit_summary = register.call_args
        return namespace

    def verify(self):
        with contextlib.redirect_stdout(self.output), contextlib.redirect_stderr(io.StringIO()):
            self.env.alias[2](None, None, self.env)

    def test_active_only_after_valid_map_and_once_on_cached_check(self):
        self.run_hook()
        self.assertEqual(self.output.getvalue(), "")
        self.assertIn(("MQTT_TLS_OUT_RECORD_LEN", 4096), self.env["CPPDEFINES"])
        self.assertEqual(self.env.always, self.env.default)
        self.assertEqual(self.env.dependency[1], self.env.default)
        (self.root / "build/firmware.map").write_text("\n".join(
            f".mbedtls-4k/esp32s3/{name}(member.o)" for name in ARCHIVES))
        self.verify()
        self.verify()
        self.assertEqual(self.output.getvalue().count("reduced-TLS: fixture: active"), 1)

    def test_pre_hook_resolves_default_and_custom_program_names(self):
        for name, expected in (("program", "firmware"), ("custom", "custom")):
            with self.subTest(name=name):
                self.env["PROGNAME"] = name
                self.run_hook()
                self.assertEqual(self.env.alias[1], str(self.root / "build" / (expected + ".elf")))

    def test_incomplete_build_prints_one_nonactive_summary_at_exit(self):
        self.run_hook()
        callback, status = self.exit_summary.args
        with contextlib.redirect_stdout(self.output):
            callback(status)
            callback(status)
        self.assertEqual(self.output.getvalue(),
                         "reduced-TLS: fixture: not applied (linker map check did not run)\n")

    def test_selection_uses_project_and_board_flags(self):
        for flags, board, arch, reason in (
            (["-DWITH_MQTT_BRIDGE=1"], ["-DBOARD_HAS_PSRAM"], "esp32s3", "PSRAM"),
            (["-DWITH_MQTT_BRIDGE=1", "-DBOARD_HAS_PSRAM"], [], "esp32s3", "PSRAM"),
            (["-DOTHER"], [], "esp32s3", "not an observer"),
            (["-D WITH_MQTT_BRIDGE=1"], [], "esp32", "no archives for esp32"),
        ):
            with self.subTest(reason=reason):
                self.output = io.StringIO()
                self.env = BuildEnv(self.root, arch, flags, board)
                self.run_hook()
                self.assertEqual(self.output.getvalue(), f"reduced-TLS: fixture: not applied ({reason})\n")
                self.assertIsNone(self.env.alias)

    def test_unset_is_silent_and_off_values_report_disabled(self):
        for mode in (None, "", "0"):
            self.output = io.StringIO()
            self.run_hook(mode)
            expected = "" if mode is None else "reduced-TLS: fixture: not applied (disabled)\n"
            self.assertEqual(self.output.getvalue(), expected)
            self.assertIsNone(self.env.alias)

    def test_force_ignores_psram_but_requires_manifest_arch(self):
        self.env.board["build.extra_flags"] = ["-DBOARD_HAS_PSRAM"]
        self.run_hook("1")
        self.assertIsNotNone(self.env.alias)
        self.env.board["build.mcu"] = "esp32"
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_hook("1")
        self.assertNotIn(": active", self.output.getvalue())

    def test_missing_or_changed_archives_fail_closed(self):
        for path in (".mbedtls-4k/esp32s3/libmbedtls.a", "framework/tools/sdk/esp32s3/lib/libmbedtls.a"):
            archive = self.root / path
            original = archive.read_bytes()
            for content in (None, b"wrong"):
                with self.subTest(path=path, content=content):
                    self.output = io.StringIO()
                    if content is None:
                        archive.unlink()
                    else:
                        archive.write_bytes(content)
                    with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                        self.run_hook()
                    self.assertNotIn(": active", self.output.getvalue())
                    archive.write_bytes(original)

    def test_missing_partial_or_stock_map_cannot_report_active(self):
        self.run_hook()
        map_path = self.root / "build/firmware.map"
        for content in (None, ".mbedtls-4k/esp32s3/libmbedtls.a(member.o)", "\n".join(
                f"framework/tools/sdk/esp32s3/lib/{name}(member.o)" for name in ARCHIVES)):
            with self.subTest(content=content):
                if content is not None:
                    map_path.write_text(content)
                with self.assertRaises(SystemExit):
                    self.verify()
                self.assertNotIn(": active", self.output.getvalue())


if __name__ == "__main__":
    unittest.main()
