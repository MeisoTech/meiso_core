#!/usr/bin/env python3
"""Tests for the loopback Composition Editor recorded/local boundary."""

from __future__ import annotations

import http.client
import json
import threading
import time
import unittest

from server import CommandError, CompositionEditorState, create_server


class RunningCompositionEditor:
    def __init__(self, *, event_capacity: int = 256):
        self.server, self.state = create_server(event_capacity=event_capacity)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, _type, _value, _traceback):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def request(self, method: str, path: str, *, headers=None, body=None):
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.server.server_port, timeout=5
        )
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        content = response.read()
        result = response.status, dict(response.getheaders()), content
        connection.close()
        return result

    def get_json(self, path: str):
        status, headers, body = self.request("GET", path)
        return status, headers, json.loads(body)

    def post(self, value, *, origin=None, csrf=None, content_type="application/json"):
        body = json.dumps(value, separators=(",", ":")).encode("utf-8")
        return self.post_raw(
            body, origin=origin, csrf=csrf, content_type=content_type
        )

    def post_raw(
        self, body: bytes, *, origin=None, csrf=None, content_type="application/json"
    ):
        headers = {
            "Content-Type": content_type,
            "Content-Length": str(len(body)),
            "Origin": origin or self.server.expected_origin,
            "X-Meiso-CSRF": csrf or self.state.csrf_token,
        }
        return self.request("POST", "/api/v1/command", headers=headers, body=body)

    def sse_event(self, *, after=0):
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.server.server_port, timeout=5
        )
        connection.request("GET", f"/api/v1/events?after={after}")
        response = connection.getresponse()
        if response.status != 200:
            raise AssertionError(f"unexpected SSE status {response.status}")
        event = {}
        while "data" not in event:
            line = response.readline().decode("utf-8").rstrip("\r\n")
            if line.startswith("id: "):
                event["id"] = int(line[4:])
            elif line == "event: composition-editor":
                event["name"] = "composition-editor"
            elif line.startswith("data: "):
                event["data"] = json.loads(line[6:])
        connection.close()
        return event


class CompositionEditorTests(unittest.TestCase):
    def test_initial_snapshot_is_explicitly_local_and_unbound(self):
        with RunningCompositionEditor() as editor:
            status, headers, snapshot = editor.get_json("/api/v1/snapshot")
            self.assertEqual(status, 200)
            self.assertEqual(headers["Cache-Control"], "no-store")
            self.assertEqual(snapshot["backend"], {
                "mode": "RECORDED_LOCAL",
                "device_connected": False,
                "native_host_bound": False,
                "session_status": "ABSENT",
                "catalog_status": "ABSENT",
                "profile": None,
                "proof": "LOCAL_DRAFT_ONLY",
            })
            self.assertEqual(snapshot["bindings"], {
                "render_result_stream": "UNBOUND",
                "camera_media_stream": "UNSUPPORTED",
                "wifi_telemetry": "UNBOUND",
            })
            self.assertTrue(all(
                value is None for value in snapshot["protocol_evidence"].values()
            ))
            self.assertEqual(snapshot["scene"]["objects"][0]["asset"], "builtin:cube")
            self.assertNotIn("object_id", json.dumps(snapshot))

    def test_complete_local_scene_and_intent_editing(self):
        with RunningCompositionEditor() as editor:
            commands = [
                {"command": "scene.object.add", "asset": "builtin:cube"},
                {
                    "command": "scene.object.transform",
                    "draft_object": 2,
                    "position": [1.5, -2, 3],
                    "rotation_deg": [10, 20, 30],
                    "scale": [0.5, 2, 1],
                },
                {
                    "command": "scene.camera.set",
                    "position": [7, 5, 9],
                    "rotation_deg": [-15, 40, 0],
                    "vertical_fov_deg": 63,
                    "near": 0.2,
                    "far": 240,
                },
                {"command": "intent.render.set", "enabled": False, "max_hz": 24},
                {"command": "intent.camera.set", "enabled": True, "fps": 30},
                {
                    "command": "intent.telemetry.set",
                    "enabled": True,
                    "sample_period_ms": 500,
                    "selected_fields": 63,
                },
                {"command": "scene.object.select", "draft_object": 1},
                {"command": "scene.object.delete", "draft_object": 1},
            ]
            for revision, command in enumerate(commands, start=1):
                status, _, body = editor.post(command)
                self.assertEqual(status, 202, body)
                receipt = json.loads(body)
                self.assertEqual(receipt["revision"], revision)

            status, _, snapshot = editor.get_json("/api/v1/snapshot")
            self.assertEqual(status, 200)
            self.assertEqual(snapshot["revision"], len(commands))
            self.assertEqual(len(snapshot["scene"]["objects"]), 1)
            object_value = snapshot["scene"]["objects"][0]
            self.assertEqual(object_value["draft_object"], 2)
            self.assertEqual(object_value["position"], [1.5, -2.0, 3.0])
            self.assertEqual(object_value["rotation_deg"], [10.0, 20.0, 30.0])
            self.assertEqual(object_value["scale"], [0.5, 2.0, 1.0])
            self.assertEqual(snapshot["scene"]["camera"]["vertical_fov_deg"], 63.0)
            self.assertEqual(snapshot["intents"]["render"]["max_hz"], 24)
            self.assertEqual(snapshot["intents"]["camera"]["status"], "MEDIA_ROUTE_UNSUPPORTED")
            self.assertEqual(snapshot["intents"]["telemetry"]["selected_fields"], 63)
            self.assertFalse(snapshot["backend"]["device_connected"])
            self.assertTrue(all(
                value is None for value in snapshot["protocol_evidence"].values()
            ))

    def test_sse_is_sticky_and_snapshot_refresh_is_read_only(self):
        with RunningCompositionEditor() as editor:
            initial = editor.sse_event(after=0)
            self.assertEqual(initial["id"], 1)
            self.assertEqual(initial["data"]["command"], "bootstrap")
            status, _, receipt = editor.post({
                "command": "scene.object.add", "asset": "builtin:cube"
            })
            self.assertEqual(status, 202)
            receipt = json.loads(receipt)
            update = editor.sse_event(after=initial["id"])
            self.assertEqual(update["id"], receipt["event_id"])
            self.assertEqual(update["data"]["snapshot"]["revision"], 1)
            before_events = editor.state.event_count()
            for _ in range(3):
                status, _, snapshot = editor.get_json("/api/v1/snapshot")
                self.assertEqual(status, 200)
                self.assertEqual(snapshot["revision"], 1)
            self.assertEqual(editor.state.event_count(), before_events)

    def test_invalid_commands_have_no_partial_mutation(self):
        with RunningCompositionEditor() as editor:
            before = editor.state.snapshot()
            event_count = editor.state.event_count()
            invalid = [
                ({"command": "scene.object.add", "asset": "local-secret.glb"}, 422),
                ({"command": "scene.object.add", "asset": "builtin:cube", "id": 9}, 422),
                ({
                    "command": "scene.object.transform", "draft_object": 1,
                    "position": [8, 9, 10], "rotation_deg": [45, 90, 180],
                    "scale": [1, 0, 1],
                }, 422),
                ({
                    "command": "scene.camera.set", "position": [0, 0, 0],
                    "rotation_deg": [0, 0, 0], "vertical_fov_deg": 50,
                    "near": 10, "far": 1,
                }, 422),
                ({"command": "intent.camera.set", "enabled": True, "fps": 0}, 422),
                ({"command": "unknown"}, 422),
            ]
            for command, expected_status in invalid:
                status, _, _ = editor.post(command)
                self.assertEqual(status, expected_status)
                self.assertEqual(editor.state.snapshot(), before)
                self.assertEqual(editor.state.event_count(), event_count)

            status, _, _ = editor.post(
                {"command": "scene.object.add", "asset": "builtin:cube"},
                origin="http://127.0.0.1:9",
            )
            self.assertEqual(status, 403)
            status, _, _ = editor.post(
                {"command": "scene.object.add", "asset": "builtin:cube"},
                csrf="wrong",
            )
            self.assertEqual(status, 403)
            self.assertEqual(editor.state.snapshot(), before)

    def test_event_ring_is_bounded_and_keeps_latest_sticky_truth(self):
        state = CompositionEditorState(event_capacity=4)
        for _ in range(9):
            state.apply({"command": "scene.object.select", "draft_object": 1})
        self.assertEqual(state.event_count(), 4)
        events = state.events_after(0, 0)
        self.assertEqual([event_id for event_id, _ in events], [7, 8, 9, 10])
        self.assertEqual(events[-1][1]["snapshot"]["revision"], 9)
        self.assertEqual(state.snapshot()["revision"], 9)

    def test_control_body_exact_2048_and_2049_boundary(self):
        with RunningCompositionEditor() as editor:
            base = json.dumps(
                {"command": "scene.object.select", "draft_object": 1},
                separators=(",", ":"),
            ).encode("utf-8")
            exact = base + b" " * (2048 - len(base))
            self.assertEqual(len(exact), 2048)
            status, _, body = editor.post_raw(exact)
            self.assertEqual(status, 202, body)
            self.assertEqual(editor.state.snapshot()["revision"], 1)

            before = editor.state.snapshot()
            event_count = editor.state.event_count()
            status, _, _ = editor.post_raw(exact + b" ")
            self.assertEqual(status, 413)
            self.assertEqual(editor.state.snapshot(), before)
            self.assertEqual(editor.state.event_count(), event_count)

    def test_object_capacity_exact_32_and_33_boundary(self):
        state = CompositionEditorState()
        for _ in range(31):
            state.apply({"command": "scene.object.add", "asset": "builtin:cube"})
        self.assertEqual(len(state.snapshot()["scene"]["objects"]), 32)
        before = state.snapshot()
        event_count = state.event_count()
        with self.assertRaisesRegex(CommandError, "^OBJECT_CAPACITY$"):
            state.apply({"command": "scene.object.add", "asset": "builtin:cube"})
        self.assertEqual(state.snapshot(), before)
        self.assertEqual(state.event_count(), event_count)

    def test_sse_batch_exact_64_plus_37_without_gap(self):
        state = CompositionEditorState(event_capacity=256)
        for _ in range(100):
            state.apply({"command": "scene.object.select", "draft_object": 1})
        first = state.events_after(0, 0)
        self.assertEqual(len(first), 64)
        self.assertEqual([event_id for event_id, _ in first], list(range(1, 65)))
        second = state.events_after(first[-1][0], 0)
        self.assertEqual(len(second), 37)
        self.assertEqual(
            [event_id for event_id, _ in second], list(range(65, 102))
        )
        self.assertEqual(second[-1][1]["snapshot"]["revision"], 100)

    def test_static_boundary_and_headers(self):
        with RunningCompositionEditor() as editor:
            status, headers, index = editor.request("GET", "/")
            self.assertEqual(status, 200)
            self.assertIn(b"Composition Editor", index)
            self.assertNotIn(b"__MEISO_CSRF_TOKEN__", index)
            self.assertIn(b'name="meiso-csrf"', index)
            self.assertIn(b'min="0.001" step="0.001" aria-label="scale x"', index)
            self.assertIn(b'id="camera-far" type="number" min="0.002" step="0.001"', index)
            self.assertIn("default-src 'self'", headers["Content-Security-Policy"])
            for path in ["/static/app.js", "/static/model.mjs", "/static/style.css"]:
                status, _, body = editor.request("GET", path)
                self.assertEqual(status, 200)
                self.assertGreater(len(body), 10)
            status, _, _ = editor.request("GET", "/static/../server.py")
            self.assertEqual(status, 404)


if __name__ == "__main__":
    unittest.main(verbosity=2)
