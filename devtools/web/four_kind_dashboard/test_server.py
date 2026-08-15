#!/usr/bin/env python3
"""Tests for the loopback-only four-kind dashboard server."""

from __future__ import annotations

import http.client
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest

from server import (
    DashboardState,
    MAX_REQUEST_BODY_BYTES,
    create_server,
    normalize_probe_event,
)


FAKE_PROBE = r'''#!/usr/bin/env python3
import json
import sys

if "--test" in sys.argv:
    test = sys.argv[sys.argv.index("--test") + 1]
else:
    test = sys.argv[-1]

def emit(value):
    print(json.dumps(value, separators=(",", ":")), flush=True)

emit({
    "type": "session",
    "layer": "session",
    "phase": "handshake",
    "status": "READY",
    "state": "READY",
    "failure": "NONE",
    "remote_identity_known": True,
    "identity_match": True,
    "session_id_known": True,
    "limits": {
        "max_frame_bytes": 256,
        "max_runtime_payload_bytes": 128,
        "max_registry_entries": 4
    },
    "ssid": "DO_NOT_DISPLAY_NETWORK",
    "ip": "198.51.100.24",
    "mac": "aa:bb:cc:dd:ee:ff",
    "peer_id": "FULL-PEER-ID-MUST-NOT-APPEAR",
    "session_id": "FULL-SESSION-ID-MUST-NOT-APPEAR"
})
emit({
    "type": "registry",
    "layer": "registry",
    "phase": "accepted",
    "status": "ACCEPTED",
    "state": "FIXTURE",
    "entries": 4,
    "capacity": 4,
    "source": "fixture"
})

selected = ("state", "stream", "action", "blob") if test == "all" else (test,)
for kind in selected:
    if kind == "state":
        emit({
            "type": "state",
            "layer": "primitive",
            "direction": "edge_to_host",
            "phase": "snapshot",
            "status": "ACCEPTED",
            "name": "EdgeState",
            "version": 3,
            "window_id": 11,
            "interface_present": True,
            "carrier_present": True,
            "wifi_operstate": "UP",
            "signal_bucket": "GOOD",
            "rx_bytes": 999,
            "temperature_millic": 52125,
            "temperature_uncertainty_millic": 3000,
            "raw_sysfs_temperature": 151,
            "verified": True
        })
    elif kind == "stream":
        emit({
            "type": "stream",
            "layer": "primitive",
            "direction": "edge_to_host",
            "phase": "sample",
            "status": "ACCEPTED",
            "name": "TelemetrySample",
            "epoch": 7,
            "sequence": 9,
            "source_time_ns": 123456789,
            "window_id": 11,
            "elapsed_ns": 987654321,
            "rx_bytes": 4096,
            "tx_bytes": 2048,
            "rx_packets": 40,
            "tx_packets": 20,
            "rx_dropped": 1,
            "tx_dropped": 2,
            "signal_bucket": "GOOD",
            "signal_mbm": -5500,
            "wifi_operstate": "UP",
            "temperature_millic": 52250,
            "temperature_uncertainty_millic": 3000,
            "verified": True
        })
    elif kind == "action":
        emit({
            "type": "action",
            "layer": "primitive",
            "direction": "host_to_edge",
            "phase": "result",
            "status": "SUCCEEDED",
            "operation": "telemetry.reset_window",
            "delivery_accepted": True,
            "edge_accepted": True,
            "executed_once": True,
            "result_code": "OK",
            "result_value": 1,
            "verified": True
        })
    elif kind == "blob":
        emit({
            "type": "blob",
            "layer": "primitive",
            "direction": "edge_to_host",
            "phase": "published",
            "status": "PUBLISHED",
            "label": "edge.capability.manifest",
            "content_size": 96,
            "integrity_verified": True,
            "published": True,
            "duplicate": False,
            "verified": True
        })

for kind in selected:
    emit({
        "type": "verification",
        "layer": "fixture",
        "phase": "verified",
        "status": "PASS",
        "test": kind,
        "result": "PASS",
        "checks_passed": 5,
        "checks_total": 5
    })
'''


INVALID_PROBE = r'''#!/usr/bin/env python3
print("not-json", flush=True)
'''


PASS_THEN_INVALID_PROBE = r'''#!/usr/bin/env python3
import json

print(json.dumps({
    "type": "verification",
    "layer": "fixture",
    "phase": "verified",
    "status": "PASS",
    "test": "state",
    "result": "PASS",
    "checks_passed": 1,
    "checks_total": 1
}), flush=True)
print("not-json", flush=True)
'''


CONFLICTING_VERDICT_PROBE = r'''#!/usr/bin/env python3
import json

def emit(result):
    print(json.dumps({
        "type": "verification",
        "layer": "fixture",
        "phase": "verified",
        "status": result,
        "test": "state",
        "result": result,
        "checks_passed": 1 if result == "PASS" else 0,
        "checks_total": 1
    }), flush=True)

emit("FAIL")
emit("PASS")
'''


HUGE_INTEGER_PROBE = r'''#!/usr/bin/env python3
print('{"type":"state","name":"EdgeState","version":' + ('1' * 5000) + '}', flush=True)
'''


INHERITED_STDOUT_PROBE = r'''#!/usr/bin/env python3
import subprocess
import sys

subprocess.Popen([sys.executable, "-c", "import time; time.sleep(10)"])
'''


class RunningDashboard:
    def __init__(
        self,
        command: list[str],
        event_capacity: int = 512,
        probe_timeout_seconds: float = 120.0,
    ):
        self.server, self.state = create_server(
            command,
            event_capacity=event_capacity,
            probe_timeout_seconds=probe_timeout_seconds,
        )
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self) -> "RunningDashboard":
        self.thread.start()
        return self

    def __exit__(self, _type, _value, _traceback) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2.0)
        self.state.shutdown()

    @property
    def port(self) -> int:
        return self.server.server_port

    @property
    def origin(self) -> str:
        return self.server.expected_origin

    def request(
        self,
        method: str,
        path: str,
        body: bytes | None = None,
        headers: dict[str, str] | None = None,
    ) -> tuple[int, dict[str, str], bytes]:
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3.0)
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        content = response.read()
        result = (
            response.status,
            {key.lower(): value for key, value in response.getheaders()},
            content,
        )
        connection.close()
        return result

    def post(
        self,
        path: str,
        value: dict[str, object],
        *,
        token: str | None = None,
        origin: str | None = None,
    ) -> tuple[int, dict[str, str], bytes]:
        body = json.dumps(value).encode("utf-8")
        headers = {
            "Content-Type": "application/json",
            "Origin": self.origin if origin is None else origin,
            "X-Meiso-CSRF": self.state.csrf_token if token is None else token,
        }
        return self.request("POST", path, body, headers)

    def wait_for_overall(self, expected: str, timeout: float = 5.0) -> dict[str, object]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            snapshot = self.state.snapshot()
            if snapshot["overall"] == expected:
                return snapshot
            time.sleep(0.02)
        self.fail_snapshot(expected)
        raise AssertionError("unreachable")

    def fail_snapshot(self, expected: str) -> None:
        raise AssertionError(f"dashboard did not reach {expected}: {self.state.snapshot()!r}")


class DashboardServerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.fake_path = Path(self.temporary.name) / "fake_probe.py"
        self.fake_path.write_text(FAKE_PROBE, encoding="utf-8")
        self.command = [sys.executable, str(self.fake_path), "{test}"]

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_static_assets_and_security_headers(self) -> None:
        with RunningDashboard(self.command) as dashboard:
            self.assertEqual(dashboard.server.server_address[0], "127.0.0.1")
            self.assertGreater(dashboard.port, 0)
            status, headers, body = dashboard.request("GET", "/")
            self.assertEqual(status, 200)
            self.assertIn(b"K230D", body)
            self.assertIn(dashboard.state.csrf_token.encode("ascii"), body)
            self.assertNotIn(b"__MEISO_CSRF_TOKEN__", body)
            self.assertEqual(headers["cache-control"], "no-store, max-age=0")
            self.assertEqual(headers["x-content-type-options"], "nosniff")
            self.assertEqual(headers["x-frame-options"], "DENY")
            self.assertIn("frame-ancestors 'none'", headers["content-security-policy"])
            self.assertNotIn("access-control-allow-origin", headers)

            status, _, _ = dashboard.request("GET", "/../server.py")
            self.assertEqual(status, 404)

    def test_host_origin_and_csrf_are_required(self) -> None:
        with RunningDashboard(self.command) as dashboard:
            status, _, _ = dashboard.request(
                "GET",
                "/api/v1/snapshot",
                headers={"Host": "evil.invalid"},
            )
            self.assertEqual(status, 421)

            body = b'{"kind":"all"}'
            status, _, _ = dashboard.request(
                "POST",
                "/api/v1/tests",
                body,
                {"Content-Type": "application/json"},
            )
            self.assertEqual(status, 403)

            status, _, _ = dashboard.post(
                "/api/v1/tests",
                {"kind": "all"},
                origin="http://evil.invalid",
            )
            self.assertEqual(status, 403)

            status, _, _ = dashboard.post(
                "/api/v1/tests",
                {"kind": "all"},
                token="incorrect",
            )
            self.assertEqual(status, 403)

    def test_all_semantics_and_sensitive_probe_fields_are_not_retained(self) -> None:
        with RunningDashboard(self.command) as dashboard:
            status, _, body = dashboard.post("/api/v1/tests", {"kind": "all"})
            self.assertEqual(status, 202, body)
            snapshot = dashboard.wait_for_overall("PASS")
            self.assertEqual(snapshot["session"]["state"], "READY")
            self.assertTrue(snapshot["session"]["identity_match"])
            self.assertEqual(snapshot["session"]["limits"]["max_frame_bytes"], 256)
            self.assertEqual(snapshot["registry"]["state"], "FIXTURE")
            state = snapshot["semantics"]["state"]
            stream = snapshot["semantics"]["stream"]
            self.assertEqual(state["name"], "EdgeState")
            self.assertEqual(state["window_id"], 11)
            self.assertTrue(state["interface_present"])
            self.assertTrue(state["carrier_present"])
            self.assertEqual(state["temperature_millic"], 52125)
            self.assertNotIn("wifi_operstate", state)
            self.assertNotIn("signal_bucket", state)
            self.assertNotIn("rx_bytes", state)
            self.assertEqual(stream["name"], "TelemetrySample")
            self.assertEqual(stream["window_id"], 11)
            self.assertEqual(stream["elapsed_ns"], 987654321)
            self.assertEqual(stream["rx_bytes"], 4096)
            self.assertEqual(stream["tx_bytes"], 2048)
            self.assertEqual(stream["rx_packets"], 40)
            self.assertEqual(stream["tx_packets"], 20)
            self.assertEqual(stream["rx_dropped"], 1)
            self.assertEqual(stream["tx_dropped"], 2)
            self.assertEqual(stream["signal_bucket"], "GOOD")
            self.assertNotIn("signal_mbm", stream)
            self.assertNotIn("wifi_operstate", stream)
            self.assertEqual(snapshot["sensors"][0]["name"], "wifi_network_telemetry")
            self.assertEqual(snapshot["sensors"][0]["kind"], "network endpoint")
            self.assertEqual(snapshot["sensors"][1]["name"], "k230_on_die_temperature")
            self.assertEqual(snapshot["sensors"][1]["status"], "VERIFIED")
            self.assertEqual(snapshot["sensors"][2]["status"], "UNAVAILABLE")
            self.assertEqual(snapshot["sensors"][3]["status"], "UNAVAILABLE")
            self.assertEqual(snapshot["sensors"][4]["status"], "UNVERIFIED")
            self.assertEqual(snapshot["sensors"][5]["status"], "UNVERIFIED")
            for kind in ("state", "stream", "action", "blob"):
                self.assertEqual(snapshot["semantics"][kind]["status"], "PASS")
                self.assertTrue(snapshot["semantics"][kind]["verified"])

            status, headers, body = dashboard.request("GET", "/api/v1/events?after=0&limit=128")
            self.assertEqual(status, 200)
            self.assertNotIn("access-control-allow-origin", headers)
            status2, _, snapshot_body = dashboard.request("GET", "/api/v1/snapshot")
            self.assertEqual(status2, 200)
            combined = body + snapshot_body
            for forbidden in (
                b"DO_NOT_DISPLAY_NETWORK",
                b"198.51.100.24",
                b"aa:bb:cc:dd:ee:ff",
                b"FULL-PEER-ID-MUST-NOT-APPEAR",
                b"FULL-SESSION-ID-MUST-NOT-APPEAR",
                b"raw_sysfs_temperature",
            ):
                self.assertNotIn(forbidden, combined)

    def test_body_limit_and_exact_test_shape(self) -> None:
        with RunningDashboard(self.command) as dashboard:
            headers = {
                "Content-Type": "application/json",
                "Origin": dashboard.origin,
                "X-Meiso-CSRF": dashboard.state.csrf_token,
            }
            status, _, _ = dashboard.request(
                "POST",
                "/api/v1/tests",
                b"x" * (MAX_REQUEST_BODY_BYTES + 1),
                headers,
            )
            self.assertEqual(status, 413)

            status, _, _ = dashboard.post(
                "/api/v1/tests",
                {"kind": "all", "payload": "not allowed"},
            )
            self.assertEqual(status, 400)

    def test_probe_lifecycle_control_surface_is_not_exposed(self) -> None:
        with RunningDashboard(self.command) as dashboard:
            status, _, body = dashboard.request("GET", "/")
            self.assertEqual(status, 200)
            self.assertNotIn(b"close-session", body)
            status, _, _ = dashboard.post("/api/v1/session/close", {})
            self.assertEqual(status, 404)

    def test_invalid_probe_output_fails_closed(self) -> None:
        invalid_path = Path(self.temporary.name) / "invalid_probe.py"
        invalid_path.write_text(INVALID_PROBE, encoding="utf-8")
        with RunningDashboard([sys.executable, str(invalid_path), "{test}"]) as dashboard:
            status, _, _ = dashboard.post("/api/v1/tests", {"kind": "state"})
            self.assertEqual(status, 202)
            snapshot = dashboard.wait_for_overall("FAIL")
            self.assertEqual(snapshot["semantics"]["state"]["status"], "FAIL")

    def test_malformed_output_revokes_an_earlier_pass(self) -> None:
        invalid_path = Path(self.temporary.name) / "pass_then_invalid_probe.py"
        invalid_path.write_text(PASS_THEN_INVALID_PROBE, encoding="utf-8")
        with RunningDashboard([sys.executable, str(invalid_path), "{test}"]) as dashboard:
            status, _, _ = dashboard.post("/api/v1/tests", {"kind": "state"})
            self.assertEqual(status, 202)
            snapshot = dashboard.wait_for_overall("FAIL")
            self.assertEqual(snapshot["semantics"]["state"]["status"], "FAIL")
            self.assertFalse(snapshot["semantics"]["state"]["verified"])

    def test_failure_verdict_is_latched_against_later_pass(self) -> None:
        conflict_path = Path(self.temporary.name) / "conflicting_verdict_probe.py"
        conflict_path.write_text(CONFLICTING_VERDICT_PROBE, encoding="utf-8")
        with RunningDashboard([sys.executable, str(conflict_path), "{test}"]) as dashboard:
            status, _, _ = dashboard.post("/api/v1/tests", {"kind": "state"})
            self.assertEqual(status, 202)
            snapshot = dashboard.wait_for_overall("FAIL")
            self.assertEqual(snapshot["semantics"]["state"]["status"], "FAIL")
            self.assertFalse(snapshot["semantics"]["state"]["verified"])

    def test_duplicate_pass_verdict_fails_closed(self) -> None:
        duplicate_path = Path(self.temporary.name) / "duplicate_verdict_probe.py"
        duplicate_path.write_text(
            CONFLICTING_VERDICT_PROBE.replace('emit("FAIL")', 'emit("PASS")'),
            encoding="utf-8",
        )
        with RunningDashboard([sys.executable, str(duplicate_path), "{test}"]) as dashboard:
            status, _, _ = dashboard.post("/api/v1/tests", {"kind": "state"})
            self.assertEqual(status, 202)
            snapshot = dashboard.wait_for_overall("FAIL")
            self.assertEqual(snapshot["semantics"]["state"]["status"], "FAIL")
            self.assertFalse(snapshot["semantics"]["state"]["verified"])

    def test_json_integer_limit_error_fails_closed(self) -> None:
        huge_path = Path(self.temporary.name) / "huge_integer_probe.py"
        huge_path.write_text(HUGE_INTEGER_PROBE, encoding="utf-8")
        with RunningDashboard([sys.executable, str(huge_path), "{test}"]) as dashboard:
            status, _, _ = dashboard.post("/api/v1/tests", {"kind": "state"})
            self.assertEqual(status, 202)
            snapshot = dashboard.wait_for_overall("FAIL")
            self.assertFalse(snapshot["probe"]["running"])
            self.assertEqual(snapshot["semantics"]["state"]["status"], "FAIL")

    def test_probe_timeout_kills_process_group_and_fails_closed(self) -> None:
        inherited_path = Path(self.temporary.name) / "inherited_stdout_probe.py"
        inherited_path.write_text(INHERITED_STDOUT_PROBE, encoding="utf-8")
        command = [sys.executable, str(inherited_path), "{test}"]
        with RunningDashboard(command, probe_timeout_seconds=0.1) as dashboard:
            status, _, _ = dashboard.post("/api/v1/tests", {"kind": "state"})
            self.assertEqual(status, 202)
            snapshot = dashboard.wait_for_overall("FAIL")
            self.assertFalse(snapshot["probe"]["running"])
            self.assertEqual(snapshot["semantics"]["state"]["status"], "FAIL")

    def test_event_ring_is_bounded(self) -> None:
        state = DashboardState(["unused-probe"], event_capacity=4)
        try:
            for index in range(10):
                state._process_event(
                    {
                        "type": "stream",
                        "layer": "primitive",
                        "phase": "sample",
                        "status": "ACCEPTED",
                        "sequence": index,
                    }
                )
            response = state.events_after(0, 128)
            self.assertEqual(len(response["events"]), 4)
            self.assertIsNotNone(response["dropped_before"])
        finally:
            state.shutdown()

    def test_normalizer_drops_unknown_fields_and_invalid_enums(self) -> None:
        event = normalize_probe_event(
            {
                "type": "state",
                "name": "EdgeState",
                "direction": "edge_to_host",
                "phase": "snapshot",
                "version": 3,
                "window_id": 4,
                "interface_present": True,
                "carrier_present": True,
                "wifi_operstate": "UP",
                "signal_bucket": "GOOD",
                "rx_bytes": 64,
                "ssid": "private",
                "ip": "203.0.113.9",
                "peer_id": "full",
                "status": "made-up",
            }
        )
        self.assertEqual(
            event,
            {
                "type": "state",
                "direction": "edge_to_host",
                "phase": "snapshot",
                "name": "EdgeState",
                "version": 3,
                "window_id": 4,
                "interface_present": True,
                "carrier_present": True,
            },
        )
        self.assertIsNone(
            normalize_probe_event(
                {
                    "type": "state",
                    "name": "EdgeState",
                    "direction": "edge_to_host",
                    "phase": "snapshot",
                    "version": 3,
                    "window_id": 4,
                    "interface_present": True,
                    "carrier_present": True,
                    "temperature_millic": 52_000,
                    "temperature_uncertainty_millic": 2_500,
                }
            )
        )
        self.assertIsNone(
            normalize_probe_event(
                {
                    "type": "verification",
                    "status": "PASS",
                    "test": "state",
                    "result": "PASS",
                    "checks_passed": 1,
                    "checks_total": 2,
                }
            )
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
