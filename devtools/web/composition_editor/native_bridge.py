#!/usr/bin/env python3
"""Bounded local bridge to the native CompositionHostBridge process.

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
import time
from typing import Any, Callable


MAX_NATIVE_LINE_BYTES = 16 * 1024
MAX_NATIVE_QUEUE_LINES = 64
MAX_NATIVE_REQUESTS = 64
CMFB_HEADER_BYTES = 80
MAX_RENDER_FRAME_BYTES = 320 * 240 * 4
MAX_CAMERA_FRAME_BYTES = 640 * 480 * 3 // 2
MAX_CMFB_RECORD_BYTES = CMFB_HEADER_BYTES + MAX_CAMERA_FRAME_BYTES
NATIVE_DRAIN_TIMEOUT_SECONDS = 2.0
NATIVE_EXIT_ARBITRATION_GRACE_SECONDS = 0.25
NATIVE_CLOSE_JOIN_SECONDS = 3.0
NATIVE_PROCESS_WAIT_POLL_SECONDS = 0.05
NATIVE_TERMINAL_EXIT_TIMEOUT_SECONDS = 1.0
NATIVE_TERMINAL_KILL_TIMEOUT_SECONDS = 1.0

# Fixed COMPOSE_/_PROFILE:v1 editor envelope. A connected catalog may expose
# a smaller dynamic subset; the native Host Runtime remains final authority.
COMPOSITION_MAX_RESULT_HZ = 10
CAMERA_MAX_FPS = 30
TELEMETRY_MIN_PERIOD_MS = 100
TELEMETRY_MAX_PERIOD_MS = 10000
TELEMETRY_PERIOD_STEP_MS = 100
WIFI_FIELD_MASK_ALL = 0x03FF
DEVICE_FIELD_MASK_ALL = 0x000F

SOURCE_NAMES = {1: "render", 2: "camera"}
PIXEL_FORMAT_NAMES = {1: "rgba8888", 2: "rgb565", 3: "nv12", 4: "yuyv"}


class NativeBridgeError(RuntimeError):
    """The explicit native bridge cannot accept or continue an operation."""


def _profile_boolean(value: Any) -> bool:
    if not isinstance(value, bool):
        raise NativeBridgeError("INVALID_PROFILE_INTENT")
    return value


def _profile_uint(
    value: Any, minimum: int, maximum: int, *, step: int = 1
) -> int:
    if (isinstance(value, bool) or not isinstance(value, int) or
            value < minimum or value > maximum or
            (value - minimum) % step != 0):
        raise NativeBridgeError("INVALID_PROFILE_INTENT")
    return value


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


def _enabled_periodic(
    command: str, intent: dict[str, Any], field_mask_all: int
) -> dict[str, Any]:
    enabled = _profile_boolean(intent.get("enabled"))
    selected_fields = _profile_uint(
        intent.get("selected_fields"), 1, field_mask_all
    )
    sample_period_ms = _profile_uint(
        intent.get("sample_period_ms"),
        TELEMETRY_MIN_PERIOD_MS,
        TELEMETRY_MAX_PERIOD_MS,
        step=TELEMETRY_PERIOD_STEP_MS,
    )
    result: dict[str, Any] = {
        "command": command,
        "enabled": enabled,
    }
    if enabled:
        result["selected_fields"] = selected_fields
        result["sample_period_ms"] = sample_period_ms
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
        enabled = _profile_boolean(intent.get("enabled"))
        max_hz = _profile_uint(
            intent.get("max_hz"), 1, COMPOSITION_MAX_RESULT_HZ
        )
        presentation: dict[str, Any] = {
            "command": "set_presentation", "enabled": enabled
        }
        if enabled:
            presentation.update({
                "use_safety_frame": False,
                "target_width": 320,
                "target_height": 240,
                "color_format": "rgba8888",
                "depth_format": "implementation32",
                "max_result_hz": max_hz,
                "offline_lease_ms": 1000,
            })
        return [
            presentation,
            _media_command(
                "set_render_media", enabled, "rgba8888",
                320, 240, max_hz
            ),
        ]
    if command_name == "intent.camera.set":
        intent = snapshot["intents"]["camera"]
        enabled = _profile_boolean(intent.get("enabled"))
        fps = _profile_uint(intent.get("fps"), 1, CAMERA_MAX_FPS)
        return [_media_command(
            "set_camera_media", enabled, "nv12", 320, 240, fps
        )]
    if command_name == "intent.telemetry.set":
        return [_enabled_periodic(
            "set_wifi", snapshot["intents"]["telemetry"],
            WIFI_FIELD_MASK_ALL,
        )]
    if command_name == "intent.device.set":
        return [_enabled_periodic(
            "set_device", snapshot["intents"]["device"],
            DEVICE_FIELD_MASK_ALL,
        )]
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
    ("service.ready", "profile"): {"composition"},
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
        "HOST_RUNTIME_TRANSPORT_TERMINAL", "HOST_RUNTIME_SESSION_TERMINAL",
        "HOST_RUNTIME_CATALOG_TERMINAL", "HOST_RUNTIME_LOCAL_TERMINAL",
        "HOST_RUNTIME_INTERNAL_TERMINAL",
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


def _request_completion_matches(
    request: dict[str, Any], event: dict[str, Any]
) -> bool:
    """Bind each successful completion family to the submitted command."""
    if event["type"] == "error":
        return True
    command = request["native_command"]
    if command == "asset.load_builtin":
        return (
            event["type"] == "asset.loaded" or
            (event["type"] == "command.receipt" and
             event["kind"] == "put_mesh")
        )
    if command == "snapshot":
        return event["type"] == "snapshot"
    if command == "close":
        return event["type"] == "closing"
    return (
        event["type"] == "command.receipt" and event["kind"] == command
    )


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
        self._dispatch_lock = threading.RLock()
        self._stdin_lock = threading.Lock()
        self._outbound: deque[bytes] = deque()
        self._next_request_id = 1
        self._requests: dict[int, dict[str, Any]] = {}
        self._frames: dict[str, tuple[bytes, dict[str, Any]]] = {}
        self._process: subprocess.Popen[bytes] | None = None
        self._frame_socket: socket.socket | None = None
        self._stdin_fd: int | None = None
        self._threads: list[threading.Thread] = []
        self._lifecycle_state = "NEW"
        self._stopping = False
        self._terminal_emitted = False
        self._terminal_code: str | None = None
        self._owner_close_requested = False
        self._stdin_open = False
        self._stdin_close_origin = "NOT_OPENED"
        self._stdin_close_attempt_origin = "NONE"
        self._stdin_close_result = "NOT_ATTEMPTED"
        self._stdin_close_errno: int | None = None
        self._stdin_open_at_process_exit: bool | None = None
        self._process_return_code: int | None = None
        self._shutdown_failure: str | None = None
        self._drain_timeout_pending = False
        self._stdout_drain_result = "PENDING"
        self._stdout_drained = threading.Event()
        self._stderr_drained = threading.Event()
        self._frame_drained = threading.Event()
        self._process_exited = threading.Event()
        self._terminal_shutdown_complete = threading.Event()

    def start(self) -> None:
        with self._condition:
            if self._lifecycle_state != "NEW":
                raise NativeBridgeError("NATIVE_ALREADY_BOUND")
            self._lifecycle_state = "STARTING"
            self._condition.notify_all()
        if (not self.executable.is_file() or
            not os.access(self.executable, os.X_OK) or
            (self.simulator and self.config is not None) or
            (not self.simulator and
             (self.config is None or not self.config.is_file()))):
            self._finish_failed_start()
            raise NativeBridgeError("NATIVE_PATH")

        parent_frame: socket.socket | None = None
        child_frame: socket.socket | None = None
        stdin_read_fd: int | None = None
        stdin_write_fd: int | None = None
        process: subprocess.Popen[bytes] | None = None
        try:
            parent_frame, child_frame = socket.socketpair(
                socket.AF_UNIX, socket.SOCK_STREAM
            )
            child_frame.setblocking(False)
            stdin_read_fd, stdin_write_fd = os.pipe()
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
                stdin=stdin_read_fd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                close_fds=True,
                pass_fds=(child_frame.fileno(),),
                bufsize=0,
            )
            os.close(stdin_read_fd)
            stdin_read_fd = None
            child_frame.close()
            child_frame = None
            if process.stdout is None or process.stderr is None:
                raise NativeBridgeError("NATIVE_PIPES")
            os.set_blocking(stdin_write_fd, False)
        except BaseException as error:
            self._close_fd(stdin_read_fd)
            if process is not None and stdin_write_fd is not None:
                with self._stdin_lock:
                    self._stdin_fd = stdin_write_fd
                with self._condition:
                    self._stdin_open = True
                    self._stdin_close_origin = "OPEN"
                self._close_stdin_writer("START_ABORT")
                stdin_write_fd = None
            else:
                self._close_fd(stdin_write_fd)
            if child_frame is not None:
                try:
                    child_frame.close()
                except OSError:
                    pass
            if parent_frame is not None:
                try:
                    parent_frame.close()
                except OSError:
                    pass
            cleanup_failed = bool(
                process is not None and not self._stop_process(process)
            )
            if process is not None:
                self._close_process_streams(process)
            self._finish_failed_start(
                process if cleanup_failed else None
            )
            if cleanup_failed:
                raise NativeBridgeError(
                    "NATIVE_PROCESS_SHUTDOWN"
                ) from error
            if isinstance(error, NativeBridgeError):
                raise
            if isinstance(error, Exception):
                raise NativeBridgeError("NATIVE_START") from error
            raise

        assert parent_frame is not None
        assert process is not None
        assert stdin_write_fd is not None
        with self._stdin_lock:
            self._stdin_fd = stdin_write_fd
        with self._condition:
            self._process = process
            self._frame_socket = parent_frame
            self._stdin_open = True
            self._stdin_close_origin = "OPEN"
        targets = (
            self._writer_loop, self._stdout_loop, self._stderr_loop,
            self._frame_loop, self._wait_loop,
        )
        try:
            threads = [
                threading.Thread(
                    target=target, daemon=True, name=target.__name__
                )
                for target in targets
            ]
        except BaseException as error:
            with self._condition:
                self._stopping = True
                self._lifecycle_state = "STOPPING"
                self._condition.notify_all()
            cleanup_succeeded = self._cleanup_start_abort(process, [])
            if not cleanup_succeeded:
                raise NativeBridgeError(
                    "NATIVE_PROCESS_SHUTDOWN"
                ) from error
            if isinstance(error, Exception):
                raise NativeBridgeError("NATIVE_START") from error
            raise
        start_error: BaseException | None = None
        abort_start = False
        with self._condition:
            self._threads = threads
            if self._stopping:
                self._lifecycle_state = "STOPPING"
                abort_start = True
            else:
                try:
                    for thread in threads:
                        thread.start()
                except BaseException as error:
                    start_error = error
                    self._stopping = True
                    self._lifecycle_state = "STOPPING"
                else:
                    self._lifecycle_state = "RUNNING"
            self._condition.notify_all()

        if abort_start or start_error is not None:
            cleanup_succeeded = self._cleanup_start_abort(process, threads)
            if not cleanup_succeeded:
                raise NativeBridgeError(
                    "NATIVE_PROCESS_SHUTDOWN"
                ) from start_error
            if start_error is not None:
                if isinstance(start_error, Exception):
                    raise NativeBridgeError("NATIVE_START") from start_error
                raise start_error

    @staticmethod
    def _close_fd(fd: int | None) -> None:
        if fd is None:
            return
        try:
            os.close(fd)
        except OSError:
            pass

    @staticmethod
    def _close_process_streams(process: subprocess.Popen[bytes]) -> None:
        for stream in (process.stdout, process.stderr):
            if stream is not None and not stream.closed:
                try:
                    stream.close()
                except OSError:
                    pass

    @staticmethod
    def _stop_process(process: subprocess.Popen[bytes]) -> bool:
        if process.poll() is not None:
            return True
        try:
            process.wait(timeout=0.5)
            return True
        except subprocess.TimeoutExpired:
            pass
        try:
            process.terminate()
        except OSError:
            pass
        try:
            process.wait(timeout=1.0)
            return True
        except subprocess.TimeoutExpired:
            pass
        try:
            process.kill()
        except OSError:
            pass
        try:
            process.wait(timeout=1.0)
        except subprocess.TimeoutExpired:
            return False
        return process.poll() is not None

    def _finish_failed_start(
        self, unreaped_process: subprocess.Popen[bytes] | None = None
    ) -> None:
        with self._condition:
            self._stopping = True
            if unreaped_process is None:
                self._lifecycle_state = "CLOSED"
            else:
                self._process = unreaped_process
                self._shutdown_failure = "NATIVE_PROCESS_SHUTDOWN"
                self._lifecycle_state = "FAILED"
            self._condition.notify_all()

    def _cleanup_start_abort(
        self,
        process: subprocess.Popen[bytes],
        threads: list[threading.Thread],
    ) -> bool:
        self._close_stdin_writer("BRIDGE_CLOSE")
        stopped = self._stop_process(process)
        if not stopped:
            with self._condition:
                self._shutdown_failure = "NATIVE_PROCESS_SHUTDOWN"
        self._record_process_exit(process)
        self._close_frame_socket()
        self._close_process_streams(process)
        workers_stopped = self._join_workers(threads)
        with self._condition:
            self._lifecycle_state = (
                "CLOSED" if stopped and workers_stopped else "FAILED"
            )
            self._condition.notify_all()
        return stopped and workers_stopped

    def _record_process_exit(self, process: subprocess.Popen[bytes]) -> None:
        return_code = process.poll()
        if return_code is None:
            return
        with self._condition:
            if self._process_return_code is None:
                self._process_return_code = return_code
                self._stdin_open_at_process_exit = self._stdin_open
                self._condition.notify_all()
        self._process_exited.set()

    def _worker_may_run(self) -> bool:
        with self._condition:
            while (self._lifecycle_state == "STARTING" and
                   not self._stopping):
                self._condition.wait()
            return (
                self._lifecycle_state == "RUNNING" and
                not self._stopping
            )

    def _lifecycle_snapshot(self) -> dict[str, Any]:
        """Return fixed local process ownership evidence for tests/audits."""
        with self._condition:
            return {
                "state": self._lifecycle_state,
                "owner_close_requested": self._owner_close_requested,
                "stdin_close_origin": self._stdin_close_origin,
                "stdin_close_attempt_origin": (
                    self._stdin_close_attempt_origin
                ),
                "stdin_close_result": self._stdin_close_result,
                "stdin_close_errno": self._stdin_close_errno,
                "stdin_open": self._stdin_open,
                "stdin_open_at_process_exit": (
                    self._stdin_open_at_process_exit
                ),
                "process_return_code": self._process_return_code,
                "stdout_drain_result": self._stdout_drain_result,
                "stdout_drained": self._stdout_drained.is_set(),
                "stderr_drained": self._stderr_drained.is_set(),
                "frame_drained": self._frame_drained.is_set(),
                "terminal_code": self._terminal_code,
                "shutdown_failure": self._shutdown_failure,
            }

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
            if (self._lifecycle_state != "RUNNING" or self._stopping or
                not self._stdin_open or self._process is None or
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
            batch_count = len(commands)
            batch_id = candidate_ids[0]
            for batch_index, (request_id, encoded, command) in enumerate(
                zip(candidate_ids, candidate_lines, commands), start=1
            ):
                self._outbound.append(encoded)
                self._requests[request_id] = {
                    "origin": origin,
                    "revision": revision,
                    "native_command": command["command"],
                    "batch_id": batch_id,
                    "batch_index": batch_index,
                    "batch_count": batch_count,
                }
            self._condition.notify_all()
            return candidate_ids

    def latest_frame(self, source: str) -> tuple[bytes, dict[str, Any]] | None:
        if source not in {"render", "camera"}:
            raise NativeBridgeError("INVALID_FRAME_SOURCE")
        with self._condition:
            if (self._lifecycle_state != "RUNNING" or self._stopping or
                    self._terminal_emitted):
                return None
            value = self._frames.get(source)
            if value is None:
                return None
            return bytes(value[0]), copy.deepcopy(value[1])

    def close(self) -> None:
        owns_shutdown = False
        wait_for_start = False
        terminal_shutdown = False
        with self._dispatch_lock:
            with self._condition:
                state = self._lifecycle_state
                if state == "CLOSED":
                    return
                if state == "FAILED":
                    self._owner_close_requested = True
                    self._condition.notify_all()
                    process = self._process
                    if process is not None and process.poll() is None:
                        raise NativeBridgeError(
                            self._shutdown_failure or
                            "NATIVE_PROCESS_SHUTDOWN"
                        )
                    self._lifecycle_state = "STOPPING"
                    terminal_shutdown = False
                    owns_shutdown = True
                if state == "NEW":
                    self._stopping = True
                    self._owner_close_requested = True
                    self._stdin_close_attempt_origin = "BRIDGE_CLOSE"
                    self._stdin_close_result = "NOT_OPENED"
                    self._lifecycle_state = "CLOSED"
                    self._condition.notify_all()
                    return
                if state == "STARTING":
                    self._stopping = True
                    self._owner_close_requested = True
                    self._condition.notify_all()
                    wait_for_start = True
                elif state == "RUNNING":
                    terminal_shutdown = self._terminal_emitted
                    self._stopping = True
                    self._owner_close_requested = True
                    self._lifecycle_state = "STOPPING"
                    self._outbound.clear()
                    self._requests.clear()
                    self._frames.clear()
                    self._condition.notify_all()
                    owns_shutdown = True

        if wait_for_start:
            with self._condition:
                while self._lifecycle_state not in {"CLOSED", "FAILED"}:
                    self._condition.wait()
                if self._lifecycle_state == "FAILED":
                    raise NativeBridgeError(
                        self._shutdown_failure or "NATIVE_PROCESS_SHUTDOWN"
                    )
            return
        if not owns_shutdown:
            with self._condition:
                while self._lifecycle_state not in {"CLOSED", "FAILED"}:
                    self._condition.wait()
                if self._lifecycle_state == "FAILED":
                    raise NativeBridgeError(
                        self._shutdown_failure or "NATIVE_PROCESS_SHUTDOWN"
                    )
            return

        self._close_stdin_writer("BRIDGE_CLOSE")
        process = self._process
        if process is not None:
            if terminal_shutdown:
                terminal_wait = (
                    NATIVE_TERMINAL_EXIT_TIMEOUT_SECONDS +
                    NATIVE_TERMINAL_KILL_TIMEOUT_SECONDS +
                    NATIVE_DRAIN_TIMEOUT_SECONDS +
                    NATIVE_CLOSE_JOIN_SECONDS +
                    NATIVE_PROCESS_WAIT_POLL_SECONDS * 4.0
                )
                if not self._terminal_shutdown_complete.wait(terminal_wait):
                    with self._condition:
                        if self._process_exited.is_set():
                            self._shutdown_failure = "NATIVE_OUTPUT_DRAIN"
                            if not self._stdout_drained.is_set():
                                self._stdout_drain_result = "FORCED_TIMEOUT"
                        else:
                            self._shutdown_failure = "NATIVE_PROCESS_SHUTDOWN"
            elif not self._stop_process(process):
                with self._condition:
                    self._shutdown_failure = "NATIVE_PROCESS_SHUTDOWN"
            self._record_process_exit(process)
        self._close_frame_socket()
        process_alive = process is not None and process.poll() is None
        if process_alive:
            with self._condition:
                self._lifecycle_state = "FAILED"
                self._shutdown_failure = "NATIVE_PROCESS_SHUTDOWN"
                self._condition.notify_all()
            raise NativeBridgeError("NATIVE_PROCESS_SHUTDOWN")
        if process is not None:
            self._close_process_streams(process)
        workers_stopped = self._join_workers(self._threads)
        with self._condition:
            self._lifecycle_state = "CLOSED" if workers_stopped else "FAILED"
            self._condition.notify_all()
        if not workers_stopped:
            raise NativeBridgeError("NATIVE_THREAD_SHUTDOWN")

    def _close_frame_socket(self) -> None:
        with self._condition:
            frame_socket = self._frame_socket
            self._frame_socket = None
        if frame_socket is None:
            return
        try:
            frame_socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            frame_socket.close()
        except OSError:
            pass

    def _join_workers(self, threads: list[threading.Thread]) -> bool:
        current = threading.current_thread()
        deadline = time.monotonic() + NATIVE_CLOSE_JOIN_SECONDS
        current_worker = False
        for thread in threads:
            if thread is current:
                current_worker = True
                continue
            if thread.ident is None:
                continue
            remaining = deadline - time.monotonic()
            if remaining > 0.0:
                thread.join(remaining)
        alive = [
            thread for thread in threads
            if thread is not current and thread.is_alive()
        ]
        with self._condition:
            if alive:
                self._shutdown_failure = "NATIVE_THREAD_SHUTDOWN"
            elif current_worker:
                self._shutdown_failure = "NATIVE_CURRENT_WORKER_CLOSE"
        return not alive and not current_worker

    def _close_stdin_writer(self, origin: str) -> bool:
        with self._condition:
            if not self._stdin_open:
                return False
            self._stdin_open = False
            self._stdin_close_attempt_origin = origin
            self._stdin_close_result = "PENDING"
            self._stdin_close_errno = None
            self._condition.notify_all()
        with self._stdin_lock:
            fd = self._stdin_fd
            self._stdin_fd = None
            if fd is None:
                closed = False
                close_errno = None
            else:
                try:
                    os.close(fd)
                except OSError as error:
                    closed = False
                    close_errno = error.errno
                else:
                    closed = True
                    close_errno = None
        with self._condition:
            self._stdin_close_result = "CLOSED" if closed else "FAILED"
            self._stdin_close_errno = close_errno
            self._stdin_close_origin = origin if closed else "UNPROVEN"
            self._condition.notify_all()
        return closed

    def _request_owner_eof(self) -> None:
        """Close only the owned stdin writer; private lifecycle test seam."""
        with self._condition:
            if (self._lifecycle_state != "RUNNING" or self._stopping or
                    self._terminal_emitted or not self._stdin_open or
                    self._outbound or self._requests):
                raise NativeBridgeError("NATIVE_OWNER_EOF_UNSAFE")
        if not self._close_stdin_writer("OWNER_EOF"):
            raise NativeBridgeError("NATIVE_OWNER_EOF_FAILED")

    def _publish_event(
        self, event: dict[str, Any], *, correlate: bool = False
    ) -> bool:
        with self._dispatch_lock:
            with self._condition:
                if (self._stopping or self._terminal_emitted or
                        self._drain_timeout_pending):
                    return False
                if correlate:
                    request_id = event.get("request_id")
                    if not isinstance(request_id, int) or request_id == 0:
                        raise NativeBridgeError("NATIVE_REQUEST_CORRELATION")
                    request = self._requests.pop(request_id, None)
                    if (request is None or
                            not _request_completion_matches(request, event)):
                        raise NativeBridgeError("NATIVE_REQUEST_CORRELATION")
                    event.update({
                        "browser_command": request["origin"],
                        "browser_revision": request["revision"],
                        "native_command": request["native_command"],
                        "native_batch_id": request["batch_id"],
                        "native_batch_index": request["batch_index"],
                        "native_batch_count": request["batch_count"],
                    })
            self._on_event(event)
            return True

    def _publish_frame(
        self, source: str, record: bytes, metadata: dict[str, Any]
    ) -> bool:
        with self._dispatch_lock:
            with self._condition:
                if (self._stopping or self._terminal_emitted or
                        self._drain_timeout_pending):
                    return False
                self._frames[source] = (record, metadata)
            self._on_frame(source, copy.deepcopy(metadata))
            return True

    def _emit_terminal(
        self, code: str, *, leading_event: dict[str, Any] | None = None
    ) -> bool:
        claimed = False
        try:
            with self._dispatch_lock:
                with self._condition:
                    if (self._stopping or self._terminal_emitted or
                            (self._drain_timeout_pending and
                             code != "NATIVE_OUTPUT_DRAIN")):
                        return False
                    self._terminal_emitted = True
                    self._terminal_code = code
                    self._drain_timeout_pending = False
                    self._outbound.clear()
                    self._requests.clear()
                    self._frames.clear()
                    self._condition.notify_all()
                    claimed = True
                if leading_event is not None:
                    try:
                        self._on_event(leading_event)
                    finally:
                        self._on_event({
                            "type": "native.bridge_terminal", "code": code
                        })
                else:
                    self._on_event({
                        "type": "native.bridge_terminal", "code": code
                    })
        finally:
            if claimed:
                process = self._process
                self._close_stdin_writer("TERMINAL")
                self._close_frame_socket()
                if process is not None and process.poll() is None:
                    try:
                        process.terminate()
                    except OSError:
                        pass
        return True

    def _writer_loop(self) -> None:
        if not self._worker_may_run():
            return
        current = b""
        offset = 0
        while True:
            with self._condition:
                while (not self._stopping and not self._terminal_emitted and
                       self._stdin_open and not current and
                       not self._outbound):
                    self._condition.wait(0.5)
                if (self._stopping or self._terminal_emitted or
                        not self._stdin_open):
                    return
                if not current:
                    current = self._outbound.popleft()
                    offset = 0
            try:
                with self._stdin_lock:
                    fd = self._stdin_fd
                    if fd is None:
                        return
                    _, writable, _ = select.select([], [fd], [], 0.1)
                    if not writable:
                        continue
                    written = os.write(fd, current[offset:])
                if written <= 0:
                    raise OSError("short native write")
                offset += written
                if offset == len(current):
                    current = b""
            except (OSError, ValueError):
                if not self._stopping:
                    self._emit_terminal("NATIVE_STDIN")
                return

    def _finish_stdout_drain(self, result: str) -> None:
        with self._condition:
            if self._stdout_drain_result != "FORCED_TIMEOUT":
                self._stdout_drain_result = result
            self._stdout_drained.set()
            self._condition.notify_all()

    def _stdout_loop(self) -> None:
        if not self._worker_may_run():
            self._finish_stdout_drain("NOT_STARTED")
            return
        process = self._process
        if process is None or process.stdout is None:
            self._finish_stdout_drain("ERROR")
            return
        buffer = bytearray()
        clean_eof = False
        discard_to_eof = False
        try:
            while True:
                chunk = process.stdout.read(4096)
                if not chunk:
                    clean_eof = True
                    break
                if discard_to_eof:
                    continue
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
                    correlate = event["type"] in {
                        "command.receipt", "asset.loaded", "error",
                        "snapshot", "closing",
                    }
                    if event["type"] == "terminal":
                        self._emit_terminal(
                            "NATIVE_HOST_TERMINAL", leading_event=event
                        )
                        discard_to_eof = True
                        buffer.clear()
                        break
                    if not self._publish_event(event, correlate=correlate):
                        discard_to_eof = True
                        buffer.clear()
                        break
            if buffer:
                raise NativeBridgeError("NATIVE_OUTPUT_PARTIAL")
            if clean_eof and not discard_to_eof and not self._stopping:
                if (not self._process_exited.wait(
                        NATIVE_EXIT_ARBITRATION_GRACE_SECONDS)):
                    with self._condition:
                        terminal = self._terminal_emitted
                    if not terminal and process.poll() is None:
                        self._emit_terminal("NATIVE_STDOUT_EOF")
        except NativeBridgeError as error:
            if not self._stopping:
                self._emit_terminal(
                    "NATIVE_REQUEST_CORRELATION" if
                    str(error) == "NATIVE_REQUEST_CORRELATION" else
                    "NATIVE_STDOUT"
                )
        except (UnicodeDecodeError, json.JSONDecodeError):
            if not self._stopping:
                self._emit_terminal("NATIVE_STDOUT")
        except (OSError, ValueError):
            if not self._stopping:
                self._emit_terminal("NATIVE_STDOUT")
        finally:
            self._finish_stdout_drain("EOF" if clean_eof else "ERROR")

    def _stderr_loop(self) -> None:
        if not self._worker_may_run():
            self._stderr_drained.set()
            return
        process = self._process
        if process is None or process.stderr is None:
            self._stderr_drained.set()
            return
        buffer = bytearray()
        try:
            while True:
                chunk = process.stderr.read(256)
                if not chunk:
                    break
                buffer.extend(chunk)
                while b"\n" in buffer:
                    raw, _, remainder = buffer.partition(b"\n")
                    buffer[:] = remainder
                    text = raw.decode("ascii", "ignore")
                    code = (
                        text if re.fullmatch(r"E_[A-Z_]{1,48}", text)
                        else "E_NATIVE"
                    )
                    self._emit_terminal(
                        "NATIVE_STDERR",
                        leading_event={
                            "type": "native.process_error", "code": code
                        },
                    )
                    return
                if len(buffer) > 256:
                    self._emit_terminal(
                        "NATIVE_STDERR",
                        leading_event={
                            "type": "native.process_error",
                            "code": "E_NATIVE",
                        },
                    )
                    return
            if buffer and not self._stopping:
                self._emit_terminal(
                    "NATIVE_STDERR",
                    leading_event={
                        "type": "native.process_error", "code": "E_NATIVE"
                    },
                )
        except (OSError, ValueError):
            if not self._stopping:
                self._emit_terminal("NATIVE_STDERR")
        finally:
            self._stderr_drained.set()

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
        if not self._worker_may_run():
            self._frame_drained.set()
            return
        frame_socket = self._frame_socket
        if frame_socket is None:
            self._frame_drained.set()
            return
        buffer = bytearray()
        clean_eof = False
        try:
            while not self._stopping and not self._terminal_emitted:
                chunk = frame_socket.recv(65536)
                if not chunk:
                    clean_eof = True
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
                    if not self._publish_frame(source, record, metadata):
                        return
            if buffer and not self._stopping:
                raise NativeBridgeError("CMFB_PARTIAL")
            if clean_eof and not self._stopping:
                if (not self._process_exited.wait(
                        NATIVE_EXIT_ARBITRATION_GRACE_SECONDS)):
                    process = self._process
                    with self._condition:
                        terminal = self._terminal_emitted
                    if (not terminal and process is not None and
                            process.poll() is None):
                        self._emit_terminal("NATIVE_FRAME_EOF")
        except NativeBridgeError:
            if not self._stopping:
                self._emit_terminal("NATIVE_FRAME")
        except OSError:
            if not self._stopping:
                with self._condition:
                    terminal = self._terminal_emitted
                if not terminal:
                    self._emit_terminal("NATIVE_FRAME")
        finally:
            self._frame_drained.set()

    def _wait_for_process_exit(
        self, process: subprocess.Popen[bytes]
    ) -> int | None:
        terminal_deadline: float | None = None
        kill_deadline: float | None = None
        kill_sent = False
        while True:
            try:
                return process.wait(timeout=NATIVE_PROCESS_WAIT_POLL_SECONDS)
            except subprocess.TimeoutExpired:
                pass
            with self._condition:
                terminal = self._terminal_emitted
            if not terminal:
                continue
            now = time.monotonic()
            if terminal_deadline is None:
                terminal_deadline = now + NATIVE_TERMINAL_EXIT_TIMEOUT_SECONDS
                continue
            if not kill_sent and now >= terminal_deadline:
                try:
                    process.kill()
                except OSError:
                    pass
                kill_sent = True
                kill_deadline = now + NATIVE_TERMINAL_KILL_TIMEOUT_SECONDS
                continue
            if kill_sent and kill_deadline is not None and now >= kill_deadline:
                with self._condition:
                    self._shutdown_failure = "NATIVE_PROCESS_SHUTDOWN"
                    self._condition.notify_all()
                return None

    def _wait_for_reader_drains(self, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        for drained in (
            self._stdout_drained, self._stderr_drained, self._frame_drained,
        ):
            remaining = deadline - time.monotonic()
            if remaining <= 0.0 or not drained.wait(remaining):
                return False
        return True

    def _wait_loop(self) -> None:
        if not self._worker_may_run():
            return
        process = self._process
        if process is None:
            return
        try:
            return_code = self._wait_for_process_exit(process)
            if return_code is None:
                # The child still owns its output endpoints.  Do not force
                # readers closed or report a completed drain: retain the
                # process identity in FAILED so the owner cannot publish a
                # replacement until an external recovery actually reaps it.
                return
            with self._condition:
                self._process_return_code = return_code
                self._stdin_open_at_process_exit = self._stdin_open
                self._condition.notify_all()
            self._process_exited.set()

            complete = self._wait_for_reader_drains(
                NATIVE_DRAIN_TIMEOUT_SECONDS
            )
            if not complete:
                with self._condition:
                    terminal = self._terminal_emitted
                    if terminal:
                        self._shutdown_failure = "NATIVE_OUTPUT_DRAIN"
                    elif not self._stopping:
                        self._drain_timeout_pending = True
                    if not self._stdout_drained.is_set():
                        self._stdout_drain_result = "FORCED_TIMEOUT"
                    self._condition.notify_all()
                self._emit_terminal("NATIVE_OUTPUT_DRAIN")
                self._close_frame_socket()
                self._close_process_streams(process)
                self._wait_for_reader_drains(NATIVE_CLOSE_JOIN_SECONDS)
                return
            with self._condition:
                stopping = self._stopping
                terminal = self._terminal_emitted
            if not stopping and not terminal:
                self._emit_terminal(
                    "NATIVE_EXIT_OK" if return_code == 0 else
                    "NATIVE_EXIT_FAILURE"
                )
        finally:
            self._terminal_shutdown_complete.set()
