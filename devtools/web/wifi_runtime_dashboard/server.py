#!/usr/bin/env python3
"""Loopback-only viewer for meiso_wifi_runtime_host NDJSON.

The native Host remains the only Meiso protocol participant.  This server starts
one fixed Host binary with one fixed config path and forwards only a bounded,
allow-listed projection of the Host's existing NDJSON over Server-Sent Events.
"""

from __future__ import annotations

import argparse
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
from typing import Any
from urllib.parse import parse_qs, urlsplit


LOOPBACK_ADDRESS = "127.0.0.1"
DEFAULT_EVENT_CAPACITY = 512
DEFAULT_HOST_TIMEOUT_SECONDS = 180.0
MAX_NDJSON_LINE_BYTES = 8192
MAX_EVENTS_PER_RUN = 4096
MAX_STATIC_ASSET_BYTES = 256 * 1024
MAX_SSE_BATCH = 128
SSE_HEARTBEAT_SECONDS = 10.0
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
WIFI_FIELD_ALL = 0x000003FF
WIFI_FIELD_LINK_STATE = 1 << 0
WIFI_FIELD_SIGNAL_MBM = 1 << 1

EVENT_TYPES = frozenset(
    (
        "bootstrap",
        "session",
        "catalog",
        "desired",
        "desired_delivery",
        "actual",
        "sample",
        "run",
        "failure",
    )
)
PRODUCTION_STATUSES = frozenset(
    ("STOPPED", "STARTING", "RUNNING", "DEGRADED", "STOPPING", "FAILED")
)
APPLY_RESULTS = frozenset(("NONE", "PENDING", "APPLIED", "REJECTED", "FAILED"))
FAILURE_STAGES = frozenset(
    (
        "session_ready",
        "runtime_init",
        "catalog",
        "enable_desired",
        "samples",
        "disable_desired",
        "normal_close",
    )
)
DASHBOARD_FAILURE_STAGES = frozenset(
    ("host_exit", "host_timeout", "host_output", "missing_complete")
)
COUNTER_FIELDS = (
    "rx_bytes",
    "tx_bytes",
    "rx_packets",
    "tx_packets",
    "rx_dropped",
    "tx_dropped",
    "rx_errors",
    "tx_errors",
)
COUNTER_FIELD_BITS = {
    name: 1 << (index + 2) for index, name in enumerate(COUNTER_FIELDS)
}


class DashboardError(Exception):
    """An expected start request failure."""


def _uint(value: Any, maximum: int = UINT64_MAX, *, positive: bool = False) -> int | None:
    if type(value) is not int or value < (1 if positive else 0) or value > maximum:
        return None
    return value


def _boolean(value: Any) -> bool | None:
    return value if type(value) is bool else None


def _exact_status(raw: dict[str, Any], expected: str) -> bool:
    return raw.get("status") == expected


def _nullable_uint(value: Any) -> int | None | object:
    if value is None:
        return None
    converted = _uint(value)
    return converted if converted is not None else _INVALID


_INVALID = object()


def normalize_host_event(raw: Any) -> dict[str, Any] | None:
    """Validate and return the browser-safe projection of one Host event.

    This is intentionally a closed schema.  Unknown keys (including locator,
    identity, SSID, MAC, credential, config path, and raw frames) are discarded.
    """

    if not isinstance(raw, dict) or raw.get("type") not in EVENT_TYPES:
        return None
    event_type = raw["type"]

    if event_type == "bootstrap":
        if not _exact_status(raw, "CONNECTED") or raw.get("locator_redacted") is not True:
            return None
        return {
            "type": "bootstrap",
            "status": "CONNECTED",
            "locator_redacted": True,
        }

    if event_type == "session":
        if _exact_status(raw, "CLOSED") and raw.get("reason") == "NORMAL":
            return {"type": "session", "status": "CLOSED", "reason": "NORMAL"}
        if not _exact_status(raw, "READY"):
            return None
        identity_known = _boolean(raw.get("remote_identity_known"))
        identity_match = _boolean(raw.get("identity_match"))
        session_id_known = _boolean(raw.get("session_id_known"))
        limits = raw.get("limits")
        if (
            identity_known is None
            or identity_match is None
            or session_id_known is None
            or not isinstance(limits, dict)
        ):
            return None
        safe_limits: dict[str, int] = {}
        for key in (
            "max_frame_bytes",
            "max_runtime_payload_bytes",
            "max_registry_entries",
        ):
            value = _uint(limits.get(key), UINT32_MAX, positive=True)
            if value is None:
                return None
            safe_limits[key] = value
        return {
            "type": "session",
            "status": "READY",
            "remote_identity_known": identity_known,
            "identity_match": identity_match,
            "session_id_known": session_id_known,
            "limits": safe_limits,
        }

    if event_type == "catalog":
        capability = raw.get("capability")
        generation = _uint(raw.get("generation"), positive=True)
        endpoint_count = _uint(raw.get("endpoint_count"), UINT32_MAX)
        route_count = _uint(raw.get("route_count"), UINT32_MAX)
        if (
            not _exact_status(raw, "ACCEPTED")
            or generation is None
            or endpoint_count != 1
            or route_count != 3
            or not isinstance(capability, dict)
        ):
            return None
        supported_fields = _uint(capability.get("supported_fields"), WIFI_FIELD_ALL)
        min_period_ms = _uint(capability.get("min_period_ms"), UINT32_MAX, positive=True)
        max_period_ms = _uint(capability.get("max_period_ms"), UINT32_MAX, positive=True)
        period_step_ms = _uint(capability.get("period_step_ms"), UINT32_MAX, positive=True)
        if (
            supported_fields is None
            or supported_fields == 0
            or min_period_ms is None
            or max_period_ms is None
            or period_step_ms is None
            or min_period_ms > max_period_ms
        ):
            return None
        return {
            "type": "catalog",
            "status": "ACCEPTED",
            "generation": generation,
            "endpoint_count": endpoint_count,
            "route_count": route_count,
            "capability": {
                "supported_fields": supported_fields,
                "min_period_ms": min_period_ms,
                "max_period_ms": max_period_ms,
                "period_step_ms": period_step_ms,
            },
        }

    if event_type == "desired":
        version = _uint(raw.get("version"), positive=True)
        enabled = _boolean(raw.get("enabled"))
        selected_fields = _uint(raw.get("selected_fields"), WIFI_FIELD_ALL)
        sample_period_ms = _uint(raw.get("sample_period_ms"), UINT32_MAX)
        if (
            not _exact_status(raw, "PUBLISHED")
            or version is None
            or enabled is None
            or selected_fields is None
            or selected_fields == 0
            or sample_period_ms is None
            or sample_period_ms == 0
        ):
            return None
        return {
            "type": "desired",
            "status": "PUBLISHED",
            "version": version,
            "enabled": enabled,
            "selected_fields": selected_fields,
            "sample_period_ms": sample_period_ms,
        }

    if event_type == "desired_delivery":
        version = _uint(raw.get("version"), positive=True)
        if not _exact_status(raw, "ACCEPTED") or version is None:
            return None
        return {
            "type": "desired_delivery",
            "status": "ACCEPTED",
            "version": version,
        }

    if event_type == "actual":
        status = raw.get("status")
        state_version = _uint(raw.get("state_version"), positive=True)
        apply_result = raw.get("apply_result")
        effective_enabled = _boolean(raw.get("effective_enabled"))
        data_active = _boolean(raw.get("data_active"))
        selected_fields = _uint(raw.get("selected_fields"), WIFI_FIELD_ALL)
        sample_period_ms = _uint(raw.get("sample_period_ms"), UINT32_MAX)
        production_epoch_known = _boolean(raw.get("production_epoch_known"))
        if (
            status not in PRODUCTION_STATUSES
            or state_version is None
            or apply_result not in APPLY_RESULTS
            or effective_enabled is None
            or data_active is None
            or selected_fields is None
            or sample_period_ms is None
            or production_epoch_known is None
            or ((selected_fields == 0) != (sample_period_ms == 0))
        ):
            return None
        expected_active = (
            effective_enabled
            and production_epoch_known
            and status in ("RUNNING", "DEGRADED", "STOPPING")
        )
        if data_active != expected_active:
            return None
        if status == "STOPPED" and (
            effective_enabled
            or production_epoch_known
            or selected_fields != 0
            or sample_period_ms != 0
        ):
            return None
        return {
            "type": "actual",
            "status": status,
            "state_version": state_version,
            "apply_result": apply_result,
            "effective_enabled": effective_enabled,
            "data_active": data_active,
            "selected_fields": selected_fields,
            "sample_period_ms": sample_period_ms,
            "production_epoch_known": production_epoch_known,
        }

    if event_type == "sample":
        ordinal = _uint(raw.get("ordinal"), positive=True)
        sequence = _uint(raw.get("sequence"), positive=True)
        selected_fields = _uint(raw.get("selected_fields"), WIFI_FIELD_ALL)
        present_fields = _uint(raw.get("present_fields"), WIFI_FIELD_ALL)
        gap = _boolean(raw.get("gap"))
        missing_before = _uint(raw.get("missing_before"))
        link_state = raw.get("link_state")
        signal_mbm = raw.get("signal_mbm")
        if (
            ordinal is None
            or sequence is None
            or selected_fields is None
            or present_fields is None
            or (present_fields & ~selected_fields) != 0
            or gap is None
            or missing_before is None
            or (link_state is not None and type(link_state) is not bool)
            or (
                signal_mbm is not None
                and (type(signal_mbm) is not int or not -20_000 <= signal_mbm <= 0)
            )
            or gap != (missing_before > 0)
        ):
            return None
        if ((present_fields & WIFI_FIELD_LINK_STATE) != 0) != (link_state is not None):
            return None
        if ((present_fields & WIFI_FIELD_SIGNAL_MBM) != 0) != (signal_mbm is not None):
            return None
        event: dict[str, Any] = {
            "type": "sample",
            "ordinal": ordinal,
            "sequence": sequence,
            "selected_fields": selected_fields,
            "present_fields": present_fields,
            "gap": gap,
            "missing_before": missing_before,
            "link_state": link_state,
            "signal_mbm": signal_mbm,
        }
        for key in COUNTER_FIELDS:
            value = _nullable_uint(raw.get(key))
            if value is _INVALID or (
                ((present_fields & COUNTER_FIELD_BITS[key]) != 0) !=
                (value is not None)
            ):
                return None
            event[key] = value
        return event

    if event_type == "run":
        requested = _uint(raw.get("requested_samples"), UINT32_MAX, positive=True)
        received = _uint(raw.get("received_samples"))
        if (
            not _exact_status(raw, "COMPLETE")
            or requested is None
            or received != requested
        ):
            return None
        return {
            "type": "run",
            "status": "COMPLETE",
            "requested_samples": requested,
            "received_samples": received,
        }

    stage = raw.get("stage")
    if not _exact_status(raw, "FAILED") or stage not in FAILURE_STAGES:
        return None
    return {"type": "failure", "status": "FAILED", "stage": stage}


class DashboardState:
    """Own one fixed Host command and a bounded sanitized SSE transcript."""

    def __init__(
        self,
        host_binary: str,
        config_path: str,
        *,
        event_capacity: int = DEFAULT_EVENT_CAPACITY,
        host_timeout_seconds: float = DEFAULT_HOST_TIMEOUT_SECONDS,
    ):
        host = Path(host_binary).expanduser().resolve(strict=True)
        config = Path(config_path).expanduser().resolve(strict=True)
        if not host.is_file() or not os.access(host, os.X_OK):
            raise ValueError("Host binary must be an executable regular file")
        if not config.is_file():
            raise ValueError("config must be a regular file")
        if not 1 <= event_capacity <= 4096:
            raise ValueError("invalid event capacity")
        if (
            type(host_timeout_seconds) not in (int, float)
            or not 0.05 <= float(host_timeout_seconds) <= 600.0
        ):
            raise ValueError("invalid Host timeout")

        self.argv = (str(host), "--config", str(config))
        self.csrf_token = secrets.token_urlsafe(32)
        self.host_timeout_seconds = float(host_timeout_seconds)
        self._condition = threading.Condition(threading.RLock())
        self._events: deque[tuple[int, dict[str, Any]]] = deque(maxlen=event_capacity)
        self._next_sequence = 1
        self._process: subprocess.Popen[bytes] | None = None
        self._reader: threading.Thread | None = None
        self._watchdog: threading.Timer | None = None
        self._timed_out = False

    def _append(self, event: dict[str, Any]) -> None:
        with self._condition:
            self._events.append((self._next_sequence, event))
            self._next_sequence += 1
            self._condition.notify_all()

    def start(self) -> None:
        with self._condition:
            if self._process is not None:
                raise DashboardError("Host is already running")
            try:
                process = subprocess.Popen(
                    list(self.argv),
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    shell=False,
                    close_fds=True,
                    start_new_session=True,
                    bufsize=0,
                )
            except OSError as error:
                raise DashboardError("Host could not be started") from error
            self._events.clear()
            self._process = process
            self._timed_out = False
            reader = threading.Thread(
                target=self._consume,
                args=(process,),
                name="meiso-wifi-runtime-host",
                daemon=True,
            )
            watchdog = threading.Timer(
                self.host_timeout_seconds,
                self._expire,
                args=(process,),
            )
            watchdog.daemon = True
            self._reader = reader
            self._watchdog = watchdog
            reader.start()
            watchdog.start()

    @staticmethod
    def _signal_process(process: subprocess.Popen[bytes], signal_number: int) -> None:
        try:
            os.killpg(process.pid, signal_number)
        except (ProcessLookupError, PermissionError):
            pass

    def _terminate(self, process: subprocess.Popen[bytes]) -> None:
        self._signal_process(process, signal.SIGTERM)
        try:
            process.wait(timeout=1.0)
        except subprocess.TimeoutExpired:
            self._signal_process(process, signal.SIGKILL)
            try:
                process.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                pass

    def _expire(self, process: subprocess.Popen[bytes]) -> None:
        with self._condition:
            if self._process is not process:
                return
            self._timed_out = True
        self._terminate(process)

    def _consume(self, process: subprocess.Popen[bytes]) -> None:
        rejected = False
        native_failure = False
        pending_complete: dict[str, Any] | None = None
        count = 0
        assert process.stdout is not None
        try:
            while True:
                line = process.stdout.readline(MAX_NDJSON_LINE_BYTES + 1)
                if not line:
                    break
                if pending_complete is not None:
                    rejected = True
                    break
                count += 1
                if (
                    count > MAX_EVENTS_PER_RUN
                    or len(line) > MAX_NDJSON_LINE_BYTES
                    or not line.endswith(b"\n")
                ):
                    rejected = True
                    break
                try:
                    raw = json.loads(line.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError):
                    rejected = True
                    break
                event = normalize_host_event(raw)
                if event is None:
                    rejected = True
                    break
                if event["type"] == "run":
                    pending_complete = event
                else:
                    if event["type"] == "failure":
                        native_failure = True
                    self._append(event)
        finally:
            process.stdout.close()

        if rejected:
            self._terminate(process)
        returncode = process.wait()
        with self._condition:
            if self._process is not process:
                return
            if not native_failure:
                if self._timed_out:
                    terminal = {
                        "type": "failure", "status": "FAILED",
                        "stage": "host_timeout",
                    }
                elif rejected:
                    terminal = {
                        "type": "failure", "status": "FAILED",
                        "stage": "host_output",
                    }
                elif returncode != 0:
                    terminal = {
                        "type": "failure", "status": "FAILED",
                        "stage": "host_exit",
                    }
                elif pending_complete is None:
                    terminal = {
                        "type": "failure", "status": "FAILED",
                        "stage": "missing_complete",
                    }
                else:
                    terminal = pending_complete
                self._events.append((self._next_sequence, terminal))
                self._next_sequence += 1
            self._process = None
            watchdog = self._watchdog
            self._watchdog = None
            if watchdog is not None:
                watchdog.cancel()
            self._condition.notify_all()

    def events_after(
        self,
        after: int,
        *,
        timeout: float = SSE_HEARTBEAT_SECONDS,
    ) -> list[tuple[int, dict[str, Any]]]:
        with self._condition:
            if not any(sequence > after for sequence, _ in self._events):
                self._condition.wait(timeout=timeout)
            return [
                (sequence, dict(event))
                for sequence, event in self._events
                if sequence > after
            ][:MAX_SSE_BATCH]

    def shutdown(self) -> None:
        with self._condition:
            process = self._process
            watchdog = self._watchdog
            self._watchdog = None
        if watchdog is not None:
            watchdog.cancel()
        if process is not None:
            self._terminate(process)
        reader = self._reader
        if reader is not None and reader.is_alive():
            reader.join(timeout=2.0)


class StaticAssets:
    def __init__(self, directory: Path, csrf_token: str):
        index = (directory / "index.html").read_bytes()
        placeholder = b"__MEISO_CSRF_TOKEN__"
        if len(index) > MAX_STATIC_ASSET_BYTES or index.count(placeholder) != 1:
            raise ValueError("invalid index asset")
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
        for route, name, content_type in (
            ("/app.js", "app.js", "text/javascript; charset=utf-8"),
            ("/style.css", "style.css", "text/css; charset=utf-8"),
        ):
            content = (directory / name).read_bytes()
            if len(content) > MAX_STATIC_ASSET_BYTES:
                raise ValueError("static asset is too large")
            self._assets[route] = (content, content_type)

    def get(self, route: str) -> tuple[bytes, str] | None:
        return self._assets.get(route)


class DashboardHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False

    def __init__(self, state: DashboardState, static_directory: Path):
        self.dashboard_state = state
        self.static_assets = StaticAssets(static_directory, state.csrf_token)
        super().__init__((LOOPBACK_ADDRESS, 0), DashboardRequestHandler)
        self.expected_host = f"{LOOPBACK_ADDRESS}:{self.server_port}"
        self.expected_origin = f"http://{self.expected_host}"


class DashboardRequestHandler(BaseHTTPRequestHandler):
    server: DashboardHTTPServer
    server_version = "MeisoWifiRuntimeDashboard/1"
    sys_version = ""
    protocol_version = "HTTP/1.1"

    def log_message(self, _format: str, *_args: Any) -> None:
        return

    def _headers(self) -> None:
        self.send_header("Cache-Control", "no-store, max-age=0")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; base-uri 'none'; connect-src 'self'; "
            "img-src 'none'; object-src 'none'; script-src 'self'; "
            "style-src 'self'; frame-ancestors 'none'; form-action 'none'",
        )

    def _send_bytes(self, status: int, content: bytes, content_type: str) -> None:
        self.send_response(status)
        self._headers()
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    def _send_json(self, status: int, value: Any) -> None:
        content = json.dumps(value, separators=(",", ":"), sort_keys=True).encode("utf-8")
        self._send_bytes(status, content, "application/json; charset=utf-8")

    def _reject(self, status: int, code: str) -> None:
        self._send_json(status, {"error": code})

    def _expected_request(self) -> bool:
        if self.client_address[0] != LOOPBACK_ADDRESS:
            self._reject(HTTPStatus.FORBIDDEN, "loopback_required")
            return False
        if self.headers.get_all("Host", failobj=[]) != [self.server.expected_host]:
            self._reject(HTTPStatus.MISDIRECTED_REQUEST, "invalid_host")
            return False
        return True

    def _authorized_post(self) -> bool:
        origins = self.headers.get_all("Origin", failobj=[])
        tokens = self.headers.get_all("X-Meiso-CSRF", failobj=[])
        if origins != [self.server.expected_origin]:
            self._reject(HTTPStatus.FORBIDDEN, "invalid_origin")
            return False
        if len(tokens) != 1 or not secrets.compare_digest(
            tokens[0], self.server.dashboard_state.csrf_token
        ):
            self._reject(HTTPStatus.FORBIDDEN, "invalid_csrf")
            return False
        return True

    def _sse_after(self, query: str) -> int | None:
        try:
            values = parse_qs(query, keep_blank_values=True, strict_parsing=True)
        except ValueError:
            return None
        if set(values) - {"after"} or any(len(items) != 1 for items in values.values()):
            return None
        supplied = values.get("after", [self.headers.get("Last-Event-ID", "0")])[0]
        try:
            after = int(supplied, 10)
        except ValueError:
            return None
        return after if after >= 0 else None

    def _send_sse(self, after: int) -> None:
        self.send_response(HTTPStatus.OK)
        self._headers()
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        self.wfile.write(b"retry: 1000\n\n")
        self.wfile.flush()
        cursor = after
        while True:
            events = self.server.dashboard_state.events_after(cursor)
            try:
                if not events:
                    self.wfile.write(b": keep-alive\n\n")
                for sequence, event in events:
                    encoded = json.dumps(
                        event,
                        ensure_ascii=True,
                        separators=(",", ":"),
                        sort_keys=True,
                    ).encode("utf-8")
                    self.wfile.write(
                        f"id: {sequence}\nevent: host\ndata: ".encode("ascii")
                        + encoded
                        + b"\n\n"
                    )
                    cursor = sequence
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                self.close_connection = True
                return

    def do_GET(self) -> None:
        if not self._expected_request():
            return
        parsed = urlsplit(self.path)
        if parsed.fragment:
            self._reject(HTTPStatus.BAD_REQUEST, "invalid_path")
            return
        if parsed.path in ("/", "/index.html", "/app.js", "/style.css"):
            if parsed.query:
                self._reject(HTTPStatus.BAD_REQUEST, "query_not_allowed")
                return
            asset = self.server.static_assets.get(parsed.path)
            if asset is None:
                self._reject(HTTPStatus.NOT_FOUND, "not_found")
                return
            self._send_bytes(HTTPStatus.OK, asset[0], asset[1])
            return
        if parsed.path == "/api/v1/events":
            after = self._sse_after(parsed.query)
            if after is None:
                self._reject(HTTPStatus.BAD_REQUEST, "invalid_cursor")
                return
            self._send_sse(after)
            return
        self._reject(HTTPStatus.NOT_FOUND, "not_found")

    def do_POST(self) -> None:
        if not self._expected_request() or not self._authorized_post():
            return
        parsed = urlsplit(self.path)
        if parsed.path != "/api/v1/start" or parsed.query or parsed.fragment:
            self._reject(HTTPStatus.NOT_FOUND, "not_found")
            return
        if self.headers.get("Transfer-Encoding") is not None:
            self._reject(HTTPStatus.BAD_REQUEST, "body_not_allowed")
            return
        lengths = self.headers.get_all("Content-Length", failobj=[])
        if lengths not in ([], ["0"]):
            self._reject(HTTPStatus.BAD_REQUEST, "body_not_allowed")
            return
        try:
            self.server.dashboard_state.start()
        except DashboardError:
            self._reject(HTTPStatus.CONFLICT, "host_busy_or_unavailable")
            return
        self._send_json(HTTPStatus.ACCEPTED, {"started": True})


def create_server(
    host_binary: str,
    config_path: str,
    *,
    event_capacity: int = DEFAULT_EVENT_CAPACITY,
    host_timeout_seconds: float = DEFAULT_HOST_TIMEOUT_SECONDS,
) -> tuple[DashboardHTTPServer, DashboardState]:
    state = DashboardState(
        host_binary,
        config_path,
        event_capacity=event_capacity,
        host_timeout_seconds=host_timeout_seconds,
    )
    static_directory = Path(__file__).resolve().with_name("static")
    return DashboardHTTPServer(state, static_directory), state


def _parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Serve the loopback-only Meiso Wi-Fi Runtime dashboard."
    )
    parser.add_argument("--host-binary", required=True)
    parser.add_argument("--config", required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = _parse_arguments(sys.argv[1:] if argv is None else argv)
    try:
        server, state = create_server(arguments.host_binary, arguments.config)
    except (OSError, ValueError) as error:
        print(f"dashboard: {error}", file=sys.stderr)
        return 2
    print(f"Wi-Fi Runtime dashboard: http://{server.expected_host}/", flush=True)
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
