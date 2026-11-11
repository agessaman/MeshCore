#!/usr/bin/env python3
"""Collect observer crash occurrences; see README.md for evidence limits."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import re
import secrets
import shlex
import signal
import sys
import threading
import time
from collections import OrderedDict
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from typing import Any, Iterable, List, Optional, Tuple

ABNORMAL_RESETS = frozenset({'panic', 'int_wdt', 'task_wdt', 'wdt', 'brownout'})
RESET_REASONS = ABNORMAL_RESETS | {'poweron', 'ext', 'sw', 'deepsleep', 'sdio', 'unknown'}
SELFHEAL_CAUSES = frozenset({'stop_unproven', 'client_quarantined', 'all_slots_down'})
KINDS = ('crash', 'selfheal', 'reset')
BOOT_TOLERANCE_SECS = 30.0
UPTIME_PERIOD = 2**32 / 1000
MAX_PAYLOAD = 65536
MAX_RECORD = 8192
MAX_NODES = 20000
SUMMARY_CRASH_LIMIT = 200
HEX64 = re.compile(r'[0-9a-fA-F]{64}')
HEX8 = re.compile(r'[0-9a-fA-F]{8}')
BOOT_ID = re.compile(r'[0-9a-fA-F]{1,32}')
SCALARS = ('heap_min', 'internal_heap', 'heap_largest', 'mqtt_slots_up',
           'mqtt_slots_total', 'mqtt_outage_secs', 'mqtt_connect_failures',
           'mqtt_slots_breaker', 'battery_mv')


def safe_text(value: Any, limit: int = 128) -> Optional[str]:
    if not isinstance(value, str) or not value:
        return None
    return ''.join(c if ' ' <= c <= '~' else '?' for c in value[:limit])


def log(message: str) -> None:
    print(json.dumps(message[:1024], ensure_ascii=True)[1:-1], file=sys.stderr, flush=True)


def _as_int(value: Any) -> Optional[int]:
    return value if type(value) is int and 0 <= value <= 0xffffffff else None


def hex_value(value: Any, pattern=HEX64) -> Optional[str]:
    return value.lower() if isinstance(value, str) and pattern.fullmatch(value) else None


def reject_constant(value):
    raise ValueError('nonfinite JSON number')


def decode_json(data):
    return json.loads(data, parse_constant=reject_constant,
                      parse_float=finite_float)


def finite_float(value):
    number = float(value)
    if not math.isfinite(number):
        raise ValueError('nonfinite JSON number')
    return number


def validate_crash(value: Any) -> Optional[dict]:
    if not isinstance(value, dict):
        return None
    if not {'task', 'pc', 'exc', 'va'} <= value.keys():
        return None
    if value.keys() - {'task', 'pc', 'exc', 'va', 'sha', 'bt'}:
        return None
    task = value['task']
    if not isinstance(task, str) or not 1 <= len(task) <= 15:
        return None
    if any(c < '!' or c > '~' or c in '\\"' for c in task):
        return None
    pc, va = hex_value(value['pc'], HEX8), hex_value(value['va'], HEX8)
    exc = _as_int(value['exc'])
    if pc is None or va is None or exc is None:
        return None
    result = dict(task=task, pc=pc, exc=exc, va=va)
    if 'sha' in value:
        sha = hex_value(value['sha'], HEX8)
        if sha is None:
            return None
        result['sha'] = sha
    if 'bt' in value:
        bt = value['bt']
        if not isinstance(bt, str) or len(bt) > 73:
            return None
        if not re.fullmatch(r'(?:[0-9a-fA-F]{8}(?: [0-9a-fA-F]{8}){0,7}(?: ?!)?|!)', bt):
            return None
        result['bt'] = bt.lower()
    return result


def crash_signature(crash: dict) -> str:
    return hashlib.sha256(json.dumps(crash, sort_keys=True).encode('ascii')).hexdigest()


@dataclass
class ParsedStatus:
    received_at: str
    receive_time: float
    iata: str
    node_id: str
    digest: str
    boot_id: Optional[str]
    uptime_secs: Optional[int]
    crash: Optional[dict]
    report_kinds: Tuple[str, ...]
    context: dict


def parse_status(payload: bytes, topic: str, received_at: datetime) -> Optional[ParsedStatus]:
    if not isinstance(payload, (bytes, bytearray)) or len(payload) > MAX_PAYLOAD:
        return None
    if not isinstance(topic, str) or len(topic) > 128:
        return None
    parts = topic.split('/')
    if (len(parts) != 4 or parts[0] != 'meshcore' or parts[3] != 'status'
            or not re.fullmatch(r'[A-Za-z0-9_-]{1,16}', parts[1])):
        return None
    node = hex_value(parts[2])
    if node is None:
        return None
    try:
        doc = decode_json(bytes(payload))
    except (ValueError, TypeError, RecursionError):
        return None
    if not isinstance(doc, dict):
        return None
    if 'origin_id' in doc and hex_value(doc['origin_id']) != node:
        return None
    stats = doc.get('stats')
    if not isinstance(stats, dict):
        stats = {}
    context = {}
    for source, target in (('origin', 'node_name'), ('model', 'model'),
                           ('firmware_version', 'firmware_version'), ('timestamp', 'device_timestamp')):
        value = safe_text(doc.get(source))
        if value is not None:
            context[target] = value
    reason, heal = stats.get('reset_reason'), stats.get('selfheal')
    reason = reason if isinstance(reason, str) and reason in RESET_REASONS else None
    heal = heal if isinstance(heal, str) and heal in SELFHEAL_CAUSES else None
    if reason is not None:
        context['reset_reason'] = reason
    if heal is not None:
        context['selfheal'] = heal
    count = _as_int(stats.get('selfheal_n'))
    if count is not None:
        context['selfheal_n'] = count
    for key in SCALARS:
        number = _as_int(stats.get(key))
        if number is not None:
            context[key] = number
    kinds = []
    if reason == 'sw' and heal is not None and count is not None and count > 0:
        kinds.append('selfheal')
    if reason in ABNORMAL_RESETS:
        kinds.append('reset')
    uptime = _as_int(stats.get('uptime_secs'))
    if uptime is not None and uptime > int(UPTIME_PERIOD):
        uptime = None
    return ParsedStatus(received_at.isoformat(timespec='microseconds'), received_at.timestamp(),
                        parts[1], node, hashlib.sha256(payload).hexdigest(),
                        hex_value(stats.get('boot_id'), BOOT_ID), uptime,
                        validate_crash(doc.get('crash')), tuple(kinds), context)


@dataclass
class NodeState:
    receive_time: float
    uptime_secs: Optional[int]
    boot_id: Optional[str]
    digest: Optional[str]
    reported: Tuple[str, ...]


class Deduper:
    """Prepare a candidate without acknowledging any event."""

    def __init__(self, node_capacity: int = MAX_NODES) -> None:
        self.nodes = OrderedDict()
        self.node_capacity = min(MAX_NODES, max(1, node_capacity))
        self.sequence = 0

    def _put_node(self, node_id: str, state: NodeState) -> None:
        self.nodes[node_id] = state
        self.nodes.move_to_end(node_id)
        while len(self.nodes) > self.node_capacity:
            self.nodes.popitem(last=False)

    @staticmethod
    def _rebooted(previous: NodeState, event: ParsedStatus) -> bool:
        if previous.boot_id is not None and event.boot_id is not None:
            return previous.boot_id != event.boot_id
        if previous.digest == event.digest:
            return False
        if previous.uptime_secs is None or event.uptime_secs is None:
            return False
        expected = previous.uptime_secs + max(0, event.receive_time - previous.receive_time)
        wraps = round((expected - event.uptime_secs) / UPTIME_PERIOD)
        if wraps >= 1 and abs(event.uptime_secs + wraps * UPTIME_PERIOD - expected) <= BOOT_TOLERANCE_SECS:
            return False
        return event.uptime_secs < expected - BOOT_TOLERANCE_SECS

    def prepare(self, event: ParsedStatus):
        previous = self.nodes.get(event.node_id)
        reboot = previous is None or self._rebooted(previous, event)
        reported = set() if reboot else set(previous.reported)
        available = set(event.report_kinds)
        if event.crash is not None:
            available.add('crash')
        kinds = [kind for kind in KINDS if kind in available - reported]
        if previous is not None and previous.digest == event.digest and not reboot:
            return None, previous
        state = NodeState(event.receive_time, event.uptime_secs,
                          event.boot_id or (previous.boot_id if previous and not reboot else None),
                          event.digest, tuple(k for k in KINDS if k in reported | available))
        record = None
        if kinds:
            record = dict(received_at=event.received_at, kinds=kinds, iata=event.iata,
                          node_id=event.node_id, **event.context)
            if event.uptime_secs is not None:
                record['uptime_secs'] = event.uptime_secs
            if 'crash' in kinds:
                record['crash'] = event.crash
            record['state_seq'] = self.sequence + 1
            record['boot_evidence'] = asdict(state)
        return record, state

    def commit(self, event: ParsedStatus, state: NodeState) -> None:
        self.sequence += 1
        self._put_node(event.node_id, state)


def fsync_directory(path: str) -> None:
    fd = os.open(os.path.dirname(os.path.abspath(path)), os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def bounded_lines(handle, limit: int):
    while True:
        line = handle.readline(limit + 1)
        if not line:
            return
        if len(line) > limit:
            while line and not line.endswith(b'\n'):
                line = handle.readline(limit + 1)
            yield None
        else:
            yield line


def read_records(output_path: str) -> Iterable[dict]:
    for path in (output_path + '.1', output_path):
        try:
            handle = open(path, 'rb')
        except FileNotFoundError:
            continue
        with handle:
            for line in bounded_lines(handle, MAX_RECORD):
                if line is None:
                    continue
                try:
                    record = decode_json(line)
                except (ValueError, RecursionError):
                    continue
                if isinstance(record, dict):
                    yield record


def repair_tail(path: str) -> None:
    try:
        handle = open(path, 'rb')
    except FileNotFoundError:
        return
    with handle:
        handle.seek(0, os.SEEK_END)
        size = handle.tell()
        if not size:
            return
        handle.seek(size - 1)
        if handle.read(1) == b'\n':
            return
        position = size
        end = 0
        while position:
            start = max(0, position - MAX_RECORD)
            handle.seek(start)
            chunk = handle.read(position - start)
            newline = chunk.rfind(b'\n')
            if newline >= 0:
                end = start + newline + 1
                break
            position = start
    with open(path, 'r+b') as handle:
        handle.truncate(end)
        handle.flush()
        os.fsync(handle.fileno())


def state_from_dict(value: Any) -> NodeState:
    if not isinstance(value, dict):
        raise ValueError('invalid state')
    receive = value.get('receive_time')
    if type(receive) not in (float, int) or not 0 <= receive <= 1e12 or not math.isfinite(receive):
        raise ValueError('invalid receive time')
    uptime = value.get('uptime_secs')
    if uptime is not None and (_as_int(uptime) is None or uptime > int(UPTIME_PERIOD)):
        raise ValueError('invalid uptime')
    boot, digest, reported = value.get('boot_id'), value.get('digest'), value.get('reported')
    if boot is not None and hex_value(boot, BOOT_ID) is None:
        raise ValueError('invalid boot ID')
    if digest is not None and hex_value(digest) is None:
        raise ValueError('invalid digest')
    if not isinstance(reported, list) or len(reported) > 3 or any(k not in KINDS for k in reported):
        raise ValueError('invalid kinds')
    return NodeState(float(receive), uptime, boot, digest, tuple(reported))


def load_state(deduper: Deduper, output_path: str) -> None:
    for path in (output_path + '.1', output_path):
        repair_tail(path)
    try:
        with open(output_path + '.state', 'rb') as handle:
            raw = handle.read(MAX_NODES * 512 + 1024)
            if handle.read(1):
                raise ValueError('oversized state')
        saved = decode_json(raw)
        if not isinstance(saved, dict) or saved.get('version') != 1:
            raise ValueError('invalid state version')
        seq = saved['sequence']
        if type(seq) is not int or not 0 <= seq <= 2**63 - 1:
            raise ValueError('invalid sequence')
        rows = saved['nodes']
        if not isinstance(rows, list) or len(rows) > MAX_NODES:
            raise ValueError('invalid nodes')
        for node, value in rows:
            if hex_value(node) is None:
                raise ValueError('invalid node key')
            deduper._put_node(node, state_from_dict(value))
        deduper.sequence = seq
    except FileNotFoundError:
        pass
    except (ValueError, TypeError, KeyError, RecursionError) as error:
        raise OSError('invalid collector state') from error
    for record in read_records(output_path):
        node = hex_value(record.get('node_id'))
        if node is None:
            continue
        if 'boot_evidence' in record:
            try:
                seq = record['state_seq']
                if type(seq) is not int or not 0 <= seq <= 2**63 - 1:
                    raise ValueError('invalid sequence')
                state = state_from_dict(record['boot_evidence'])
            except (ValueError, TypeError, KeyError) as error:
                raise OSError('invalid event evidence') from error
            if seq > deduper.sequence:
                deduper._put_node(node, state)
                deduper.sequence = seq
        elif not deduper.sequence:
            try:
                receive = datetime.fromisoformat(record['received_at']).timestamp()
                state = NodeState(receive, _as_int(record.get('uptime_secs')), None, None,
                                  tuple(k for k in KINDS if k in record.get('kinds', [])))
                deduper._put_node(node, state)
            except (ValueError, TypeError, KeyError):
                continue


def save_state(deduper: Deduper, path: str, event: ParsedStatus, state: NodeState) -> None:
    nodes = deduper.nodes.copy()
    nodes[event.node_id] = state
    nodes.move_to_end(event.node_id)
    while len(nodes) > deduper.node_capacity:
        nodes.popitem(last=False)
    saved = dict(version=1, sequence=deduper.sequence + 1,
                 nodes=[[key, asdict(value)] for key, value in nodes.items()])
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path + '.state.tmp', 'w', encoding='ascii') as handle:
        json.dump(saved, handle, ensure_ascii=True, allow_nan=False, separators=(',', ':'))
        handle.write('\n')
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(path + '.state.tmp', path + '.state')
    fsync_directory(path)


def append_record(output_path: str, record: dict, max_bytes: int) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    line = (json.dumps(record, separators=(',', ':'), ensure_ascii=True, allow_nan=False) + '\n').encode('ascii')
    if len(line) > MAX_RECORD:
        raise OSError('oversized event record')
    try:
        size = os.path.getsize(output_path)
    except FileNotFoundError:
        size = 0
    if size and size + len(line) > max_bytes:
        os.replace(output_path, output_path + '.1')
        fsync_directory(output_path)
    with open(output_path, 'ab') as handle:
        if handle.write(line) != len(line):
            raise OSError('short event write')
        handle.flush()
        os.fsync(handle.fileno())
    fsync_directory(output_path)

class Counters:
    def __init__(self) -> None:
        self.seen = 0
        self.kept = 0
        self.discarded = 0
        self.malformed = 0
        self._last_log = time.monotonic()
        self._pending = False

    def count(self, name: str) -> None:
        setattr(self, name, getattr(self, name) + 1)
        self._pending = True

    def maybe_log(self) -> None:
        now = time.monotonic()
        if not self._pending or now - self._last_log < 3600:
            return
        log(
            "counters seen=%d kept=%d discarded=%d malformed=%d"
            % (self.seen, self.kept, self.discarded, self.malformed)
        )
        self._last_log = now
        self._pending = False


class Collector:
    """Transport-independent glue: parse, dedup, append, count."""

    def __init__(self, deduper: Deduper, output_path: str, max_bytes: int, max_payload: int) -> None:
        self.deduper = deduper
        self.output_path = output_path
        self.max_bytes = max_bytes
        self.max_payload = max_payload
        self.counters = Counters()

    def handle(self, topic: str, payload: bytes, received_at: Optional[datetime] = None) -> Optional[dict]:
        self.counters.count("seen")
        event = None
        if len(payload) <= self.max_payload:
            event = parse_status(payload, topic, received_at or datetime.now(timezone.utc))
        if event is None:
            self.counters.count("malformed")
            self.counters.maybe_log()
            return None
        record, state = self.deduper.prepare(event)
        if state is not self.deduper.nodes.get(event.node_id):
            if record is not None:
                append_record(self.output_path, record, self.max_bytes)
            save_state(self.deduper, self.output_path, event, state)
            self.deduper.commit(event, state)
        if record is None:
            self.counters.count("discarded")
        else:
            self.counters.count("kept")
            log("kept %s node=%s iata=%s at=%s" % (
                ",".join(record["kinds"]), event.node_id, event.iata, event.received_at))
        self.counters.maybe_log()
        return record


@dataclass
class Config:
    host: str = ""
    port: int = 1883
    tls: bool = False
    ws: bool = False
    ws_path: str = "/mqtt"
    username: Optional[str] = None
    password: Optional[str] = field(default=None, repr=False)
    client_id: str = ""
    topic: str = "meshcore/+/+/status"
    keepalive: int = 60
    ca_file: Optional[str] = None
    tls_insecure: bool = False
    output: str = "crash-events.jsonl"
    max_bytes: int = 20 * 1024 * 1024
    max_payload: int = 65536
    node_cache: int = 20000
    backoff_max: int = 60


class ConfigError(Exception):
    pass


def _env_int(name: str, default: int) -> int:
    raw = os.environ.get(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        value = int(raw.strip())
    except ValueError:
        raise ConfigError("%s must be an integer" % name)
    if value <= 0:
        raise ConfigError("%s must be positive" % name)
    return value


def _env_bool(name: str, default: bool) -> bool:
    raw = os.environ.get(name)
    if raw is None or raw.strip() == "":
        return default
    value = raw.strip().lower()
    if value in ("1", "true", "yes", "on"):
        return True
    if value in ("0", "false", "no", "off"):
        return False
    raise ConfigError("%s must be true or false" % name)


def load_config(require_host: bool) -> Config:
    config = Config()
    config.host = os.environ.get("OBSERVER_CRASH_HOST", "").strip()
    config.tls = _env_bool("OBSERVER_CRASH_TLS", False)
    config.ws = _env_bool("OBSERVER_CRASH_WS", False)
    config.ws_path = os.environ.get("OBSERVER_CRASH_WS_PATH", "/mqtt").strip() or "/mqtt"
    config.port = _env_int("OBSERVER_CRASH_PORT", 8883 if config.tls else 1883)
    config.username = os.environ.get("OBSERVER_CRASH_USERNAME") or None
    config.client_id = os.environ.get("OBSERVER_CRASH_CLIENT_ID", "").strip()
    config.topic = os.environ.get("OBSERVER_CRASH_TOPIC", "meshcore/+/+/status").strip() or "meshcore/+/+/status"
    config.keepalive = _env_int("OBSERVER_CRASH_KEEPALIVE", 60)
    config.ca_file = os.environ.get("OBSERVER_CRASH_CA_FILE") or None
    config.tls_insecure = _env_bool("OBSERVER_CRASH_TLS_INSECURE", False)
    config.output = os.environ.get("OBSERVER_CRASH_OUTPUT", "crash-events.jsonl")
    config.max_bytes = _env_int("OBSERVER_CRASH_MAX_BYTES", 20 * 1024 * 1024)
    config.max_payload = min(MAX_PAYLOAD, _env_int("OBSERVER_CRASH_MAX_PAYLOAD", MAX_PAYLOAD))
    config.node_cache = min(MAX_NODES, _env_int("OBSERVER_CRASH_NODE_CACHE", MAX_NODES))
    config.backoff_max = _env_int("OBSERVER_CRASH_BACKOFF_MAX", 60)
    if not config.client_id:
        config.client_id = "observer-crash-collector-" + secrets.token_hex(4)

    config.password = os.environ.get("OBSERVER_CRASH_PASSWORD") or None
    password_file = os.environ.get("OBSERVER_CRASH_PASSWORD_FILE")
    if config.password is None and password_file:
        try:
            with open(password_file, "r", encoding="utf-8") as handle:
                config.password = handle.read().rstrip("\r\n")
        except OSError:
            raise ConfigError("cannot read OBSERVER_CRASH_PASSWORD_FILE")

    if require_host and not config.host:
        raise ConfigError("OBSERVER_CRASH_HOST is required")
    return config


def run_stdin(config: Config) -> int:
    deduper = Deduper(config.node_cache)
    load_state(deduper, config.output)
    collector = Collector(deduper, config.output, config.max_bytes, config.max_payload)
    for raw in bounded_lines(sys.stdin.buffer, config.max_payload + 130):
        if raw is None or b'\t' not in raw:
            collector.counters.count('seen')
            collector.counters.count('malformed')
            continue
        topic, payload = raw.rstrip(b'\r\n').split(b'\t', 1)
        collector.handle(topic.decode('ascii', errors='replace'), payload)
    log('stdin seen=%d kept=%d discarded=%d malformed=%d' % (
        collector.counters.seen, collector.counters.kept,
        collector.counters.discarded, collector.counters.malformed))
    return 0


class SetupTimeout(Exception):
    pass


def connect_bounded(client, config: Config, stop, timeout: float = 8.0) -> bool:
    done = threading.Event()
    succeeded = []

    def setup():
        try:
            succeeded.append(client.connect(config.host, config.port, config.keepalive) == 0)
        except (OSError, ValueError):
            succeeded.append(False)
        finally:
            done.set()

    threading.Thread(target=setup, daemon=True).start()
    deadline = time.monotonic() + timeout
    while not done.is_set():
        if stop.wait(0.05):
            return False
        if time.monotonic() >= deadline:
            raise SetupTimeout()
    return bool(succeeded and succeeded[0])


class Subscription:
    def __init__(self, client, config: Config, collector: Collector):
        self.ready = False
        self.failed = False
        self.mid = None
        self.config = config
        self.collector = collector
        client.on_connect = self.on_connect
        client.on_disconnect = self.on_disconnect
        client.on_subscribe = self.on_subscribe
        client.on_message = self.on_message

    def on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            self.failed = True
            log('MQTT connection rejected')
            return
        result, self.mid = client.subscribe(self.config.topic, qos=0)
        if result != 0:
            self.failed = True
            log('MQTT subscribe failed')

    def on_subscribe(self, client, userdata, mid, reason_codes, properties):
        if mid != self.mid or not reason_codes or any(code.is_failure or code.value not in (0, 1, 2) for code in reason_codes):
            self.failed = True
            log('MQTT subscription rejected')
            return
        self.ready = True
        log('MQTT ready: subscription granted')

    def on_disconnect(self, client, userdata, flags, reason_code, properties):
        self.ready = False
        self.failed = True

    def on_message(self, client, userdata, message):
        self.collector.handle(message.topic, message.payload)

    def pump(self, client, stop, timeout: float = 8.0) -> None:
        deadline = time.monotonic() + timeout
        while not stop.is_set() and not self.failed:
            if not self.ready and time.monotonic() >= deadline:
                log('MQTT subscription handshake timed out')
                break
            if client.loop(timeout=0.5) != 0:
                self.failed = True
                break


def bounded_client(mqtt, config: Config):
    class Client(mqtt.Client):
        def _sock_recv(self, size):
            # Paho 2.x otherwise buffers the entire advertised MQTT packet.
            if self._in_packet['remaining_length'] > config.max_payload + 256:
                raise ConnectionError('MQTT packet exceeds limit')
            # Paho retains consumed bytes until the WebSocket frame ends.
            if config.ws and len(getattr(self._sock, '_readbuffer', b'')) > config.max_payload + 512:
                raise ConnectionError('WebSocket frame exceeds limit')
            return super()._sock_recv(size)

    client = Client(mqtt.CallbackAPIVersion.VERSION2, client_id=config.client_id,
                    clean_session=True, protocol=mqtt.MQTTv311,
                    transport='websockets' if config.ws else 'tcp', reconnect_on_failure=False)
    client.connect_timeout = 5.0
    if config.ws:
        client.ws_set_options(path=config.ws_path)
    if config.tls:
        client.tls_set(ca_certs=config.ca_file)
        if config.tls_insecure:
            client.tls_insecure_set(True)
    if config.username is not None:
        client.username_pw_set(config.username, config.password)
    return client


def run_mqtt(config: Config, mqtt_module=None, stop=None) -> int:
    if mqtt_module is None:
        try:
            import paho.mqtt.client as mqtt_module
        except ImportError:
            log("paho-mqtt 2.x is required (pip install 'paho-mqtt>=2,<3')")
            return 2
    if not hasattr(mqtt_module, 'CallbackAPIVersion'):
        log('paho-mqtt 2.x is required')
        return 2
    if stop is None:
        stop = threading.Event()
        def request_stop(signum, frame):
            stop.set()
        signal.signal(signal.SIGTERM, request_stop)
        signal.signal(signal.SIGINT, request_stop)
    deduper = Deduper(config.node_cache)
    load_state(deduper, config.output)
    collector = Collector(deduper, config.output, config.max_bytes, config.max_payload)
    backoff = 1.0
    log('starting MQTT collector')
    while not stop.is_set():
        client = bounded_client(mqtt_module, config)
        subscription = Subscription(client, config, collector)
        started = time.monotonic()
        connected = False
        try:
            connected = connect_bounded(client, config, stop)
            if connected and not stop.is_set():
                subscription.pump(client, stop)
        except SetupTimeout:
            log('MQTT setup deadline exceeded; stopping')
            return 1
        finally:
            if connected:
                try:
                    client.disconnect()
                except OSError:
                    log('MQTT disconnect failed')
        if stop.is_set():
            break
        log('MQTT attempt ended; retrying with backoff')
        if subscription.ready and time.monotonic() - started >= 30:
            backoff = 1.0
        delay = min(config.backoff_max, backoff)
        stop.wait(delay / 2 + random.uniform(0, delay / 2))
        backoff = min(config.backoff_max, backoff * 2)
    return 0


def _crash_addresses(crash: dict) -> List[str]:
    crash = validate_crash(crash)
    return crash.get('bt', '').replace('!', '').split() if crash else []


def print_summary(output_path: str) -> int:
    groups = {}
    selfheal = {}
    resets = {}
    omitted = 0
    omitted_categories = 0
    for record in read_records(output_path):
        firmware = safe_text(record.get('firmware_version')) or '(unknown)'
        node = hex_value(record.get('node_id'))
        seen = safe_text(record.get('received_at'), 40)
        kinds = record.get('kinds', [])
        if not isinstance(kinds, list):
            continue
        crash = validate_crash(record.get('crash'))
        if crash is not None and 'crash' in kinds:
            key = (firmware, crash.get('sha', ''), crash['task'], crash['pc'], crash_signature(crash))
            if key not in groups and len(groups) >= SUMMARY_CRASH_LIMIT:
                omitted += 1
            else:
                entry = groups.setdefault(key, dict(count=0, nodes=set(), capped=False,
                                                    first=seen, last=seen, addresses=[]))
                entry['count'] += 1
                if node:
                    if len(entry['nodes']) < 1024:
                        entry['nodes'].add(node)
                    elif node not in entry['nodes']:
                        entry['capped'] = True
                if seen is not None:
                    entry['first'] = min(entry['first'] or seen, seen)
                    entry['last'] = max(entry['last'] or seen, seen)
                addresses = _crash_addresses(crash)
                if len(addresses) > len(entry['addresses']):
                    entry['addresses'] = addresses
        cause = record.get('selfheal')
        if 'selfheal' in kinds and isinstance(cause, str) and cause in SELFHEAL_CAUSES:
            if firmware in selfheal or len(selfheal) < SUMMARY_CRASH_LIMIT:
                counts = selfheal.setdefault(firmware, {})
                counts[cause] = counts.get(cause, 0) + 1
            else:
                omitted_categories += 1
        reason = record.get('reset_reason')
        if 'reset' in kinds and isinstance(reason, str) and reason in ABNORMAL_RESETS:
            if firmware in resets or len(resets) < SUMMARY_CRASH_LIMIT:
                counts = resets.setdefault(firmware, {})
                counts[reason] = counts.get(reason, 0) + 1
            else:
                omitted_categories += 1
    print('Crash signatures')
    if not groups:
        print('  none')
    for (firmware, sha, task, pc, signature), entry in sorted(groups.items(), key=lambda item: (-item[1]['count'], item[0])):
        print('  fw=%s sha=%s task=%s pc=%s' % (firmware, sha or '?', task, pc))
        print('    count=%d nodes=%s%d first=%s last=%s' % (
            entry['count'], '>=' if entry['capped'] else '', len(entry['nodes']),
            entry['first'] or '?', entry['last'] or '?'))
        arguments = ['xtensa-esp32s3-elf-addr2line', '-pfiaC', '-e',
                     'firmware-matching-%s.elf' % (sha or 'unknown'), pc] + entry['addresses']
        print('    hint: ' + shlex.join(arguments))
    if omitted:
        print('  omitted crash occurrences outside the first 200 groups: %d' % omitted)
    for title, categories in (('Self-heal causes', selfheal), ('Abnormal reset reasons', resets)):
        print(title)
        if not categories:
            print('  none')
        for firmware, counts in sorted(categories.items()):
            print('  %s: %s' % (firmware, ' '.join('%s=%d' % item for item in sorted(counts.items()))))
    if omitted_categories:
        print('  omitted category occurrences outside the first 200 firmware versions: %d' % omitted_categories)
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description='Collect observer crash reports from MQTT status topics.')
    parser.add_argument('--stdin', action='store_true', help='read topic<TAB>payload lines from stdin')
    parser.add_argument('--dry-run', action='store_true', help='alias for --stdin')
    parser.add_argument('--summary', action='store_true', help='summarize existing output files')
    args = parser.parse_args(argv)
    try:
        if args.summary:
            return print_summary(os.environ.get('OBSERVER_CRASH_OUTPUT', 'crash-events.jsonl'))
        config = load_config(require_host=not (args.stdin or args.dry_run))
        return run_stdin(config) if args.stdin or args.dry_run else run_mqtt(config)
    except ConfigError as error:
        log(str(error))
        return 2
    except OSError:
        log('storage failure; stopping collector')
        return 1
    except KeyboardInterrupt:
        return 0


if __name__ == '__main__':
    sys.exit(main())
