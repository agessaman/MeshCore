#!/usr/bin/env python3
"""Collect MeshCore observer crash reports from MQTT status topics.

Read-only subscriber. Keeps only panic, self-heal, and abnormal-reset status
messages in an append-only JSON Lines file. See README.md for the contract.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import secrets
import signal
import sys
import threading
import time
from collections import OrderedDict
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Dict, Iterable, List, Optional, Tuple

# From src/helpers/bridges/MQTTBridge.cpp, resetReasonToken().
ABNORMAL_RESETS = frozenset({"panic", "int_wdt", "task_wdt", "wdt", "brownout"})

# CrashReportFormat::formatJson() keys: task, pc, exc, va, sha, bt.
CRASH_KEYS = ("task", "pc", "exc", "va", "sha", "bt")

# Boot-time estimates closer than this are the same boot. Uptime has one-second
# resolution and the device timestamp is NTP-synced; the first status after a
# boot is published at connect, about 70 s in (firmware example), so a real
# reboot shifts the estimate by at least that much.
BOOT_TOLERANCE_SECS = 30.0

SUMMARY_CRASH_LIMIT = 200


def log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def _as_int(value: Any) -> Optional[int]:
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    return None


def _as_str(value: Any) -> Optional[str]:
    return value if isinstance(value, str) and value else None


def _normalize_node_id(value: Optional[str]) -> str:
    if not value:
        return ""
    if len(value) == 64 and all(c in "0123456789abcdefABCDEF" for c in value):
        return value.lower()
    return value


def crash_signature(node_id: str, crash: dict) -> str:
    """Stable, bounded identity for a crash object on one node."""
    canonical = json.dumps(crash, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    digest = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
    return node_id + "\x00" + digest


def _parse_time(value: Any) -> Optional[float]:
    if not isinstance(value, str):
        return None
    text = value.strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.timestamp()


@dataclass
class ParsedStatus:
    received_at: str
    iata: str
    node_id: str
    node_name: Optional[str]
    model: Optional[str]
    firmware_version: Optional[str]
    crash: Optional[dict]
    crash_signature: Optional[str]
    report_key: Optional[Tuple[Optional[str], Optional[str], Optional[int]]]
    report_kinds: Tuple[str, ...]
    uptime_secs: Optional[int]
    boot_time: Optional[float]
    scalars: Dict[str, Any] = field(default_factory=dict)


def parse_status(payload: bytes, topic: str, received_at: datetime) -> Optional[ParsedStatus]:
    """Parse one status payload. Returns None for malformed or non-object input."""
    if not isinstance(payload, (bytes, bytearray)):
        return None
    try:
        doc = json.loads(bytes(payload))
    except (ValueError, TypeError, RecursionError):
        return None
    if not isinstance(doc, dict):
        return None

    stats = doc.get("stats")
    if not isinstance(stats, dict):
        stats = {}

    parts = topic.split("/")
    iata = parts[1] if len(parts) > 1 else ""
    topic_node = _normalize_node_id(parts[2] if len(parts) > 2 else "")
    node_id = _normalize_node_id(_as_str(doc.get("origin_id"))) or topic_node

    crash = doc.get("crash")
    crash = crash if isinstance(crash, dict) else None
    signature = None
    if crash is not None:
        try:
            signature = crash_signature(node_id, crash)
        except (TypeError, ValueError):
            crash = None

    reset_reason = _as_str(stats.get("reset_reason"))
    selfheal = _as_str(stats.get("selfheal"))
    selfheal_n = _as_int(stats.get("selfheal_n"))
    uptime = _as_int(stats.get("uptime_secs"))
    if uptime is not None and uptime < 0:
        uptime = None

    message_time = _parse_time(doc.get("timestamp"))
    boot_time = None
    if uptime is not None:
        base = message_time if message_time is not None else received_at.timestamp()
        boot_time = base - uptime

    report_key = None
    if reset_reason is not None or selfheal is not None:
        report_key = (reset_reason, selfheal, selfheal_n)

    kinds: List[str] = []
    if selfheal is not None and selfheal_n is not None and selfheal_n > 0:
        kinds.append("selfheal")
    if reset_reason in ABNORMAL_RESETS:
        kinds.append("reset")

    scalars: Dict[str, Any] = {}
    for key in ("heap_min", "internal_heap", "heap_largest", "mqtt_slots_up", "mqtt_slots_total"):
        value = _as_int(stats.get(key))
        if value is not None:
            scalars[key] = value

    return ParsedStatus(
        received_at=received_at.isoformat(timespec="microseconds"),
        iata=iata,
        node_id=node_id,
        node_name=_as_str(doc.get("origin")),
        model=_as_str(doc.get("model")),
        firmware_version=_as_str(doc.get("firmware_version")),
        crash=crash,
        crash_signature=signature,
        report_key=report_key,
        report_kinds=tuple(kinds),
        uptime_secs=uptime,
        boot_time=boot_time,
        scalars=scalars,
    )


class BoundedSet:
    """LRU set with a hard cap, so a busy broker cannot grow memory without limit."""

    def __init__(self, capacity: int) -> None:
        self.capacity = max(1, capacity)
        self._items: "OrderedDict[str, None]" = OrderedDict()

    def add(self, value: str) -> None:
        self._items[value] = None
        self._items.move_to_end(value)
        while len(self._items) > self.capacity:
            self._items.popitem(last=False)

    def __contains__(self, value: str) -> bool:
        return value in self._items


@dataclass
class NodeState:
    report_key: Optional[Tuple[Optional[str], Optional[str], Optional[int]]]
    uptime_secs: Optional[int]
    boot_time: Optional[float]


class Deduper:
    """Decides whether a parsed status is a new event. No I/O."""

    def __init__(self, node_capacity: int = 20000) -> None:
        self.nodes: "OrderedDict[str, NodeState]" = OrderedDict()
        self.node_capacity = max(1, node_capacity)
        self.crashes = BoundedSet(self.node_capacity * 4)

    def _node(self, node_id: str) -> Optional[NodeState]:
        state = self.nodes.get(node_id)
        if state is not None:
            self.nodes.move_to_end(node_id)
        return state

    def _put_node(self, node_id: str, state: NodeState) -> None:
        self.nodes[node_id] = state
        self.nodes.move_to_end(node_id)
        while len(self.nodes) > self.node_capacity:
            self.nodes.popitem(last=False)

    @staticmethod
    def _rebooted(previous: NodeState, event: ParsedStatus) -> bool:
        if previous.boot_time is not None and event.boot_time is not None:
            # The same boot predicts a constant boot time for every status.
            return abs(event.boot_time - previous.boot_time) > BOOT_TOLERANCE_SECS
        if previous.uptime_secs is not None and event.uptime_secs is not None:
            return event.uptime_secs < previous.uptime_secs
        return False

    def observe(self, event: ParsedStatus) -> Optional[dict]:
        """Return a projected record for a new event, or None for a duplicate/non-event."""
        previous = self._node(event.node_id)
        crash_new = event.crash is not None and event.crash_signature not in self.crashes

        report_new = False
        if event.report_key is not None:
            if previous is None or previous.report_key != event.report_key:
                report_new = True
            else:
                report_new = self._rebooted(previous, event)
        report_event = report_new and bool(event.report_kinds)

        # Keep the observation window fresh even for discarded statuses: a later
        # reboot is easier to see against the newest uptime, not the last stored one.
        self._put_node(
            event.node_id,
            NodeState(
                report_key=event.report_key if event.report_key is not None else (previous.report_key if previous else None),
                uptime_secs=event.uptime_secs,
                boot_time=event.boot_time,
            ),
        )
        if event.crash_signature is not None:
            self.crashes.add(event.crash_signature)

        if not crash_new and not report_event:
            return None

        kinds: List[str] = []
        if crash_new:
            kinds.append("crash")
        if report_event:
            kinds.extend(event.report_kinds)
        return project(event, kinds, event.crash if crash_new else None)


def project(event: ParsedStatus, kinds: Iterable[str], crash: Optional[dict]) -> dict:
    record: Dict[str, Any] = {
        "received_at": event.received_at,
        "kinds": list(kinds),
        "iata": event.iata,
        "node_id": event.node_id,
    }
    if event.node_name is not None:
        record["node_name"] = event.node_name
    if event.model is not None:
        record["model"] = event.model
    if event.firmware_version is not None:
        record["firmware_version"] = event.firmware_version
    if crash is not None:
        record["crash"] = crash
    if event.report_key is not None:
        reset_reason, selfheal, selfheal_n = event.report_key
        if reset_reason is not None:
            record["reset_reason"] = reset_reason
        if selfheal is not None:
            record["selfheal"] = selfheal
        if selfheal_n is not None:
            record["selfheal_n"] = selfheal_n
    if event.uptime_secs is not None:
        record["uptime_secs"] = event.uptime_secs
    record.update(event.scalars)
    return record


def absorb_record(deduper: Deduper, record: dict) -> None:
    """Rebuild dedup state from a stored record. Boot time is left unset: it was
    derived from the collector's receive time, not the device's publish time."""
    node_id = _normalize_node_id(_as_str(record.get("node_id")))
    if "crash" in record and isinstance(record["crash"], dict):
        try:
            deduper.crashes.add(crash_signature(node_id, record["crash"]))
        except (TypeError, ValueError):
            pass

    reset_reason = _as_str(record.get("reset_reason"))
    selfheal = _as_str(record.get("selfheal"))
    selfheal_n = _as_int(record.get("selfheal_n"))
    report_key = (reset_reason, selfheal, selfheal_n) if (reset_reason or selfheal) else None
    previous = deduper.nodes.get(node_id)
    deduper._put_node(
        node_id,
        NodeState(
            report_key=report_key if report_key is not None else (previous.report_key if previous else None),
            uptime_secs=_as_int(record.get("uptime_secs")),
            boot_time=None,
        ),
    )


def load_state(deduper: Deduper, output_path: str) -> None:
    for path in (output_path + ".1", output_path):
        try:
            handle = open(path, "r", encoding="utf-8", errors="replace")
        except OSError:
            continue
        with handle:
            for line in handle:
                try:
                    record = json.loads(line)
                except (ValueError, RecursionError):
                    continue
                if isinstance(record, dict):
                    absorb_record(deduper, record)


def append_record(output_path: str, record: dict, max_bytes: int) -> None:
    directory = os.path.dirname(os.path.abspath(output_path))
    os.makedirs(directory, exist_ok=True)
    line = json.dumps(record, separators=(",", ":"), ensure_ascii=False) + "\n"
    with open(output_path, "a", encoding="utf-8") as handle:
        handle.write(line)
        handle.flush()
    try:
        if os.path.getsize(output_path) >= max_bytes:
            os.replace(output_path, output_path + ".1")
    except OSError:
        pass


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

    def handle(self, topic: str, payload: bytes) -> Optional[dict]:
        self.counters.count("seen")
        if len(payload) > self.max_payload:
            self.counters.count("malformed")
            self.counters.maybe_log()
            return None
        event = parse_status(payload, topic, datetime.now(timezone.utc))
        if event is None:
            self.counters.count("malformed")
            self.counters.maybe_log()
            return None
        record = self.deduper.observe(event)
        if record is None:
            self.counters.count("discarded")
            self.counters.maybe_log()
            return None
        append_record(self.output_path, record, self.max_bytes)
        self.counters.count("kept")
        log("kept %s node=%s iata=%s at=%s" % (",".join(record["kinds"]), record["node_id"], record["iata"], record["received_at"]))
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
    config.max_payload = _env_int("OBSERVER_CRASH_MAX_PAYLOAD", 65536)
    config.node_cache = _env_int("OBSERVER_CRASH_NODE_CACHE", 20000)
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
    for raw in sys.stdin:
        line = raw.rstrip("\r\n")
        if not line:
            continue
        topic, sep, payload = line.partition("\t")
        if not sep:
            collector.counters.count("seen")
            collector.counters.count("malformed")
            collector.counters.maybe_log()
            continue
        collector.handle(topic, payload.encode("utf-8", errors="replace"))
    return 0


def run_mqtt(config: Config) -> int:
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        log("paho-mqtt 2.x is required (pip install 'paho-mqtt>=2,<3')")
        return 2
    if not hasattr(mqtt, "CallbackAPIVersion"):
        log("paho-mqtt 2.x is required (pip install 'paho-mqtt>=2,<3')")
        return 2

    deduper = Deduper(config.node_cache)
    load_state(deduper, config.output)
    collector = Collector(deduper, config.output, config.max_bytes, config.max_payload)
    stop = threading.Event()

    def request_stop(signum, frame):
        stop.set()

    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)

    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=config.client_id,
        clean_session=True,
        protocol=mqtt.MQTTv311,
        transport="websockets" if config.ws else "tcp",
        reconnect_on_failure=False,
    )
    if config.ws:
        client.ws_set_options(path=config.ws_path)
    if config.tls:
        client.tls_set(ca_certs=config.ca_file)
        if config.tls_insecure:
            client.tls_insecure_set(True)
    if config.username is not None:
        client.username_pw_set(config.username, config.password)

    connected = threading.Event()
    connect_failed = threading.Event()

    def on_connect(client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            connect_failed.set()
            return
        client.subscribe(config.topic, qos=0)
        connected.set()
        log("connected %s:%d topic=%s" % (config.host, config.port, config.topic))

    def on_disconnect(client, userdata, flags, reason_code, properties):
        connected.clear()
        if not stop.is_set():
            log("disconnected rc=%s" % reason_code)

    def on_message(client, userdata, message):
        try:
            collector.handle(message.topic, message.payload)
        except Exception:
            collector.counters.count("malformed")

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    log("starting output=%s client_id=%s" % (config.output, config.client_id))
    backoff = 1.0
    while not stop.is_set():
        connected.clear()
        connect_failed.clear()
        try:
            client.connect(config.host, config.port, config.keepalive)
        except OSError:
            delay = min(config.backoff_max, backoff)
            if stop.wait(delay / 2 + random.uniform(0, delay / 2)):
                break
            backoff = min(config.backoff_max, backoff * 2)
            continue

        # connect() only sends CONNECT; pump the loop until CONNACK or failure.
        deadline = time.monotonic() + max(10.0, float(config.keepalive))
        while not stop.is_set() and not connected.is_set() and not connect_failed.is_set():
            client.loop(timeout=1.0)
            if time.monotonic() >= deadline:
                break

        if stop.is_set():
            break
        if connected.is_set():
            backoff = 1.0
            while not stop.is_set() and connected.is_set():
                client.loop(timeout=1.0)

        if stop.is_set():
            break
        delay = min(config.backoff_max, backoff)
        backoff = min(config.backoff_max, backoff * 2)
        stop.wait(delay / 2 + random.uniform(0, delay / 2))

    try:
        client.disconnect()
        client.loop(timeout=1.0)
    except Exception:
        pass
    return 0


def read_records(output_path: str) -> Iterable[dict]:
    for path in (output_path + ".1", output_path):
        try:
            handle = open(path, "r", encoding="utf-8", errors="replace")
        except OSError:
            continue
        with handle:
            for line in handle:
                try:
                    record = json.loads(line)
                except (ValueError, RecursionError):
                    continue
                if isinstance(record, dict):
                    yield record


def _crash_addresses(crash: dict) -> List[str]:
    bt = crash.get("bt")
    if not isinstance(bt, str):
        return []
    return [token for token in bt.split() if token != "!" and all(c in "0123456789abcdefABCDEF" for c in token)]


def print_summary(output_path: str) -> int:
    groups: Dict[Tuple[str, str, str, str], dict] = {}
    selfheal: Dict[str, Dict[str, int]] = {}
    resets: Dict[str, Dict[str, int]] = {}

    for record in read_records(output_path):
        firmware = (_as_str(record.get("firmware_version")) or "(unknown)")
        node = (_as_str(record.get("node_id")) or "")
        seen = _as_str(record.get("received_at"))
        crash = record.get("crash")
        if isinstance(crash, dict):
            sha = _as_str(crash.get("sha")) or ""
            task = _as_str(crash.get("task")) or ""
            pc = _as_str(crash.get("pc")) or ""
            key = (firmware, sha, task, pc)
            entry = groups.get(key)
            if entry is None:
                entry = {
                    "count": 0,
                    "nodes": set(),
                    "first": seen,
                    "last": seen,
                    "addresses": _crash_addresses(crash),
                }
                groups[key] = entry
            entry["count"] += 1
            if node:
                entry["nodes"].add(node)
            if seen is not None:
                if entry["first"] is None or seen < entry["first"]:
                    entry["first"] = seen
                if entry["last"] is None or seen > entry["last"]:
                    entry["last"] = seen
            if len(entry["addresses"]) < len(_crash_addresses(crash)):
                entry["addresses"] = _crash_addresses(crash)

        cause = _as_str(record.get("selfheal"))
        if cause is not None:
            counts = selfheal.setdefault(firmware, {})
            counts[cause] = counts.get(cause, 0) + 1
        reason = _as_str(record.get("reset_reason"))
        if reason in ABNORMAL_RESETS:
            counts = resets.setdefault(firmware, {})
            counts[reason] = counts.get(reason, 0) + 1

    print("Crash signatures")
    if not groups:
        print("  none")
    ordered = sorted(groups.items(), key=lambda item: (-item[1]["count"], item[0]))
    for (firmware, sha, task, pc), entry in ordered[:SUMMARY_CRASH_LIMIT]:
        print("  fw=%s sha=%s task=%s pc=%s" % (firmware, sha or "?", task or "?", pc or "?"))
        print(
            "    count=%d nodes=%d first=%s last=%s"
            % (entry["count"], len(entry["nodes"]), entry["first"] or "?", entry["last"] or "?")
        )
        hint = "xtensa-esp32s3-elf-addr2line -pfiaC -e <firmware.elf matching sha %s> %s" % (
            sha or "????????",
            " ".join([pc or "????????"] + entry["addresses"]),
        )
        print("    hint: " + hint)
    if len(ordered) > SUMMARY_CRASH_LIMIT:
        print("  ... %d more signatures" % (len(ordered) - SUMMARY_CRASH_LIMIT))

    print("Self-heal causes")
    if not selfheal:
        print("  none")
    for firmware in sorted(selfheal):
        parts = ["%s=%d" % (cause, count) for cause, count in sorted(selfheal[firmware].items())]
        print("  %s: %s" % (firmware, " ".join(parts)))

    print("Abnormal reset reasons")
    if not resets:
        print("  none")
    for firmware in sorted(resets):
        parts = ["%s=%d" % (reason, count) for reason, count in sorted(resets[firmware].items())]
        print("  %s: %s" % (firmware, " ".join(parts)))
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Collect MeshCore observer crash reports from MQTT status topics.")
    parser.add_argument("--stdin", action="store_true", help="read topic<TAB>payload lines from stdin")
    parser.add_argument("--dry-run", action="store_true", help="alias for --stdin")
    parser.add_argument("--summary", action="store_true", help="summarize the existing output file(s)")
    args = parser.parse_args(argv)

    try:
        if args.summary:
            return print_summary(os.environ.get("OBSERVER_CRASH_OUTPUT", "crash-events.jsonl"))
        config = load_config(require_host=not (args.stdin or args.dry_run))
    except ConfigError as error:
        log(str(error))
        return 2

    if args.stdin or args.dry_run:
        try:
            return run_stdin(config)
        except KeyboardInterrupt:
            return 0
    return run_mqtt(config)


if __name__ == "__main__":
    sys.exit(main())
