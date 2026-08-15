#!/usr/bin/env python3
"""Loopback-only dashboard for the Meiso four-kind lab probe.

The browser-facing HTTP API is a Dev Tool boundary.  It never opens the Meiso
transport and never forwards arbitrary browser input to an Edge.  A configured
native probe owns the real Session/transport work and reports a small,
allow-listed NDJSON event schema on stdout.
"""

from __future__ import annotations

import argparse
import copy
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import sys
import threading
from collections import deque
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Iterable
from urllib.parse import parse_qs, urlsplit


LOOPBACK_ADDRESS = "127.0.0.1"
MAX_REQUEST_BODY_BYTES = 4096
MAX_NDJSON_LINE_BYTES = 8192
MAX_EVENTS_PER_RUN = 4096
DEFAULT_EVENT_CAPACITY = 512
DEFAULT_PROBE_TIMEOUT_SECONDS = 120.0
MAX_EVENT_QUERY_LIMIT = 256
MAX_STATIC_ASSET_BYTES = 256 * 1024
TEMPERATURE_MIN_MILLIC = -40_000
TEMPERATURE_MAX_MILLIC = 125_000
TEMPERATURE_UNCERTAINTY_MILLIC = 3_000
UINT64_MAX = (1 << 64) - 1

TEST_KINDS = frozenset(("state", "stream", "action", "blob", "all"))
RESOURCE_TESTS = ("state", "stream", "action", "blob")

EVENT_TYPES = frozenset(
    ("session", "registry", "state", "stream", "action", "blob", "verification")
)
LAYERS = frozenset(("bootstrap", "transport", "session", "registry", "fixture", "primitive"))
DIRECTIONS = frozenset(("host_to_edge", "edge_to_host", "local", "none"))
PHASES = frozenset(
    (
        "starting",
        "connected",
        "handshake",
        "received",
        "applied",
        "accepted",
        "result",
        "verified",
        "closed",
        "failed",
        "sample",
        "published",
        "snapshot",
    )
)
STATUSES = frozenset(
    (
        "NEW",
        "RUNNING",
        "READY",
        "CLOSING",
        "CLOSED",
        "FAILED",
        "PASS",
        "FAIL",
        "INCOMPLETE",
        "UNKNOWN",
        "ACCEPTED",
        "SUCCEEDED",
        "PUBLISHED",
        "DUPLICATE",
        "REJECTED",
        "UNAVAILABLE",
    )
)
SESSION_STATES = frozenset(("NEW", "HANDSHAKING", "READY", "CLOSING", "CLOSED", "FAILED"))
SESSION_FAILURES = frozenset(
    (
        "NONE",
        "MALFORMED_FRAME",
        "UNEXPECTED_MESSAGE",
        "ROLE_MISMATCH",
        "PEER_MISMATCH",
        "INVALID_INCARNATION",
        "INVALID_NONCE",
        "PROTOCOL_MISMATCH",
        "PROFILE_MISMATCH",
        "LIMITS_INCOMPATIBLE",
        "SESSION_MISMATCH",
        "EARLY_RUNTIME",
        "TRANSPORT_LOST",
        "LOCAL_CLOSED",
        "PEER_CLOSED",
        "UNKNOWN",
    )
)
REGISTRY_STATES = frozenset(("UNKNOWN", "FIXTURE", "OFFERED", "ACCEPTED", "REJECTED"))
SIGNAL_BUCKETS = frozenset(("UNKNOWN", "POOR", "FAIR", "GOOD", "EXCELLENT"))
ACTION_OPERATIONS = frozenset(("telemetry.reset_window",))
ACTION_RESULT_CODES = frozenset(("OK", "FAILED", "REJECTED", "TIMEOUT", "INDETERMINATE"))
VERIFICATION_RESULTS = frozenset(("PASS", "FAIL", "INCOMPLETE"))
BLOB_LABELS = frozenset(("edge.capability.manifest",))


class DashboardError(Exception):
    """An expected dashboard request or probe error."""


def _bounded_uint(value: Any, maximum: int = UINT64_MAX) -> int | None:
    if type(value) is int and 0 <= value <= maximum:
        return value
    return None


def _bounded_int(value: Any, minimum: int, maximum: int) -> int | None:
    if type(value) is int and minimum <= value <= maximum:
        return value
    return None


def _boolean(value: Any) -> bool | None:
    return value if type(value) is bool else None


def _enum(value: Any, allowed: frozenset[str]) -> str | None:
    if isinstance(value, str) and value in allowed:
        return value
    return None


def _copy_if_present(
    destination: dict[str, Any],
    source: dict[str, Any],
    key: str,
    converter: Any,
) -> None:
    converted = converter(source.get(key))
    if converted is not None:
        destination[key] = converted


def _copy_optional_temperature(
    destination: dict[str, Any],
    source: dict[str, Any],
) -> bool:
    has_value = "temperature_millic" in source
    has_uncertainty = "temperature_uncertainty_millic" in source
    if not has_value and not has_uncertainty:
        return True
    value = _bounded_int(
        source.get("temperature_millic"),
        TEMPERATURE_MIN_MILLIC,
        TEMPERATURE_MAX_MILLIC,
    )
    uncertainty = _bounded_uint(
        source.get("temperature_uncertainty_millic"),
        TEMPERATURE_UNCERTAINTY_MILLIC,
    )
    if value is None or uncertainty != TEMPERATURE_UNCERTAINTY_MILLIC:
        return False
    destination["temperature_millic"] = value
    destination["temperature_uncertainty_millic"] = uncertainty
    return True


def normalize_probe_event(raw: Any) -> dict[str, Any] | None:
    """Return the safe public projection of one probe event.

    The projection is deliberately allow-listed.  Unknown keys—including
    addresses, SSIDs, MACs and full peer/session identifiers—are discarded and
    never retained by dashboard state.
    """

    if not isinstance(raw, dict):
        return None
    event_type = _enum(raw.get("type"), EVENT_TYPES)
    if event_type is None:
        return None

    event: dict[str, Any] = {"type": event_type}
    _copy_if_present(event, raw, "layer", lambda value: _enum(value, LAYERS))
    _copy_if_present(event, raw, "direction", lambda value: _enum(value, DIRECTIONS))
    _copy_if_present(event, raw, "phase", lambda value: _enum(value, PHASES))
    _copy_if_present(event, raw, "status", lambda value: _enum(value, STATUSES))
    _copy_if_present(event, raw, "test", lambda value: _enum(value, TEST_KINDS))
    _copy_if_present(event, raw, "monotonic_ns", _bounded_uint)
    _copy_if_present(event, raw, "payload_size", lambda value: _bounded_uint(value, 1 << 30))

    if event_type == "session":
        _copy_if_present(event, raw, "state", lambda value: _enum(value, SESSION_STATES))
        _copy_if_present(event, raw, "failure", lambda value: _enum(value, SESSION_FAILURES))
        for key in ("remote_identity_known", "identity_match", "session_id_known"):
            _copy_if_present(event, raw, key, _boolean)
        limits = raw.get("limits")
        if isinstance(limits, dict):
            safe_limits: dict[str, int] = {}
            for key in (
                "max_frame_bytes",
                "max_runtime_payload_bytes",
                "max_registry_entries",
            ):
                converted = _bounded_uint(limits.get(key), (1 << 32) - 1)
                if converted is not None:
                    safe_limits[key] = converted
            if safe_limits:
                event["limits"] = safe_limits
    elif event_type == "registry":
        _copy_if_present(event, raw, "state", lambda value: _enum(value, REGISTRY_STATES))
        _copy_if_present(event, raw, "entries", lambda value: _bounded_uint(value, 1 << 20))
        _copy_if_present(event, raw, "capacity", lambda value: _bounded_uint(value, 1 << 20))
        source = raw.get("source")
        if source in ("fixture", "runtime", "unknown"):
            event["source"] = source
    elif event_type == "state":
        if (
            raw.get("name") != "EdgeState"
            or event.get("phase") != "snapshot"
            or event.get("direction") != "edge_to_host"
        ):
            return None
        event["name"] = "EdgeState"
        version = _bounded_uint(raw.get("version"))
        window_id = _bounded_uint(raw.get("window_id"))
        interface_present = _boolean(raw.get("interface_present"))
        carrier_present = _boolean(raw.get("carrier_present"))
        if (
            version is None
            or version == 0
            or window_id is None
            or window_id == 0
            or interface_present is None
            or carrier_present is None
        ):
            return None
        event.update(
            {
                "version": version,
                "window_id": window_id,
                "interface_present": interface_present,
                "carrier_present": carrier_present,
            }
        )
        if not _copy_optional_temperature(event, raw):
            return None
        _copy_if_present(event, raw, "verified", _boolean)
    elif event_type == "stream":
        if raw.get("name") != "TelemetrySample":
            return None
        event["name"] = "TelemetrySample"
        phase = event.get("phase")
        if phase == "sample" and event.get("direction") == "edge_to_host":
            required_uints = (
                "epoch",
                "sequence",
                "source_time_ns",
                "window_id",
                "elapsed_ns",
                "rx_bytes",
                "tx_bytes",
                "rx_packets",
                "tx_packets",
                "rx_dropped",
                "tx_dropped",
            )
            converted = {key: _bounded_uint(raw.get(key)) for key in required_uints}
            signal_bucket = _enum(raw.get("signal_bucket"), SIGNAL_BUCKETS)
            if (
                any(value is None for value in converted.values())
                or converted["epoch"] == 0
                or converted["sequence"] == 0
                or converted["window_id"] == 0
                or signal_bucket is None
            ):
                return None
            event.update(converted)
            event["signal_bucket"] = signal_bucket
        elif phase == "applied" and event.get("direction") == "local":
            epoch = _bounded_uint(raw.get("epoch"))
            sequence = _bounded_uint(raw.get("sequence"))
            if epoch is None or epoch == 0 or sequence is None or sequence == 0:
                return None
            event["epoch"] = epoch
            event["sequence"] = sequence
        else:
            return None
        if not _copy_optional_temperature(event, raw):
            return None
        _copy_if_present(event, raw, "verified", _boolean)
    elif event_type == "action":
        _copy_if_present(
            event,
            raw,
            "operation",
            lambda value: _enum(value, ACTION_OPERATIONS),
        )
        for key in ("delivery_accepted", "edge_accepted", "executed_once", "verified"):
            _copy_if_present(event, raw, key, _boolean)
        _copy_if_present(
            event,
            raw,
            "result_code",
            lambda value: _enum(value, ACTION_RESULT_CODES),
        )
        _copy_if_present(event, raw, "result_value", _bounded_uint)
    elif event_type == "blob":
        _copy_if_present(event, raw, "label", lambda value: _enum(value, BLOB_LABELS))
        _copy_if_present(event, raw, "content_size", lambda value: _bounded_uint(value, 1 << 30))
        for key in ("integrity_verified", "published", "duplicate", "verified"):
            _copy_if_present(event, raw, key, _boolean)
    elif event_type == "verification":
        test = _enum(raw.get("test"), frozenset(RESOURCE_TESTS))
        result = _enum(raw.get("result"), VERIFICATION_RESULTS)
        checks_passed = _bounded_uint(raw.get("checks_passed"), 1 << 20)
        checks_total = _bounded_uint(raw.get("checks_total"), 1 << 20)
        if (
            test is None
            or result is None
            or raw.get("status") != result
            or checks_total is None
            or checks_total == 0
            or checks_passed is None
            or checks_passed > checks_total
            or (result == "PASS" and checks_passed != checks_total)
        ):
            return None
        event["test"] = test
        event["result"] = result
        event["checks_passed"] = checks_passed
        event["checks_total"] = checks_total

    return event


def _initial_snapshot(probe_configured: bool) -> dict[str, Any]:
    return {
        "schema": "meiso.lab.dashboard-snapshot/1",
        "overall": "NOT_RUN",
        "probe": {
            "configured": probe_configured,
            "running": False,
            "run_id": 0,
            "active_test": None,
            "last_exit_code": None,
        },
        "session": {
            "state": "NEW",
            "failure": "NONE",
            "remote_identity_known": False,
            "identity_match": False,
            "session_id_known": False,
            "protocol_profile": "configured exact-match",
            "security": "unauthenticated lab TCP",
            "limits": {
                "max_frame_bytes": None,
                "max_runtime_payload_bytes": None,
                "max_registry_entries": None,
            },
        },
        "registry": {
            "state": "UNKNOWN",
            "entries": None,
            "capacity": None,
            "source": "unknown",
            "notice": "No product Runtime Registry is implied by a fixture registry.",
        },
        "semantics": {
            "state": {
                "status": "NOT_RUN",
                "name": "EdgeState",
                "version": None,
                "window_id": None,
                "interface_present": False,
                "carrier_present": False,
                "temperature_millic": None,
                "temperature_uncertainty_millic": None,
                "verified": False,
            },
            "stream": {
                "status": "NOT_RUN",
                "name": "TelemetrySample",
                "epoch": None,
                "sequence": None,
                "source_time_ns": None,
                "window_id": None,
                "elapsed_ns": None,
                "rx_bytes": None,
                "tx_bytes": None,
                "rx_packets": None,
                "tx_packets": None,
                "rx_dropped": None,
                "tx_dropped": None,
                "signal_bucket": "UNKNOWN",
                "temperature_millic": None,
                "temperature_uncertainty_millic": None,
                "verified": False,
            },
            "action": {
                "status": "NOT_RUN",
                "operation": None,
                "delivery_accepted": False,
                "edge_accepted": False,
                "executed_once": False,
                "result_code": None,
                "result_value": None,
                "verified": False,
            },
            "blob": {
                "status": "NOT_RUN",
                "label": None,
                "content_size": None,
                "integrity_verified": False,
                "published": False,
                "duplicate": False,
                "verified": False,
            },
        },
        "sensors": [
            {
                "name": "wifi_network_telemetry",
                "kind": "network endpoint",
                "status": "VERIFIED",
            },
            {
                "name": "k230_on_die_temperature",
                "kind": "physical sensor",
                "status": "VERIFIED",
            },
            {"name": "camera", "kind": "media endpoint", "status": "UNAVAILABLE"},
            {"name": "imu", "kind": "physical sensor", "status": "UNAVAILABLE"},
            {"name": "microphone", "kind": "physical sensor", "status": "UNVERIFIED"},
            {"name": "display", "kind": "presentation endpoint", "status": "UNVERIFIED"},
        ],
        "latest_event_seq": 0,
    }


class DashboardState:
    """Thread-safe bounded state shared by HTTP and probe reader threads."""

    def __init__(
        self,
        probe_command: Iterable[str],
        event_capacity: int = DEFAULT_EVENT_CAPACITY,
        probe_timeout_seconds: float = DEFAULT_PROBE_TIMEOUT_SECONDS,
    ):
        command = tuple(probe_command)
        if len(command) > 64 or any(
            not isinstance(argument, str) or not argument or len(argument) > 4096
            for argument in command
        ):
            raise ValueError("invalid probe command array")
        if not 1 <= event_capacity <= 4096:
            raise ValueError("invalid event capacity")
        if (
            type(probe_timeout_seconds) not in (int, float)
            or not 0.05 <= probe_timeout_seconds <= 600.0
        ):
            raise ValueError("invalid probe timeout")

        self.probe_command = command
        self.probe_timeout_seconds = float(probe_timeout_seconds)
        self.csrf_token = secrets.token_urlsafe(32)
        self._lock = threading.RLock()
        self._events: deque[dict[str, Any]] = deque(maxlen=event_capacity)
        self._next_event_seq = 1
        self._next_run_id = 1
        self._process: subprocess.Popen[bytes] | None = None
        self._reader_thread: threading.Thread | None = None
        self._watchdog: threading.Timer | None = None
        self._terminal_results: dict[str, str | None] = {
            name: None for name in RESOURCE_TESTS
        }
        self._terminal_failure_latched = False
        self._probe_timeout_latched = False
        self._snapshot = _initial_snapshot(bool(command))

    def _append_event_locked(self, event: dict[str, Any]) -> None:
        stored = dict(event)
        stored["seq"] = self._next_event_seq
        self._next_event_seq += 1
        self._events.append(stored)
        self._snapshot["latest_event_seq"] = stored["seq"]

    def _process_event(self, event: dict[str, Any]) -> None:
        with self._lock:
            self._append_event_locked(event)
            event_type = event["type"]
            if event_type == "session":
                session = self._snapshot["session"]
                for key in (
                    "state",
                    "failure",
                    "remote_identity_known",
                    "identity_match",
                    "session_id_known",
                ):
                    if key in event:
                        session[key] = event[key]
                if "limits" in event:
                    session["limits"].update(event["limits"])
            elif event_type == "registry":
                registry = self._snapshot["registry"]
                for key in ("state", "entries", "capacity", "source"):
                    if key in event:
                        registry[key] = event[key]
            elif event_type in RESOURCE_TESTS:
                semantics = self._snapshot["semantics"][event_type]
                for key, value in event.items():
                    if key not in (
                        "type",
                        "layer",
                        "direction",
                        "phase",
                        "status",
                        "test",
                        "run_id",
                        "monotonic_ns",
                        "payload_size",
                        "payload_hex",
                        "verified",
                    ):
                        semantics[key] = value
                if "status" in event and self._terminal_results[event_type] is None:
                    semantics["status"] = event["status"]
            elif event_type == "verification":
                test = event.get("test")
                result = event.get("result")
                if test in RESOURCE_TESTS and result in ("PASS", "FAIL", "INCOMPLETE"):
                    semantics = self._snapshot["semantics"][test]
                    previous = self._terminal_results[test]
                    if previous is not None:
                        result = "FAIL"
                        self._terminal_failure_latched = True
                    elif result == "FAIL":
                        self._terminal_failure_latched = True
                    self._terminal_results[test] = result
                    semantics["status"] = result
                    semantics["verified"] = result == "PASS"

    def _probe_argv(self, test_kind: str) -> list[str]:
        argv = [test_kind if argument == "{test}" else argument for argument in self.probe_command]
        if "{test}" not in self.probe_command:
            argv.extend(("--test", test_kind))
        return argv

    def start_test(self, test_kind: str) -> int:
        if test_kind not in TEST_KINDS:
            raise DashboardError("unsupported test kind")
        if not self.probe_command:
            raise DashboardError("native probe is not configured")

        with self._lock:
            if self._process is not None:
                raise DashboardError("a native probe is already running")
            run_id = self._next_run_id
            self._next_run_id += 1
            argv = self._probe_argv(test_kind)
            try:
                process = subprocess.Popen(
                    argv,
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    shell=False,
                    close_fds=True,
                    start_new_session=True,
                    bufsize=0,
                )
            except OSError as error:
                raise DashboardError("native probe could not be started") from error

            self._process = process
            fresh = _initial_snapshot(bool(self.probe_command))
            self._snapshot["session"] = fresh["session"]
            self._snapshot["registry"] = fresh["registry"]
            self._snapshot["semantics"] = fresh["semantics"]
            self._terminal_results = {name: None for name in RESOURCE_TESTS}
            self._terminal_failure_latched = False
            self._probe_timeout_latched = False
            probe = self._snapshot["probe"]
            probe.update(
                {
                    "running": True,
                    "run_id": run_id,
                    "active_test": test_kind,
                    "last_exit_code": None,
                }
            )
            self._snapshot["overall"] = "RUNNING"
            selected = RESOURCE_TESTS if test_kind == "all" else (test_kind,)
            for name in RESOURCE_TESTS:
                self._snapshot["semantics"][name]["status"] = (
                    "RUNNING" if name in selected else "NOT_RUN"
                )
                self._snapshot["semantics"][name]["verified"] = False
            self._append_event_locked(
                {
                    "type": "process",
                    "layer": "bootstrap",
                    "phase": "starting",
                    "status": "RUNNING",
                    "test": test_kind,
                    "run_id": run_id,
                }
            )
            reader = threading.Thread(
                target=self._consume_probe,
                args=(process, run_id, test_kind),
                name=f"meiso-probe-{run_id}",
                daemon=True,
            )
            watchdog = threading.Timer(
                self.probe_timeout_seconds,
                self._expire_process,
                args=(process,),
            )
            watchdog.daemon = True
            self._reader_thread = reader
            self._watchdog = watchdog
            reader.start()
            watchdog.start()
            return run_id

    @staticmethod
    def _signal_process_group(process: subprocess.Popen[bytes], signal_number: int) -> None:
        try:
            os.killpg(process.pid, signal_number)
        except (ProcessLookupError, PermissionError):
            pass

    def _terminate_process(self, process: subprocess.Popen[bytes]) -> None:
        self._signal_process_group(process, signal.SIGTERM)
        try:
            process.wait(timeout=1.0)
        except subprocess.TimeoutExpired:
            pass
        self._signal_process_group(process, signal.SIGKILL)
        if process.poll() is None:
            process.wait(timeout=1.0)

    def _expire_process(self, process: subprocess.Popen[bytes]) -> None:
        with self._lock:
            if self._process is not process:
                return
            self._terminal_failure_latched = True
            self._probe_timeout_latched = True
        self._terminate_process(process)

    def _consume_probe(
        self,
        process: subprocess.Popen[bytes],
        run_id: int,
        test_kind: str,
    ) -> None:
        parse_failed = False
        event_count = 0
        assert process.stdout is not None
        try:
            while True:
                line = process.stdout.readline(MAX_NDJSON_LINE_BYTES + 1)
                if not line:
                    break
                event_count += 1
                if (
                    event_count > MAX_EVENTS_PER_RUN
                    or len(line) > MAX_NDJSON_LINE_BYTES
                    or not line.endswith(b"\n")
                ):
                    parse_failed = True
                    break
                try:
                    raw = json.loads(line.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError):
                    parse_failed = True
                    break
                event = normalize_probe_event(raw)
                if event is None:
                    parse_failed = True
                    break
                event["run_id"] = run_id
                self._process_event(event)
        finally:
            process.stdout.close()

        if parse_failed:
            self._terminate_process(process)
        exit_code = process.wait()
        self._finish_run(process, run_id, test_kind, exit_code, parse_failed)

    def _finish_run(
        self,
        process: subprocess.Popen[bytes],
        run_id: int,
        test_kind: str,
        exit_code: int,
        parse_failed: bool,
    ) -> None:
        selected = RESOURCE_TESTS if test_kind == "all" else (test_kind,)
        with self._lock:
            if self._process is not process:
                return
            probe = self._snapshot["probe"]
            probe.update(
                {
                    "running": False,
                    "active_test": None,
                    "last_exit_code": exit_code,
                }
            )
            results = [self._terminal_results[name] for name in selected]
            if parse_failed or exit_code != 0 or self._terminal_failure_latched:
                overall = "FAIL"
                for name in selected:
                    if (
                        parse_failed
                        or exit_code != 0
                        or self._probe_timeout_latched
                        or self._terminal_results[name] is None
                    ):
                        self._snapshot["semantics"][name]["status"] = "FAIL"
                        self._snapshot["semantics"][name]["verified"] = False
            elif all(result == "PASS" for result in results):
                overall = "PASS"
            else:
                overall = "INCOMPLETE"
                for name in selected:
                    if self._terminal_results[name] is None:
                        self._snapshot["semantics"][name]["status"] = "INCOMPLETE"
                        self._snapshot["semantics"][name]["verified"] = False
            self._snapshot["overall"] = overall
            self._process = None
            watchdog = self._watchdog
            self._watchdog = None
            if watchdog is not None:
                watchdog.cancel()
            self._append_event_locked(
                {
                    "type": "process",
                    "layer": "bootstrap",
                    "phase": "failed" if overall == "FAIL" else "verified",
                    "status": overall,
                    "test": test_kind,
                    "run_id": run_id,
                    "exit_code": exit_code,
                    "output_rejected": parse_failed,
                }
            )

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return copy.deepcopy(self._snapshot)

    def events_after(self, after: int, limit: int) -> dict[str, Any]:
        with self._lock:
            events = list(self._events)
            oldest = events[0]["seq"] if events else self._next_event_seq
            selected = [event for event in events if event["seq"] > after][:limit]
            return {
                "schema": "meiso.lab.dashboard-events/1",
                "after": after,
                "dropped_before": oldest if after < oldest - 1 else None,
                "latest": self._snapshot["latest_event_seq"],
                "events": copy.deepcopy(selected),
            }

    def shutdown(self) -> None:
        with self._lock:
            process = self._process
            watchdog = self._watchdog
            self._watchdog = None
        if watchdog is not None:
            watchdog.cancel()
        if process is not None:
            self._terminate_process(process)
        reader = self._reader_thread
        if reader is not None and reader.is_alive():
            reader.join(timeout=2.0)


class StaticAssets:
    def __init__(self, static_directory: Path, csrf_token: str):
        paths = {
            "/app.js": (static_directory / "app.js", "text/javascript; charset=utf-8"),
            "/style.css": (static_directory / "style.css", "text/css; charset=utf-8"),
        }
        index_path = static_directory / "index.html"
        index = index_path.read_bytes()
        if len(index) > MAX_STATIC_ASSET_BYTES:
            raise ValueError("index asset is too large")
        placeholder = b"__MEISO_CSRF_TOKEN__"
        if index.count(placeholder) != 1:
            raise ValueError("index must contain one CSRF placeholder")
        self._assets: dict[str, tuple[bytes, str]] = {
            "/": (
                index.replace(placeholder, csrf_token.encode("ascii")),
                "text/html; charset=utf-8",
            ),
            "/index.html": (
                index.replace(placeholder, csrf_token.encode("ascii")),
                "text/html; charset=utf-8",
            ),
        }
        for route, (path, content_type) in paths.items():
            content = path.read_bytes()
            if len(content) > MAX_STATIC_ASSET_BYTES:
                raise ValueError(f"static asset is too large: {path.name}")
            self._assets[route] = (content, content_type)

    def get(self, route: str) -> tuple[bytes, str] | None:
        return self._assets.get(route)


class DashboardHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False

    def __init__(
        self,
        state: DashboardState,
        static_directory: Path,
        port: int = 0,
    ):
        if port != 0:
            raise ValueError("dashboard requires an OS-selected random port")
        self.dashboard_state = state
        self.static_assets = StaticAssets(static_directory, state.csrf_token)
        super().__init__((LOOPBACK_ADDRESS, 0), DashboardRequestHandler)
        self.expected_host = f"{LOOPBACK_ADDRESS}:{self.server_port}"
        self.expected_origin = f"http://{self.expected_host}"


class DashboardRequestHandler(BaseHTTPRequestHandler):
    server: DashboardHTTPServer
    server_version = "MeisoLabDashboard/1"
    sys_version = ""

    def log_message(self, _format: str, *_args: Any) -> None:
        return

    def send_error(
        self,
        code: int,
        message: str | None = None,
        explain: str | None = None,
    ) -> None:
        del message, explain
        self._send_error_json(code, "http_error")

    def _security_headers(self) -> None:
        self.send_header("Cache-Control", "no-store, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header(
            "Permissions-Policy",
            "camera=(), microphone=(), geolocation=(), payment=(), usb=()",
        )
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; base-uri 'none'; connect-src 'self'; "
            "img-src 'none'; object-src 'none'; script-src 'self'; "
            "style-src 'self'; frame-ancestors 'none'; form-action 'none'",
        )

    def _send_bytes(self, status: int, content: bytes, content_type: str) -> None:
        self.send_response(status)
        self._security_headers()
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(content)

    def _send_json(self, status: int, value: Any) -> None:
        encoded = json.dumps(
            value,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
        self._send_bytes(status, encoded, "application/json; charset=utf-8")

    def _send_error_json(self, status: int, code: str) -> None:
        self._send_json(status, {"error": code})

    def _request_is_local_and_expected(self) -> bool:
        if self.client_address[0] != LOOPBACK_ADDRESS:
            self._send_error_json(HTTPStatus.FORBIDDEN, "loopback_required")
            return False
        hosts = self.headers.get_all("Host", failobj=[])
        if hosts != [self.server.expected_host]:
            self._send_error_json(HTTPStatus.MISDIRECTED_REQUEST, "invalid_host")
            return False
        return True

    def _post_is_authorized(self) -> bool:
        origins = self.headers.get_all("Origin", failobj=[])
        if origins != [self.server.expected_origin]:
            self._send_error_json(HTTPStatus.FORBIDDEN, "invalid_origin")
            return False
        tokens = self.headers.get_all("X-Meiso-CSRF", failobj=[])
        if len(tokens) != 1 or not secrets.compare_digest(
            tokens[0], self.server.dashboard_state.csrf_token
        ):
            self._send_error_json(HTTPStatus.FORBIDDEN, "invalid_csrf")
            return False
        return True

    def _read_json_body(self) -> dict[str, Any] | None:
        if self.headers.get("Transfer-Encoding") is not None:
            self._send_error_json(HTTPStatus.BAD_REQUEST, "transfer_encoding_not_supported")
            return None
        content_type = self.headers.get_content_type()
        if content_type != "application/json":
            self._send_error_json(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, "json_required")
            return None
        lengths = self.headers.get_all("Content-Length", failobj=[])
        if len(lengths) != 1:
            self._send_error_json(HTTPStatus.LENGTH_REQUIRED, "content_length_required")
            return None
        try:
            length = int(lengths[0], 10)
        except ValueError:
            self._send_error_json(HTTPStatus.BAD_REQUEST, "invalid_content_length")
            return None
        if length < 0:
            self._send_error_json(HTTPStatus.BAD_REQUEST, "invalid_content_length")
            return None
        if length > MAX_REQUEST_BODY_BYTES:
            self._send_error_json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "body_too_large")
            return None
        body = self.rfile.read(length)
        if len(body) != length:
            self._send_error_json(HTTPStatus.BAD_REQUEST, "incomplete_body")
            return None
        try:
            value = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self._send_error_json(HTTPStatus.BAD_REQUEST, "invalid_json")
            return None
        if not isinstance(value, dict):
            self._send_error_json(HTTPStatus.BAD_REQUEST, "object_required")
            return None
        return value

    def do_OPTIONS(self) -> None:
        if not self._request_is_local_and_expected():
            return
        self._send_error_json(HTTPStatus.METHOD_NOT_ALLOWED, "cors_disabled")

    def do_GET(self) -> None:
        if not self._request_is_local_and_expected():
            return
        parsed = urlsplit(self.path)
        if parsed.fragment:
            self._send_error_json(HTTPStatus.BAD_REQUEST, "invalid_path")
            return
        if parsed.path in ("/", "/index.html", "/app.js", "/style.css"):
            if parsed.query:
                self._send_error_json(HTTPStatus.BAD_REQUEST, "query_not_allowed")
                return
            asset = self.server.static_assets.get(parsed.path)
            if asset is None:
                self._send_error_json(HTTPStatus.NOT_FOUND, "not_found")
                return
            content, content_type = asset
            self._send_bytes(HTTPStatus.OK, content, content_type)
            return
        if parsed.path == "/api/v1/snapshot":
            if parsed.query:
                self._send_error_json(HTTPStatus.BAD_REQUEST, "query_not_allowed")
                return
            self._send_json(HTTPStatus.OK, self.server.dashboard_state.snapshot())
            return
        if parsed.path == "/api/v1/events":
            try:
                query = parse_qs(parsed.query, keep_blank_values=True, strict_parsing=True)
            except ValueError:
                self._send_error_json(HTTPStatus.BAD_REQUEST, "invalid_query")
                return
            if set(query) - {"after", "limit"} or any(
                len(values) != 1 for values in query.values()
            ):
                self._send_error_json(HTTPStatus.BAD_REQUEST, "invalid_query")
                return
            try:
                after = int(query.get("after", ["0"])[0], 10)
                limit = int(query.get("limit", ["128"])[0], 10)
            except ValueError:
                self._send_error_json(HTTPStatus.BAD_REQUEST, "invalid_query")
                return
            if after < 0 or not 1 <= limit <= MAX_EVENT_QUERY_LIMIT:
                self._send_error_json(HTTPStatus.BAD_REQUEST, "invalid_query")
                return
            self._send_json(
                HTTPStatus.OK,
                self.server.dashboard_state.events_after(after, limit),
            )
            return
        self._send_error_json(HTTPStatus.NOT_FOUND, "not_found")

    def do_POST(self) -> None:
        if not self._request_is_local_and_expected() or not self._post_is_authorized():
            return
        parsed = urlsplit(self.path)
        if parsed.query or parsed.fragment:
            self._send_error_json(HTTPStatus.BAD_REQUEST, "invalid_path")
            return
        body = self._read_json_body()
        if body is None:
            return
        if parsed.path == "/api/v1/tests":
            if set(body) != {"kind"} or body.get("kind") not in TEST_KINDS:
                self._send_error_json(HTTPStatus.BAD_REQUEST, "invalid_test_request")
                return
            try:
                run_id = self.server.dashboard_state.start_test(body["kind"])
            except DashboardError as error:
                code = "probe_busy" if "already running" in str(error) else "probe_unavailable"
                self._send_error_json(HTTPStatus.CONFLICT, code)
                return
            self._send_json(HTTPStatus.ACCEPTED, {"run_id": run_id})
            return
        self._send_error_json(HTTPStatus.NOT_FOUND, "not_found")


def create_server(
    probe_command: Iterable[str],
    *,
    event_capacity: int = DEFAULT_EVENT_CAPACITY,
    probe_timeout_seconds: float = DEFAULT_PROBE_TIMEOUT_SECONDS,
) -> tuple[DashboardHTTPServer, DashboardState]:
    state = DashboardState(
        probe_command,
        event_capacity=event_capacity,
        probe_timeout_seconds=probe_timeout_seconds,
    )
    static_directory = Path(__file__).resolve().with_name("static")
    return DashboardHTTPServer(state, static_directory), state


def _parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Serve the loopback-only Meiso four-kind lab dashboard."
    )
    parser.add_argument(
        "--probe-command",
        nargs=argparse.REMAINDER,
        required=True,
        help=(
            "native probe argv; use a literal {test} argument or the server "
            "appends --test <kind>"
        ),
    )
    arguments = parser.parse_args(argv)
    if not arguments.probe_command:
        parser.error("--probe-command requires at least one argument")
    return arguments


def main(argv: list[str] | None = None) -> int:
    arguments = _parse_arguments(sys.argv[1:] if argv is None else argv)
    server, state = create_server(arguments.probe_command)
    url = f"http://{server.expected_host}/"
    print(f"Meiso lab dashboard: {url}", flush=True)
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()
        server.server_close()
        state.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
