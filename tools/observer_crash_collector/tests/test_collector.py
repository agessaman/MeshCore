import contextlib
import io
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
from datetime import datetime, timedelta, timezone
from unittest.mock import patch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import collector

NODE_A = 'a' * 64
NODE_B = 'b' * 64
TOPIC = 'meshcore/LAX/%s/status' % NODE_A
BASE = datetime(2026, 11, 11, 15, 44, 47, tzinfo=timezone.utc)
FW = 'v1.18.0-observer'
CRASH = dict(task='loopTask', pc='400d1a2b', exc=28, va='00000010',
             sha='deadbeef', bt='400d1a2b 400d5678')


def payload(uptime=70, reason='panic', crash=CRASH, **stats):
    fields = dict(uptime_secs=uptime, reset_reason=reason, heap_min=120000)
    fields.update(stats)
    fields = {key: value for key, value in fields.items() if value is not None}
    value = dict(origin_id=NODE_A, origin='Some Node', model='Heltec V4',
                 firmware_version=FW, timestamp=BASE.isoformat(), stats=fields)
    if crash is not None:
        value['crash'] = crash
    return json.dumps(value).encode()


def changed(data, **values):
    doc = json.loads(data)
    doc.update(values)
    return json.dumps(doc).encode()


class FileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = os.path.join(self.temp.name, 'events.jsonl')
        self.clock = BASE
        self.elapsed = 0
        self.logs = io.StringIO()
        self.redirect = contextlib.redirect_stderr(self.logs)
        self.redirect.__enter__()
        self.addCleanup(self.redirect.__exit__, None, None, None)
        self.counter = self.make_collector()

    def make_collector(self, capacity=20, max_bytes=1000000, snapshot_interval=60):
        deduper = collector.Deduper(capacity)
        collector.load_state(deduper, self.path)
        return collector.Collector(deduper, self.path, max_bytes, 65536, snapshot_interval,
                                   monotonic=lambda: self.elapsed)

    def send(self, data=None, after=0, topic=TOPIC, counter=None, retained=False):
        self.elapsed = after
        self.clock = BASE + timedelta(seconds=after)
        clock = types.SimpleNamespace(now=lambda tz: self.clock, fromisoformat=datetime.fromisoformat)
        with patch.object(collector, 'datetime', clock):
            return (counter or self.counter).handle(topic, data if data is not None else payload(), retained=retained)

    def records(self):
        return list(collector.read_records(self.path))

    def test_projection_and_diagnostics(self):
        record = self.send(payload(mqtt_outage_secs=7200, mqtt_connect_failures=9,
                                   mqtt_slots_breaker=3, battery_mv=3100,
                                   internal_heap=80000, heap_largest=60000))
        self.assertIsNotNone(record)
        self.assertEqual(['crash', 'reset'], record['kinds'])
        self.assertEqual(CRASH, record['crash'])
        for key, expected in dict(node_id=NODE_A, iata='LAX', node_name='Some Node',
                                  firmware_version=FW, uptime_secs=70,
                                  mqtt_outage_secs=7200, mqtt_connect_failures=9,
                                  mqtt_slots_breaker=3, battery_mv=3100).items():
            self.assertIn(key, record)
            self.assertEqual(expected, record[key])

    def test_normal_status_is_not_event(self):
        self.assertIsNone(self.send(payload(reason='sw', crash=None)))
        self.assertEqual([], self.records())

    def test_selfheal_software_boot(self):
        record = self.send(payload(reason='sw', crash=None, selfheal='all_slots_down', selfheal_n=2))
        self.assertEqual(['selfheal'], record['kinds'])

    def test_inherited_selfheal_is_only_context_on_panic(self):
        record = self.send(payload(selfheal='all_slots_down', selfheal_n=2))
        self.assertIsNotNone(record)
        self.assertEqual(['crash', 'reset'], record['kinds'])
        self.assertEqual('all_slots_down', record['selfheal'])

    def test_zero_selfheal_is_not_event(self):
        self.assertIsNone(self.send(payload(reason='sw', crash=None,
                                            selfheal='stop_unproven', selfheal_n=0)))

    def test_missing_stats_with_crash(self):
        doc = json.loads(payload())
        del doc['stats']
        record = self.send(json.dumps(doc).encode())
        self.assertEqual(['crash'], record['kinds'])

    def test_identical_crash_consecutive_boots(self):
        self.send()
        record = self.send(changed(payload(), timestamp='1970-01-01T00:01:10Z'), after=3600)
        self.assertIsNotNone(record)
        self.assertEqual(['crash', 'reset'], record['kinds'])
        self.assertEqual(CRASH, record['crash'])
        self.assertEqual(2, len(self.records()))

    def test_periodic_repeat_is_suppressed(self):
        self.send()
        for elapsed in (300, 600, 900):
            self.assertIsNone(self.send(payload(uptime=70 + elapsed), after=elapsed))
        self.assertEqual(1, len(self.records()))

    def test_ntp_step_forward(self):
        self.send()
        data = changed(payload(uptime=370), timestamp=(BASE + timedelta(seconds=900)).isoformat())
        self.assertIsNone(self.send(data, after=300))

    def test_ntp_step_backward(self):
        self.send()
        data = changed(payload(uptime=370), timestamp=(BASE - timedelta(seconds=900)).isoformat())
        self.assertIsNone(self.send(data, after=300))

    def test_two_no_ntp_boots(self):
        data = changed(payload(), timestamp='1970-01-01T00:01:10+00:00')
        self.send(data)
        self.send(changed(payload(uptime=600), timestamp='1970-01-01T00:10:00+00:00'), after=530)
        record = self.send(data, after=3600)
        self.assertIsNotNone(record)
        self.assertEqual(['crash', 'reset'], record['kinds'])

    def test_uptime_wrap(self):
        self.send(payload(uptime=4294900))
        data = changed(payload(uptime=232), timestamp=(BASE + timedelta(seconds=300)).isoformat())
        self.assertIsNone(self.send(data, after=300))
        self.assertEqual(1, len(self.records()))

    def test_selfheal_clears_midboot(self):
        self.send(payload(selfheal='all_slots_down', selfheal_n=2))
        self.assertIsNone(self.send(payload(uptime=370), after=300))
        self.assertEqual(1, len(self.records()))

    def test_tuple_change_without_boot_evidence(self):
        self.send(payload(uptime=None, crash=None))
        self.assertIsNone(self.send(payload(uptime=None, crash=None, reason='brownout'), after=300))

    def test_different_crash_same_boot_is_one_occurrence(self):
        self.send()
        self.assertIsNone(self.send(payload(uptime=370, crash=dict(CRASH, pc='400dffff')), after=300))

    def test_delayed_crash_only(self):
        first = self.send(payload(crash=None))
        second = self.send(payload(uptime=370, selfheal='all_slots_down', selfheal_n=2), after=300)
        self.assertEqual(['reset'], first['kinds'])
        self.assertEqual(['crash'], second['kinds'])

    def test_boot_id_overrides_uptime(self):
        self.send(payload(boot_id='aa'))
        self.assertIsNone(self.send(payload(uptime=10, boot_id='aa'), after=3600))
        record = self.send(payload(uptime=9000, boot_id='bb'), after=3601)
        self.assertIsNotNone(record)
        self.assertEqual(['crash', 'reset'], record['kinds'])

    def test_boot_id_case_normalized(self):
        self.send(payload(boot_id='ab'))
        self.assertIsNone(self.send(payload(uptime=1, boot_id='AB'), after=3600))

    def test_restart_with_intervening_boot(self):
        self.send()
        self.send(payload(uptime=370), after=300)
        second = self.make_collector()
        record = self.send(payload(uptime=100), after=3600, counter=second)
        self.assertIsNotNone(record)
        self.assertEqual(['crash', 'reset'], record['kinds'])
        self.assertEqual(CRASH, record['crash'])

    def test_restart_restores_latest_non_event_evidence(self):
        self.send()
        self.send(payload(uptime=370), after=300)
        self.counter.flush_state(force=True)
        restarted = self.make_collector()
        state = restarted.deduper.nodes[NODE_A]
        self.assertEqual(370, state.uptime_secs)
        self.assertEqual((BASE + timedelta(seconds=300)).timestamp(), state.receive_time)
        self.assertEqual(64, len(state.digest))

    def test_snapshot_interval_batches_distinct_statuses(self):
        class NoCopyNodes(collector.OrderedDict):
            def copy(self):
                raise AssertionError('per-message map copy')
        self.counter.deduper.nodes = NoCopyNodes()
        with patch.object(collector, 'save_state', wraps=collector.save_state) as save:
            for index in range(100):
                node = '%064x' % index
                self.send(changed(payload(crash=None, reason='sw'), origin_id=node), after=index / 2,
                          topic=TOPIC.replace(NODE_A, node))
                self.counter.flush_state()
            save.assert_not_called()
            self.elapsed = 60
            self.counter.flush_state()
            self.assertEqual(1, save.call_count)
            self.send(payload(uptime=130, reason='sw', crash=None), after=61)
            self.counter.flush_state()
            self.assertEqual(1, save.call_count)
            self.assertEqual(130, self.counter.deduper.nodes[NODE_A].uptime_secs)
            self.elapsed = 120
            self.counter.flush_state()
            self.assertEqual(2, save.call_count)
            self.elapsed = 180
            self.counter.flush_state()
            self.assertEqual(2, save.call_count)

    def test_snapshot_shutdown_flushes_dirty_state_once(self):
        self.send(payload(reason='sw', crash=None))
        with patch.object(collector, 'save_state', wraps=collector.save_state) as save:
            self.counter.flush_state()
            save.assert_not_called()
            self.counter.flush_state(force=True)
            self.counter.flush_state(force=True)
            self.assertEqual(1, save.call_count)
        self.assertEqual(70, self.make_collector().deduper.nodes[NODE_A].uptime_secs)

    def test_snapshot_interval_configuration(self):
        with patch.dict(os.environ, {'OBSERVER_CRASH_SNAPSHOT_INTERVAL': '17'}):
            self.assertEqual(17, collector.load_config(False).snapshot_interval)
        with patch.dict(os.environ, {'OBSERVER_CRASH_SNAPSHOT_INTERVAL': '0'}):
            with self.assertRaises(collector.ConfigError):
                collector.load_config(False)
        self.assertEqual(86400, collector.Config().snapshot_interval)
        with patch.dict(os.environ, {}, clear=False):
            os.environ.pop('OBSERVER_CRASH_SNAPSHOT_INTERVAL', None)
            self.assertEqual(86400, collector.load_config(False).snapshot_interval)

    def test_stale_snapshot_replays_newer_event_kinds(self):
        self.send(payload(uptime=1000, reason='poweron', crash=None))
        self.counter.flush_state(force=True)
        self.send(payload(uptime=5, crash=None), after=10)
        self.send(payload(uptime=15), after=20)
        restarted = self.make_collector()
        self.assertEqual(('crash', 'reset'), restarted.deduper.nodes[NODE_A].reported)
        self.assertIsNone(self.send(payload(uptime=15), after=3600, counter=restarted, retained=True))
        self.assertIsNone(self.send(payload(uptime=3895), after=3900, counter=restarted))
        record = self.send(payload(uptime=10, reason='sw', crash=None,
                                   selfheal='all_slots_down', selfheal_n=1), after=4000, counter=restarted)
        self.assertEqual(['selfheal'], record['kinds'])
        self.assertEqual([['reset'], ['crash'], ['selfheal']], [record['kinds'] for record in self.records()])

    def test_stale_snapshot_retained_replay_preserves_anchor(self):
        for index, (uptime, repeat, fresh) in enumerate(((70, 90, 3970), (4294960, 12, 3892))):
            with self.subTest(uptime=uptime):
                self.path = os.path.join(self.temp.name, 'stale-%d.jsonl' % index)
                self.elapsed = 0
                self.counter = self.make_collector()
                self.send(payload(uptime=uptime))
                self.counter.flush_state(force=True)
                data = changed(payload(uptime=repeat), timestamp='1970-01-01T00:00:00Z')
                self.assertIsNone(self.send(data, after=20))
                restarted = self.make_collector()
                self.assertIsNone(self.send(data, after=3600, counter=restarted, retained=True))
                self.assertEqual(BASE.timestamp(), restarted.deduper.nodes[NODE_A].receive_time)
                self.assertIsNone(self.send(payload(uptime=fresh), after=3900, counter=restarted))
                record = self.send(payload(uptime=10), after=4000, counter=restarted)
                self.assertEqual(['crash', 'reset'], record['kinds'])
                self.assertEqual(2, len(self.records()))

    def test_stale_snapshot_retained_new_boot_waits_for_fresh_status(self):
        self.send()
        self.counter.flush_state(force=True)
        restarted = self.make_collector()
        self.assertIsNone(self.send(payload(uptime=100), after=3600, counter=restarted, retained=True))
        record = self.send(payload(uptime=400), after=3900, counter=restarted)
        self.assertEqual(['crash', 'reset'], record['kinds'])
        self.assertEqual(CRASH, record['crash'])
        self.assertEqual(2, len(self.records()))

    def test_retained_different_boot_id_is_immediate(self):
        self.send(payload(boot_id='aa'))
        self.counter.flush_state(force=True)
        restarted = self.make_collector()
        record = self.send(payload(boot_id='bb'), after=3600, counter=restarted, retained=True)
        self.assertEqual(['crash', 'reset'], record['kinds'])

    def test_idle_network_pump_flushes_snapshot(self):
        self.send(payload(crash=None, reason='sw'))
        stop = threading.Event()
        def step(client):
            self.elapsed += 30
            if self.elapsed == 90:
                stop.set()
            return 0
        client = FakeClient([step, step, step])
        session = collector.Subscription(client, collector.Config(), self.counter)
        session.ready = True
        with patch.object(collector, 'save_state', wraps=collector.save_state) as save:
            session.pump(client, stop)
            self.assertEqual(1, save.call_count)
        self.assertEqual(70, self.make_collector().deduper.nodes[NODE_A].uptime_secs)

    def test_connection_setup_ticks_snapshot_scheduler(self):
        self.send(payload(crash=None, reason='sw'))
        release = threading.Event()
        self.addCleanup(release.set)
        client = FakeClient()
        client.connect = lambda *args: 0 if release.wait(3) else 1
        def tick():
            self.elapsed = 60
            self.counter.flush_state()
            release.set()
        self.assertTrue(collector.connect_bounded(client, collector.Config(), threading.Event(), tick=tick))
        self.assertTrue(os.path.exists(self.path + '.state'))

    def test_consistent_retained_status_advances_memory(self):
        self.send()
        self.assertIsNone(self.send(payload(uptime=90), after=20, retained=True))
        self.assertEqual(90, self.counter.deduper.nodes[NODE_A].uptime_secs)

    def test_stdin_signals_flush_snapshot(self):
        for sig in (signal.SIGTERM, signal.SIGINT):
            with self.subTest(signal=sig):
                path = os.path.join(self.temp.name, 'stdin-%d.jsonl' % sig)
                process = subprocess.Popen([sys.executable, os.path.join(os.path.dirname(HERE), 'collector.py'), '--stdin'],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    env=dict(os.environ, OBSERVER_CRASH_OUTPUT=path))
                try:
                    process.stdin.write(TOPIC.encode() + b'\t' + payload() + b'\n')
                    process.stdin.flush()
                    self.assertIn(b'kept crash,reset', process.stderr.readline())
                    self.assertFalse(os.path.exists(path + '.state'))
                    process.send_signal(sig)
                    self.assertEqual(0, process.wait(timeout=3))
                    with open(path + '.state') as handle:
                        self.assertEqual(70, json.load(handle)['nodes'][0][1]['uptime_secs'])
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.communicate()

    def test_mqtt_signals_flush_snapshot(self):
        program = r"""
import json, sys, time, types
import collector as c
class Client:
    sent = False
    def loop(self, **kwargs):
        if not self.sent:
            self.sent = True
            doc = dict(origin_id='a'*64, stats=dict(uptime_secs=70, reset_reason='sw'))
            self.on_message(self, None, types.SimpleNamespace(topic='meshcore/LAX/'+ 'a'*64 + '/status',
                payload=json.dumps(doc).encode(), retain=False))
            print('status handled', file=sys.stderr, flush=True)
        time.sleep(0.02)
        return 0
    def disconnect(self):
        return 0
c.bounded_client = lambda *args: Client()
c.connect_bounded = lambda *args, **kwargs: True
sys.exit(c.run_mqtt(c.Config(output=sys.argv[1]), types.SimpleNamespace(CallbackAPIVersion=2)))
"""
        for sig in (signal.SIGTERM, signal.SIGINT):
            with self.subTest(signal=sig):
                path = os.path.join(self.temp.name, 'mqtt-%d.jsonl' % sig)
                env = dict(os.environ, PYTHONPATH=os.path.dirname(HERE))
                process = subprocess.Popen([sys.executable, '-c', program, path],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
                try:
                    self.assertEqual(b'starting MQTT collector\n', process.stderr.readline())
                    self.assertEqual(b'status handled\n', process.stderr.readline())
                    self.assertFalse(os.path.exists(path + '.state'))
                    process.send_signal(sig)
                    self.assertEqual(0, process.wait(timeout=3))
                    with open(path + '.state') as handle:
                        self.assertEqual(70, json.load(handle)['nodes'][0][1]['uptime_secs'])
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.communicate()

    def test_retained_replay_after_long_gap(self):
        data = changed(payload(), timestamp=None)
        self.send(data)
        self.assertIsNone(self.send(data, after=86400))
        restarted = self.make_collector()
        self.assertIsNone(self.send(data, after=172800, counter=restarted))
        self.assertEqual(1, len(self.records()))

    def test_append_failure_then_retry(self):
        with patch.object(collector, 'append_record', side_effect=OSError('ENOSPC')):
            with self.assertRaises(OSError):
                self.send()
        record = self.send()
        self.assertIsNotNone(record)
        self.assertEqual(['crash', 'reset'], record['kinds'])
        self.assertEqual(1, len(self.records()))

    def test_event_recovery_after_snapshot_failure(self):
        with patch.object(collector, 'save_state', side_effect=OSError('ENOSPC')) as save:
            self.assertIsNotNone(self.send())
            save.assert_not_called()
            with self.assertRaises(OSError):
                self.counter.flush_state(force=True)
        self.assertIsNone(self.send())
        restarted = self.make_collector()
        self.assertIsNone(self.send(counter=restarted))
        self.assertEqual(1, len(self.records()))

    def test_event_fsync_failure_is_fatal(self):
        with patch.object(collector.os, 'fsync', side_effect=OSError('disk')):
            with self.assertRaises(OSError):
                self.send()
        self.assertEqual(0, len(self.counter.deduper.nodes))

    def test_torn_tail(self):
        with open(self.path, 'wb') as handle:
            handle.write(b'{"received_at":"broken')
        restarted = self.make_collector()
        self.send(counter=restarted)
        self.assertEqual(1, len(self.records()))
        with open(self.path, 'rb') as handle:
            self.assertEqual(['crash', 'reset'], json.loads(handle.readline())['kinds'])

    def test_torn_tail_preserves_complete_lines(self):
        self.send()
        with open(self.path, 'ab') as handle:
            handle.write(b'{"torn":')
        restarted = self.make_collector()
        self.send(payload(boot_id='bb', uptime=1), after=3600, counter=restarted)
        self.assertEqual(2, len(self.records()))

    def test_partial_append_failure_then_restart(self):
        real_open = open
        class BrokenWriter:
            def __enter__(inner):
                inner.handle = real_open(self.path, 'ab')
                return inner
            def write(inner, data):
                inner.handle.write(data[:20])
                raise OSError('partial write')
            def __exit__(inner, *args):
                inner.handle.close()
        def failing_open(path, mode='r', **kwargs):
            return BrokenWriter() if path == self.path and mode == 'ab' else real_open(path, mode, **kwargs)
        with patch('builtins.open', side_effect=failing_open):
            with self.assertRaises(OSError):
                self.send()
        restarted = self.make_collector()
        self.assertIsNotNone(self.send(counter=restarted))
        self.assertEqual(1, len(self.records()))

    def test_torn_tail_larger_than_reader_limit(self):
        self.send()
        with open(self.path, 'ab') as handle:
            handle.write(b'x' * 20000)
        self.make_collector()
        self.assertEqual(1, len(self.records()))

    def test_rotation_failure_stops_append(self):
        counter = self.make_collector()
        self.send(counter=counter)
        counter.max_bytes = 1
        size = os.path.getsize(self.path)
        os.mkdir(self.path + '.1')
        with self.assertRaises(OSError):
            self.send(payload(boot_id='bb', uptime=1), after=3600, counter=counter)
        self.assertEqual(size, os.path.getsize(self.path))

    def test_rotation_syncs_directory(self):
        counter = self.make_collector(max_bytes=1)
        self.send(counter=counter)
        real_replace = collector.os.replace
        seen = []
        def replace(src, dest):
            seen.append(('replace', dest))
            real_replace(src, dest)
        with patch.object(collector.os, 'replace', side_effect=replace), patch.object(
                collector, 'fsync_directory', side_effect=lambda path: seen.append(('sync', path))):
            self.send(payload(boot_id='bb', uptime=1), after=3600, counter=counter)
        pos = seen.index(('replace', self.path + '.1'))
        self.assertEqual(('sync', self.path), seen[pos + 1])

    def test_rotation_and_restart(self):
        counter = self.make_collector(max_bytes=1)
        for index in range(4):
            self.send(payload(boot_id='%x' % index, uptime=70), after=index * 3600, counter=counter)
        restarted = self.make_collector()
        self.assertIsNone(self.send(payload(boot_id='3'), after=20000, counter=restarted))
        self.assertEqual(2, len(self.records()))

    def test_rotation_snapshots_evidence_before_dropping_history(self):
        counter = self.make_collector(max_bytes=1, snapshot_interval=86400)
        nodes = ['%064x' % index for index in range(3)]
        for index, node in enumerate(nodes):
            self.send(changed(payload(), origin_id=node), after=index,
                      topic=TOPIC.replace(NODE_A, node), counter=counter)
        # Hard kill: no shutdown flush. The first node's event is no longer in either log file.
        self.assertNotIn(nodes[0], [record['node_id'] for record in self.records()])
        restarted = self.make_collector(snapshot_interval=86400)
        self.assertIsNone(self.send(changed(payload(uptime=80), origin_id=nodes[0]), after=10,
                                    topic=TOPIC.replace(NODE_A, nodes[0]), counter=restarted))

    def test_default_interval_writes_nothing_for_non_event_statuses(self):
        counter = self.make_collector(snapshot_interval=collector.DEFAULT_SNAPSHOT_INTERVAL)
        with patch.object(collector, 'save_state') as save, \
                patch.object(collector, 'append_record') as append:
            for minute in range(600):
                for index in range(20):
                    node = '%064x' % index
                    self.send(changed(payload(uptime=70 + minute * 60, reason='sw', crash=None),
                                      origin_id=node),
                              after=minute * 60, topic=TOPIC.replace(NODE_A, node), counter=counter)
                counter.flush_state()
            save.assert_not_called()
            append.assert_not_called()

    def test_unreadable_history_is_fatal(self):
        os.mkdir(self.path)
        with self.assertRaises(OSError):
            self.make_collector()

    def test_unreadable_rotated_history_is_fatal(self):
        os.mkdir(self.path + '.1')
        with self.assertRaises(OSError):
            self.make_collector()

    def test_invalid_state_is_fatal(self):
        with open(self.path + '.state', 'w') as handle:
            handle.write('{"invalid":true}')
        with self.assertRaises(OSError):
            self.make_collector()

    def test_node_cache_and_state_file_are_bounded(self):
        counter = self.make_collector(capacity=3)
        for index in range(20):
            node = '%064x' % index
            data = changed(payload(reason='sw', crash=None), origin_id=node, origin='x' * 60000)
            self.send(data, topic='meshcore/LAX/%s/status' % node, counter=counter)
        self.assertEqual(3, len(counter.deduper.nodes))
        self.assertTrue(all(len(key) == 64 for key in counter.deduper.nodes))
        counter.flush_state(force=True)
        self.assertLess(os.path.getsize(self.path + '.state'), 2000)

    def test_evicted_history_can_be_counted_again(self):
        counter = self.make_collector(capacity=1)
        self.send(counter=counter)
        self.send(changed(payload(), origin_id=NODE_B), topic=TOPIC.replace(NODE_A, NODE_B), counter=counter)
        self.assertIsNotNone(self.send(counter=counter))

    def test_invalid_topic_keys(self):
        for key in ('short', 'g' * 64, 'a' * 60000):
            self.assertIsNone(self.send(topic='meshcore/LAX/%s/status' % key))
        self.assertEqual([], self.records())

    def test_payload_origin_must_match_topic(self):
        for value in (NODE_B, 'x' * 60000, '\nFORGED\x1b[0m', None, ['bad']):
            self.assertIsNone(self.send(changed(payload(), origin_id=value)))
        self.assertEqual([], self.records())

    def test_identity_case_normalization(self):
        record = self.send(changed(payload(), origin_id=NODE_A.upper()), topic=TOPIC.upper().replace('MESHCORE', 'meshcore').replace('STATUS', 'status'))
        self.assertEqual(NODE_A, record['node_id'])

    def test_invalid_json(self):
        for data in (b'not json', b'[]', b'null', b'\xff', b'{' * 2000):
            self.assertIsNone(self.send(data))
        self.assertEqual(5, self.counter.counters.malformed)

    def test_nonfinite_json_is_rejected(self):
        for number in (b'NaN', b'Infinity', b'-Infinity', b'1e999'):
            data = payload().replace(b'"exc": 28', b'"exc": ' + number)
            self.assertIsNone(self.send(data))
        self.assertEqual([], self.records())

    def test_surrogates_are_sanitized(self):
        record = self.send(changed(payload(), origin='\ud800', model='\udfff'))
        self.assertEqual('?', record['node_name'])
        with open(self.path, encoding='utf-8') as handle:
            self.assertNotIn('\\ud800', handle.read())

    def test_long_and_control_text_is_bounded(self):
        record = self.send(changed(payload(), origin='x' * 60000,
                                   model='\nFORGED\x1b\r\x00', firmware_version='bad\nfw'))
        self.assertLessEqual(len(record['node_name']), 128)
        self.assertNotIn('\n', record['model'])
        self.assertNotIn('\x1b', record['model'])
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            collector.print_summary(self.path)
        self.assertNotIn('\x1b', output.getvalue())
        self.assertNotIn('\nfw', output.getvalue())
        self.assertNotIn('FORGED', self.logs.getvalue())

    def test_unknown_enum_tokens_are_dropped(self):
        record = self.send(payload(reason='panic\nFORGED', selfheal='x' * 60000, selfheal_n=1))
        self.assertEqual(['crash'], record['kinds'])
        self.assertNotIn('reset_reason', record)
        self.assertNotIn('selfheal', record)

    def test_shell_metacharacters_in_pc_are_rejected(self):
        record = self.send(payload(crash=dict(CRASH, pc='$(echo injected)')))
        self.assertEqual(['reset'], record['kinds'])
        self.assertNotIn('crash', record)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            collector.print_summary(self.path)
        self.assertNotIn('injected', output.getvalue())

    def test_crash_schema(self):
        invalid = [dict(CRASH, task='\nFORGED'), dict(CRASH, task='x' * 16),
                   dict(CRASH, task='bad name'), dict(CRASH, exc=True),
                   dict(CRASH, exc=-1), dict(CRASH, exc=2**32),
                   dict(CRASH, va='xyz'), dict(CRASH, sha='abcd'),
                   dict(CRASH, bt='400d1a2b ' * 9), dict(CRASH, bt='400d1a2b;evil'),
                   dict(CRASH, bt=[]), dict(CRASH, extra='oops')]
        for index, value in enumerate(invalid):
            with self.subTest(value=value):
                event = collector.parse_status(payload(crash=value), TOPIC, BASE)
                self.assertIsNone(event.crash)
        for bt in ('!', '400d1a2b!', '400d1a2b !', ' '.join(['400d1a2b'] * 8) + ' !'):
            self.assertIsNotNone(collector.parse_status(payload(crash=dict(CRASH, bt=bt)), TOPIC, BASE).crash)

    def test_integer_projection_excludes_bool_and_strings(self):
        record = self.send(payload(battery_mv=True, mqtt_outage_secs='3', mqtt_connect_failures=2.5))
        for key in ('battery_mv', 'mqtt_outage_secs', 'mqtt_connect_failures'):
            self.assertNotIn(key, record)

    def test_oversized_payload(self):
        self.assertIsNone(self.send(b'x' * 65537))
        self.assertEqual(1, self.counter.counters.malformed)

    def test_summary_counts_only_kinds(self):
        first = dict(kinds=['reset', 'selfheal'], firmware_version=FW,
                     reset_reason='panic', selfheal='all_slots_down', selfheal_n=2)
        second = dict(first, kinds=['crash'], crash=CRASH)
        with open(self.path, 'w') as handle:
            for record in (first, second):
                handle.write(json.dumps(record) + '\n')
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            collector.print_summary(self.path)
        text = output.getvalue()
        self.assertIn('panic=1', text)
        self.assertIn('all_slots_down=1', text)
        self.assertNotIn('panic=2', text)
        self.assertIn('firmware-matching-deadbeef.elf 400d1a2b 400d1a2b 400d5678', text)

    def test_summary_groups_occurrences(self):
        self.send()
        self.send(payload(boot_id='bb'), after=3600)
        self.send(changed(payload(), origin_id=NODE_B), topic=TOPIC.replace(NODE_A, NODE_B))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            collector.print_summary(self.path)
        self.assertIn('count=3 nodes=2', output.getvalue())

    def test_summary_sanitizes_legacy_records(self):
        with open(self.path, 'w') as handle:
            handle.write(json.dumps(dict(kinds=['crash', 'reset', 'selfheal'],
                firmware_version='bad\n\x1b[0m', reset_reason='panic', selfheal='$(bad)',
                crash=dict(CRASH, pc='$(bad)'))) + '\n')
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            collector.print_summary(self.path)
        self.assertNotIn('$(bad)', output.getvalue())
        self.assertNotIn('\x1b', output.getvalue())
        self.assertNotIn('hint:', output.getvalue())

    def test_summary_reports_group_limits(self):
        with open(self.path, 'w') as handle:
            for index in range(201):
                record = dict(kinds=['crash', 'reset', 'selfheal'], firmware_version=str(index),
                              node_id=NODE_A, crash=CRASH, reset_reason='panic', selfheal='all_slots_down')
                handle.write(json.dumps(record) + '\n')
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            collector.print_summary(self.path)
        self.assertIn('omitted crash occurrences outside the first 200 groups: 1', output.getvalue())
        self.assertIn('omitted category occurrences outside the first 200 firmware versions: 2', output.getvalue())

    def test_summary_reports_node_limit(self):
        with open(self.path, 'w') as handle:
            for index in range(1025):
                handle.write(json.dumps(dict(kinds=['crash'], crash=CRASH, node_id='%064x' % index)) + '\n')
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            collector.print_summary(self.path)
        self.assertIn('count=1025 nodes=>=1024', output.getvalue())

    def test_password_file_and_repr(self):
        secret_path = os.path.join(self.temp.name, 'password')
        with open(secret_path, 'w') as handle:
            handle.write('secret\n')
        with patch.dict(os.environ, {'OBSERVER_CRASH_PASSWORD': '', 'OBSERVER_CRASH_PASSWORD_FILE': secret_path}):
            config = collector.load_config(False)
        self.assertEqual('secret', config.password)
        self.assertNotIn('secret', repr(config))

    def test_storage_error_logged_once_and_nonzero(self):
        with patch.object(collector, 'run_stdin', side_effect=OSError('secret path')):
            self.assertEqual(1, collector.main(['--stdin']))
        self.assertEqual(1, self.logs.getvalue().count('storage failure'))
        self.assertNotIn('secret path', self.logs.getvalue())

    def test_stdin_sample(self):
        env = dict(os.environ, OBSERVER_CRASH_OUTPUT=self.path)
        with open(os.path.join(HERE, 'stdin-sample.txt'), 'rb') as handle:
            result = subprocess.run([sys.executable, os.path.join(os.path.dirname(HERE), 'collector.py'), '--stdin'],
                                    stdin=handle, capture_output=True, env=env, timeout=10)
        self.assertEqual(0, result.returncode)
        self.assertIn(b'seen=6 kept=3 discarded=2 malformed=1', result.stderr)
        records = self.records()
        self.assertEqual([['crash', 'reset'], ['crash', 'reset'], ['selfheal']],
                         [record['kinds'] for record in records])
        self.assertEqual(records[0]['crash'], records[1]['crash'])
        with open(self.path + '.state') as handle:
            self.assertEqual(10, json.load(handle)['nodes'][0][1]['uptime_secs'])

    def test_stdin_bounds_and_password_nondisclosure(self):
        env = dict(os.environ, OBSERVER_CRASH_OUTPUT=self.path, OBSERVER_CRASH_PASSWORD='do-not-log')
        data = TOPIC.encode() + b'\t' + payload() + b'\n' + b'x' * 100000 + b'\nmalformed\n'
        result = subprocess.run([sys.executable, os.path.join(os.path.dirname(HERE), 'collector.py'), '--stdin'],
                                input=data, capture_output=True, env=env, timeout=10)
        self.assertEqual(0, result.returncode)
        self.assertIn(b'seen=3 kept=1 discarded=0 malformed=2', result.stderr)
        self.assertNotIn(b'do-not-log', result.stderr + result.stdout)


class Reason:
    def __init__(self, value):
        self.value = value
        self.is_failure = value >= 128


class FakeClient:
    def __init__(self, steps=(), subscribe_result=0):
        self.steps = iter(steps)
        self.calls = 0
        self.subscribe_result = subscribe_result
        self.disconnected = False

    def subscribe(self, topic, qos):
        return self.subscribe_result, 7

    def connect(self, host, port, keepalive):
        return 0

    def loop(self, timeout):
        self.calls += 1
        return next(self.steps)(self)

    def disconnect(self):
        self.disconnected = True
        return 0


def connack(client):
    client.on_connect(client, None, None, Reason(0), None)
    return 0


def suback(value):
    def step(client):
        client.on_subscribe(client, None, 7, [Reason(value)], None)
        return 0
    return step


class MQTTTests(unittest.TestCase):
    def setUp(self):
        self.logs = io.StringIO()
        redirect = contextlib.redirect_stderr(self.logs)
        redirect.__enter__()
        self.addCleanup(redirect.__exit__, None, None, None)
        self.stop = threading.Event()
        self.config = collector.Config(host='127.0.0.1')

    def session(self, client):
        return collector.Subscription(client, self.config, None)

    def test_suback_rejection_is_failure(self):
        client = FakeClient([connack, suback(128)])
        session = self.session(client)
        session.pump(client, self.stop)
        self.assertFalse(session.ready)
        self.assertTrue(session.failed)
        self.assertEqual(2, client.calls)
        self.assertNotIn('ready', self.logs.getvalue())
        self.assertIn('subscription rejected', self.logs.getvalue())

    def test_immediate_subscribe_failure(self):
        client = FakeClient([connack], subscribe_result=4)
        session = self.session(client)
        session.pump(client, self.stop)
        self.assertTrue(session.failed)
        self.assertEqual(1, client.calls)

    def test_ready_only_after_grant(self):
        def verify(client):
            self.assertFalse(session.ready)
            return suback(0)(client)
        def finish(client):
            self.assertTrue(session.ready)
            self.stop.set()
            return 0
        client = FakeClient([connack, verify, finish])
        session = self.session(client)
        session.pump(client, self.stop)
        self.assertEqual(1, self.logs.getvalue().count('ready'))

    def test_pre_connack_disconnect(self):
        def disconnect(client):
            client.on_disconnect(client, None, None, Reason(128), None)
            return 0
        client = FakeClient([disconnect])
        session = self.session(client)
        session.pump(client, self.stop)
        self.assertEqual(1, client.calls)
        self.assertTrue(session.failed)

    def test_non_success_loop_before_connack(self):
        client = FakeClient([lambda client: 7])
        session = self.session(client)
        session.pump(client, self.stop)
        self.assertEqual(1, client.calls)

    def test_non_success_loop_after_ready(self):
        client = FakeClient([connack, suback(0), lambda client: 7])
        self.session(client).pump(client, self.stop)
        self.assertEqual(3, client.calls)

    def test_retry_wait_after_suback_rejection(self):
        self.retry_with([connack, suback(128)])

    def test_retry_wait_after_pre_connack_disconnect(self):
        def disconnect(client):
            client.on_disconnect(client, None, None, Reason(128), None)
            return 7
        self.retry_with([disconnect])

    def retry_with(self, steps):
        class Stop:
            stopped = False
            waits = []
            def is_set(self):
                return self.stopped
            def wait(self, delay):
                self.waits.append(delay)
                self.stopped = True
                return True
        stop = Stop()
        client = FakeClient(steps)
        module = types.SimpleNamespace(CallbackAPIVersion=object())
        with tempfile.TemporaryDirectory() as folder:
            self.config.output = os.path.join(folder, 'events')
            with patch.object(collector, 'bounded_client', return_value=client), patch.object(
                    collector, 'connect_bounded', return_value=True):
                self.assertEqual(0, collector.run_mqtt(self.config, module, stop))
        self.assertEqual(1, len(stop.waits))
        self.assertGreaterEqual(stop.waits[0], 0.5)
        self.assertTrue(client.disconnected)

    def test_run_retries_rejected_suback(self):
        self.run_transport_regression('suback')

    def test_run_retries_pre_connack_disconnect(self):
        self.run_transport_regression('disconnect')

    def run_transport_regression(self, scenario):
        real_event = threading.Event
        stop = real_event()
        waits = []
        original_wait = stop.wait
        def wait(delay):
            if delay >= 0.5:
                waits.append(delay)
                stop.set()
                return True
            return original_wait(delay)
        stop.wait = wait
        events = [stop]
        def event_factory():
            return events.pop() if events else real_event()
        class Client:
            def __init__(inner, *args, **kwargs):
                inner.calls = 0
            def connect(inner, *args):
                return 0
            def subscribe(inner, *args, **kwargs):
                return 0, 7
            def disconnect(inner):
                return 0
            def loop(inner, **kwargs):
                inner.calls += 1
                if scenario == 'disconnect':
                    inner.on_disconnect(inner, None, None, Reason(128), None)
                    if inner.calls >= 3:
                        stop.set()
                    return 7
                if inner.calls == 1:
                    inner.on_connect(inner, None, None, Reason(0), None)
                elif inner.calls == 2 and hasattr(inner, 'on_subscribe'):
                    inner.on_subscribe(inner, None, 7, [Reason(128)], None)
                else:
                    stop.set()
                return 0
        root, package, module = (types.ModuleType(name) for name in ('paho', 'paho.mqtt', 'paho.mqtt.client'))
        root.mqtt, package.client = package, module
        module.Client = Client
        module.CallbackAPIVersion = types.SimpleNamespace(VERSION2=2)
        module.MQTTv311 = 4
        modules = {'paho': root, 'paho.mqtt': package, 'paho.mqtt.client': module}
        with tempfile.TemporaryDirectory() as folder:
            config = collector.Config(host='127.0.0.1', output=os.path.join(folder, 'events'))
            with patch.dict(sys.modules, modules), patch.object(collector.threading, 'Event', side_effect=event_factory), patch.object(collector.signal, 'signal'):
                self.assertEqual(0, collector.run_mqtt(config))
        self.assertEqual(1, len(waits))
        if scenario == 'suback':
            self.assertNotIn('ready', self.logs.getvalue())

    def test_sigterm_during_setup_exits_cleanly(self):
        program = r"""
import sys, time
import collector as c
class Client:
    def connect(self, *args):
        print('setup entered', file=sys.stderr, flush=True)
        time.sleep(60)
c.bounded_client = lambda *args: Client()
sys.exit(c.run_mqtt(c.Config(host='127.0.0.1', output=sys.argv[1]),
                   type('MQTT', (), {'CallbackAPIVersion': 2})))
"""
        with tempfile.TemporaryDirectory() as folder:
            env = dict(os.environ, PYTHONPATH=os.path.dirname(HERE), PYTHONDONTWRITEBYTECODE='1')
            process = subprocess.Popen([sys.executable, '-c', program, os.path.join(folder, 'events')],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
            try:
                self.assertEqual(b'starting MQTT collector\n', process.stderr.readline())
                self.assertEqual(b'setup entered\n', process.stderr.readline())
                started = time.monotonic()
                process.send_signal(signal.SIGTERM)
                process.communicate(timeout=3)
                self.assertEqual(0, process.returncode)
                self.assertLess(time.monotonic() - started, 2)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()

    def test_stalled_setup_has_deadline(self):
        release = threading.Event()
        self.addCleanup(release.set)
        client = FakeClient()
        client.connect = lambda *args: release.wait(5)
        started = time.monotonic()
        with self.assertRaises(collector.SetupTimeout):
            collector.connect_bounded(client, self.config, self.stop, timeout=0.1)
        self.assertLess(time.monotonic() - started, 1)

    def test_stalled_setup_honors_stop(self):
        release = threading.Event()
        self.addCleanup(release.set)
        client = FakeClient()
        client.connect = lambda *args: release.wait(5)
        self.stop.set()
        started = time.monotonic()
        self.assertFalse(collector.connect_bounded(client, self.config, self.stop))
        self.assertLess(time.monotonic() - started, 1)

    def test_paho_one_is_rejected(self):
        self.assertEqual(2, collector.run_mqtt(self.config, types.SimpleNamespace(), self.stop))

    def test_wire_packet_limit_with_installed_paho(self):
        try:
            import paho.mqtt.client as mqtt
        except ImportError:
            self.skipTest('paho 2.x not installed')
        client = collector.bounded_client(mqtt, self.config)
        reader, writer = socket.socketpair()
        self.addCleanup(reader.close)
        self.addCleanup(writer.close)
        client._sock = reader
        writer.sendall(b'\x30\xff\xff\x7f')
        self.assertNotEqual(0, client._packet_read())
        self.assertEqual(0, len(client._in_packet['packet']))
        client._sock = None

    def test_websocket_buffer_limit_with_installed_paho(self):
        try:
            import paho.mqtt.client as mqtt
        except ImportError:
            self.skipTest('paho 2.x not installed')
        self.config.ws = True
        client = collector.bounded_client(mqtt, self.config)
        client._sock = types.SimpleNamespace(_readbuffer=b'x' * 70000)
        with self.assertRaises(ConnectionError):
            client._sock_recv(1)
        client._sock = None


if __name__ == '__main__':
    unittest.main()
