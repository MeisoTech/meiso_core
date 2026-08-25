#!/usr/bin/env python3
"""Independent native-bridge and CMFB boundary tests."""

from __future__ import annotations

import copy
import hashlib
import http.client
import json
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

import server as server_module
from native_bridge import (
    MAX_CAMERA_FRAME_BYTES,
    NativeBridge,
    NativeBridgeError,
    bootstrap_commands,
    commands_for_mutation,
    euler_xyz_degrees_to_xyzw,
    full_scene_command,
    sanitize_native_event,
)
from server import DashboardState, _default_snapshot, create_server


FAKE_CHILD = r'''#!/usr/bin/env python3
import json
from pathlib import Path
import sys

simulator = "--simulator" in sys.argv
if simulator:
    mode = "simulator"
else:
    config_index = sys.argv.index("--config") + 1
    mode = Path(sys.argv[config_index]).read_text(encoding="utf-8").strip()
if mode == "invalid":
    print(json.dumps({
        "type": "service.ready", "profile": "composition-v3",
        "wire_owner": "native_service", "asset_import": "builtin_cube_only",
        "frame_projection": "CMFB/v1", "unexpected": 1,
    }, separators=(",", ":")), flush=True)
else:
    print(json.dumps({
        "type": "service.ready", "profile": "composition-v3",
        "wire_owner": (
            "simulator_service" if simulator or mode == "wrong_mode" else
            "native_service"
        ), "asset_import": "builtin_cube_only",
        "frame_projection": "CMFB/v1",
    }, separators=(",", ":")), flush=True)
    for raw in sys.stdin:
        value = json.loads(raw)
        request_id = value["request_id"]
        command = value["command"]
        if command == "snapshot":
            output = {
                "type": "snapshot", "request_id": request_id,
                "phase": "ready", "terminal_reason": "none",
                "control_ready": not simulator, "catalog_generation": 1,
                "media_bound": False, "media_mask": "none",
                "media_slots_consumed": 0, "mesh_active": False,
                "mesh_next_offset": 0, "wifi_actual": False,
                "presentation_actual": False, "scene_actual": False,
                "device_actual": False, "render_actual": False,
                "camera_actual": False, "wifi_sample": False,
                "render_result": False, "device_sample": False,
                "render_frame": False, "camera_frame": False,
            }
        else:
            output = {
                "type": "command.receipt", "request_id": request_id,
                "command_id": request_id, "kind": (
                    "put_mesh" if command == "asset.load_builtin" else command
                ), "local_result": "queued", "replaced": False,
                "replaced_command_id": 0, "state_version": 0,
                "plan_generation": 0,
            }
        print(json.dumps(output, separators=(",", ":")), flush=True)
'''


def _wait_for(predicate, timeout: float = 2.0) -> None:
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() >= deadline:
            raise AssertionError("timed out waiting for native bridge")
        time.sleep(0.005)


def _cmfb_record(source: int = 1) -> bytes:
    pixel_format = 1 if source == 1 else 3
    width, height = 4, 2
    payload_size = width * height * (4 if source == 1 else 3) // (
        1 if source == 1 else 2
    )
    header = bytearray(80)
    header[:4] = b"CMFB"
    header[8] = source
    struct.pack_into(">HH", header, 4, 1, 80)
    struct.pack_into(
        ">HHHHH", header, 10, pixel_format, 1, 4, width, height
    )
    struct.pack_into(">I", header, 20, payload_size)
    scene = (4, 5, 6) if source == 1 else (0, 0, 0)
    struct.pack_into(">QQQQQQ", header, 24, 1, 2, 3, *scene)
    return bytes(header) + bytes([0x5A]) * payload_size


class NativeBridgeTests(unittest.TestCase):
    def _fake_child(self, directory: Path, mode: str) -> tuple[Path, Path]:
        executable = directory / "fake_native_host.py"
        config = directory / "fake.conf"
        executable.write_text(FAKE_CHILD, encoding="utf-8")
        executable.chmod(0o700)
        config.write_text(mode, encoding="utf-8")
        return executable, config

    def test_fake_child_stdout_is_strict_and_correlated(self) -> None:
        events: list[dict[str, object]] = []
        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(Path(raw_directory), "valid")
            bridge = NativeBridge(
                str(executable), str(config), events.append, lambda *_: None
            )
            bridge.start()
            try:
                _wait_for(lambda: any(
                    event["type"] == "service.ready" for event in events
                ))
                request_ids = bridge.submit_batch(
                    [{"command": "snapshot"}],
                    origin="scene.camera.set", revision=7,
                )
                self.assertEqual(request_ids, [1])
                _wait_for(lambda: any(
                    event["type"] == "snapshot" for event in events
                ))
                ready = next(
                    event for event in events
                    if event["type"] == "service.ready"
                )
                self.assertNotIn("wire_owner", ready)
                snapshot = next(
                    event for event in events if event["type"] == "snapshot"
                )
                self.assertEqual(snapshot["browser_command"], "scene.camera.set")
                self.assertEqual(snapshot["browser_revision"], 7)
            finally:
                bridge.close()

    def test_simulator_child_needs_no_config_and_stays_non_session(self) -> None:
        events: list[dict[str, object]] = []
        with tempfile.TemporaryDirectory() as raw_directory:
            executable, _config = self._fake_child(
                Path(raw_directory), "unused"
            )
            bridge = NativeBridge(
                str(executable), None, events.append, lambda *_: None,
                simulator=True,
            )
            bridge.start()
            try:
                _wait_for(lambda: any(
                    event["type"] == "service.ready" for event in events
                ))
                bridge.submit_batch(
                    [{"command": "snapshot"}],
                    origin="native.simulator", revision=0,
                )
                _wait_for(lambda: any(
                    event["type"] == "snapshot" for event in events
                ))
                snapshot = next(
                    event for event in events if event["type"] == "snapshot"
                )
                self.assertFalse(snapshot["control_ready"])
                self.assertFalse(snapshot["media_bound"])
                self.assertEqual(snapshot["media_mask"], "none")
            finally:
                bridge.close()

        state = DashboardState()
        state._snapshot["backend"].update({
            "mode": "NATIVE_SIMULATOR", "native_host_bound": True,
        })
        state._native_event({
            "type": "snapshot", "request_id": 1, "phase": "ready",
            "terminal_reason": "none", "control_ready": False,
            "catalog_generation": 1, "media_bound": False,
            "media_mask": "none", "media_slots_consumed": 0,
            "mesh_active": False, "mesh_next_offset": 304,
            "wifi_actual": True, "presentation_actual": True,
            "scene_actual": True, "device_actual": True,
            "render_actual": True, "camera_actual": True,
            "wifi_sample": True, "render_result": True,
            "device_sample": True, "render_frame": False,
            "camera_frame": False,
        })
        for channel in ("render_media", "camera_media"):
            state._native_event({
                "type": "actual", "channel": channel,
                "state_version": 1, "apply_result": "applied",
                "availability": "present", "profile_decoded": True,
                "enabled": True, "production_status": "running",
                "reason": "none", "production_epoch": 1,
            })
        self.assertFalse(state.snapshot()["backend"]["device_connected"])
        self.assertEqual(
            state.snapshot()["backend"]["session_status"], "SIMULATED"
        )
        self.assertEqual(
            state.snapshot()["bindings"]["render_result_stream"],
            "SIMULATED",
        )
        self.assertEqual(
            state.snapshot()["bindings"]["camera_media_stream"],
            "SIMULATED",
        )

    @unittest.skipUnless(
        os.environ.get("MEISO_COMPOSITION_DEBUG_HOST"),
        "built CompositionDebugHost not supplied",
    )
    def test_real_simulator_child_completes_offline_editor_bootstrap(self) -> None:
        executable = os.environ["MEISO_COMPOSITION_DEBUG_HOST"]
        state = DashboardState()
        state.enable_native(executable, None, simulator=True)
        try:
            _wait_for(
                lambda: (
                    state.snapshot()["native_truth"]["render_result"]
                    is not None and
                    state.snapshot()["bindings"]["render_result_stream"] ==
                    "SIMULATED"
                ),
                timeout=5.0,
            )
            snapshot = state.snapshot()
            self.assertEqual(snapshot["backend"]["mode"], "NATIVE_SIMULATOR")
            self.assertEqual(snapshot["backend"]["session_status"], "SIMULATED")
            self.assertFalse(snapshot["backend"]["device_connected"])
            self.assertEqual(
                snapshot["backend"]["proof"],
                "SYNTHETIC_OFFLINE_TYPED_ONLY",
            )
            self.assertEqual(
                snapshot["native_truth"]["scene_status"]["reason"], "ready"
            )
            self.assertEqual(
                snapshot["bindings"]["render_result_stream"], "SIMULATED"
            )
            self.assertEqual(
                snapshot["bindings"]["camera_media_stream"], "UNBOUND"
            )
            self.assertFalse(snapshot["frames"]["render"]["available"])
            self.assertFalse(snapshot["frames"]["camera"]["available"])
        finally:
            state.close()

    def test_fake_child_malformed_stdout_is_sticky_terminal(self) -> None:
        events: list[dict[str, object]] = []
        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(Path(raw_directory), "invalid")
            bridge = NativeBridge(
                str(executable), str(config), events.append, lambda *_: None
            )
            bridge.start()
            try:
                _wait_for(lambda: any(
                    event["type"] == "native.bridge_terminal"
                    for event in events
                ))
                self.assertFalse(any(
                    event["type"] == "service.ready" for event in events
                ))
                with self.assertRaisesRegex(
                    NativeBridgeError, "NATIVE_TERMINAL"
                ):
                    bridge.submit_batch(
                        [{"command": "snapshot"}], origin="x", revision=1
                    )
            finally:
                bridge.close()

    def test_selected_native_mode_must_match_child(self) -> None:
        events: list[dict[str, object]] = []
        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(
                Path(raw_directory), "wrong_mode"
            )
            bridge = NativeBridge(
                str(executable), str(config), events.append, lambda *_: None
            )
            bridge.start()
            try:
                _wait_for(lambda: any(
                    event["type"] == "native.bridge_terminal"
                    for event in events
                ))
                self.assertFalse(any(
                    event["type"] == "service.ready" for event in events
                ))
            finally:
                bridge.close()

    def test_sanitizer_rejects_unknown_fields_types_and_enums(self) -> None:
        ready = {
            "type": "service.ready", "profile": "composition-v3",
            "wire_owner": "native_service",
            "asset_import": "builtin_cube_only",
            "frame_projection": "CMFB/v1",
        }
        self.assertNotIn("wire_owner", sanitize_native_event(ready))
        for invalid in (
            {**ready, "extra": 1},
            {**ready, "profile": "composition-v4"},
            {
                "type": "actual", "channel": "wifi",
                "state_version": "1", "apply_result": "applied",
                "availability": "present", "profile_decoded": True,
                "enabled": True, "production_status": "running",
                "reason": "none", "production_epoch": 1,
            },
        ):
            with self.subTest(invalid=invalid):
                with self.assertRaisesRegex(
                    NativeBridgeError, "INVALID_NATIVE_EVENT"
                ):
                    sanitize_native_event(invalid)

    def test_batch_admission_is_bounded_and_candidate_first(self) -> None:
        class Alive:
            terminated = False

            @staticmethod
            def poll() -> None:
                return None

            def terminate(self) -> None:
                self.terminated = True

        process = Alive()
        bridge = NativeBridge(
            "/bin/false", "/dev/null", lambda _: None, lambda *_: None
        )
        bridge._process = process
        before = (
            bridge._next_request_id,
            list(bridge._outbound),
            dict(bridge._requests),
        )
        with self.assertRaisesRegex(
            NativeBridgeError, "INVALID_NATIVE_COMMAND"
        ):
            bridge.submit_batch(
                [{"command": "snapshot"}, {"invalid": True}],
                origin="x", revision=1,
            )
        self.assertEqual(before, (
            bridge._next_request_id,
            list(bridge._outbound),
            dict(bridge._requests),
        ))
        for revision in range(64):
            self.assertEqual(bridge.submit_batch(
                [{"command": "snapshot"}], origin="x", revision=revision
            ), [revision + 1])
        saturated = (
            bridge._next_request_id,
            list(bridge._outbound),
            dict(bridge._requests),
        )
        with self.assertRaisesRegex(
            NativeBridgeError, "NATIVE_BACKPRESSURE"
        ):
            bridge.submit_batch(
                [{"command": "snapshot"}], origin="x", revision=65
            )
        self.assertEqual(saturated, (
            bridge._next_request_id,
            list(bridge._outbound),
            dict(bridge._requests),
        ))
        bridge._emit_terminal("TEST")
        self.assertTrue(process.terminated)
        self.assertEqual(len(bridge._outbound), 0)
        self.assertEqual(len(bridge._requests), 0)

    def test_start_failure_restores_snapshot_and_event_ring(self) -> None:
        state = DashboardState()
        state.apply({"command": "scene.object.select", "draft_object": 1})
        before = (
            state.snapshot(), copy.deepcopy(list(state._events)),
            state._next_event_id,
        )

        class FailingBridge:
            closed = False

            def __init__(self, _executable, _config, on_event, _on_frame):
                self.on_event = on_event

            def start(self) -> None:
                self.on_event({
                    "type": "service.ready", "profile": "composition-v3",
                    "asset_import": "builtin_cube_only",
                    "frame_projection": "CMFB/v1",
                })

            @staticmethod
            def submit_batch(*_args, **_kwargs):
                raise NativeBridgeError("NATIVE_BACKPRESSURE")

            def close(self) -> None:
                self.closed = True

        with mock.patch.object(server_module, "NativeBridge", FailingBridge):
            with self.assertRaisesRegex(
                NativeBridgeError, "NATIVE_BACKPRESSURE"
            ):
                state.enable_native("unused", "unused")
        self.assertEqual(before, (
            state.snapshot(), list(state._events), state._next_event_id
        ))
        self.assertIsNone(state._native_bridge)

    def test_native_truth_refresh_is_deduplicated_and_pump_is_not_projected(
        self,
    ) -> None:
        class RecordingBridge:
            def __init__(self):
                self.calls: list[tuple[list[dict[str, object]], str, int]] = []

            def submit_batch(self, commands, *, origin, revision):
                self.calls.append((commands, origin, revision))
                return [len(self.calls)]

        state = DashboardState()
        bridge = RecordingBridge()
        state._native_bridge = bridge
        state._snapshot["backend"].update({
            "mode": "NATIVE_HOST", "native_host_bound": True,
        })
        before_events = state.event_count()
        state._native_event({
            "type": "pump.event", "event": "control_progress",
            "command_id": 0, "desired_channel": "none",
            "actual_channel": "none", "inline_channel": "none",
            "media_source": "none",
        })
        self.assertEqual(state.event_count(), before_events)
        actual = {
            "type": "actual", "channel": "render_media",
            "state_version": 1, "apply_result": "applied",
            "availability": "present", "profile_decoded": True,
            "enabled": True, "production_status": "running",
            "reason": "none", "production_epoch": 1,
        }
        state._native_event(actual)
        state._native_event({**actual, "channel": "camera_media"})
        self.assertEqual(bridge.calls, [
            ([{"command": "snapshot"}], "native.refresh", 0),
        ])
        state._native_event({
            "type": "snapshot", "request_id": 1, "phase": "ready",
            "terminal_reason": "none", "control_ready": True,
            "catalog_generation": 1, "media_bound": True,
            "media_mask": "both", "media_slots_consumed": 2,
            "mesh_active": False, "mesh_next_offset": 304,
            "wifi_actual": True, "presentation_actual": True,
            "scene_actual": True, "device_actual": True,
            "render_actual": True, "camera_actual": True,
            "wifi_sample": True, "render_result": True,
            "device_sample": True, "render_frame": True,
            "camera_frame": True,
        })
        snapshot = state.snapshot()
        self.assertTrue(snapshot["backend"]["device_connected"])
        self.assertEqual(snapshot["backend"]["session_status"], "READY")
        self.assertEqual(snapshot["bindings"]["render_result_stream"], "BOUND")
        self.assertEqual(snapshot["bindings"]["camera_media_stream"], "BOUND")
        state._native_event({"type": "media.connected", "transport": "tcp"})
        self.assertEqual(bridge.calls[-1], (
            [{"command": "snapshot"}], "native.refresh", 0,
        ))

    def test_euler_complete_scene_and_intent_mapping(self) -> None:
        snapshot = _default_snapshot()
        snapshot["scene"]["objects"].append({
            "draft_object": 9, "name": "Cube 9", "asset": "builtin:cube",
            "position": [1.0, 2.0, 3.0],
            "rotation_deg": [10.0, 20.0, 30.0],
            "scale": [0.5, 1.0, 2.0],
        })
        scene = full_scene_command(snapshot)
        expected = [
            0.03813457647485015, 0.18930785741200004,
            0.2392983377447303, 0.9515485246437886,
        ]
        for actual, wanted in zip(
            scene["objects"][1]["orientation_xyzw"], expected
        ):
            self.assertAlmostEqual(actual, wanted, places=14)
        self.assertEqual(len(scene["objects"]), 2)
        encoded = json.dumps(scene)
        for forbidden in (
            "state_version", "plan_generation", "space_epoch", "viewport",
            "ObjectId", "ContentId",
        ):
            self.assertNotIn(forbidden, encoded)
        for mutation in (
            "scene.object.add", "scene.object.delete",
            "scene.object.transform", "scene.camera.set",
        ):
            self.assertEqual(commands_for_mutation(mutation, snapshot), [scene])
        self.assertEqual(
            commands_for_mutation("scene.object.select", snapshot), []
        )
        self.assertEqual(
            commands_for_mutation("intent.device.set", snapshot),
            [{"command": "set_device", "enabled": False}],
        )
        self.assertEqual(
            [command["command"] for command in bootstrap_commands(snapshot)],
            [
                "asset.load_builtin", "set_scene", "set_presentation",
                "set_render_media", "set_camera_media", "set_wifi",
                "set_device", "snapshot",
            ],
        )

    def _run_frame_loop(
        self, chunks: list[bytes]
    ) -> tuple[NativeBridge, list[dict[str, object]], threading.Thread, socket.socket]:
        events: list[dict[str, object]] = []
        bridge = NativeBridge(
            "/bin/false", "/dev/null", events.append, lambda *_: None
        )
        receiver, sender = socket.socketpair()
        bridge._frame_socket = receiver
        thread = threading.Thread(target=bridge._frame_loop)
        thread.start()
        for chunk in chunks:
            sender.sendall(chunk)
        return bridge, events, thread, sender

    def test_cmfb_split_read_publishes_only_whole_record(self) -> None:
        published = threading.Event()
        metadata: list[tuple[str, dict[str, object]]] = []
        bridge = NativeBridge(
            "/bin/false", "/dev/null", lambda _: None,
            lambda source, value: (metadata.append((source, value)),
                                   published.set()),
        )
        receiver, sender = socket.socketpair()
        bridge._frame_socket = receiver
        thread = threading.Thread(target=bridge._frame_loop)
        thread.start()
        record = _cmfb_record()
        sender.sendall(record[:37])
        time.sleep(0.02)
        self.assertIsNone(bridge.latest_frame("render"))
        sender.sendall(record[37:])
        self.assertTrue(published.wait(1.0))
        self.assertEqual(bridge.latest_frame("render")[0], record)
        self.assertEqual(metadata[0][0], "render")
        self.assertEqual(metadata[0][1]["frame_sequence"], 1)
        bridge.close()
        sender.close()
        thread.join(1.0)
        self.assertFalse(thread.is_alive())

    def test_cmfb_bad_header_and_oversize_fail_terminal(self) -> None:
        oversize = bytearray(80)
        oversize[:4] = b"CMFB"
        oversize[8] = 2
        struct.pack_into(">HH", oversize, 4, 1, 80)
        struct.pack_into(">I", oversize, 20, MAX_CAMERA_FRAME_BYTES + 1)
        for candidate in (b"BAD!" + bytes(76), bytes(oversize)):
            with self.subTest(candidate=candidate[:4]):
                bridge, events, thread, sender = self._run_frame_loop([candidate])
                _wait_for(lambda: any(
                    event["type"] == "native.bridge_terminal"
                    for event in events
                ))
                self.assertIsNone(bridge.latest_frame("camera"))
                bridge.close()
                sender.close()
                thread.join(1.0)
                self.assertFalse(thread.is_alive())

    def test_loopback_binary_endpoint_is_whole_or_404(self) -> None:
        class Frames:
            closed = False

            @staticmethod
            def latest_frame(source: str):
                return (_cmfb_record(), {"source": source}) \
                    if source == "render" else None

            def close(self) -> None:
                self.closed = True

        server, state = create_server()
        frames = Frames()
        state._native_bridge = frames
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            connection = http.client.HTTPConnection(
                "127.0.0.1", server.server_port, timeout=2
            )
            connection.request("GET", "/api/v1/frames/render")
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(
                response.getheader("Content-Type"),
                "application/vnd.meiso.cmfb",
            )
            self.assertEqual(response.read(), _cmfb_record())
            connection.request("GET", "/api/v1/frames/camera")
            response = connection.getresponse()
            self.assertEqual(response.status, 404)
            self.assertEqual(json.loads(response.read()), {
                "error": "FRAME_UNAVAILABLE"
            })
            connection.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join(2.0)
        self.assertTrue(frames.closed)
        self.assertFalse(thread.is_alive())

    def test_recorded_default_snapshot_bytes_remain_frozen(self) -> None:
        encoded = json.dumps(
            _default_snapshot(), separators=(",", ":")
        ).encode("utf-8")
        self.assertEqual(len(encoded), 1462)
        self.assertEqual(
            hashlib.sha256(encoded).hexdigest(),
            "ae6829794d8072e7ca128b74bdf5ab32d0b750acde68688e4c71cc5d7e42920a",
        )
        with mock.patch.object(
            server_module, "NativeBridge",
            side_effect=AssertionError("recorded mode started native child"),
        ):
            server, state = create_server()
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            self.assertEqual(state.snapshot()["backend"]["mode"], "RECORDED_LOCAL")
            connection = http.client.HTTPConnection(
                "127.0.0.1", server.server_port, timeout=2
            )
            connection.request("GET", "/api/v1/snapshot")
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(response.read(), encoded)
            connection.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join(2.0)
        self.assertFalse(thread.is_alive())


if __name__ == "__main__":
    unittest.main(verbosity=2)
