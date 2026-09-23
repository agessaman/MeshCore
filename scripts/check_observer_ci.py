#!/usr/bin/env python3
"""Check the observer release verification graph without publishing a build."""

from pathlib import Path
import copy
import unittest
import yaml


ROOT = Path(__file__).resolve().parents[1]
CHANNELS = {"observer-firmware", "observer-firmware-dev"}
SMOKE_ENVS = {
    "Heltec_v3_repeater_observer_mqtt",
    "Heltec_v3_room_server_observer_mqtt",
    "T_Beam_S3_Supreme_SX1262_repeater_observer_mqtt",
    "ThinkNode_M7_repeater_observer_mqtt",
    "Heltec_v3_repeater",
}


def read_workflow(name):
    # BaseLoader keeps GitHub's `on` key as text under YAML 1.1 parsers too.
    return yaml.load((ROOT / ".github/workflows" / name).read_text(), Loader=yaml.BaseLoader)


def check_release(workflow):
    jobs = workflow["jobs"]
    release = jobs["release"]
    required = {
        "verify": "./.github/workflows/run-unit-tests.yml",
        "preset-parity": "./.github/workflows/check-mqtt-preset-parity.yml",
    }
    for name, target in required.items():
        assert name in release["needs"], f"release must need {name}"
        assert jobs[name]["uses"] == target, f"{name} must call {target}"
        assert "if" not in jobs[name], f"{name} must not be optional"
        assert "continue-on-error" not in jobs[name], f"{name} must fail the run"
    # GitHub's default success() condition blocks publication if a dependency
    # fails or is skipped. An explicit condition must be separately reviewed.
    assert "if" not in release, "release must retain its default success gate"


class ReleaseGateTests(unittest.TestCase):
    def setUp(self):
        self.workflow = read_workflow("build-observer-firmwares.yml")

    def test_release_requires_both_checks(self):
        check_release(self.workflow)

    def test_removed_dependency_is_rejected(self):
        for name in ("verify", "preset-parity"):
            broken = copy.deepcopy(self.workflow)
            broken["jobs"]["release"]["needs"].remove(name)
            with self.assertRaises(AssertionError):
                check_release(broken)

    def test_failed_checks_cannot_be_bypassed(self):
        broken = copy.deepcopy(self.workflow)
        broken["jobs"]["release"]["if"] = "always()"
        with self.assertRaises(AssertionError):
            check_release(broken)


def main():
    for name in ("build-observer-firmwares.yml", "build-observer-firmwares-beta.yml"):
        check_release(read_workflow(name))
    for name in ("run-unit-tests.yml", "check-mqtt-preset-parity.yml"):
        assert "workflow_call" in read_workflow(name)["on"], f"{name} must be reusable"
    smoke = read_workflow("pr-build-check.yml")
    assert CHANNELS <= set(smoke["on"]["pull_request"]["branches"])
    assert SMOKE_ENVS <= set(smoke["jobs"]["build"]["strategy"]["matrix"]["environment"])
    native = read_workflow("run-unit-tests.yml")
    commands = "\n".join(step.get("run", "") for step in native["jobs"]["test"]["steps"])
    assert "-e native_sanitized" in commands
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ReleaseGateTests)
    if not unittest.TextTestRunner().run(suite).wasSuccessful():
        raise SystemExit(1)
    print("Observer release gates and smoke coverage passed.")


if __name__ == "__main__":
    main()
