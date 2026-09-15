#!/usr/bin/env python3
"""Independent native-bridge and CMFB boundary tests."""

from __future__ import annotations

import copy
import errno
import hashlib
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

import native_bridge as native_bridge_module
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
from server import (
    CommandError,
    CompositionEditorState,
    _default_snapshot,
    create_server,
)


FAKE_CHILD = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import signal
import sys
import time

simulator = "--simulator" in sys.argv
if simulator:
    mode = "simulator"
else:
    config_index = sys.argv.index("--config") + 1
    mode = Path(sys.argv[config_index]).read_text(encoding="utf-8").strip()
if mode == "invalid":
    print(json.dumps({
        "type": "service.ready", "profile": "composition",
        "wire_owner": "native_service", "asset_import": "builtin_cube_only",
        "frame_projection": "CMFB/v1", "unexpected": 1,
    }, separators=(",", ":")), flush=True)
else:
    print(json.dumps({
        "type": "service.ready", "profile": "composition",
        "wire_owner": (
            "simulator_service" if simulator or mode == "wrong_mode" else
            "native_service"
        ), "asset_import": "builtin_cube_only",
        "frame_projection": "CMFB/v1",
    }, separators=(",", ":")), flush=True)
    pump = {
        "type": "pump.event", "event": "control_progress",
        "command_id": 0, "desired_channel": "none",
        "actual_channel": "none", "inline_channel": "none",
        "media_source": "none",
    }
    if mode == "raw_terminal":
        terminal = {
            "type": "terminal", "code": "HOST_RUNTIME_TRANSPORT_TERMINAL"
        }
        os.write(1, (
            json.dumps(terminal, separators=(",", ":")) + "\n" +
            json.dumps(pump, separators=(",", ":")) + "\n"
        ).encode("ascii"))
        sys.exit(0)
    if mode == "raw_terminal_split_tail":
        signal.signal(signal.SIGTERM, lambda *_: None)
        terminal = {
            "type": "terminal", "code": "HOST_RUNTIME_TRANSPORT_TERMINAL"
        }
        os.write(1, (
            json.dumps(terminal, separators=(",", ":")) + "\n"
        ).encode("ascii"))
        deadline = time.monotonic() + 0.1
        while time.monotonic() < deadline:
            time.sleep(0.01)
        tail = b"\xffpost-terminal-not-json\n" + b"x" * 4070
        for _ in range(128):
            os.write(1, tail)
        while True:
            time.sleep(1.0)
    if mode == "exit_after_events":
        os.write(1, (
            json.dumps(pump, separators=(",", ":")) + "\n" +
            json.dumps({**pump, "command_id": 1}, separators=(",", ":")) +
            "\n"
        ).encode("ascii"))
        sys.exit(0)
    if mode == "late_raw_after_pump":
        terminal = {
            "type": "terminal", "code": "HOST_RUNTIME_TRANSPORT_TERMINAL"
        }
        os.write(1, (
            json.dumps(pump, separators=(",", ":")) + "\n" +
            json.dumps(terminal, separators=(",", ":")) + "\n" +
            json.dumps({**pump, "command_id": 1}, separators=(",", ":")) +
            "\n"
        ).encode("ascii"))
        sys.exit(0)
    if mode == "late_stderr":
        os.write(2, b"E_LATE\n")
        sys.exit(0)
    if mode == "frame_eof_live":
        frame_index = sys.argv.index("--frame-fd") + 1
        os.close(int(sys.argv[frame_index]))
        time.sleep(5.0)
    if mode == "stdout_eof_live":
        os.close(1)
        time.sleep(5.0)
    for raw in sys.stdin:
        value = json.loads(raw)
        request_id = value["request_id"]
        command = value["command"]
        if mode == "zero_error":
            output = {"type": "error", "request_id": 0, "code": "CONFLICT"}
        elif mode == "unknown_error":
            output = {
                "type": "error", "request_id": request_id + 100,
                "code": "CONFLICT",
            }
        elif mode == "mixed_error" and command == "set_presentation":
            output = {
                "type": "error", "request_id": request_id,
                "code": "CONFLICT",
            }
        elif mode == "wrong_completion":
            output = {
                "type": "command.receipt", "request_id": request_id,
                "command_id": request_id, "kind": "set_scene",
                "local_result": "queued", "replaced": False,
                "replaced_command_id": 0, "state_version": 1,
                "plan_generation": 0,
            }
        elif command == "snapshot":
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
        if mode == "duplicate":
            print(json.dumps(output, separators=(",", ":")), flush=True)
    if mode == "hold_after_stdin_eof":
        time.sleep(5.0)
'''

ASYNC_TEST_TIMEOUT_SECONDS = 10.0


def _wait_for(
    predicate, timeout: float = ASYNC_TEST_TIMEOUT_SECONDS
) -> None:
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

    def test_raw_host_terminal_wins_exit_and_drops_postterminal_chunk(
        self,
    ) -> None:
        events: list[dict[str, object]] = []
        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(
                Path(raw_directory), "raw_terminal"
            )
            bridge = NativeBridge(
                str(executable), str(config), events.append, lambda *_: None
            )
            bridge.start()
            try:
                _wait_for(lambda: any(
                    event.get("code") == "NATIVE_HOST_TERMINAL"
                    for event in events
                ))
                terminal_index = next(
                    index for index, event in enumerate(events)
                    if event.get("type") == "terminal"
                )
                self.assertEqual(
                    events[terminal_index]["code"],
                    "HOST_RUNTIME_TRANSPORT_TERMINAL",
                )
                self.assertEqual(events[terminal_index + 1], {
                    "type": "native.bridge_terminal",
                    "code": "NATIVE_HOST_TERMINAL",
                })
                self.assertEqual(sum(
                    event.get("type") == "native.bridge_terminal"
                    for event in events
                ), 1)
                self.assertFalse(any(
                    event.get("type") == "pump.event" for event in events
                ))
                self.assertFalse(any(
                    str(event.get("code", "")).startswith("NATIVE_EXIT_")
                    for event in events
                ))
            finally:
                bridge.close()

    def test_raw_terminal_discards_split_tail_then_kills_and_reaps(
        self,
    ) -> None:
        events: list[dict[str, object]] = []
        kill_calls: list[object] = []
        real_kill = native_bridge_module.subprocess.Popen.kill

        def counted_kill(process) -> None:
            kill_calls.append(process)
            real_kill(process)

        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(
                Path(raw_directory), "raw_terminal_split_tail"
            )
            bridge = NativeBridge(
                str(executable), str(config), events.append, lambda *_: None
            )
            close_thread: threading.Thread | None = None
            with (
                mock.patch.object(
                    native_bridge_module,
                    "NATIVE_TERMINAL_EXIT_TIMEOUT_SECONDS",
                    0.3,
                ),
                mock.patch.object(
                    native_bridge_module,
                    "NATIVE_TERMINAL_KILL_TIMEOUT_SECONDS",
                    0.5,
                ),
                mock.patch.object(
                    native_bridge_module.subprocess.Popen,
                    "kill",
                    counted_kill,
                ),
            ):
                bridge.start()
                try:
                    _wait_for(lambda: any(
                        event.get("code") == "NATIVE_HOST_TERMINAL"
                        for event in events
                    ))
                    self.assertFalse(bridge._stdout_drained.wait(0.05))
                    close_thread = threading.Thread(target=bridge.close)
                    close_thread.start()
                    close_thread.join(3.0)
                    self.assertFalse(close_thread.is_alive())
                    self.assertEqual(len(kill_calls), 1)
                    lifecycle = bridge._lifecycle_snapshot()
                    self.assertEqual(lifecycle["state"], "CLOSED")
                    self.assertEqual(lifecycle["stdout_drain_result"], "EOF")
                    self.assertTrue(lifecycle["stdout_drained"])
                    self.assertTrue(lifecycle["stderr_drained"])
                    self.assertTrue(lifecycle["frame_drained"])
                    self.assertIsNotNone(lifecycle["process_return_code"])
                    self.assertIsNone(lifecycle["shutdown_failure"])
                    self.assertFalse(any(
                        thread.is_alive() for thread in bridge._threads
                    ))
                    self.assertEqual([
                        event.get("type") for event in events
                        if event.get("type") in {
                            "terminal", "native.bridge_terminal",
                        }
                    ], ["terminal", "native.bridge_terminal"])
                    self.assertFalse(any(
                        str(event.get("code", "")).startswith("NATIVE_EXIT_")
                        for event in events
                    ))
                finally:
                    bridge.close()
                    if close_thread is not None:
                        close_thread.join(1.0)

    def test_unreaped_terminal_is_failed_until_external_recovery(
        self,
    ) -> None:
        events: list[dict[str, object]] = []
        kill_calls: list[int] = []

        def ignored_kill(process) -> None:
            kill_calls.append(process.pid)

        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(
                Path(raw_directory), "raw_terminal_split_tail"
            )
            bridge = NativeBridge(
                str(executable), str(config), events.append, lambda *_: None
            )
            process = None
            try:
                with (
                    mock.patch.object(
                        native_bridge_module,
                        "NATIVE_TERMINAL_EXIT_TIMEOUT_SECONDS", 0.05,
                    ),
                    mock.patch.object(
                        native_bridge_module,
                        "NATIVE_TERMINAL_KILL_TIMEOUT_SECONDS", 0.05,
                    ),
                    mock.patch.object(
                        native_bridge_module.subprocess.Popen,
                        "kill", ignored_kill,
                    ),
                ):
                    bridge.start()
                    process = bridge._process
                    self.assertIsNotNone(process)
                    _wait_for(lambda: any(
                        event.get("code") == "NATIVE_HOST_TERMINAL"
                        for event in events
                    ))
                    with self.assertRaisesRegex(
                        NativeBridgeError, "^NATIVE_PROCESS_SHUTDOWN$"
                    ):
                        bridge.close()
                    self.assertEqual(kill_calls, [process.pid])
                    self.assertIsNone(process.poll())
                    lifecycle = bridge._lifecycle_snapshot()
                    self.assertEqual(lifecycle["state"], "FAILED")
                    self.assertEqual(
                        lifecycle["shutdown_failure"],
                        "NATIVE_PROCESS_SHUTDOWN",
                    )
                    self.assertEqual(
                        lifecycle["stdout_drain_result"], "PENDING"
                    )
                    with self.assertRaisesRegex(
                        NativeBridgeError, "^NATIVE_PROCESS_SHUTDOWN$"
                    ):
                        bridge.close()
                    self.assertEqual(kill_calls, [process.pid])
            finally:
                if process is not None and process.poll() is None:
                    os.kill(process.pid, signal.SIGKILL)
                    process.wait(timeout=2.0)
                bridge.close()
            self.assertEqual(bridge._lifecycle_snapshot()["state"], "CLOSED")
            self.assertFalse(any(
                thread.is_alive() for thread in bridge._threads
            ))

    def test_thread_construction_abort_retains_unreaped_process(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(
                Path(raw_directory), "hold_after_stdin_eof"
            )
            bridge = NativeBridge(
                str(executable), str(config), lambda *_: None, lambda *_: None
            )
            process = None
            try:
                with (
                    mock.patch.object(
                        native_bridge_module.threading, "Thread",
                        side_effect=RuntimeError("injected thread constructor"),
                    ),
                    mock.patch.object(
                        NativeBridge, "_stop_process", return_value=False,
                    ),
                ):
                    with self.assertRaisesRegex(
                        NativeBridgeError, "^NATIVE_PROCESS_SHUTDOWN$"
                    ):
                        bridge.start()
                process = bridge._process
                self.assertIsNotNone(process)
                self.assertIsNone(process.poll())
                self.assertEqual(
                    bridge._lifecycle_snapshot()["state"], "FAILED"
                )
                with self.assertRaisesRegex(
                    NativeBridgeError, "^NATIVE_PROCESS_SHUTDOWN$"
                ):
                    bridge.close()
            finally:
                if process is not None and process.poll() is None:
                    os.kill(process.pid, signal.SIGKILL)
                    process.wait(timeout=2.0)
                bridge.close()

    def test_post_popen_acquisition_abort_retains_unreaped_process(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(
                Path(raw_directory), "hold_after_stdin_eof"
            )
            bridge = NativeBridge(
                str(executable), str(config), lambda *_: None, lambda *_: None
            )
            process = None
            try:
                with (
                    mock.patch.object(
                        native_bridge_module.os, "set_blocking",
                        side_effect=OSError("injected post-Popen failure"),
                    ),
                    mock.patch.object(
                        NativeBridge, "_stop_process", return_value=False,
                    ),
                ):
                    with self.assertRaisesRegex(
                        NativeBridgeError, "^NATIVE_PROCESS_SHUTDOWN$"
                    ):
                        bridge.start()
                process = bridge._process
                self.assertIsNotNone(process)
                self.assertIsNone(process.poll())
                lifecycle = bridge._lifecycle_snapshot()
                self.assertEqual(lifecycle["state"], "FAILED")
                self.assertEqual(
                    lifecycle["shutdown_failure"],
                    "NATIVE_PROCESS_SHUTDOWN",
                )
                self.assertEqual(
                    lifecycle["stdin_close_origin"], "START_ABORT"
                )
                self.assertEqual(
                    lifecycle["stdin_close_attempt_origin"], "START_ABORT"
                )
                self.assertEqual(lifecycle["stdin_close_result"], "CLOSED")
                self.assertIsNone(lifecycle["stdin_close_errno"])
                self.assertFalse(lifecycle["stdin_open"])
            finally:
                if process is not None and process.poll() is None:
                    os.kill(process.pid, signal.SIGKILL)
                    process.wait(timeout=2.0)
                bridge.close()

    def test_owner_eof_is_observable_and_only_falls_back_after_drain(
        self,
    ) -> None:
        events: list[dict[str, object]] = []
        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(Path(raw_directory), "valid")
            bridge = NativeBridge(
                str(executable), str(config), events.append, lambda *_: None
            )
            bridge.start()
            try:
                _wait_for(lambda: any(
                    event.get("type") == "service.ready" for event in events
                ))
                process = bridge._process
                self.assertIsNotNone(process)
                self.assertIsNone(process.stdin)
                bridge._request_owner_eof()
                _wait_for(lambda: any(
                    event.get("code") == "NATIVE_EXIT_OK" for event in events
                ))
                lifecycle = bridge._lifecycle_snapshot()
                self.assertEqual(lifecycle, {
                    "state": "RUNNING",
                    "owner_close_requested": False,
                    "stdin_close_origin": "OWNER_EOF",
                    "stdin_close_attempt_origin": "OWNER_EOF",
                    "stdin_close_result": "CLOSED",
                    "stdin_close_errno": None,
                    "stdin_open": False,
                    "stdin_open_at_process_exit": False,
                    "process_return_code": 0,
                    "stdout_drain_result": "EOF",
                    "stdout_drained": True,
                    "stderr_drained": True,
                    "frame_drained": True,
                    "terminal_code": "NATIVE_EXIT_OK",
                    "shutdown_failure": None,
                })
                self.assertEqual([
                    event["code"] for event in events
                    if event.get("type") == "native.bridge_terminal"
                ], ["NATIVE_EXIT_OK"])
            finally:
                bridge.close()
            self.assertEqual(
                bridge._lifecycle_snapshot()["stdin_close_origin"],
                "OWNER_EOF",
            )

    def test_external_stdin_close_is_unproven_and_never_retried(self) -> None:
        events: list[dict[str, object]] = []
        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(
                Path(raw_directory), "hold_after_stdin_eof"
            )
            bridge = NativeBridge(
                str(executable), str(config), events.append, lambda *_: None
            )
            bridge.start()
            source_fd: int | None = None
            reused_fd: int | None = None
            try:
                _wait_for(lambda: any(
                    event.get("type") == "service.ready" for event in events
                ))
                stdin_fd = bridge._stdin_fd
                self.assertIsNotNone(stdin_fd)
                os.close(stdin_fd)
                with self.assertRaisesRegex(
                    NativeBridgeError, "NATIVE_OWNER_EOF_FAILED"
                ):
                    bridge._request_owner_eof()
                lifecycle = bridge._lifecycle_snapshot()
                self.assertEqual(lifecycle["stdin_close_origin"], "UNPROVEN")
                self.assertEqual(
                    lifecycle["stdin_close_attempt_origin"], "OWNER_EOF"
                )
                self.assertEqual(lifecycle["stdin_close_result"], "FAILED")
                self.assertEqual(lifecycle["stdin_close_errno"], errno.EBADF)
                self.assertFalse(lifecycle["stdin_open"])
                self.assertIsNone(bridge._stdin_fd)
                source_fd = os.open(os.devnull, os.O_RDONLY)
                if source_fd == stdin_fd:
                    reused_fd = source_fd
                else:
                    os.dup2(source_fd, stdin_fd)
                    reused_fd = stdin_fd
                bridge.close()
                os.fstat(reused_fd)
            finally:
                bridge.close()
                for fd in {source_fd, reused_fd} - {None}:
                    os.close(fd)

    def test_process_exit_waits_for_delayed_stdout_and_frame_drain(self) -> None:
        events: list[dict[str, object]] = []
        first_pump = threading.Event()
        release = threading.Event()

        def on_event(event: dict[str, object]) -> None:
            if (event.get("type") == "pump.event" and
                    event.get("command_id") == 0):
                first_pump.set()
                self.assertTrue(release.wait(5.0))
            events.append(event)

        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(
                Path(raw_directory), "exit_after_events"
            )
            bridge = NativeBridge(
                str(executable), str(config), on_event, lambda *_: None
            )
            bridge.start()
            try:
                self.assertTrue(first_pump.wait(2.0))
                _wait_for(lambda: (
                    bridge._lifecycle_snapshot()["process_return_code"] == 0
                ))
                self.assertFalse(any(
                    event.get("type") == "native.bridge_terminal"
                    for event in events
                ))
                release.set()
                _wait_for(lambda: any(
                    event.get("code") == "NATIVE_EXIT_OK" for event in events
                ))
                self.assertEqual([
                    event.get("command_id") for event in events
                    if event.get("type") == "pump.event"
                ], [0, 1])
                self.assertEqual(events[-1], {
                    "type": "native.bridge_terminal", "code": "NATIVE_EXIT_OK"
                })
                lifecycle = bridge._lifecycle_snapshot()
                self.assertTrue(lifecycle["stdout_drained"])
                self.assertTrue(lifecycle["stderr_drained"])
                self.assertTrue(lifecycle["frame_drained"])
            finally:
                release.set()
                bridge.close()

    def test_bounded_drain_failure_suppresses_late_output_and_exit(self) -> None:
        events: list[dict[str, object]] = []
        first_pump = threading.Event()
        release = threading.Event()

        def on_event(event: dict[str, object]) -> None:
            if (event.get("type") == "pump.event" and
                    event.get("command_id") == 0):
                first_pump.set()
                self.assertTrue(release.wait(5.0))
            events.append(event)

        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(
                Path(raw_directory), "late_raw_after_pump"
            )
            bridge = NativeBridge(
                str(executable), str(config), on_event, lambda *_: None
            )
            with mock.patch.object(
                native_bridge_module, "NATIVE_DRAIN_TIMEOUT_SECONDS", 0.05
            ):
                bridge.start()
                try:
                    self.assertTrue(first_pump.wait(2.0))
                    _wait_for(lambda: (
                        bridge._lifecycle_snapshot()["process_return_code"] == 0
                    ))
                    time.sleep(0.1)
                    release.set()
                    _wait_for(lambda: any(
                        event.get("code") == "NATIVE_OUTPUT_DRAIN"
                        for event in events
                    ))
                    self.assertEqual([
                        event.get("command_id") for event in events
                        if event.get("type") == "pump.event"
                    ], [0])
                    self.assertEqual([
                        event["code"] for event in events
                        if event.get("type") == "native.bridge_terminal"
                    ], ["NATIVE_OUTPUT_DRAIN"])
                    self.assertFalse(any(
                        event.get("type") == "terminal" for event in events
                    ))
                    _wait_for(lambda: not any(
                        thread.is_alive() for thread in bridge._threads
                    ))
                finally:
                    release.set()
                    bridge.close()

    def test_late_stderr_beats_exit_with_one_atomic_terminal(self) -> None:
        events: list[dict[str, object]] = []
        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(
                Path(raw_directory), "late_stderr"
            )
            bridge = NativeBridge(
                str(executable), str(config), events.append, lambda *_: None
            )
            bridge.start()
            try:
                _wait_for(lambda: any(
                    event.get("code") == "NATIVE_STDERR" for event in events
                ))
                error_index = next(
                    index for index, event in enumerate(events)
                    if event.get("type") == "native.process_error"
                )
                self.assertEqual(events[error_index], {
                    "type": "native.process_error", "code": "E_LATE"
                })
                self.assertEqual(events[error_index + 1], {
                    "type": "native.bridge_terminal", "code": "NATIVE_STDERR"
                })
                self.assertEqual(sum(
                    event.get("type") == "native.bridge_terminal"
                    for event in events
                ), 1)
            finally:
                bridge.close()

    def test_live_child_frame_eof_remains_terminal(self) -> None:
        events: list[dict[str, object]] = []
        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(
                Path(raw_directory), "frame_eof_live"
            )
            bridge = NativeBridge(
                str(executable), str(config), events.append, lambda *_: None
            )
            bridge.start()
            try:
                _wait_for(lambda: any(
                    event.get("code") == "NATIVE_FRAME_EOF"
                    for event in events
                ))
                self.assertEqual([
                    event["code"] for event in events
                    if event.get("type") == "native.bridge_terminal"
                ], ["NATIVE_FRAME_EOF"])
            finally:
                bridge.close()

    def test_live_child_stdout_eof_is_bounded_terminal(self) -> None:
        events: list[dict[str, object]] = []
        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(
                Path(raw_directory), "stdout_eof_live"
            )
            bridge = NativeBridge(
                str(executable), str(config), events.append, lambda *_: None
            )
            bridge.start()
            try:
                _wait_for(lambda: any(
                    event.get("code") == "NATIVE_STDOUT_EOF"
                    for event in events
                ))
                self.assertEqual([
                    event["code"] for event in events
                    if event.get("type") == "native.bridge_terminal"
                ], ["NATIVE_STDOUT_EOF"])
                _wait_for(lambda: (
                    bridge._lifecycle_snapshot()["process_return_code"] is not
                    None and
                    bridge._lifecycle_snapshot()["stderr_drained"] and
                    bridge._lifecycle_snapshot()["frame_drained"]
                ))
                _wait_for(lambda: not any(
                    thread.is_alive() for thread in bridge._threads
                ))
            finally:
                bridge.close()

    def test_correlated_error_rejects_only_one_request_and_child_continues(
        self,
    ) -> None:
        events: list[dict[str, object]] = []
        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(
                Path(raw_directory), "mixed_error"
            )
            bridge = NativeBridge(
                str(executable), str(config), events.append, lambda *_: None
            )
            bridge.start()
            try:
                _wait_for(lambda: any(
                    event["type"] == "service.ready" for event in events
                ))
                process = bridge._process
                self.assertIsNotNone(process)
                process_id = process.pid
                with bridge._condition:
                    bridge._frames["render"] = (
                        b"kept-frame", {"frame_sequence": 7}
                    )
                self.assertEqual(bridge.submit_batch([
                    {"command": "set_presentation"},
                    {"command": "set_render_media"},
                ], origin="intent.render.set", revision=3), [1, 2])
                _wait_for(lambda: sum(
                    event["type"] in {"error", "command.receipt"}
                    for event in events
                ) == 2)
                rejection = next(
                    event for event in events if event["type"] == "error"
                )
                receipt = next(
                    event for event in events
                    if event["type"] == "command.receipt"
                )
                self.assertEqual(rejection["code"], "CONFLICT")
                self.assertEqual(rejection["browser_command"],
                                 "intent.render.set")
                self.assertEqual(rejection["browser_revision"], 3)
                self.assertEqual(rejection["native_command"],
                                 "set_presentation")
                self.assertEqual(rejection["native_batch_id"], 1)
                self.assertEqual(rejection["native_batch_index"], 1)
                self.assertEqual(rejection["native_batch_count"], 2)
                self.assertEqual(receipt["native_command"],
                                 "set_render_media")
                self.assertEqual(receipt["native_batch_index"], 2)
                self.assertFalse(any(
                    event["type"] == "native.bridge_terminal"
                    for event in events
                ))
                self.assertEqual(bridge._requests, {})
                self.assertEqual(
                    bridge.latest_frame("render"),
                    (b"kept-frame", {"frame_sequence": 7}),
                )
                self.assertIsNone(process.poll())
                self.assertEqual(process.pid, process_id)

                self.assertEqual(bridge.submit_batch(
                    [{"command": "snapshot"}],
                    origin="native.refresh", revision=3,
                ), [3])
                _wait_for(lambda: any(
                    event["type"] == "snapshot" and
                    event["request_id"] == 3 for event in events
                ))
                self.assertIsNone(process.poll())
                self.assertEqual(process.pid, process_id)
            finally:
                bridge.close()

    def test_unowned_or_duplicate_completion_is_terminal(self) -> None:
        for mode in (
            "zero_error", "unknown_error", "duplicate", "wrong_completion",
        ):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as raw:
                events: list[dict[str, object]] = []
                executable, config = self._fake_child(Path(raw), mode)
                bridge = NativeBridge(
                    str(executable), str(config), events.append,
                    lambda *_: None,
                )
                bridge.start()
                try:
                    _wait_for(lambda: any(
                        event["type"] == "service.ready" for event in events
                    ))
                    bridge.submit_batch(
                        [{"command": "snapshot"}],
                        origin="native.refresh", revision=1,
                    )
                    _wait_for(lambda: any(
                        event.get("type") == "native.bridge_terminal"
                        for event in events
                    ))
                    terminal = next(
                        event for event in events
                        if event["type"] == "native.bridge_terminal"
                    )
                    self.assertEqual(
                        terminal["code"], "NATIVE_REQUEST_CORRELATION"
                    )
                    self.assertEqual(bridge._requests, {})
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

        state = CompositionEditorState()
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
        os.environ.get("MEISO_COMPOSITION_HOST_BRIDGE"),
        "built Composition Host Bridge not supplied",
    )
    def test_real_simulator_child_completes_offline_editor_bootstrap(self) -> None:
        executable = os.environ["MEISO_COMPOSITION_HOST_BRIDGE"]
        state = CompositionEditorState()
        state.enable_native(executable, None, simulator=True)
        try:
            def editor_bootstrap_complete() -> bool:
                snapshot = state.snapshot()
                scene_status = snapshot["native_truth"]["scene_status"]
                return bool(
                    snapshot["native_truth"]["render_result"] is not None and
                    snapshot["frames"]["render"]["available"] and
                    scene_status is not None and
                    scene_status["reason"] == "ready" and
                    snapshot["bindings"]["render_result_stream"] ==
                    "SIMULATED"
                )

            _wait_for(
                editor_bootstrap_complete,
                timeout=5.0,
            )
            snapshot = state.snapshot()
            self.assertEqual(snapshot["backend"]["mode"], "NATIVE_SIMULATOR")
            self.assertEqual(snapshot["backend"]["session_status"], "SIMULATED")
            self.assertFalse(snapshot["backend"]["device_connected"])
            self.assertEqual(
                snapshot["backend"]["proof"],
                "SYNTHETIC_OFFLINE_CPU_RASTER",
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
            self.assertTrue(snapshot["frames"]["render"]["available"])
            self.assertFalse(snapshot["frames"]["camera"]["available"])
            record, metadata = state.latest_frame("render")
            self.assertEqual(record[:4], b"CMFB")
            self.assertEqual(len(record), 80 + 320 * 240 * 4)
            self.assertEqual(metadata["pixel_format"], "rgba8888")
            self.assertEqual(metadata["payload_bytes"], 320 * 240 * 4)
            self.assertEqual(
                hashlib.sha256(record[80:]).hexdigest(),
                snapshot["native_truth"]["render_result"]
                    ["full_target_sha256"],
            )
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

    def test_close_claim_during_popen_cannot_publish_or_orphan_child(
        self,
    ) -> None:
        events: list[dict[str, object]] = []
        entered_popen = threading.Event()
        release_popen = threading.Event()
        close_done = threading.Event()
        start_errors: list[BaseException] = []
        real_popen = native_bridge_module.subprocess.Popen

        def gated_popen(*args, **kwargs):
            entered_popen.set()
            if not release_popen.wait(5.0):
                raise RuntimeError("test Popen gate timed out")
            return real_popen(*args, **kwargs)

        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(Path(raw_directory), "valid")
            bridge = NativeBridge(
                str(executable), str(config), events.append, lambda *_: None
            )

            def run_start() -> None:
                try:
                    bridge.start()
                except BaseException as error:  # captured for thread assertion
                    start_errors.append(error)

            def run_close() -> None:
                bridge.close()
                close_done.set()

            with mock.patch.object(
                native_bridge_module.subprocess, "Popen", side_effect=gated_popen
            ):
                start_thread = threading.Thread(target=run_start)
                start_thread.start()
                self.assertTrue(entered_popen.wait(2.0))
                close_thread = threading.Thread(target=run_close)
                close_thread.start()
                time.sleep(0.05)
                self.assertFalse(close_done.is_set())
                release_popen.set()
                start_thread.join(5.0)
                close_thread.join(5.0)

            self.assertFalse(start_thread.is_alive())
            self.assertFalse(close_thread.is_alive())
            self.assertEqual(start_errors, [])
            self.assertEqual(events, [])
            lifecycle = bridge._lifecycle_snapshot()
            self.assertEqual(lifecycle["state"], "CLOSED")
            self.assertTrue(lifecycle["owner_close_requested"])
            self.assertEqual(lifecycle["stdin_close_origin"], "BRIDGE_CLOSE")
            self.assertFalse(lifecycle["stdin_open"])
            self.assertIsNone(lifecycle["shutdown_failure"])
            process = bridge._process
            self.assertIsNotNone(process)
            self.assertIsNotNone(process.poll())
            self.assertFalse(any(thread.is_alive() for thread in bridge._threads))

    def test_post_popen_thread_failure_wakes_concurrent_close(self) -> None:
        events: list[dict[str, object]] = []
        entered_constructor = threading.Event()
        release_constructor = threading.Event()
        close_done = threading.Event()
        start_errors: list[BaseException] = []

        def failing_thread(*_args, **_kwargs):
            entered_constructor.set()
            if not release_constructor.wait(5.0):
                raise RuntimeError("test thread-constructor gate timed out")
            raise RuntimeError("test post-Popen thread failure")

        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(Path(raw_directory), "valid")
            bridge = NativeBridge(
                str(executable), str(config), events.append, lambda *_: None
            )

            def run_start() -> None:
                try:
                    bridge.start()
                except BaseException as error:  # captured for thread assertion
                    start_errors.append(error)

            def run_close() -> None:
                bridge.close()
                close_done.set()

            start_thread = threading.Thread(target=run_start)
            close_thread = threading.Thread(target=run_close)
            with mock.patch.object(
                native_bridge_module.threading, "Thread",
                side_effect=failing_thread,
            ):
                start_thread.start()
                self.assertTrue(entered_constructor.wait(2.0))
                close_thread.start()
                time.sleep(0.05)
                self.assertFalse(close_done.is_set())
                release_constructor.set()
                start_thread.join(5.0)
                close_thread.join(5.0)

            self.assertFalse(start_thread.is_alive())
            self.assertFalse(close_thread.is_alive())
            self.assertEqual(len(start_errors), 1)
            self.assertIsInstance(start_errors[0], NativeBridgeError)
            self.assertEqual(str(start_errors[0]), "NATIVE_START")
            self.assertEqual(events, [])
            lifecycle = bridge._lifecycle_snapshot()
            self.assertEqual(lifecycle["state"], "CLOSED")
            self.assertTrue(lifecycle["owner_close_requested"])
            self.assertEqual(lifecycle["stdin_close_origin"], "BRIDGE_CLOSE")
            self.assertIsNone(lifecycle["shutdown_failure"])
            process = bridge._process
            self.assertIsNotNone(process)
            self.assertIsNotNone(process.poll())
            self.assertIsNone(bridge._stdin_fd)
            self.assertIsNone(bridge._frame_socket)

    def test_running_close_reaps_all_owned_workers_and_channels(self) -> None:
        events: list[dict[str, object]] = []
        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(Path(raw_directory), "valid")
            bridge = NativeBridge(
                str(executable), str(config), events.append, lambda *_: None
            )
            bridge.start()
            _wait_for(lambda: any(
                event.get("type") == "service.ready" for event in events
            ))
            bridge.close()
            lifecycle = bridge._lifecycle_snapshot()
            self.assertEqual(lifecycle["state"], "CLOSED")
            self.assertTrue(lifecycle["owner_close_requested"])
            self.assertEqual(lifecycle["stdin_close_origin"], "BRIDGE_CLOSE")
            self.assertFalse(lifecycle["stdin_open"])
            self.assertIsNone(lifecycle["shutdown_failure"])
            process = bridge._process
            self.assertIsNotNone(process)
            self.assertIsNotNone(process.poll())
            self.assertIsNone(bridge._stdin_fd)
            self.assertIsNone(bridge._frame_socket)
            self.assertFalse(any(thread.is_alive() for thread in bridge._threads))

    def test_sanitizer_rejects_unknown_fields_types_and_enums(self) -> None:
        ready = {
            "type": "service.ready", "profile": "composition",
            "wire_owner": "native_service",
            "asset_import": "builtin_cube_only",
            "frame_projection": "CMFB/v1",
        }
        self.assertNotIn("wire_owner", sanitize_native_event(ready))
        for code in (
            "HOST_RUNTIME_TRANSPORT_TERMINAL", "HOST_RUNTIME_SESSION_TERMINAL",
            "HOST_RUNTIME_CATALOG_TERMINAL", "HOST_RUNTIME_LOCAL_TERMINAL",
            "HOST_RUNTIME_INTERNAL_TERMINAL",
            "SIMULATOR_TERMINAL",
        ):
            self.assertEqual(
                sanitize_native_event({"type": "terminal", "code": code}),
                {"type": "terminal", "code": code},
            )
        self.assertEqual(
            server_module._RECONNECTABLE_NATIVE_TERMINALS,
            frozenset({
                "HOST_RUNTIME_TRANSPORT_TERMINAL",
                "HOST_RUNTIME_SESSION_TERMINAL",
                "HOST_RUNTIME_CATALOG_TERMINAL",
            }),
        )
        for invalid in (
            {**ready, "extra": 1},
            {**ready, "profile": "not-composition"},
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
        bridge._lifecycle_state = "RUNNING"
        bridge._stdin_open = True
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

    def test_request_rejection_is_bounded_revision_aware_browser_truth(
        self,
    ) -> None:
        state = CompositionEditorState()
        common = {
            "browser_command": "intent.render.set",
            "browser_revision": 3,
            "native_batch_id": 21,
            "native_batch_count": 2,
        }
        state._native_event({
            "type": "error", "request_id": 21, "code": "CONFLICT",
            "native_command": "set_presentation",
            "native_batch_index": 1, **common,
        })
        state._native_event({
            "type": "command.receipt", "request_id": 22,
            "command_id": 8, "kind": "set_render_media",
            "local_result": "queued", "replaced": False,
            "replaced_command_id": 0, "state_version": 2,
            "plan_generation": 0, "native_command": "set_render_media",
            "native_batch_index": 2, **common,
        })
        snapshot = state.snapshot()
        batch = snapshot["native_truth"]["request_batches"][
            "intent.render.set"
        ]
        self.assertEqual(batch["status"], "PARTIAL_REJECTED")
        self.assertEqual(batch["completed"], 2)
        self.assertEqual(batch["accepted"], 1)
        self.assertEqual(batch["rejected"], 1)
        self.assertEqual(
            [item["native_command"] for item in batch["outcomes"]],
            ["set_presentation", "set_render_media"],
        )
        self.assertEqual(
            list(state._events)[-2][1]["status"],
            "NATIVE_REQUEST_REJECTED",
        )
        self.assertEqual(snapshot["backend"]["session_status"], "ABSENT")

        state._native_event({
            "type": "command.receipt", "request_id": 30,
            "command_id": 9, "kind": "set_presentation",
            "local_result": "queued", "replaced": False,
            "replaced_command_id": 0, "state_version": 4,
            "plan_generation": 3, "native_command": "set_presentation",
            "browser_command": "intent.render.set",
            "browser_revision": 4, "native_batch_id": 30,
            "native_batch_index": 1, "native_batch_count": 1,
        })
        state._native_event({
            "type": "error", "request_id": 20, "code": "CONFLICT",
            "native_command": "set_presentation",
            "browser_command": "intent.render.set",
            "browser_revision": 2, "native_batch_id": 20,
            "native_batch_index": 1, "native_batch_count": 1,
        })
        snapshot = state.snapshot()
        current = snapshot["native_truth"]["request_batches"][
            "intent.render.set"
        ]
        self.assertEqual(current["revision"], 4)
        self.assertEqual(current["status"], "ACCEPTED")
        self.assertEqual(
            snapshot["native_truth"]["request_outcomes"][-1]["result"],
            "REJECTED",
        )

    def test_start_failure_restores_snapshot_and_event_ring(self) -> None:
        state = CompositionEditorState()
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
                    "type": "service.ready", "profile": "composition",
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

    def test_close_wins_over_late_native_start_failure_without_rollback(
        self,
    ) -> None:
        start_entered = threading.Event()
        release_start = threading.Event()

        class FailingBridge:
            def __init__(self, _executable, _config, _on_event, _on_frame):
                self.close_count = 0

            def start(self) -> None:
                start_entered.set()
                if not release_start.wait(2.0):
                    raise AssertionError("test did not release native start")
                raise OSError("injected late native start failure")

            @staticmethod
            def submit_batch(*_args, **_kwargs):
                raise AssertionError("submit must not follow failed start")

            def close(self) -> None:
                self.close_count += 1

        state = CompositionEditorState()
        failures: list[BaseException] = []

        def enable() -> None:
            try:
                state.enable_native("unused", "unused")
            except BaseException as error:
                failures.append(error)

        with mock.patch.object(server_module, "NativeBridge", FailingBridge):
            thread = threading.Thread(target=enable)
            thread.start()
            self.assertTrue(start_entered.wait(2.0))
            bridge = state._native_bridge
            self.assertIsNotNone(bridge)
            state.close()
            event_id, after_close = state.apply({
                "command": "scene.object.add", "asset": "builtin:cube",
            })
            events_after_close = copy.deepcopy(list(state._events))
            release_start.set()
            thread.join(2.0)

        self.assertFalse(thread.is_alive())
        self.assertEqual(len(failures), 1)
        self.assertIsInstance(failures[0], OSError)
        self.assertEqual(state.snapshot(), after_close)
        self.assertEqual(list(state._events), events_after_close)
        self.assertEqual(after_close["revision"], 1)
        self.assertEqual(len(after_close["scene"]["objects"]), 2)
        self.assertEqual(list(state._events)[-1][0], event_id)
        self.assertIsNone(state._native_bridge)
        self.assertEqual(bridge.close_count, 2)

    def test_remote_transport_terminal_restarts_fresh_child_and_replays_latest(
        self,
    ) -> None:
        class RecordingBridge:
            instances: list["RecordingBridge"] = []

            def __init__(self, _executable, _config, on_event, on_frame):
                self.on_event = on_event
                self.on_frame = on_frame
                self.calls: list[tuple[list[dict[str, object]], str, int]] = []
                self.closed = False
                self.__class__.instances.append(self)

            def start(self) -> None:
                return None

            def submit_batch(self, commands, *, origin, revision):
                self.calls.append((copy.deepcopy(commands), origin, revision))
                return list(range(1, len(commands) + 1))

            def close(self) -> None:
                self.closed = True

            @staticmethod
            def latest_frame(_source):
                return None

        state = CompositionEditorState()
        with (
            mock.patch.object(server_module, "NativeBridge", RecordingBridge),
            mock.patch.object(
                server_module, "NATIVE_RECONNECT_INITIAL_SECONDS", 0.05
            ),
            mock.patch.object(
                server_module, "NATIVE_RECONNECT_MAX_SECONDS", 0.05
            ),
        ):
            state.enable_native("unused", "unused")
            first = RecordingBridge.instances[0]
            first.on_event({
                "type": "terminal",
                "code": "HOST_RUNTIME_TRANSPORT_TERMINAL",
            })
            event_id, snapshot = state.apply({
                "command": "scene.object.transform",
                "draft_object": 1,
                "position": [9.0, 8.0, 7.0],
                "rotation_deg": [1.0, 2.0, 3.0],
                "scale": [2.0, 2.0, 2.0],
            })
            self.assertGreater(event_id, 0)
            self.assertEqual(snapshot["backend"]["session_status"],
                             "RECONNECTING")
            self.assertEqual(list(state._events)[-1][1]["status"],
                             "NATIVE_RECONNECT_PENDING")
            _wait_for(lambda: len(RecordingBridge.instances) == 2)
            second = RecordingBridge.instances[1]
            _wait_for(lambda: bool(second.calls))
            self.assertTrue(first.closed)
            self.assertEqual(second.calls[0][1], "native.reconnect")
            self.assertEqual(second.calls[0][2], 1)
            scene = next(
                command for command in second.calls[0][0]
                if command["command"] == "set_scene"
            )
            self.assertEqual(scene["objects"][0]["position"], [9.0, 8.0, 7.0])
            first.on_event({
                "type": "actual", "channel": "wifi", "state_version": 99,
                "apply_result": "applied", "availability": "present",
                "profile_decoded": True, "enabled": True,
                "production_status": "running", "reason": "none",
                "production_epoch": 99,
            })
            first.on_frame("render", {"frame_sequence": 99})
            self.assertNotIn("wifi", state.snapshot()["native_truth"]["actual"])
            self.assertEqual(
                state.snapshot()["frames"]["render"], {"available": False}
            )
        state.close()

    def test_internal_terminal_is_sticky_and_never_auto_restarts(self) -> None:
        class RecordingBridge:
            instances: list["RecordingBridge"] = []

            def __init__(self, _executable, _config, on_event, _on_frame):
                self.on_event = on_event
                self.closed = False
                self.__class__.instances.append(self)

            @staticmethod
            def start() -> None:
                return None

            @staticmethod
            def submit_batch(commands, *, origin, revision):
                return list(range(1, len(commands) + 1))

            def close(self) -> None:
                self.closed = True

            @staticmethod
            def latest_frame(_source):
                return None

        state = CompositionEditorState()
        with (
            mock.patch.object(server_module, "NativeBridge", RecordingBridge),
            mock.patch.object(
                server_module, "NATIVE_RECONNECT_INITIAL_SECONDS", 0.01
            ),
        ):
            state.enable_native("unused", "unused")
            RecordingBridge.instances[0].on_event({
                "type": "terminal",
                "code": "HOST_RUNTIME_INTERNAL_TERMINAL",
            })
            time.sleep(0.05)
            self.assertEqual(len(RecordingBridge.instances), 1)
            self.assertEqual(
                state.snapshot()["backend"]["session_status"], "FAILED"
            )
        state.close()

    def test_remote_reconnect_retries_failed_fresh_child_and_close_stops(self) -> None:
        class FlakyBridge:
            instances: list["FlakyBridge"] = []

            def __init__(self, _executable, _config, on_event, _on_frame):
                self.on_event = on_event
                self.closed = False
                self.calls = 0
                self.__class__.instances.append(self)

            def start(self) -> None:
                if len(self.__class__.instances) == 2:
                    raise OSError("injected fresh-child start failure")

            def submit_batch(self, commands, *, origin, revision):
                self.calls += 1
                return list(range(1, len(commands) + 1))

            def close(self) -> None:
                self.closed = True

            @staticmethod
            def latest_frame(_source):
                return None

        state = CompositionEditorState()
        with (
            mock.patch.object(server_module, "NativeBridge", FlakyBridge),
            mock.patch.object(
                server_module, "NATIVE_RECONNECT_INITIAL_SECONDS", 0.01
            ),
            mock.patch.object(
                server_module, "NATIVE_RECONNECT_MAX_SECONDS", 0.01
            ),
        ):
            state.enable_native("unused", "unused")
            FlakyBridge.instances[0].on_event({
                "type": "terminal",
                "code": "HOST_RUNTIME_SESSION_TERMINAL",
            })
            _wait_for(lambda: len(FlakyBridge.instances) == 3)
            _wait_for(lambda: FlakyBridge.instances[2].calls == 1)
            self.assertTrue(FlakyBridge.instances[0].closed)
            self.assertTrue(FlakyBridge.instances[1].closed)
            self.assertFalse(FlakyBridge.instances[2].closed)
            self.assertTrue(any(
                event[1]["status"] == "NATIVE_RECONNECT_RETRY"
                for event in state._events
            ))
        state.close()
        self.assertTrue(FlakyBridge.instances[2].closed)
        self.assertTrue(
            state._native_reconnect_thread is None or
            not state._native_reconnect_thread.is_alive()
        )

    def test_close_interrupts_an_active_reconnect_retry_loop(self) -> None:
        class AlwaysFailFreshBridge:
            instances: list["AlwaysFailFreshBridge"] = []

            def __init__(self, _executable, _config, on_event, _on_frame):
                self.on_event = on_event
                self.closed = False
                self.__class__.instances.append(self)

            def start(self) -> None:
                if len(self.__class__.instances) > 1:
                    raise OSError("injected persistent reconnect failure")

            @staticmethod
            def submit_batch(commands, *, origin, revision):
                return list(range(1, len(commands) + 1))

            def close(self) -> None:
                self.closed = True

            @staticmethod
            def latest_frame(_source):
                return None

        state = CompositionEditorState()
        with (
            mock.patch.object(
                server_module, "NativeBridge", AlwaysFailFreshBridge
            ),
            mock.patch.object(
                server_module, "NATIVE_RECONNECT_INITIAL_SECONDS", 0.01
            ),
            mock.patch.object(
                server_module, "NATIVE_RECONNECT_MAX_SECONDS", 0.01
            ),
        ):
            state.enable_native("unused", "unused")
            AlwaysFailFreshBridge.instances[0].on_event({
                "type": "terminal",
                "code": "HOST_RUNTIME_CATALOG_TERMINAL",
            })
            _wait_for(lambda: len(AlwaysFailFreshBridge.instances) >= 3)
            state.close()
            instance_count = len(AlwaysFailFreshBridge.instances)
            time.sleep(0.05)
            self.assertEqual(
                len(AlwaysFailFreshBridge.instances), instance_count
            )
            self.assertTrue(all(
                bridge.closed for bridge in AlwaysFailFreshBridge.instances
            ))
            self.assertTrue(
                state._native_reconnect_thread is None or
                not state._native_reconnect_thread.is_alive()
            )

    def test_unreaped_terminal_blocks_replacement_and_preserves_state(
        self,
    ) -> None:
        class CountingBridge(NativeBridge):
            instances: list["CountingBridge"] = []

            def __init__(self, *args, **kwargs):
                super().__init__(*args, **kwargs)
                self.__class__.instances.append(self)

        kill_calls: list[int] = []

        def ignored_kill(process) -> None:
            kill_calls.append(process.pid)

        state = CompositionEditorState()
        process = None
        with tempfile.TemporaryDirectory() as raw_directory:
            executable, config = self._fake_child(
                Path(raw_directory), "raw_terminal_split_tail"
            )
            try:
                with (
                    mock.patch.object(
                        server_module, "NativeBridge", CountingBridge
                    ),
                    mock.patch.object(
                        native_bridge_module,
                        "NATIVE_TERMINAL_EXIT_TIMEOUT_SECONDS", 0.05,
                    ),
                    mock.patch.object(
                        native_bridge_module,
                        "NATIVE_TERMINAL_KILL_TIMEOUT_SECONDS", 0.05,
                    ),
                    mock.patch.object(
                        server_module, "NATIVE_RECONNECT_INITIAL_SECONDS", 0.01
                    ),
                    mock.patch.object(
                        native_bridge_module.subprocess.Popen,
                        "kill", ignored_kill,
                    ),
                ):
                    try:
                        state.enable_native(str(executable), str(config))
                    except NativeBridgeError as error:
                        self.assertEqual(
                            str(error), "NATIVE_PROCESS_SHUTDOWN"
                        )
                    _wait_for(lambda: state._native_cleanup_error is not None)
                    self.assertEqual(len(CountingBridge.instances), 1)
                    bridge = CountingBridge.instances[0]
                    process = bridge._process
                    self.assertIsNotNone(process)
                    self.assertIsNone(process.poll())
                    self.assertEqual(kill_calls, [process.pid])
                    generation = state._native_generation
                    snapshot = state.snapshot()
                    self.assertEqual(snapshot["revision"], 0)
                    self.assertEqual(
                        snapshot["backend"]["session_status"], "FAILED"
                    )
                    self.assertEqual(
                        snapshot["native_truth"],
                        _default_snapshot()["native_truth"],
                    )
                    time.sleep(0.05)
                    self.assertEqual(len(CountingBridge.instances), 1)
                    self.assertEqual(state._native_generation, generation)
                    with self.assertRaisesRegex(
                        NativeBridgeError, "^NATIVE_PROCESS_SHUTDOWN$"
                    ):
                        state.enable_native(str(executable), str(config))
            finally:
                if process is not None and process.poll() is None:
                    os.kill(process.pid, signal.SIGKILL)
                    process.wait(timeout=2.0)
                state.close()
        self.assertIsNone(state._native_retiring_bridge)
        self.assertIsNone(state._native_cleanup_error)

    def test_state_close_propagates_concurrent_stale_cleanup_failure(
        self,
    ) -> None:
        close_entered = threading.Event()
        release_close = threading.Event()

        class FailingStaleBridge:
            instances: list["FailingStaleBridge"] = []

            def __init__(self, _executable, _config, on_event, _on_frame):
                self.on_event = on_event
                self.fail_close = True
                self.closed = False
                self.__class__.instances.append(self)

            @staticmethod
            def start() -> None:
                return None

            @staticmethod
            def submit_batch(commands, *, origin, revision):
                return list(range(1, len(commands) + 1))

            def close(self) -> None:
                close_entered.set()
                if self.fail_close:
                    self.assert_release()
                    raise NativeBridgeError("NATIVE_PROCESS_SHUTDOWN")
                self.closed = True

            @staticmethod
            def assert_release() -> None:
                if not release_close.wait(2.0):
                    raise AssertionError("stale cleanup was not released")

            @staticmethod
            def latest_frame(_source):
                return None

        state = CompositionEditorState()
        close_errors: list[Exception] = []

        def close_state() -> None:
            try:
                state.close()
            except Exception as error:
                close_errors.append(error)

        with mock.patch.object(
            server_module, "NativeBridge", FailingStaleBridge
        ):
            state.enable_native("unused", "unused")
            stale = FailingStaleBridge.instances[0]
            stale.on_event({
                "type": "terminal",
                "code": "HOST_RUNTIME_TRANSPORT_TERMINAL",
            })
            self.assertTrue(close_entered.wait(1.0))
            close_thread = threading.Thread(target=close_state)
            close_thread.start()
            _wait_for(lambda: state._native_reconnect_stop.is_set())
            release_close.set()
            close_thread.join(2.0)
            self.assertFalse(close_thread.is_alive())
            self.assertEqual(
                [str(error) for error in close_errors],
                ["NATIVE_PROCESS_SHUTDOWN"],
            )
            self.assertEqual(len(FailingStaleBridge.instances), 1)
            self.assertIs(state._native_retiring_bridge, stale)
            self.assertIsNotNone(state._native_cleanup_error)
            stale.fail_close = False
            state.close()
            self.assertTrue(stale.closed)
            self.assertIsNone(state._native_retiring_bridge)
            self.assertIsNone(state._native_cleanup_error)

    def test_active_close_reserves_owner_until_enable_is_safe(self) -> None:
        close_entered = threading.Event()
        release_close = threading.Event()

        class BlockingCloseBridge:
            instances: list["BlockingCloseBridge"] = []

            def __init__(self, _executable, _config, _on_event, _on_frame):
                self.index = len(self.__class__.instances)
                self.closed = False
                self.__class__.instances.append(self)

            @staticmethod
            def start() -> None:
                return None

            @staticmethod
            def submit_batch(commands, *, origin, revision):
                return list(range(1, len(commands) + 1))

            def close(self) -> None:
                if self.index == 0:
                    close_entered.set()
                    if not release_close.wait(2.0):
                        raise AssertionError("active close was not released")
                self.closed = True

            @staticmethod
            def latest_frame(_source):
                return None

        state = CompositionEditorState()
        close_errors: list[Exception] = []

        def close_state() -> None:
            try:
                state.close()
            except Exception as error:
                close_errors.append(error)

        with mock.patch.object(
            server_module, "NativeBridge", BlockingCloseBridge
        ):
            state.enable_native("unused", "unused")
            first = BlockingCloseBridge.instances[0]
            close_thread = threading.Thread(target=close_state)
            close_thread.start()
            self.assertTrue(close_entered.wait(1.0))
            generation = state._native_generation
            self.assertIs(state._native_retiring_bridge, first)
            with self.assertRaisesRegex(
                NativeBridgeError, "^NATIVE_PROCESS_SHUTDOWN$"
            ):
                state.enable_native("unused", "unused")
            self.assertEqual(len(BlockingCloseBridge.instances), 1)
            self.assertEqual(state._native_generation, generation)
            release_close.set()
            close_thread.join(2.0)
            self.assertFalse(close_thread.is_alive())
            self.assertEqual(close_errors, [])
            self.assertTrue(first.closed)
            self.assertIsNone(state._native_retiring_bridge)
            state.enable_native("unused", "unused")
            self.assertEqual(len(BlockingCloseBridge.instances), 2)
        state.close()
        self.assertTrue(BlockingCloseBridge.instances[1].closed)

    def test_reconnect_scheduler_failure_retains_stale_owner(self) -> None:
        class RecordingBridge:
            instances: list["RecordingBridge"] = []

            def __init__(self, _executable, _config, on_event, _on_frame):
                self.on_event = on_event
                self.closed = False
                self.__class__.instances.append(self)

            @staticmethod
            def start() -> None:
                return None

            @staticmethod
            def submit_batch(commands, *, origin, revision):
                return list(range(1, len(commands) + 1))

            def close(self) -> None:
                self.closed = True

            @staticmethod
            def latest_frame(_source):
                return None

        class StartFailThread:
            @staticmethod
            def start() -> None:
                raise RuntimeError("injected reconnect thread start failure")

        failures = (
            mock.patch.object(
                server_module.threading, "Thread",
                side_effect=RuntimeError(
                    "injected reconnect thread constructor failure"
                ),
            ),
            mock.patch.object(
                server_module.threading, "Thread",
                return_value=StartFailThread(),
            ),
        )
        for failure in failures:
            with self.subTest(failure=failure):
                RecordingBridge.instances.clear()
                state = CompositionEditorState()
                with mock.patch.object(
                    server_module, "NativeBridge", RecordingBridge
                ):
                    state.enable_native("unused", "unused")
                    stale = RecordingBridge.instances[0]
                    with failure:
                        stale.on_event({
                            "type": "terminal",
                            "code": "HOST_RUNTIME_TRANSPORT_TERMINAL",
                        })
                    self.assertIs(state._native_bridge, stale)
                    self.assertIs(state._native_retiring_bridge, stale)
                    self.assertIsNone(state._native_reconnect_thread)
                    self.assertFalse(stale.closed)
                    self.assertEqual(
                        state.snapshot()["backend"]["session_status"],
                        "FAILED",
                    )
                    self.assertTrue(any(
                        event[1]["status"] == "NATIVE_RECONNECT_BLOCKED"
                        for event in state._events
                    ))
                    with self.assertRaisesRegex(
                        NativeBridgeError, "^NATIVE_PROCESS_SHUTDOWN$"
                    ):
                        state.enable_native("unused", "unused")
                    self.assertEqual(len(RecordingBridge.instances), 1)
                    state.close()
                    self.assertTrue(stale.closed)
                    self.assertIsNone(state._native_retiring_bridge)

    def test_old_enable_failure_cannot_claim_same_spec_rebind(self) -> None:
        old_start_entered = threading.Event()
        release_old_start = threading.Event()

        class DelayedStartBridge:
            instances: list["DelayedStartBridge"] = []

            def __init__(self, _executable, _config, _on_event, _on_frame):
                self.index = len(self.__class__.instances)
                self.closed = False
                self.__class__.instances.append(self)

            def start(self) -> None:
                if self.index == 0:
                    old_start_entered.set()
                    if not release_old_start.wait(2.0):
                        raise AssertionError("old start was not released")
                    raise OSError("injected old start failure")

            @staticmethod
            def submit_batch(commands, *, origin, revision):
                return list(range(1, len(commands) + 1))

            def close(self) -> None:
                self.closed = True

            @staticmethod
            def latest_frame(_source):
                return None

        state = CompositionEditorState()
        old_errors: list[Exception] = []

        def start_old() -> None:
            try:
                state.enable_native("same", "same")
            except Exception as error:
                old_errors.append(error)

        with mock.patch.object(
            server_module, "NativeBridge", DelayedStartBridge
        ):
            old_thread = threading.Thread(target=start_old)
            old_thread.start()
            self.assertTrue(old_start_entered.wait(1.0))
            state.close()
            state.enable_native("same", "same")
            replacement = DelayedStartBridge.instances[1]
            generation = state._native_generation
            release_old_start.set()
            old_thread.join(2.0)
            self.assertFalse(old_thread.is_alive())
            self.assertEqual(
                [str(error) for error in old_errors],
                ["injected old start failure"],
            )
            self.assertIs(state._native_bridge, replacement)
            self.assertEqual(state._native_generation, generation)
            self.assertFalse(any(
                event[1]["status"] == "NATIVE_RECONNECTING"
                for event in state._events
            ))
        state.close()
        self.assertTrue(replacement.closed)

    def test_synchronous_terminal_start_hands_off_one_bootstrap(
        self,
    ) -> None:
        fresh_ready = threading.Event()

        class TerminalOnStartBridge:
            instances: list["TerminalOnStartBridge"] = []

            def __init__(self, _executable, _config, on_event, _on_frame):
                self.on_event = on_event
                self.closed = False
                self.submit_calls: list[tuple[str, int]] = []
                self.index = len(self.__class__.instances)
                self.__class__.instances.append(self)

            def start(self) -> None:
                if self.index == 0:
                    self.on_event({
                        "type": "terminal",
                        "code": "HOST_RUNTIME_TRANSPORT_TERMINAL",
                    })
                    return
                self.on_event({"type": "service.ready"})
                self.on_event({
                    "type": "snapshot", "phase": "ready",
                    "control_ready": True, "catalog_generation": 1,
                    "media_bound": False, "media_mask": "none",
                })
                fresh_ready.set()

            def submit_batch(self, commands, *, origin, revision):
                self.submit_calls.append((origin, revision))
                if self.index == 0:
                    if not fresh_ready.wait(2.0):
                        raise AssertionError("fresh bridge did not become ready")
                    raise NativeBridgeError("NATIVE_TERMINAL")
                return list(range(1, len(commands) + 1))

            def close(self) -> None:
                self.closed = True

            @staticmethod
            def latest_frame(_source):
                return None

        state = CompositionEditorState()
        with (
            mock.patch.object(
                server_module, "NativeBridge", TerminalOnStartBridge
            ),
            mock.patch.object(
                server_module, "NATIVE_RECONNECT_INITIAL_SECONDS", 0.001
            ),
            mock.patch.object(
                server_module, "NATIVE_RECONNECT_MAX_SECONDS", 0.001
            ),
        ):
            state.enable_native("unused", "unused")
            self.assertEqual(len(TerminalOnStartBridge.instances), 2)
            first, replacement = TerminalOnStartBridge.instances
            self.assertTrue(first.closed)
            self.assertFalse(replacement.closed)
            self.assertEqual(first.submit_calls, [("native.bootstrap", 0)])
            self.assertEqual(
                replacement.submit_calls, [("native.reconnect", 0)]
            )
            self.assertEqual(
                state.snapshot()["backend"]["session_status"], "READY"
            )
            self.assertTrue(any(
                event[1]["status"] == "NATIVE_RECONNECTING"
                for event in state._events
            ))
            time.sleep(0.02)
            self.assertEqual(len(TerminalOnStartBridge.instances), 2)
        state.close()
        self.assertTrue(replacement.closed)

    def test_native_truth_refresh_is_deduplicated_and_pump_is_not_projected(
        self,
    ) -> None:
        class RecordingBridge:
            def __init__(self):
                self.calls: list[tuple[list[dict[str, object]], str, int]] = []

            def submit_batch(self, commands, *, origin, revision):
                self.calls.append((commands, origin, revision))
                return [len(self.calls)]

        state = CompositionEditorState()
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
        snapshot_event = {
            "type": "snapshot", "phase": "ready",
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
        }
        state._native_event({
            **snapshot_event, "request_id": 99,
            "browser_command": "native.bootstrap", "browser_revision": 0,
            "native_command": "snapshot", "native_batch_id": 90,
            "native_batch_index": 9, "native_batch_count": 9,
        })
        self.assertTrue(state._native_refresh_pending)
        state._native_event({**actual, "state_version": 2})
        self.assertEqual(len(bridge.calls), 1)
        state._native_event({
            "type": "error", "request_id": 1,
            "code": "SNAPSHOT_FAILED", "native_command": "snapshot",
            "browser_command": "native.refresh", "browser_revision": 0,
            "native_batch_id": 1, "native_batch_index": 1,
            "native_batch_count": 1,
        })
        self.assertFalse(state._native_refresh_pending)
        state._native_event({**actual, "state_version": 3})
        self.assertEqual(len(bridge.calls), 2)
        state._native_event({
            **snapshot_event, "request_id": 2,
            "browser_command": "native.refresh", "browser_revision": 0,
            "native_command": "snapshot", "native_batch_id": 2,
            "native_batch_index": 1, "native_batch_count": 1,
        })
        snapshot = state.snapshot()
        self.assertTrue(snapshot["backend"]["device_connected"])
        self.assertEqual(snapshot["backend"]["session_status"], "READY")
        self.assertEqual(snapshot["bindings"]["render_result_stream"], "BOUND")
        self.assertEqual(snapshot["bindings"]["camera_media_stream"], "BOUND")
        state._native_event({"type": "media.connected", "transport": "tcp"})
        self.assertEqual(len(bridge.calls), 3)
        self.assertEqual(bridge.calls[-1], (
            [{"command": "snapshot"}], "native.refresh", 0,
        ))

    def test_euler_complete_scene_and_intent_mapping(self) -> None:
        snapshot = _default_snapshot()
        snapshot["intents"]["render"].update(enabled=True, max_hz=1)
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
            commands_for_mutation("intent.render.set", snapshot),
            [
                {
                    "command": "set_presentation", "enabled": True,
                    "use_safety_frame": False, "target_width": 320,
                    "target_height": 240, "color_format": "rgba8888",
                    "depth_format": "implementation32",
                    "max_result_hz": 1, "offline_lease_ms": 1000,
                },
                {
                    "command": "set_render_media", "enabled": True,
                    "pixel_format": "rgba8888", "codec": "raw",
                    "width": 320, "height": 240, "fps_millihz": 1000,
                    "bitrate_bps": 0, "gop_frames": 1,
                    "request_key": True,
                },
            ],
        )
        self.assertEqual(
            [command["command"] for command in bootstrap_commands(snapshot)],
            [
                "asset.load_builtin", "set_scene", "set_presentation",
                "set_render_media", "set_camera_media", "set_wifi",
                "set_device", "snapshot",
            ],
        )

    def test_profile_intent_mapping_defends_static_bounds(self) -> None:
        snapshot = _default_snapshot()
        snapshot["intents"]["render"].update(enabled=True, max_hz=10)
        snapshot["intents"]["camera"].update(enabled=True, fps=30)
        snapshot["intents"]["telemetry"].update(
            enabled=True, sample_period_ms=10000, selected_fields=1023
        )
        snapshot["intents"]["device"].update(
            enabled=True, sample_period_ms=100, selected_fields=15
        )
        self.assertEqual(
            commands_for_mutation("intent.render.set", snapshot)[0][
                "max_result_hz"
            ],
            10,
        )
        self.assertEqual(
            commands_for_mutation("intent.camera.set", snapshot)[0][
                "fps_millihz"
            ],
            30000,
        )
        self.assertEqual(
            commands_for_mutation("intent.telemetry.set", snapshot)[0],
            {
                "command": "set_wifi", "enabled": True,
                "selected_fields": 1023, "sample_period_ms": 10000,
            },
        )
        self.assertEqual(
            commands_for_mutation("intent.device.set", snapshot)[0],
            {
                "command": "set_device", "enabled": True,
                "selected_fields": 15, "sample_period_ms": 100,
            },
        )

        invalid = (
            ("intent.render.set", "render", {"enabled": False, "max_hz": 0}),
            ("intent.render.set", "render", {"enabled": False, "max_hz": 11}),
            ("intent.camera.set", "camera", {"enabled": False, "fps": 31}),
            (
                "intent.telemetry.set", "telemetry",
                {"enabled": False, "sample_period_ms": 150,
                 "selected_fields": 1},
            ),
            (
                "intent.telemetry.set", "telemetry",
                {"enabled": False, "sample_period_ms": 100,
                 "selected_fields": 1024},
            ),
            (
                "intent.device.set", "device",
                {"enabled": False, "sample_period_ms": 150,
                 "selected_fields": 1},
            ),
            (
                "intent.device.set", "device",
                {"enabled": False, "sample_period_ms": 100,
                 "selected_fields": 16},
            ),
        )
        for command_name, intent_name, values in invalid:
            with self.subTest(command=command_name, values=values):
                candidate = _default_snapshot()
                candidate["intents"][intent_name].update(values)
                with self.assertRaisesRegex(
                    NativeBridgeError, "^INVALID_PROFILE_INTENT$"
                ):
                    commands_for_mutation(command_name, candidate)

    def test_profile_rejection_keeps_bound_native_session_alive(self) -> None:
        class BoundBridge:
            def __init__(self) -> None:
                self.calls: list[object] = []
                self.closed = False

            def submit_batch(self, *args, **kwargs):
                self.calls.append((args, kwargs))
                return [1]

            def close(self) -> None:
                self.closed = True

            @staticmethod
            def latest_frame(_source):
                return None

        state = CompositionEditorState()
        bridge = BoundBridge()
        state._native_bridge = bridge
        before = state.snapshot()
        event_count = state.event_count()
        invalid = (
            {"command": "intent.render.set", "enabled": False, "max_hz": 11},
            {"command": "intent.camera.set", "enabled": False, "fps": 31},
            {
                "command": "intent.telemetry.set", "enabled": False,
                "sample_period_ms": 150, "selected_fields": 1,
            },
            {
                "command": "intent.device.set", "enabled": False,
                "sample_period_ms": 100, "selected_fields": 16,
            },
        )
        for command in invalid:
            with self.subTest(command=command):
                with self.assertRaises(CommandError):
                    state.apply(command)
                self.assertEqual(state.snapshot(), before)
                self.assertEqual(state.event_count(), event_count)
                self.assertIs(state._native_bridge, bridge)
                self.assertEqual(bridge.calls, [])
                self.assertFalse(bridge.closed)

    def _run_frame_loop(
        self, chunks: list[bytes]
    ) -> tuple[NativeBridge, list[dict[str, object]], threading.Thread, socket.socket]:
        events: list[dict[str, object]] = []
        bridge = NativeBridge(
            "/bin/false", "/dev/null", events.append, lambda *_: None
        )
        receiver, sender = socket.socketpair()
        bridge._frame_socket = receiver
        bridge._lifecycle_state = "RUNNING"
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
        bridge._lifecycle_state = "RUNNING"
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
        self.assertEqual(len(encoded), 1470)
        self.assertEqual(
            hashlib.sha256(encoded).hexdigest(),
            "1a952b4ac154f456bd717868cefe8bbdc70299c30097e49711aeedf339de91ba",
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
