#!/usr/bin/env python3
"""Bounded local bridge to the native CompositionDebugHost process.

This module never parses or emits Meiso wire.  It owns only the native
service's strict NDJSON command surface and its machine-local CMFB/v1 stream.
"""

from __future__ import annotations

from collections import deque
import copy
import json
import math
import os
from pathlib import Path
import re
import select
import socket
import struct
import subprocess
import threading
from typing import Any, Callable


MAX_NATIVE_LINE_BYTES = 16 * 1024
MAX_NATIVE_QUEUE_LINES = 64
MAX_NATIVE_REQUESTS = 64
CMFB_HEADER_BYTES = 80
MAX_RENDER_FRAME_BYTES = 320 * 240 * 4
MAX_CAMERA_FRAME_BYTES = 640 * 480 * 3 // 2
MAX_CMFB_RECORD_BYTES = CMFB_HEADER_BYTES + MAX_CAMERA_FRAME_BYTES

SOURCE_NAMES = {1: "render", 2: "camera"}
PIXEL_FORMAT_NAMES = {1: "rgba8888", 2: "rgb565", 3: "nv12", 4: "yuyv"}


class NativeBridgeError(RuntimeError):
    """The explicit native bridge cannot accept or continue an operation."""


def _multiply_quaternion(
    lhs: tuple[float, float, float, float],
    rhs: tuple[float, float, float, float],
) -> tuple[float, float, float, float]:
    lx, ly, lz, lw = lhs
    rx, ry, rz, rw = rhs
    return (
        lw * rx + lx * rw + ly * rz - lz * ry,
        lw * ry - lx * rz + ly * rw + lz * rx,
        lw * rz + lx * ry - ly * rx + lz * rw,
        lw * rw - lx * rx - ly * ry - lz * rz,
    )


def euler_xyz_degrees_to_xyzw(values: list[float]) -> list[float]:
    """Return normalized Hamilton qz*qy*qx for finite XYZ Euler degrees."""
    if len(values) != 3 or not all(
        isinstance(value, (int, float)) and not isinstance(value, bool) and
        math.isfinite(float(value))
        for value in values
    ):
        raise NativeBridgeError("INVALID_EULER")
    half_x, half_y, half_z = (
        math.radians(float(value)) * 0.5 for value in values
    )
    qx = (math.sin(half_x), 0.0, 0.0, math.cos(half_x))
    qy = (0.0, math.sin(half_y), 0.0, math.cos(half_y))
    qz = (0.0, 0.0, math.sin(half_z), math.cos(half_z))
    result = _multiply_quaternion(_multiply_quaternion(qz, qy), qx)
    length = math.sqrt(sum(component * component for component in result))
    if not math.isfinite(length) or length <= 0.0:
        raise NativeBridgeError("INVALID_EULER")
    normalized = [component / length for component in result]
    # Remove negative zero so the strict NDJSON surface stays deterministic.
    return [0.0 if component == 0.0 else component for component in normalized]


def full_scene_command(snapshot: dict[str, Any]) -> dict[str, Any]:
    """Build one complete browser-owned scene snapshot for the native service."""
    scene = snapshot["scene"]
    camera = scene["camera"]
    objects = scene["objects"]
    if len(objects) > 32:
        raise NativeBridgeError("OBJECT_CAPACITY")
    native_objects: list[dict[str, Any]] = []
    for item in objects:
        if item["asset"] != "builtin:cube":
            raise NativeBridgeError("ASSET_UNAVAILABLE")
        native_objects.append({
            "editor_id": item["draft_object"],
            "asset_handle": "cube",
            "position": list(item["position"]),
            "orientation_xyzw": euler_xyz_degrees_to_xyzw(
                item["rotation_deg"]
            ),
            "scale": list(item["scale"]),
            "visible": True,
        })
    return {
        "command": "set_scene",
        "enabled": True,
        "camera": {
            "position": list(camera["position"]),
            "orientation_xyzw": euler_xyz_degrees_to_xyzw(
                camera["rotation_deg"]
            ),
            "vertical_fov_degrees": camera["vertical_fov_deg"],
            "near_z": camera["near"],
            "far_z": camera["far"],
        },
        "objects": native_objects,
    }


def _enabled_periodic(command: str, intent: dict[str, Any]) -> dict[str, Any]:
    result: dict[str, Any] = {
        "command": command,
        "enabled": intent["enabled"],
    }
    if intent["enabled"]:
        result["selected_fields"] = intent["selected_fields"]
        result["sample_period_ms"] = intent["sample_period_ms"]
    return result


def _media_command(
    command: str,
    enabled: bool,
    pixel_format: str,
    width: int,
    height: int,
    fps: int,
) -> dict[str, Any]:
    result: dict[str, Any] = {"command": command, "enabled": enabled}
    if enabled:
        result.update({
            "pixel_format": pixel_format,
            "codec": "raw",
            "width": width,
            "height": height,
            "fps_millihz": fps * 1000,
            "bitrate_bps": 0,
            "gop_frames": 1,
            "request_key": True,
        })
    return result


def commands_for_mutation(
    command_name: str, snapshot: dict[str, Any]
) -> list[dict[str, Any]]:
    """Map one accepted browser mutation to strict native command values."""
    if command_name in {
        "scene.object.add", "scene.object.delete", "scene.object.transform",
        "scene.camera.set",
    }:
        return [full_scene_command(snapshot)]
    if command_name == "scene.object.select":
        return []
    if command_name == "intent.render.set":
        intent = snapshot["intents"]["render"]
        presentation: dict[str, Any] = {
            "command": "set_presentation", "enabled": intent["enabled"]
        }
        if intent["enabled"]:
            presentation.update({
                "use_safety_frame": False,
                "target_width": 320,
                "target_height": 240,
                "color_format": "rgba8888",
                "depth_format": "implementation32",
                "max_result_hz": intent["max_hz"],
                "offline_lease_ms": 1000,
            })
        return [
            presentation,
            _media_command(
                "set_render_media", intent["enabled"], "rgba8888",
                320, 240, intent["max_hz"]
            ),
        ]
    if command_name == "intent.camera.set":
        intent = snapshot["intents"]["camera"]
        return [_media_command(
            "set_camera_media", intent["enabled"], "nv12", 320, 240,
            intent["fps"]
        )]
    if command_name == "intent.telemetry.set":
        return [_enabled_periodic("set_wifi", snapshot["intents"]["telemetry"])]
    if command_name == "intent.device.set":
        return [_enabled_periodic("set_device", snapshot["intents"]["device"])]
    raise NativeBridgeError("UNKNOWN_COMMAND")


def bootstrap_commands(snapshot: dict[str, Any]) -> list[dict[str, Any]]:
    commands = [
        {"command": "asset.load_builtin", "asset_handle": "cube"},
        full_scene_command(snapshot),
    ]
    for name in (
        "intent.render.set", "intent.camera.set", "intent.telemetry.set",
        "intent.device.set",
    ):
        commands.extend(commands_for_mutation(name, snapshot))
    commands.append({"command": "snapshot"})
    return commands


# Every native stdout object is reconstructed from one exact, flat allowlist.
# Fields not listed here are never projected to the browser.
_EVENT_SCHEMAS: dict[str, tuple[set[str], set[str]]] = {
    "service.ready": (
        {"type", "profile", "wire_owner", "asset_import", "frame_projection"},
        {"type", "profile", "asset_import", "frame_projection"},
    ),
    "error": (
        {"type", "request_id", "code"},
        {"type", "request_id", "code"},
    ),
    "command.receipt": (
        {"type", "request_id", "command_id", "kind", "local_result",
         "replaced", "replaced_command_id", "state_version",
         "plan_generation"},
        {"type", "request_id", "command_id", "kind", "local_result",
         "replaced", "replaced_command_id", "state_version",
         "plan_generation"},
    ),
    "asset.loaded": (
        {"type", "request_id", "asset_handle", "already_loaded"},
        {"type", "request_id", "asset_handle", "already_loaded"},
    ),
    "snapshot": (
        {"type", "request_id", "phase", "terminal_reason", "control_ready",
         "catalog_generation", "media_bound", "media_mask",
         "media_slots_consumed", "mesh_active", "mesh_next_offset",
         "wifi_actual", "presentation_actual", "scene_actual",
         "device_actual", "render_actual", "camera_actual", "wifi_sample",
         "render_result", "device_sample", "render_frame", "camera_frame"},
        {"type", "request_id", "phase", "terminal_reason", "control_ready",
         "catalog_generation", "media_bound", "media_mask",
         "media_slots_consumed", "mesh_active", "mesh_next_offset",
         "wifi_actual", "presentation_actual", "scene_actual",
         "device_actual", "render_actual", "camera_actual", "wifi_sample",
         "render_result", "device_sample", "render_frame", "camera_frame"},
    ),
    "pump.event": (
        {"type", "event", "command_id", "desired_channel", "actual_channel",
         "inline_channel", "media_source"},
        {"type", "event", "command_id", "desired_channel", "actual_channel",
         "inline_channel", "media_source"},
    ),
    "media.connected": ({"type", "transport"}, {"type", "transport"}),
    "terminal": ({"type", "code"}, {"type", "code"}),
    "closing": ({"type", "request_id"}, {"type", "request_id"}),
    "actual": (
        {"type", "channel", "state_version", "apply_result", "availability",
         "profile_decoded", "enabled", "production_status", "reason",
         "production_epoch"},
        {"type", "channel", "state_version", "apply_result", "availability",
         "profile_decoded", "enabled", "production_status", "reason",
         "production_epoch"},
    ),
    "telemetry.wifi_effective": (
        {"type", "selected_fields", "sample_period_ms"},
        {"type", "selected_fields", "sample_period_ms"},
    ),
    "telemetry.device_effective": (
        {"type", "selected_fields", "sample_period_ms"},
        {"type", "selected_fields", "sample_period_ms"},
    ),
    "presentation.status": (
        {"type", "candidate_state", "candidate_plan_generation",
         "active_state", "active_plan_generation", "reason", "target_width",
         "target_height", "color_format", "depth_format"},
        {"type", "candidate_state", "candidate_plan_generation",
         "active_state", "active_plan_generation", "reason", "target_width",
         "target_height", "color_format", "depth_format"},
    ),
    "media.actual": (
        {"type", "source", "pixel_format", "codec", "width", "height",
         "fps_millihz", "bitrate_bps", "gop_frames", "request_key",
         "media_stream_epoch", "producer_dropped_frames", "queue_depth",
         "recovery_generation"},
        {"type", "source", "pixel_format", "codec", "width", "height",
         "fps_millihz", "bitrate_bps", "gop_frames", "request_key",
         "media_stream_epoch", "producer_dropped_frames", "queue_depth",
         "recovery_generation"},
    ),
    "telemetry.wifi": (
        {"type", "selected_fields", "present_fields", "link_state",
         "signal_mbm", "rx_bytes", "tx_bytes", "rx_packets", "tx_packets",
         "rx_dropped", "tx_dropped", "rx_errors", "tx_errors"},
        {"type", "selected_fields", "present_fields", "link_state",
         "signal_mbm", "rx_bytes", "tx_bytes", "rx_packets", "tx_packets",
         "rx_dropped", "tx_dropped", "rx_errors", "tx_errors"},
    ),
    "telemetry.device": (
        {"type", "selected_fields", "present_fields", "temperature_millic",
         "temperature_uncertainty_millic", "cpu_load_ppm",
         "memory_total_bytes", "memory_available_bytes"},
        {"type", "selected_fields", "present_fields", "temperature_millic",
         "temperature_uncertainty_millic", "cpu_load_ppm",
         "memory_total_bytes", "memory_available_bytes"},
    ),
    "scene.status": (
        {"type", "reason", "active_plan_generation", "world_generation",
         "object_count", "unique_mesh_count", "resident_mesh_bytes",
         "render_jobs_in_flight", "result_queue_depth"},
        {"type", "reason", "active_plan_generation", "world_generation",
         "object_count", "unique_mesh_count", "resident_mesh_bytes",
         "render_jobs_in_flight", "result_queue_depth"},
    ),
    "render.result": (
        {"type", "result", "active_plan_generation", "world_generation",
         "object_count", "unique_mesh_count", "target_width",
         "target_height", "color_format", "depth_format",
         "source_clock_epoch", "render_start_monotonic_ns",
         "render_end_monotonic_ns", "offscreen_complete",
         "thumbnail_included", "fence_complete", "display_commit_accepted",
         "vblank_observed", "physical_verified", "full_target_sha256"},
        {"type", "result", "active_plan_generation", "world_generation",
         "object_count", "unique_mesh_count", "target_width",
         "target_height", "color_format", "depth_format",
         "source_clock_epoch", "render_start_monotonic_ns",
         "render_end_monotonic_ns", "offscreen_complete",
         "thumbnail_included", "fence_complete", "display_commit_accepted",
         "vblank_observed", "physical_verified", "full_target_sha256"},
    ),
    "media.frame": (
        {"type", "source", "sequence", "clock_epoch", "monotonic_ns",
         "pixel_format", "codec", "width", "height", "access_unit_bytes",
         "frame_fd_queued"},
        {"type", "source", "sequence", "clock_epoch", "monotonic_ns",
         "pixel_format", "codec", "width", "height", "access_unit_bytes",
         "frame_fd_queued"},
    ),
}

_BOOLEAN_FIELDS = {
    "replaced", "already_loaded", "control_ready", "media_bound",
    "mesh_active", "wifi_actual", "presentation_actual", "scene_actual",
    "device_actual", "render_actual", "camera_actual", "wifi_sample",
    "render_result", "device_sample", "render_frame", "camera_frame",
    "profile_decoded", "enabled", "request_key", "link_state",
    "offscreen_complete", "thumbnail_included", "fence_complete",
    "display_commit_accepted", "vblank_observed", "physical_verified",
    "frame_fd_queued",
}

_SIGNED_INTEGER_FIELDS = {"signal_mbm", "temperature_millic"}

_EVENT_ENUMS: dict[tuple[str, str], set[str]] = {
    ("service.ready", "profile"): {"composition-v3"},
    ("service.ready", "wire_owner"): {
        "native_service", "simulator_service",
    },
    ("service.ready", "asset_import"): {"builtin_cube_only"},
    ("service.ready", "frame_projection"): {"CMFB/v1"},
    ("command.receipt", "kind"): {
        "put_mesh", "set_scene", "set_presentation", "set_wifi",
        "set_device", "set_render_media", "set_camera_media",
    },
    ("command.receipt", "local_result"): {"queued", "replaced_pending"},
    ("asset.loaded", "asset_handle"): {"cube"},
    ("snapshot", "phase"): {
        "handshake", "catalog", "rebind", "ready", "terminal",
    },
    ("snapshot", "terminal_reason"): {
        "none", "control_transport", "control_session", "catalog_changed",
        "local_abort", "internal_failure",
    },
    ("snapshot", "media_mask"): {"none", "render", "camera", "both"},
    ("pump.event", "event"): {
        "control_progress", "desired_delivery", "actual_updated",
        "inline_updated", "mesh_progress", "media_progress", "media_frame",
        "boundary", "terminal",
    },
    ("pump.event", "desired_channel"): {
        "none", "wifi", "presentation", "scene", "device", "render_media",
        "camera_media",
    },
    ("pump.event", "actual_channel"): {
        "none", "wifi", "presentation", "scene", "device", "render_media",
        "camera_media",
    },
    ("pump.event", "inline_channel"): {
        "none", "wifi", "render_result", "device",
    },
    ("pump.event", "media_source"): {"none", "render", "camera"},
    ("media.connected", "transport"): {"tcp"},
    ("terminal", "code"): {
        "HOST_APP_TRANSPORT_TERMINAL", "HOST_APP_SESSION_TERMINAL",
        "HOST_APP_CATALOG_TERMINAL", "HOST_APP_LOCAL_TERMINAL",
        "HOST_APP_INTERNAL_TERMINAL", "HOST_APP_TERMINAL",
        "SIMULATOR_TERMINAL",
    },
    ("actual", "channel"): {
        "wifi", "presentation", "scene", "device", "render_media",
        "camera_media",
    },
    ("actual", "apply_result"): {
        "none", "pending", "applied", "rejected", "failed",
    },
    ("actual", "availability"): {"present", "absent", "unavailable"},
    ("actual", "production_status"): {
        "stopped", "starting", "running", "degraded", "stopping", "failed",
    },
    ("actual", "reason"): {
        "none", "disabled", "applying", "unsupported_parameter",
        "admission_denied", "unavailable", "apply_failed",
    },
    ("presentation.status", "candidate_state"): {
        "none", "preparing", "prepared", "activation_pending", "rejected",
        "failed",
    },
    ("presentation.status", "active_state"): {
        "none", "active", "retiring", "safety",
    },
    ("presentation.status", "reason"): {
        "none", "waiting_scene", "capacity", "invalid_plan",
        "renderer_failed",
    },
    ("presentation.status", "color_format"): {"rgba8888", "rgb565"},
    ("presentation.status", "depth_format"): {"implementation32"},
    ("media.actual", "source"): {"render", "camera"},
    ("media.actual", "pixel_format"): {
        "rgba8888", "rgb565", "nv12", "yuyv",
    },
    ("media.actual", "codec"): {"raw"},
    ("scene.status", "reason"): {
        "none", "ready", "waiting_blob", "object_capacity", "blob_capacity",
        "mesh_bytes_capacity", "buffer_capacity", "invalid_scene",
        "renderer_failed",
    },
    ("render.result", "result"): {"complete"},
    ("render.result", "color_format"): {"rgba8888", "rgb565"},
    ("render.result", "depth_format"): {"implementation32"},
    ("media.frame", "source"): {"render", "camera"},
    ("media.frame", "pixel_format"): {
        "rgba8888", "rgb565", "nv12", "yuyv",
    },
    ("media.frame", "codec"): {"raw"},
}


def _safe_scalar(value: Any) -> bool:
    return (
        isinstance(value, bool) or
        (isinstance(value, int) and not isinstance(value, bool) and
         -(2**63) <= value <= 2**64 - 1) or
        (isinstance(value, str) and len(value) <= 128 and
         all(0x20 <= ord(character) <= 0x7E for character in value))
    )


def sanitize_native_event(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict) or not isinstance(value.get("type"), str):
        raise NativeBridgeError("INVALID_NATIVE_EVENT")
    schema = _EVENT_SCHEMAS.get(value["type"])
    if schema is None or set(value) != schema[0] or not all(
        _safe_scalar(item) for item in value.values()
    ):
        raise NativeBridgeError("INVALID_NATIVE_EVENT")
    event_type = value["type"]
    for key, item in value.items():
        if key == "type":
            continue
        allowed = _EVENT_ENUMS.get((event_type, key))
        if allowed is not None:
            if not isinstance(item, str) or item not in allowed:
                raise NativeBridgeError("INVALID_NATIVE_EVENT")
        elif key in _BOOLEAN_FIELDS:
            if not isinstance(item, bool):
                raise NativeBridgeError("INVALID_NATIVE_EVENT")
        elif key == "full_target_sha256":
            if not isinstance(item, str) or re.fullmatch(
                r"[0-9a-f]{64}", item
            ) is None:
                raise NativeBridgeError("INVALID_NATIVE_EVENT")
        elif key == "code":
            if not isinstance(item, str) or re.fullmatch(
                r"[A-Z][A-Z0-9_]{0,63}", item
            ) is None:
                raise NativeBridgeError("INVALID_NATIVE_EVENT")
        elif (not isinstance(item, int) or isinstance(item, bool) or
              (key not in _SIGNED_INTEGER_FIELDS and item < 0)):
            raise NativeBridgeError("INVALID_NATIVE_EVENT")
    if event_type in {"media.actual", "media.frame"}:
        formats = (
            {"rgba8888", "rgb565"} if value["source"] == "render" else
            {"nv12", "yuyv"}
        )
        if value["pixel_format"] not in formats:
            raise NativeBridgeError("INVALID_NATIVE_EVENT")
    return {key: copy.deepcopy(value[key]) for key in value if key in schema[1]}


class NativeBridge:
    """Own exactly one native service, bounded command queue and CMFB cache."""

    def __init__(
        self,
        executable: str,
        config: str | None,
        on_event: Callable[[dict[str, Any]], None],
        on_frame: Callable[[str, dict[str, Any]], None],
        *,
        simulator: bool = False,
    ):
        self.executable = Path(executable).expanduser().resolve()
        self.config = (
            None if config is None else Path(config).expanduser().resolve()
        )
        self.simulator = simulator
        self._on_event = on_event
        self._on_frame = on_frame
        self._condition = threading.Condition()
        self._outbound: deque[bytes] = deque()
        self._next_request_id = 1
        self._requests: dict[int, tuple[str, int]] = {}
        self._frames: dict[str, tuple[bytes, dict[str, Any]]] = {}
        self._process: subprocess.Popen[bytes] | None = None
        self._frame_socket: socket.socket | None = None
        self._threads: list[threading.Thread] = []
        self._stopping = False
        self._terminal_emitted = False

    def start(self) -> None:
        with self._condition:
            if self._process is not None or self._stopping:
                raise NativeBridgeError("NATIVE_ALREADY_BOUND")
        if (not self.executable.is_file() or
            not os.access(self.executable, os.X_OK) or
            (self.simulator and self.config is not None) or
            (not self.simulator and
             (self.config is None or not self.config.is_file()))):
            raise NativeBridgeError("NATIVE_PATH")
        parent_frame, child_frame = socket.socketpair(
            socket.AF_UNIX, socket.SOCK_STREAM
        )
        child_frame.setblocking(False)
        try:
            arguments = [str(self.executable)]
            if self.simulator:
                arguments.append("--simulator")
            else:
                arguments.extend(["--config", str(self.config)])
            arguments.extend([
                "--interactive", "--frame-fd", str(child_frame.fileno()),
            ])
            process = subprocess.Popen(
                arguments,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                close_fds=True,
                pass_fds=(child_frame.fileno(),),
                bufsize=0,
            )
        except OSError as error:
            parent_frame.close()
            child_frame.close()
            raise NativeBridgeError("NATIVE_START") from error
        child_frame.close()
        if process.stdin is None or process.stdout is None or process.stderr is None:
            parent_frame.close()
            process.terminate()
            raise NativeBridgeError("NATIVE_PIPES")
        os.set_blocking(process.stdin.fileno(), False)
        self._process = process
        self._frame_socket = parent_frame
        targets = (
            self._writer_loop, self._stdout_loop, self._stderr_loop,
            self._frame_loop, self._wait_loop,
        )
        self._threads = []
        for target in targets:
            thread = threading.Thread(
                target=target, daemon=True, name=target.__name__
            )
            thread.start()
            self._threads.append(thread)

    def submit_batch(
        self,
        commands: list[dict[str, Any]],
        *,
        origin: str,
        revision: int,
    ) -> list[int]:
        if not commands:
            return []
        with self._condition:
            if (self._stopping or self._process is None or
                self._terminal_emitted or self._process.poll() is not None):
                raise NativeBridgeError("NATIVE_TERMINAL")
            if (len(self._outbound) + len(commands) > MAX_NATIVE_QUEUE_LINES or
                len(self._requests) + len(commands) > MAX_NATIVE_REQUESTS):
                raise NativeBridgeError("NATIVE_BACKPRESSURE")
            if self._next_request_id > 2**64 - 1 - len(commands):
                raise NativeBridgeError("REQUEST_ID_EXHAUSTED")
            candidate_lines: list[bytes] = []
            candidate_ids: list[int] = []
            for offset, command in enumerate(commands):
                if (not isinstance(command, dict) or
                    not isinstance(command.get("command"), str) or
                    "request_id" in command):
                    raise NativeBridgeError("INVALID_NATIVE_COMMAND")
                request_id = self._next_request_id + offset
                value = {
                    "command": command["command"],
                    "request_id": request_id,
                }
                value.update({
                    key: copy.deepcopy(item)
                    for key, item in command.items() if key != "command"
                })
                try:
                    encoded = json.dumps(
                        value, separators=(",", ":"), ensure_ascii=True,
                        allow_nan=False,
                    ).encode("ascii") + b"\n"
                except (TypeError, ValueError) as error:
                    raise NativeBridgeError("INVALID_NATIVE_COMMAND") from error
                if len(encoded) >= MAX_NATIVE_LINE_BYTES:
                    raise NativeBridgeError("NATIVE_COMMAND_SIZE")
                candidate_lines.append(encoded)
                candidate_ids.append(request_id)
            self._next_request_id += len(commands)
            for request_id, encoded in zip(candidate_ids, candidate_lines):
                self._outbound.append(encoded)
                self._requests[request_id] = (origin, revision)
            self._condition.notify_all()
            return candidate_ids

    def latest_frame(self, source: str) -> tuple[bytes, dict[str, Any]] | None:
        if source not in {"render", "camera"}:
            raise NativeBridgeError("INVALID_FRAME_SOURCE")
        with self._condition:
            if self._stopping or self._terminal_emitted:
                return None
            value = self._frames.get(source)
            if value is None:
                return None
            return bytes(value[0]), copy.deepcopy(value[1])

    def close(self) -> None:
        with self._condition:
            if self._stopping:
                return
            self._stopping = True
            self._outbound.clear()
            self._requests.clear()
            self._frames.clear()
            self._condition.notify_all()
        process = self._process
        if process is not None:
            if process.stdin is not None:
                try:
                    process.stdin.close()
                except OSError:
                    pass
            try:
                process.wait(timeout=0.5)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=1.0)
        frame_socket = self._frame_socket
        if frame_socket is not None:
            try:
                frame_socket.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            frame_socket.close()
            self._frame_socket = None
        for thread in self._threads:
            if thread is not threading.current_thread():
                thread.join(timeout=1.0)
        if process is not None:
            for stream in (process.stdin, process.stdout, process.stderr):
                if stream is not None and not stream.closed:
                    try:
                        stream.close()
                    except OSError:
                        pass

    def _emit_terminal(self, code: str) -> None:
        with self._condition:
            if self._terminal_emitted:
                return
            self._terminal_emitted = True
            self._outbound.clear()
            self._requests.clear()
            self._frames.clear()
            self._condition.notify_all()
        process = self._process
        if process is not None and process.poll() is None:
            try:
                process.terminate()
            except OSError:
                pass
        self._on_event({"type": "native.bridge_terminal", "code": code})

    def _writer_loop(self) -> None:
        current = b""
        offset = 0
        while True:
            with self._condition:
                while (not self._stopping and not self._terminal_emitted and
                       not current and not self._outbound):
                    self._condition.wait(0.5)
                if self._stopping or self._terminal_emitted:
                    return
                if not current:
                    current = self._outbound.popleft()
                    offset = 0
            process = self._process
            if process is None or process.stdin is None:
                self._emit_terminal("NATIVE_PIPE")
                return
            try:
                _, writable, _ = select.select(
                    [], [process.stdin.fileno()], [], 0.5
                )
                if not writable:
                    continue
                written = os.write(process.stdin.fileno(), current[offset:])
                if written <= 0:
                    raise OSError("short native write")
                offset += written
                if offset == len(current):
                    current = b""
            except (OSError, ValueError):
                if not self._stopping:
                    self._emit_terminal("NATIVE_STDIN")
                return

    def _stdout_loop(self) -> None:
        process = self._process
        if process is None or process.stdout is None:
            return
        buffer = bytearray()
        try:
            while True:
                chunk = process.stdout.read(4096)
                if not chunk:
                    break
                buffer.extend(chunk)
                if len(buffer) > MAX_NATIVE_LINE_BYTES * 2:
                    raise NativeBridgeError("NATIVE_OUTPUT_SIZE")
                while b"\n" in buffer:
                    raw, _, remainder = buffer.partition(b"\n")
                    buffer[:] = remainder
                    if len(raw) >= MAX_NATIVE_LINE_BYTES:
                        raise NativeBridgeError("NATIVE_OUTPUT_SIZE")
                    value = json.loads(raw.decode("utf-8"))
                    if (isinstance(value, dict) and
                        value.get("type") == "service.ready" and
                        value.get("wire_owner") != (
                            "simulator_service" if self.simulator else
                            "native_service"
                        )):
                        raise NativeBridgeError("NATIVE_MODE_MISMATCH")
                    event = sanitize_native_event(value)
                    request_id = event.get("request_id")
                    if isinstance(request_id, int):
                        with self._condition:
                            request = self._requests.get(request_id)
                            if event["type"] in {
                                "command.receipt", "asset.loaded", "error",
                                "snapshot", "closing",
                            }:
                                self._requests.pop(request_id, None)
                        if request is not None:
                            event["browser_command"] = request[0]
                            event["browser_revision"] = request[1]
                    self._on_event(event)
                    if event["type"] in {"error", "terminal"}:
                        self._emit_terminal(
                            "NATIVE_COMMAND_ERROR" if event["type"] == "error"
                            else "NATIVE_HOST_TERMINAL"
                        )
                        return
            if buffer:
                raise NativeBridgeError("NATIVE_OUTPUT_PARTIAL")
            if not self._stopping:
                self._emit_terminal("NATIVE_STDOUT_EOF")
        except (UnicodeDecodeError, json.JSONDecodeError, NativeBridgeError):
            if not self._stopping:
                self._emit_terminal("NATIVE_STDOUT")

    def _stderr_loop(self) -> None:
        process = self._process
        if process is None or process.stderr is None:
            return
        buffer = bytearray()
        while True:
            chunk = process.stderr.read(256)
            if not chunk:
                break
            buffer.extend(chunk)
            while b"\n" in buffer:
                raw, _, remainder = buffer.partition(b"\n")
                buffer[:] = remainder
                text = raw.decode("ascii", "ignore")
                code = text if re.fullmatch(r"E_[A-Z_]{1,48}", text) else "E_NATIVE"
                self._on_event({"type": "native.process_error", "code": code})
                self._emit_terminal("NATIVE_STDERR")
                return
            if len(buffer) > 256:
                self._on_event({"type": "native.process_error", "code": "E_NATIVE"})
                self._emit_terminal("NATIVE_STDERR")
                return
        if buffer and not self._stopping:
            self._on_event({"type": "native.process_error", "code": "E_NATIVE"})
            self._emit_terminal("NATIVE_STDERR")

    @staticmethod
    def _parse_frame(record: bytes) -> tuple[str, dict[str, Any]]:
        if len(record) < CMFB_HEADER_BYTES or record[:4] != b"CMFB":
            raise NativeBridgeError("CMFB_HEADER")
        version, header_size = struct.unpack_from(">HH", record, 4)
        source = record[8]
        reserved_source = record[9]
        pixel_format, codec, flags, width, height = struct.unpack_from(
            ">HHHHH", record, 10
        )
        payload_bytes = struct.unpack_from(">I", record, 20)[0]
        sequence, clock_epoch, monotonic_ns = struct.unpack_from(">QQQ", record, 24)
        active_plan, world, scene_version = struct.unpack_from(">QQQ", record, 48)
        if (version != 1 or header_size != CMFB_HEADER_BYTES or
            source not in SOURCE_NAMES or reserved_source != 0 or codec != 1 or
            width == 0 or height == 0 or record[72:80] != bytes(8) or
            len(record) != CMFB_HEADER_BYTES + payload_bytes):
            raise NativeBridgeError("CMFB_HEADER")
        pixels = width * height
        expected = {
            1: pixels * 4,
            2: pixels * 2,
            3: pixels * 3 // 2 if width % 2 == 0 and height % 2 == 0 else -1,
            4: pixels * 2 if width % 2 == 0 else -1,
        }.get(pixel_format, -1)
        source_formats = {1: {1, 2}, 2: {3, 4}}
        limit = MAX_RENDER_FRAME_BYTES if source == 1 else MAX_CAMERA_FRAME_BYTES
        if (pixel_format not in source_formats[source] or
            payload_bytes != expected or payload_bytes > limit or
            sequence == 0 or clock_epoch == 0 or (flags & 4) == 0 or
            (flags & ~12) != 0 or
            (source == 1 and
             (active_plan == 0 or world == 0 or scene_version == 0)) or
            (source == 2 and
             (active_plan != 0 or world != 0 or scene_version != 0))):
            raise NativeBridgeError("CMFB_PAYLOAD")
        name = SOURCE_NAMES[source]
        return name, {
            "source": name,
            "pixel_format": PIXEL_FORMAT_NAMES[pixel_format],
            "codec": "raw",
            "flags": flags,
            "width": width,
            "height": height,
            "payload_bytes": payload_bytes,
            "frame_sequence": sequence,
            "source_clock_epoch": clock_epoch,
            "source_monotonic_ns": monotonic_ns,
            "active_plan_generation": active_plan,
            "world_generation": world,
            "applied_scene_state_version": scene_version,
        }

    def _frame_loop(self) -> None:
        frame_socket = self._frame_socket
        if frame_socket is None:
            return
        buffer = bytearray()
        try:
            while not self._stopping and not self._terminal_emitted:
                chunk = frame_socket.recv(65536)
                if not chunk:
                    break
                buffer.extend(chunk)
                if len(buffer) > MAX_CMFB_RECORD_BYTES * 2:
                    raise NativeBridgeError("CMFB_SIZE")
                while len(buffer) >= CMFB_HEADER_BYTES:
                    if buffer[:4] != b"CMFB":
                        raise NativeBridgeError("CMFB_HEADER")
                    payload_bytes = struct.unpack_from(">I", buffer, 20)[0]
                    if payload_bytes > MAX_CAMERA_FRAME_BYTES:
                        raise NativeBridgeError("CMFB_SIZE")
                    record_size = CMFB_HEADER_BYTES + payload_bytes
                    if len(buffer) < record_size:
                        break
                    record = bytes(buffer[:record_size])
                    del buffer[:record_size]
                    source, metadata = self._parse_frame(record)
                    with self._condition:
                        if self._stopping or self._terminal_emitted:
                            return
                        self._frames[source] = (record, metadata)
                    self._on_frame(source, copy.deepcopy(metadata))
            if buffer and not self._stopping:
                raise NativeBridgeError("CMFB_PARTIAL")
            if not self._stopping:
                self._emit_terminal("NATIVE_FRAME_EOF")
        except (OSError, NativeBridgeError):
            if not self._stopping:
                self._emit_terminal("NATIVE_FRAME")

    def _wait_loop(self) -> None:
        process = self._process
        if process is None:
            return
        return_code = process.wait()
        if not self._stopping:
            self._emit_terminal(
                "NATIVE_EXIT_OK" if return_code == 0 else "NATIVE_EXIT_FAILURE"
            )
