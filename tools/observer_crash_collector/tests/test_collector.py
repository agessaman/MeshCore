import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import collector  # noqa: E402

NODE_A = "a" * 64
NODE_B = "b" * 64
NODE_C = "c" * 64
FW = "v1.18.0.12-observer-beta-dev-3476485"
BASE_TIME = datetime(2026, 11, 11, 15, 44, 47, tzinfo=timezone.utc)
CRASH = {
    "task": "loopTask",
    "pc": "400d1a2b",
    "exc": 28,
    "va": "00000010",
    "sha": "deadbeef",
    "bt": "400d1a2b 400d5678",
}


def stats(**overrides):
    value = {
        "battery_mv": 4157,
        "uptime_secs": 70,
        "packets_sent": 3,
        "packets_received": 37,
        "errors": 0,
        "queue_len": 0,
        "noise_floor": -86,
        "tx_air_secs": 1,
        "rx_air_secs": 10,
        "recv_errors": 0,
        "internal_heap": 128548,
        "heap_largest": 118772,
        "heap_min": 126160,
    }
    value.update(overrides)
    for key in list(value):
        if value[key] is None:
            del value[key]
    return value


def payload(node=NODE_A, when=BASE_TIME, reset_reason="panic", crash=None, **stat_overrides):
    doc = {
        "status": "online",
        "timestamp": when.isoformat(),
        "origin": "Some Node",
        "origin_id": node,
        "model": "Heltec V4 OLED",
        "firmware_version": FW,
        "radio": "910.525024,62.5,7,5",
        "client_version": "meshcore/" + FW,
        "repeat": "off",
        "stats": stats(reset_reason=reset_reason, **stat_overrides),
    }
    if crash is not None:
        doc["crash"] = crash
    return json.dumps(doc).encode()


def parse(data, when=BASE_TIME, topic=None, node=NODE_A):
    if topic is None:
        topic = "meshcore/LAX/%s/status" % node
    return collector.parse_status(data, topic, when)


class EventKindTests(unittest.TestCase):
    def test_crash_event_includes_reset(self):
        event = parse(payload(crash=CRASH))
        record = collector.Deduper().observe(event)
        self.assertEqual(["crash", "reset"], record["kinds"])
        self.assertEqual(CRASH, record["crash"])
        self.assertEqual("panic", record["reset_reason"])
        self.assertEqual("LAX", record["iata"])
        self.assertEqual(NODE_A, record["node_id"])
        self.assertEqual("Some Node", record["node_name"])
        self.assertEqual(FW, record["firmware_version"])
        self.assertEqual(70, record["uptime_secs"])
        self.assertEqual(126160, record["heap_min"])
        self.assertEqual(128548, record["internal_heap"])
        self.assertEqual(118772, record["heap_largest"])
        self.assertEqual({"crash", "reset"}, set(record["kinds"]))
        self.assertEqual(
            {
                "received_at", "kinds", "iata", "node_id", "node_name", "model",
                "firmware_version", "crash", "reset_reason", "uptime_secs",
                "heap_min", "internal_heap", "heap_largest",
            },
            set(record),
        )
        self.assertEqual(set(collector.CRASH_KEYS), set(record["crash"]))

    def test_selfheal_event(self):
        event = parse(payload(reset_reason="sw", selfheal="all_slots_down", selfheal_n=2))
        record = collector.Deduper().observe(event)
        self.assertEqual(["selfheal"], record["kinds"])
        self.assertEqual("all_slots_down", record["selfheal"])
        self.assertEqual(2, record["selfheal_n"])
        self.assertNotIn("crash", record)

    def test_abnormal_reset_event(self):
        event = parse(payload(reset_reason="brownout"))
        record = collector.Deduper().observe(event)
        self.assertEqual(["reset"], record["kinds"])
        self.assertEqual("brownout", record["reset_reason"])

    def test_normal_status_is_not_an_event(self):
        self.assertIsNone(collector.Deduper().observe(parse(payload(reset_reason="sw"))))

    def test_selfheal_zero_is_not_an_event(self):
        event = parse(payload(reset_reason="sw", selfheal="stop_unproven", selfheal_n=0))
        self.assertIsNone(collector.Deduper().observe(event))

    def test_missing_stats_with_crash_is_kept(self):
        data = json.dumps(
            {
                "status": "online",
                "timestamp": BASE_TIME.isoformat(),
                "origin_id": NODE_A,
                "crash": CRASH,
            }
        ).encode()
        record = collector.Deduper().observe(parse(data))
        self.assertEqual(["crash"], record["kinds"])
        self.assertEqual(NODE_A, record["node_id"])

    def test_missing_stats_without_crash_is_discarded(self):
        data = json.dumps({"origin_id": NODE_A, "status": "online"}).encode()
        self.assertIsNone(collector.Deduper().observe(parse(data)))


class DedupTests(unittest.TestCase):
    def test_repeated_five_minute_status_is_stored_once(self):
        deduper = collector.Deduper()
        first = deduper.observe(parse(payload(crash=CRASH)))
        self.assertIsNotNone(first)
        for step in (300, 600, 900):
            repeat = payload(when=BASE_TIME + timedelta(seconds=step), crash=CRASH, uptime_secs=70 + step)
            self.assertIsNone(deduper.observe(parse(repeat, when=BASE_TIME + timedelta(seconds=step))))

    def test_reboot_produces_a_second_record(self):
        deduper = collector.Deduper()
        self.assertIsNotNone(deduper.observe(parse(payload(crash=CRASH))))
        later = BASE_TIME + timedelta(seconds=7200)
        reboot = payload(when=later, reset_reason="panic", uptime_secs=100)
        record = deduper.observe(parse(reboot, when=later))
        self.assertIsNotNone(record)
        self.assertEqual(["reset"], record["kinds"])
        self.assertEqual(100, record["uptime_secs"])

    def test_same_crash_after_reconnect_is_not_stored_again(self):
        deduper = collector.Deduper()
        data = payload(crash=CRASH)
        self.assertIsNotNone(deduper.observe(parse(data)))
        # A reconnect redelivers the retained message, same timestamp and uptime.
        self.assertIsNone(deduper.observe(parse(data)))

    def test_distinct_crash_is_stored(self):
        deduper = collector.Deduper()
        self.assertIsNotNone(deduper.observe(parse(payload(crash=CRASH))))
        other = dict(CRASH, pc="400dffff")
        later = BASE_TIME + timedelta(seconds=300)
        record = deduper.observe(
            parse(payload(when=later, crash=other, uptime_secs=370), when=later)
        )
        self.assertIsNotNone(record)
        self.assertEqual("400dffff", record["crash"]["pc"])

    def test_missing_uptime_uses_report_key(self):
        deduper = collector.Deduper()
        data = payload(reset_reason="task_wdt", uptime_secs=None)
        self.assertIsNotNone(deduper.observe(parse(data)))
        self.assertIsNone(deduper.observe(parse(data)))
        other = payload(reset_reason="panic", uptime_secs=None)
        self.assertIsNotNone(deduper.observe(parse(other)))

    def test_node_cache_is_bounded(self):
        deduper = collector.Deduper(node_capacity=3)
        for index in range(10):
            node = ("%064x" % (index + 1))
            deduper.observe(parse(payload(node=node), node=node))
        self.assertLessEqual(len(deduper.nodes), 3)


class CollectorFileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.temp.name, "events.jsonl")

    def tearDown(self):
        self.temp.cleanup()

    def make_collector(self, max_bytes=20 * 1024 * 1024, deduper=None, max_payload=65536):
        return collector.Collector(deduper or collector.Deduper(), self.path, max_bytes, max_payload)

    def lines(self):
        try:
            with open(self.path, encoding="utf-8") as handle:
                return [json.loads(line) for line in handle if line.strip()]
        except OSError:
            return []

    def test_non_event_writes_nothing(self):
        counter = self.make_collector()
        self.assertIsNone(counter.handle("meshcore/LAX/%s/status" % NODE_A, payload(reset_reason="sw")))
        self.assertFalse(os.path.exists(self.path))
        self.assertEqual(1, counter.counters.discarded)

    def test_malformed_and_oversized_payloads(self):
        counter = self.make_collector(max_payload=200)
        for data in (b"not json", b"[]", b'{"crash": 5}', b"null", b"\xff\xfe"):
            self.assertIsNone(counter.handle("meshcore/LAX/%s/status" % NODE_A, data))
        self.assertIsNone(counter.handle("meshcore/LAX/%s/status" % NODE_A, payload(crash=CRASH)))
        self.assertEqual(5, counter.counters.malformed)
        self.assertEqual(1, counter.counters.discarded)
        self.assertEqual(0, counter.counters.kept)
        self.assertFalse(os.path.exists(self.path))

    def test_state_survives_process_restart(self):
        topic = "meshcore/LAX/%s/status" % NODE_A
        first = self.make_collector()
        self.assertIsNotNone(first.handle(topic, payload(crash=CRASH)))
        self.assertEqual(1, len(self.lines()))

        deduper = collector.Deduper()
        collector.load_state(deduper, self.path)
        second = collector.Collector(deduper, self.path, 20 * 1024 * 1024, 65536)
        self.assertIsNone(second.handle(topic, payload(crash=CRASH)))
        self.assertEqual(1, len(self.lines()))

    def test_rotation_and_rebuild_reads_both_files(self):
        topic = "meshcore/LAX/%s/status" % NODE_A
        counter = self.make_collector(max_bytes=700)
        self.assertIsNotNone(counter.handle(topic, payload(crash=CRASH)))
        second_crash = dict(CRASH, pc="400d1111")
        later = BASE_TIME + timedelta(seconds=300)
        self.assertIsNotNone(counter.handle(topic, payload(when=later, crash=second_crash, uptime_secs=370)))
        third_crash = dict(CRASH, pc="400d2222")
        latest = BASE_TIME + timedelta(seconds=600)
        self.assertIsNotNone(counter.handle(topic, payload(when=latest, crash=third_crash, uptime_secs=670)))
        self.assertTrue(os.path.exists(self.path + ".1"))
        self.assertTrue(os.path.exists(self.path))

        deduper = collector.Deduper()
        collector.load_state(deduper, self.path)
        rebuilt = collector.Collector(deduper, self.path, 700, 65536)
        # The newest record lives in the current file, the two older ones in .1.
        self.assertIsNone(rebuilt.handle(topic, payload(when=latest, crash=third_crash, uptime_secs=670)))
        for crash in (CRASH, second_crash):
            self.assertIn(collector.crash_signature(NODE_A, crash), deduper.crashes)
        self.assertEqual(1, len(self.lines()))

    def test_crash_redelivered_after_restart_is_not_stored(self):
        topic = "meshcore/LAX/%s/status" % NODE_A
        self.make_collector().handle(topic, payload(crash=CRASH))
        deduper = collector.Deduper()
        collector.load_state(deduper, self.path)
        restarted = collector.Collector(deduper, self.path, 20 * 1024 * 1024, 65536)
        restarted.handle(topic, payload(crash=CRASH))
        self.assertEqual(1, len(self.lines()))

    def test_summary_groups_signatures(self):
        topic_a = "meshcore/LAX/%s/status" % NODE_A
        topic_b = "meshcore/JFK/%s/status" % NODE_B
        counter = self.make_collector()
        counter.handle(topic_a, payload(crash=CRASH))
        counter.handle(topic_b, payload(node=NODE_B, crash=CRASH))
        counter.handle(
            topic_a,
            payload(node=NODE_A, reset_reason="sw", selfheal="stop_unproven", selfheal_n=1),
        )
        counter.handle(topic_a, payload(node=NODE_A, reset_reason="brownout", uptime_secs=90))

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            collector.print_summary(self.path)
        text = output.getvalue()
        self.assertIn("fw=%s sha=deadbeef task=loopTask pc=400d1a2b" % FW, text)
        self.assertIn("count=2 nodes=2", text)
        self.assertIn(
            "hint: xtensa-esp32s3-elf-addr2line -pfiaC -e <firmware.elf matching sha deadbeef> "
            "400d1a2b 400d1a2b 400d5678",
            text,
        )
        self.assertIn("stop_unproven=1", text)
        self.assertIn("brownout=1", text)

    def test_empty_password_env_falls_back_to_file(self):
        password_path = os.path.join(self.temp.name, "mqtt-password")
        with open(password_path, "w", encoding="utf-8") as handle:
            handle.write("from-file\n")
        old = {name: os.environ.get(name) for name in ("OBSERVER_CRASH_PASSWORD", "OBSERVER_CRASH_PASSWORD_FILE")}
        os.environ["OBSERVER_CRASH_PASSWORD"] = ""
        os.environ["OBSERVER_CRASH_PASSWORD_FILE"] = password_path
        try:
            config = collector.load_config(require_host=False)
        finally:
            for name, value in old.items():
                if value is None:
                    del os.environ[name]
                else:
                    os.environ[name] = value
        self.assertEqual("from-file", config.password)
        self.assertNotIn("from-file", repr(config))

    def test_password_never_appears_in_output_or_logs(self):
        secret = "s3cret-not-for-logs"
        env = os.environ.copy()
        env.update(
            {
                "OBSERVER_CRASH_HOST": "127.0.0.1",
                "OBSERVER_CRASH_PASSWORD": secret,
                "OBSERVER_CRASH_OUTPUT": self.path,
            }
        )
        config = collector.Config()
        old_password = os.environ.get("OBSERVER_CRASH_PASSWORD")
        old_host = os.environ.get("OBSERVER_CRASH_HOST")
        os.environ["OBSERVER_CRASH_PASSWORD"] = secret
        os.environ["OBSERVER_CRASH_HOST"] = "127.0.0.1"
        try:
            config = collector.load_config(require_host=True)
        finally:
            if old_password is None:
                del os.environ["OBSERVER_CRASH_PASSWORD"]
            else:
                os.environ["OBSERVER_CRASH_PASSWORD"] = old_password
            if old_host is None:
                del os.environ["OBSERVER_CRASH_HOST"]
            else:
                os.environ["OBSERVER_CRASH_HOST"] = old_host
        self.assertNotIn(secret, repr(config))

        script = os.path.join(os.path.dirname(HERE), "collector.py")
        data = "meshcore/LAX/%s/status\t%s\n" % (NODE_A, payload(crash=CRASH).decode())
        result = subprocess.run(
            [sys.executable, script, "--stdin"],
            input=data,
            text=True,
            capture_output=True,
            env=env,
            check=False,
        )
        self.assertEqual(0, result.returncode)
        self.assertNotIn(secret, result.stdout)
        self.assertNotIn(secret, result.stderr)
        with open(self.path, encoding="utf-8") as handle:
            self.assertNotIn(secret, handle.read())


if __name__ == "__main__":
    unittest.main()
