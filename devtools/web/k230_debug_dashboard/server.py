#!/usr/bin/env python3
"""Loopback-only recorded/local backend for the K230 debug dashboard."""

from __future__ import annotations

import argparse
from collections import deque
import copy
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from pathlib import Path
import secrets
import sys
import threading
import time
from typing import Any
from urllib.parse import parse_qs, urlparse

from native_bridge import (
    NativeBridge,
    NativeBridgeError,
    bootstrap_commands,
    commands_for_mutation,
)


LOOPBACK_ADDRESS = "127.0.0.1"
DEFAULT_PORT = 50001
DEFAULT_EVENT_CAPACITY = 256
MAX_CONTROL_BODY_BYTES = 2048
MAX_STATIC_ASSET_BYTES = 256 * 1024
MAX_SSE_BATCH = 64
MAX_OBJECTS = 32
CSRF_PLACEHOLDER = "__MEISO_CSRF_TOKEN__"
STATIC_ROOT = Path(__file__).resolve().parent / "static"


class CommandError(ValueError):
    """A fixed command failed shape or value validation."""


def _finite_number(value: Any, minimum: float, maximum: float) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise CommandError("INVALID_NUMBER")
    result = float(value)
    if not math.isfinite(result) or result < minimum or result > maximum:
        raise CommandError("NUMBER_OUT_OF_RANGE")
    return result


def _uint(value: Any, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise CommandError("INVALID_INTEGER")
    if value < minimum or value > maximum:
        raise CommandError("INTEGER_OUT_OF_RANGE")
    return value


def _boolean(value: Any) -> bool:
    if not isinstance(value, bool):
        raise CommandError("INVALID_BOOLEAN")
    return value


def _vector(value: Any, count: int, minimum: float, maximum: float) -> list[float]:
    if not isinstance(value, list) or len(value) != count:
        raise CommandError("INVALID_VECTOR")
    return [_finite_number(item, minimum, maximum) for item in value]


def _exact_keys(command: dict[str, Any], expected: set[str]) -> None:
    if set(command) != expected:
        raise CommandError("INVALID_COMMAND_SHAPE")


def _default_snapshot() -> dict[str, Any]:
    return {
        "schema_version": 1,
        "revision": 0,
        "backend": {
            "mode": "RECORDED_LOCAL",
            "device_connected": False,
            "native_host_bound": False,
            "session_status": "ABSENT",
            "catalog_status": "ABSENT",
            "profile": "composition-scene-lab-v0:v2 candidate",
            "proof": "LOCAL_DRAFT_ONLY",
        },
        "bindings": {
            "render_result_stream": "UNBOUND",
            "camera_media_stream": "UNSUPPORTED",
            "wifi_telemetry": "UNBOUND",
        },
        "protocol_evidence": {
            "state_version": None,
            "content_id": None,
            "plan_generation": None,
            "world_generation": None,
            "production_epoch": None,
            "stream_sequence": None,
        },
        "scene": {
            "selected_draft_object": 1,
            "objects": [
                {
                    "draft_object": 1,
                    "name": "Cube 1",
                    "asset": "builtin:cube",
                    "position": [0.0, 0.0, 0.0],
                    "rotation_deg": [0.0, 0.0, 0.0],
                    "scale": [1.0, 1.0, 1.0],
                }
            ],
            "camera": {
                "position": [4.0, 3.0, 6.0],
                "rotation_deg": [-18.0, 34.0, 0.0],
                "vertical_fov_deg": 55.0,
                "near": 0.1,
                "far": 100.0,
            },
        },
        "intents": {
            "render": {
                "enabled": True,
                "max_hz": 10,
                "status": "LOCAL_ONLY",
            },
            "camera": {
                "enabled": False,
                "fps": 15,
                "status": "MEDIA_ROUTE_UNSUPPORTED",
            },
            "telemetry": {
                "enabled": False,
                "sample_period_ms": 1000,
                "selected_fields": 3,
                "status": "DEVICE_UNBOUND",
            },
            "device": {
                "enabled": False,
                "sample_period_ms": 1000,
                "selected_fields": 7,
                "status": "DEVICE_UNBOUND",
            },
        },
        "native_truth": {
            "last_event": None,
            "control_snapshot": None,
            "actual": {},
            "wifi_effective": None,
            "device_effective": None,
            "wifi_sample": None,
            "device_sample": None,
            "presentation_status": None,
            "scene_status": None,
            "render_result": None,
            "media_actual": {},
        },
        "frames": {
            "render": {"available": False},
            "camera": {"available": False},
        },
    }


class DashboardState:
    """Owns the bounded local draft and sticky event projection."""

    def __init__(self, event_capacity: int = DEFAULT_EVENT_CAPACITY):
        if event_capacity < 1:
            raise ValueError("event_capacity must be positive")
        self.csrf_token = secrets.token_urlsafe(24)
        self._condition = threading.Condition()
        self._events: deque[tuple[int, dict[str, Any]]] = deque(
            maxlen=event_capacity
        )
        self._next_event_id = 1
        self._next_draft_object = 2
        self._snapshot = _default_snapshot()
        self._native_bridge: NativeBridge | None = None
        with self._condition:
            self._append_event_locked("bootstrap", "READY")

    def snapshot(self) -> dict[str, Any]:
        with self._condition:
            return copy.deepcopy(self._snapshot)

    def event_count(self) -> int:
        with self._condition:
            return len(self._events)

    def _append_event_locked(
        self,
        command: str,
        status: str,
        native_event: dict[str, Any] | None = None,
    ) -> int:
        event_id = self._next_event_id
        self._next_event_id += 1
        event = {
            "type": "snapshot",
            "command": command,
            "status": status,
            "snapshot": copy.deepcopy(self._snapshot),
        }
        if native_event is not None:
            event["native_event"] = copy.deepcopy(native_event)
        self._events.append((event_id, event))
        self._condition.notify_all()
        return event_id

    def events_after(
        self, after: int, timeout_seconds: float
    ) -> list[tuple[int, dict[str, Any]]]:
        deadline = time.monotonic() + timeout_seconds
        with self._condition:
            while not any(event_id > after for event_id, _ in self._events):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return []
                self._condition.wait(remaining)
            return [
                (event_id, copy.deepcopy(event))
                for event_id, event in self._events
                if event_id > after
            ][:MAX_SSE_BATCH]

    def apply(self, command: Any) -> tuple[int, dict[str, Any]]:
        if not isinstance(command, dict) or not isinstance(
            command.get("command"), str
        ):
            raise CommandError("INVALID_COMMAND")
        name = command["command"]
        with self._condition:
            snapshot_before = copy.deepcopy(self._snapshot)
            next_draft_before = self._next_draft_object
            if name == "scene.object.add":
                _exact_keys(command, {"command", "asset"})
                if command["asset"] != "builtin:cube":
                    raise CommandError("ASSET_UNAVAILABLE")
                objects = self._snapshot["scene"]["objects"]
                if len(objects) >= MAX_OBJECTS:
                    raise CommandError("OBJECT_CAPACITY")
                draft_object = self._next_draft_object
                self._next_draft_object += 1
                objects.append(
                    {
                        "draft_object": draft_object,
                        "name": f"Cube {draft_object}",
                        "asset": "builtin:cube",
                        "position": [float(len(objects)) * 1.5, 0.0, 0.0],
                        "rotation_deg": [0.0, 0.0, 0.0],
                        "scale": [1.0, 1.0, 1.0],
                    }
                )
                self._snapshot["scene"]["selected_draft_object"] = draft_object
            elif name == "scene.object.delete":
                _exact_keys(command, {"command", "draft_object"})
                draft_object = _uint(command["draft_object"], 1, 2**31 - 1)
                objects = self._snapshot["scene"]["objects"]
                remaining = [
                    item for item in objects if item["draft_object"] != draft_object
                ]
                if len(remaining) == len(objects):
                    raise CommandError("OBJECT_NOT_FOUND")
                self._snapshot["scene"]["objects"] = remaining
                selected = self._snapshot["scene"]["selected_draft_object"]
                if selected == draft_object:
                    self._snapshot["scene"]["selected_draft_object"] = (
                        remaining[0]["draft_object"] if remaining else None
                    )
            elif name == "scene.object.select":
                _exact_keys(command, {"command", "draft_object"})
                draft_object = _uint(command["draft_object"], 1, 2**31 - 1)
                if not self._find_object_locked(draft_object):
                    raise CommandError("OBJECT_NOT_FOUND")
                self._snapshot["scene"]["selected_draft_object"] = draft_object
            elif name == "scene.object.transform":
                _exact_keys(
                    command,
                    {
                        "command",
                        "draft_object",
                        "position",
                        "rotation_deg",
                        "scale",
                    },
                )
                draft_object = _uint(command["draft_object"], 1, 2**31 - 1)
                target = self._find_object_locked(draft_object)
                if target is None:
                    raise CommandError("OBJECT_NOT_FOUND")
                position = _vector(command["position"], 3, -1000, 1000)
                rotation = _vector(
                    command["rotation_deg"], 3, -3600, 3600
                )
                object_scale = _vector(command["scale"], 3, 0.001, 1000)
                target.update(
                    position=position,
                    rotation_deg=rotation,
                    scale=object_scale,
                )
            elif name == "scene.camera.set":
                _exact_keys(
                    command,
                    {
                        "command",
                        "position",
                        "rotation_deg",
                        "vertical_fov_deg",
                        "near",
                        "far",
                    },
                )
                near = _finite_number(command["near"], 0.001, 1000)
                far = _finite_number(command["far"], 0.002, 100000)
                if far <= near:
                    raise CommandError("INVALID_CAMERA_RANGE")
                self._snapshot["scene"]["camera"] = {
                    "position": _vector(command["position"], 3, -1000, 1000),
                    "rotation_deg": _vector(
                        command["rotation_deg"], 3, -3600, 3600
                    ),
                    "vertical_fov_deg": _finite_number(
                        command["vertical_fov_deg"], 1, 179
                    ),
                    "near": near,
                    "far": far,
                }
            elif name == "intent.render.set":
                _exact_keys(command, {"command", "enabled", "max_hz"})
                self._snapshot["intents"]["render"].update(
                    enabled=_boolean(command["enabled"]),
                    max_hz=_uint(command["max_hz"], 1, 120),
                )
            elif name == "intent.camera.set":
                _exact_keys(command, {"command", "enabled", "fps"})
                self._snapshot["intents"]["camera"].update(
                    enabled=_boolean(command["enabled"]),
                    fps=_uint(command["fps"], 1, 120),
                )
            elif name == "intent.telemetry.set":
                _exact_keys(
                    command,
                    {
                        "command",
                        "enabled",
                        "sample_period_ms",
                        "selected_fields",
                    },
                )
                self._snapshot["intents"]["telemetry"].update(
                    enabled=_boolean(command["enabled"]),
                    sample_period_ms=_uint(
                        command["sample_period_ms"], 100, 10000
                    ),
                    selected_fields=_uint(command["selected_fields"], 1, 1023),
                )
            elif name == "intent.device.set":
                _exact_keys(
                    command,
                    {
                        "command",
                        "enabled",
                        "sample_period_ms",
                        "selected_fields",
                    },
                )
                self._snapshot["intents"]["device"].update(
                    enabled=_boolean(command["enabled"]),
                    sample_period_ms=_uint(
                        command["sample_period_ms"], 100, 10000
                    ),
                    selected_fields=_uint(command["selected_fields"], 1, 1023),
                )
            else:
                raise CommandError("UNKNOWN_COMMAND")

            next_revision = self._snapshot["revision"] + 1
            native_commands: list[dict[str, Any]] = []
            if self._native_bridge is not None:
                try:
                    native_commands = commands_for_mutation(name, self._snapshot)
                    self._native_bridge.submit_batch(
                        native_commands, origin=name, revision=next_revision
                    )
                except NativeBridgeError as error:
                    self._snapshot = snapshot_before
                    self._next_draft_object = next_draft_before
                    raise CommandError(str(error)) from error
            self._snapshot["revision"] = next_revision
            event_id = self._append_event_locked(
                name,
                ("NATIVE_QUEUED" if native_commands else
                 "LOCAL_SELECTION_UPDATED") if self._native_bridge is not None
                else "LOCAL_DRAFT_UPDATED",
            )
            return event_id, copy.deepcopy(self._snapshot)

    def enable_native(self, executable: str, config: str) -> None:
        with self._condition:
            if self._native_bridge is not None:
                raise NativeBridgeError("NATIVE_ALREADY_BOUND")
            bridge = NativeBridge(
                executable, config, self._native_event, self._native_frame
            )
            snapshot_before = copy.deepcopy(self._snapshot)
            events_before = copy.deepcopy(list(self._events))
            next_event_before = self._next_event_id
            self._native_bridge = bridge
            self._snapshot["backend"].update({
                "mode": "NATIVE_HOST",
                "native_host_bound": True,
                "session_status": "STARTING",
                "catalog_status": "PENDING",
                "profile": "composition-v3",
                "proof": "NATIVE_SERVICE_TYPED_ONLY",
            })
            self._snapshot["bindings"]["camera_media_stream"] = "UNBOUND"
            candidate = copy.deepcopy(self._snapshot)
        try:
            bridge.start()
            bridge.submit_batch(
                bootstrap_commands(candidate), origin="native.bootstrap",
                revision=candidate["revision"],
            )
        except Exception:
            bridge.close()
            with self._condition:
                self._native_bridge = None
                self._snapshot = snapshot_before
                self._events.clear()
                self._events.extend(events_before)
                self._next_event_id = next_event_before
            raise
        with self._condition:
            self._append_event_locked("native.start", "NATIVE_STARTING")

    def close(self) -> None:
        with self._condition:
            bridge = self._native_bridge
            self._native_bridge = None
        if bridge is not None:
            bridge.close()

    def latest_frame(
        self, source: str
    ) -> tuple[bytes, dict[str, Any]] | None:
        with self._condition:
            bridge = self._native_bridge
        return None if bridge is None else bridge.latest_frame(source)

    def _native_event(self, event: dict[str, Any]) -> None:
        event_type = event.get("type")
        with self._condition:
            backend = self._snapshot["backend"]
            truth = self._snapshot["native_truth"]
            truth["last_event"] = copy.deepcopy(event)
            if event_type == "service.ready":
                backend["native_host_bound"] = True
                backend["session_status"] = "CONNECTING"
            elif event_type == "snapshot":
                truth["control_snapshot"] = copy.deepcopy(event)
                backend["device_connected"] = bool(event["control_ready"])
                backend["session_status"] = (
                    "READY" if event["control_ready"] else event["phase"].upper()
                )
                backend["catalog_status"] = (
                    "ACTIVE" if event["catalog_generation"] else "PENDING"
                )
            elif event_type == "command.receipt":
                if event["state_version"]:
                    self._snapshot["protocol_evidence"]["state_version"] = (
                        event["state_version"]
                    )
                if event["plan_generation"]:
                    self._snapshot["protocol_evidence"]["plan_generation"] = (
                        event["plan_generation"]
                    )
            elif event_type == "actual":
                truth["actual"][event["channel"]] = copy.deepcopy(event)
                self._snapshot["protocol_evidence"]["production_epoch"] = (
                    event["production_epoch"]
                )
            elif event_type == "telemetry.wifi_effective":
                truth["wifi_effective"] = copy.deepcopy(event)
            elif event_type == "telemetry.device_effective":
                truth["device_effective"] = copy.deepcopy(event)
            elif event_type == "telemetry.wifi":
                truth["wifi_sample"] = copy.deepcopy(event)
                self._snapshot["bindings"]["wifi_telemetry"] = "BOUND"
            elif event_type == "telemetry.device":
                truth["device_sample"] = copy.deepcopy(event)
            elif event_type == "presentation.status":
                truth["presentation_status"] = copy.deepcopy(event)
                self._snapshot["protocol_evidence"]["plan_generation"] = (
                    event["active_plan_generation"]
                )
            elif event_type == "scene.status":
                truth["scene_status"] = copy.deepcopy(event)
                self._snapshot["protocol_evidence"]["world_generation"] = (
                    event["world_generation"]
                )
            elif event_type == "render.result":
                truth["render_result"] = copy.deepcopy(event)
            elif event_type == "media.actual":
                truth["media_actual"][event["source"]] = copy.deepcopy(event)
            elif event_type in {
                "terminal", "native.bridge_terminal", "native.process_error"
            }:
                backend["native_host_bound"] = False
                backend["device_connected"] = False
                backend["session_status"] = "FAILED"
                truth["control_snapshot"] = None
                self._snapshot["frames"] = {
                    "render": {"available": False},
                    "camera": {"available": False},
                }
            self._refresh_bindings_locked()
            self._append_event_locked(
                f"native:{event_type}", "NATIVE_EVENT", event
            )

    def _refresh_bindings_locked(self) -> None:
        truth = self._snapshot["native_truth"]
        control = truth["control_snapshot"]
        actual = truth["actual"]
        session_bound = bool(
            self._snapshot["backend"]["native_host_bound"] and
            self._snapshot["backend"]["device_connected"]
        )
        media_bound = bool(session_bound and control and control["media_bound"])
        mask = control["media_mask"] if control else "none"
        for channel, binding_key, selected in (
            ("render_media", "render_result_stream",
             mask in {"render", "both"}),
            ("camera_media", "camera_media_stream",
             mask in {"camera", "both"}),
        ):
            value = actual.get(channel)
            bound = bool(
                media_bound and selected and value and value["enabled"] and
                value["availability"] == "present" and
                value["production_status"] == "running"
            )
            self._snapshot["bindings"][binding_key] = (
                "BOUND" if bound else "UNBOUND"
            )
        wifi = actual.get("wifi")
        wifi_bound = bool(
            session_bound and truth["wifi_sample"] and wifi and wifi["enabled"] and
            wifi["availability"] == "present" and
            wifi["production_status"] == "running"
        )
        self._snapshot["bindings"]["wifi_telemetry"] = (
            "BOUND" if wifi_bound else "UNBOUND"
        )

    def _native_frame(self, source: str, metadata: dict[str, Any]) -> None:
        event = {"type": "native.frame", **metadata}
        with self._condition:
            self._snapshot["frames"][source] = {
                "available": True, **copy.deepcopy(metadata)
            }
            self._snapshot["protocol_evidence"]["stream_sequence"] = (
                metadata["frame_sequence"]
            )
            self._append_event_locked(
                f"native.frame:{source}", "NATIVE_FRAME", event
            )

    def _find_object_locked(self, draft_object: int) -> dict[str, Any] | None:
        for item in self._snapshot["scene"]["objects"]:
            if item["draft_object"] == draft_object:
                return item
        return None


class DashboardServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address: tuple[str, int], state: DashboardState):
        super().__init__(address, DashboardHandler)
        self.state = state
        self.expected_origin = f"http://{LOOPBACK_ADDRESS}:{self.server_port}"

    def handle_error(self, request: Any, client_address: Any) -> None:
        _type, error, _traceback = sys.exc_info()
        if isinstance(error, (BrokenPipeError, ConnectionResetError)):
            return
        super().handle_error(request, client_address)

    def server_close(self) -> None:
        self.state.close()
        super().server_close()


class DashboardHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server: DashboardServer

    def log_message(self, _format: str, *_args: Any) -> None:
        return

    def _headers(
        self,
        status: int,
        content_type: str,
        content_length: int | None = None,
    ) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self'; "
            "connect-src 'self'; img-src 'self' data:; object-src 'none'; "
            "base-uri 'none'; frame-ancestors 'none'",
        )
        if content_length is not None:
            self.send_header("Content-Length", str(content_length))
        self.end_headers()

    def _json(self, status: int, value: dict[str, Any]) -> None:
        encoded = json.dumps(value, separators=(",", ":")).encode("utf-8")
        self._headers(status, "application/json; charset=utf-8", len(encoded))
        self.wfile.write(encoded)

    def _error(self, status: int, reason: str) -> None:
        self._json(status, {"error": reason})

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/api/v1/snapshot":
            self._json(HTTPStatus.OK, self.server.state.snapshot())
            return
        if parsed.path == "/api/v1/events":
            self._serve_events(parsed.query)
            return
        if parsed.path in {"/api/v1/frames/render", "/api/v1/frames/camera"}:
            self._serve_frame(parsed.path.rsplit("/", 1)[1])
            return
        if parsed.path == "/":
            self._serve_index()
            return
        if parsed.path.startswith("/static/"):
            self._serve_static(parsed.path.removeprefix("/static/"))
            return
        self._error(HTTPStatus.NOT_FOUND, "NOT_FOUND")

    def do_POST(self) -> None:
        if urlparse(self.path).path != "/api/v1/command":
            self._error(HTTPStatus.NOT_FOUND, "NOT_FOUND")
            return
        if self.headers.get("Origin") != self.server.expected_origin:
            self._error(HTTPStatus.FORBIDDEN, "ORIGIN_REJECTED")
            return
        if self.headers.get("X-Meiso-CSRF") != self.server.state.csrf_token:
            self._error(HTTPStatus.FORBIDDEN, "CSRF_REJECTED")
            return
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            self._error(HTTPStatus.BAD_REQUEST, "INVALID_LENGTH")
            return
        if length < 2 or length > MAX_CONTROL_BODY_BYTES:
            self._error(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "BODY_SIZE")
            return
        if self.headers.get("Content-Type") != "application/json":
            self._error(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, "CONTENT_TYPE")
            return
        try:
            raw = self.rfile.read(length)
            if len(raw) != length:
                raise ValueError
            command = json.loads(raw.decode("utf-8"))
            event_id, snapshot = self.server.state.apply(command)
        except CommandError as error:
            self._error(HTTPStatus.UNPROCESSABLE_ENTITY, str(error))
            return
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError):
            self._error(HTTPStatus.BAD_REQUEST, "MALFORMED_JSON")
            return
        self._json(
            HTTPStatus.ACCEPTED,
            {
                "accepted": True,
                "event_id": event_id,
                "revision": snapshot["revision"],
            },
        )

    def _serve_index(self) -> None:
        path = STATIC_ROOT / "index.html"
        try:
            raw = path.read_bytes()
        except OSError:
            self._error(HTTPStatus.INTERNAL_SERVER_ERROR, "STATIC_MISSING")
            return
        if len(raw) > MAX_STATIC_ASSET_BYTES:
            self._error(HTTPStatus.INTERNAL_SERVER_ERROR, "STATIC_TOO_LARGE")
            return
        placeholder = CSRF_PLACEHOLDER.encode("ascii")
        if raw.count(placeholder) != 1:
            self._error(HTTPStatus.INTERNAL_SERVER_ERROR, "CSRF_TEMPLATE")
            return
        encoded = raw.replace(placeholder, self.server.state.csrf_token.encode("ascii"))
        self._headers(HTTPStatus.OK, "text/html; charset=utf-8", len(encoded))
        self.wfile.write(encoded)

    def _serve_static(self, name: str) -> None:
        allowed = {
            "app.js": "text/javascript; charset=utf-8",
            "model.mjs": "text/javascript; charset=utf-8",
            "style.css": "text/css; charset=utf-8",
        }
        if name not in allowed:
            self._error(HTTPStatus.NOT_FOUND, "NOT_FOUND")
            return
        path = STATIC_ROOT / name
        try:
            content = path.read_bytes()
        except OSError:
            self._error(HTTPStatus.NOT_FOUND, "NOT_FOUND")
            return
        if len(content) > MAX_STATIC_ASSET_BYTES:
            self._error(HTTPStatus.INTERNAL_SERVER_ERROR, "STATIC_TOO_LARGE")
            return
        self._headers(HTTPStatus.OK, allowed[name], len(content))
        self.wfile.write(content)

    def _serve_frame(self, source: str) -> None:
        value = self.server.state.latest_frame(source)
        if value is None:
            self._error(HTTPStatus.NOT_FOUND, "FRAME_UNAVAILABLE")
            return
        record, _metadata = value
        self._headers(
            HTTPStatus.OK, "application/vnd.meiso.cmfb", len(record)
        )
        self.wfile.write(record)

    def _serve_events(self, query: str) -> None:
        parameters = parse_qs(query, keep_blank_values=True)
        if set(parameters) - {"after"}:
            self._error(HTTPStatus.BAD_REQUEST, "INVALID_QUERY")
            return
        raw_after = parameters.get("after", ["0"])
        if len(raw_after) != 1:
            self._error(HTTPStatus.BAD_REQUEST, "INVALID_QUERY")
            return
        try:
            after = int(raw_after[0])
        except ValueError:
            self._error(HTTPStatus.BAD_REQUEST, "INVALID_CURSOR")
            return
        if after < 0:
            self._error(HTTPStatus.BAD_REQUEST, "INVALID_CURSOR")
            return
        self._headers(HTTPStatus.OK, "text/event-stream; charset=utf-8")
        try:
            while True:
                events = self.server.state.events_after(after, 10.0)
                if not events:
                    self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
                    continue
                for event_id, event in events:
                    data = json.dumps(event, separators=(",", ":")).encode("utf-8")
                    self.wfile.write(f"id: {event_id}\n".encode("ascii"))
                    self.wfile.write(b"event: dashboard\n")
                    self.wfile.write(b"data: " + data + b"\n\n")
                    after = event_id
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            return


def create_server(
    *,
    port: int = 0,
    event_capacity: int = DEFAULT_EVENT_CAPACITY,
    native_host: str | None = None,
    native_config: str | None = None,
) -> tuple[DashboardServer, DashboardState]:
    state = DashboardState(event_capacity)
    if (native_host is None) != (native_config is None):
        raise NativeBridgeError("NATIVE_ARGUMENT_PAIR")
    server = DashboardServer((LOOPBACK_ADDRESS, port), state)
    if native_host is not None and native_config is not None:
        try:
            state.enable_native(native_host, native_config)
        except Exception:
            server.server_close()
            raise
    return server, state


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Loopback-only K230 recorded/local debug dashboard"
    )
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--native-host")
    parser.add_argument("--native-config")
    arguments = parser.parse_args()
    if arguments.port < 1 or arguments.port > 65535:
        parser.error("--port must be in 1..65535")
    if (arguments.native_host is None) != (arguments.native_config is None):
        parser.error("--native-host and --native-config must be supplied together")
    try:
        server, _state = create_server(
            port=arguments.port,
            native_host=arguments.native_host,
            native_config=arguments.native_config,
        )
    except NativeBridgeError as error:
        print(f"Native bridge failed: {error}", file=sys.stderr)
        return 1
    print(f"K230 debug dashboard: http://{LOOPBACK_ADDRESS}:{server.server_port}/")
    print(
        "Backend: NATIVE_HOST (typed local bridge)." if arguments.native_host
        else "Backend: RECORDED_LOCAL; device/media are not connected."
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
