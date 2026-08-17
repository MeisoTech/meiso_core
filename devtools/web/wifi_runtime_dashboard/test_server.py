#!/usr/bin/env python3
"""Deterministic tests for the persistent Wi-Fi Runtime dashboard boundary."""

from __future__ import annotations

import http.client
import json
from pathlib import Path
import stat
import tempfile
import threading
import time
import unittest

from server import create_server, normalize_host_event


FAKE_HOST = r'''#!/usr/bin/env python3
import json
import sys

if len(sys.argv) != 4 or sys.argv[1] != "--config" or sys.argv[3] != "--interactive":
    raise SystemExit(2)

def emit(value):
    print(json.dumps(value, separators=(",", ":")), flush=True)

def actual(version, enabled, fields, period):
    emit({"type":"actual","status":"RUNNING" if enabled else "STOPPED",
          "state_version":version,"observed_desired_version":version,
          "apply_result":"APPLIED",
          "effective_enabled":enabled,"data_active":enabled,
          "selected_fields":fields if enabled else 0,
          "sample_period_ms":period if enabled else 0,
          "production_epoch_known":enabled})

emit({"type":"bootstrap","status":"CONNECTED","locator_redacted":True,
      "connect_ipv4":"192.0.2.10","config":sys.argv[2],"ssid":"DO_NOT_SHOW"})
emit({"type":"session","status":"READY","remote_identity_known":True,
      "identity_match":True,"session_id_known":True,"peer_id":"DO_NOT_SHOW",
      "limits":{"max_frame_bytes":4096,"max_runtime_payload_bytes":2048,
                "max_registry_entries":16}})
emit({"type":"catalog","status":"ACCEPTED","generation":1,
      "endpoint_count":1,"route_count":3,"endpoint_id":"DO_NOT_SHOW",
      "capability":{"supported_fields":1023,"min_period_ms":100,
                    "max_period_ms":10000,"period_step_ms":100}})
emit({"type":"control","status":"READY","mode":"INTERACTIVE"})

version = 0
ordinal = 0
sequence = 0
enabled = False
fields = 3
period = 1000
for line in sys.stdin:
    try:
        command = json.loads(line)
    except Exception:
        emit({"type":"control","status":"REJECTED","reason":"INVALID_COMMAND"})
        continue
    if (list(command) == ["command", "enabled", "selected_fields", "sample_period_ms"] and command["command"] == "apply"):
        enabled = command["enabled"]
        fields = command["selected_fields"]
        period = command["sample_period_ms"]
        version += 1
        emit({"type":"desired","status":"PUBLISHED","version":version,
              "enabled":enabled,"selected_fields":fields,"sample_period_ms":period})
        emit({"type":"desired_delivery","status":"ACCEPTED","version":version})
        actual(version, enabled, fields, period)
        if enabled:
            for offset in range(3):
                ordinal += 1
                sequence += 1
                present = fields
                sample = {"type":"sample","ordinal":ordinal,"sequence":sequence,
                          "selected_fields":fields,"present_fields":present,
                          "gap":False,"missing_before":0,
                          "link_state":True if fields & 1 else None,
                          "signal_mbm":-5500-offset*17 if fields & 2 else None,
                          "source_time_ns":1000000000 + sequence*period*1000000}
                names = ["rx_bytes","tx_bytes","rx_packets","tx_packets",
                         "rx_dropped","tx_dropped","rx_errors","tx_errors"]
                for index, name in enumerate(names):
                    sample[name] = sequence * (index + 1) * 10 if fields & (1 << (index + 2)) else None
                emit(sample)
        emit({"type":"control","command":"APPLY","status":"APPLIED",
              "desired_version":version,"enabled":enabled})
    elif list(command) == ["command"] and command["command"] == "close":
        if enabled:
            version += 1
            emit({"type":"desired","status":"PUBLISHED","version":version,
                  "enabled":False,"selected_fields":fields,"sample_period_ms":period})
            emit({"type":"desired_delivery","status":"ACCEPTED","version":version})
            actual(version, False, fields, period)
        emit({"type":"session","status":"CLOSED","reason":"NORMAL"})
        emit({"type":"control","command":"CLOSE","status":"COMPLETE"})
        raise SystemExit(0)
    else:
        emit({"type":"control","status":"REJECTED","reason":"INVALID_COMMAND"})
'''


class RunningDashboard:
    def __init__(self, host: str, config: str, *, startup_timeout: float = 5.0):
        self.server, self.state = create_server(
            host,
            config,
            startup_timeout_seconds=startup_timeout,
            close_timeout_seconds=2.0,
        )
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, _type, _value, _traceback):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.state.shutdown()

    def request(self, method: str, path: str, headers=None, body=None):
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.server.server_port, timeout=5
        )
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        content = response.read()
        result = response.status, dict(response.getheaders()), content
        connection.close()
        return result

    def post(self, path: str, value=None):
        headers = {
            "Origin": self.server.expected_origin,
            "X-Meiso-CSRF": self.state.csrf_token,
        }
        body = None
        if value is not None:
            body = json.dumps(value, separators=(",", ":")).encode("utf-8")
            headers["Content-Type"] = "application/json"
            headers["Content-Length"] = str(len(body))
        else:
            headers["Content-Length"] = "0"
        return self.request("POST", path, headers, body)

    def wait_for(self, predicate, message="condition was not reached"):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            with self.state._condition:
                events = [dict(event) for _, event in self.state._events]
                process = self.state._process
            if predicate(events, process):
                return events
            time.sleep(0.01)
        raise AssertionError(message)

    def wait_catalog(self):
        return self.wait_for(
            lambda events, _process: any(
                event["type"] == "control" and event["status"] == "READY"
                for event in events
            ),
            "fake Host did not become interactive",
        )

    def wait_finished(self):
        return self.wait_for(
            lambda _events, process: process is None,
            "fake Host did not finish",
        )

    def sse_events(self, count: int, *, after: int = 0):
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.server.server_port, timeout=5
        )
        connection.request("GET", f"/api/v1/events?after={after}")
        response = connection.getresponse()
        if response.status != 200:
            raise AssertionError(f"unexpected SSE status: {response.status}")
        events = []
        current = {}
        while len(events) < count:
            line = response.readline().decode("utf-8").rstrip("\r\n")
            if line.startswith("id: "):
                current["id"] = int(line[4:])
            elif line == "event: host":
                current["event"] = "host"
            elif line.startswith("data: "):
                current["data"] = json.loads(line[6:])
            elif line == "" and "data" in current:
                events.append(current)
                current = {}
        connection.close()
        return events


class DashboardTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        directory = Path(self.temporary.name)
        self.host = directory / "meiso_wifi_runtime_host"
        self.host.write_text(FAKE_HOST, encoding="utf-8")
        self.host.chmod(self.host.stat().st_mode | stat.S_IXUSR)
        self.config = directory / "wifi-host.local.conf"
        self.config.write_text("ignored test config\n", encoding="utf-8")

    def tearDown(self):
        self.temporary.cleanup()

    def make_host(self, name: str, source: str) -> Path:
        host = Path(self.temporary.name) / name
        host.write_text(source, encoding="utf-8")
        host.chmod(host.stat().st_mode | stat.S_IXUSR)
        return host

    def test_projection_drops_sensitive_fields_and_keeps_live_evidence(self):
        sample = normalize_host_event({
            "type": "sample", "ordinal": 1, "sequence": 2,
            "selected_fields": 3, "present_fields": 3, "gap": False,
            "missing_before": 0, "link_state": True, "signal_mbm": -6200,
            "source_time_ns": 123456789,
            **{name: None for name in (
                "rx_bytes", "tx_bytes", "rx_packets", "tx_packets",
                "rx_dropped", "tx_dropped", "rx_errors", "tx_errors")},
            "ssid": "secret", "locator": "198.51.100.1", "peer_id": "full",
        })
        self.assertIsNotNone(sample)
        self.assertEqual(sample["source_time_ns"], 123456789)
        self.assertIsNone(sample["rx_bytes"])
        self.assertNotIn("ssid", sample)
        self.assertNotIn("locator", sample)
        self.assertNotIn("peer_id", sample)

    def test_persistent_connect_apply_reconfigure_stop_and_close(self):
        with RunningDashboard(str(self.host), str(self.config)) as dashboard:
            self.assertEqual(
                dashboard.state.argv,
                (str(self.host.resolve()), "--config", str(self.config.resolve()), "--interactive"),
            )
            status, _, body = dashboard.post("/api/v1/connect")
            self.assertEqual(status, 202)
            self.assertEqual(json.loads(body), {"connected": True})
            dashboard.wait_catalog()
            with dashboard.state._condition:
                self.assertIsNotNone(dashboard.state._process)

            status, _, _ = dashboard.post(
                "/api/v1/apply", {"selected_fields": 63, "sample_period_ms": 500}
            )
            self.assertEqual(status, 202)
            dashboard.wait_for(lambda events, _process: sum(
                event["type"] == "sample" for event in events
            ) >= 3)

            status, _, _ = dashboard.post(
                "/api/v1/apply", {"selected_fields": 3, "sample_period_ms": 900}
            )
            self.assertEqual(status, 202)
            dashboard.wait_for(lambda events, _process: sum(
                event["type"] == "control" and event.get("status") == "APPLIED"
                for event in events
            ) >= 2)

            status, _, _ = dashboard.post("/api/v1/stop")
            self.assertEqual(status, 202)
            dashboard.wait_for(lambda events, _process: any(
                event["type"] == "actual" and event["status"] == "STOPPED"
                for event in events
            ))
            with dashboard.state._condition:
                self.assertIsNotNone(dashboard.state._process)

            # Restart and close while active.  The native Host, not the UI,
            # guarantees desired=false/actual STOPPED before normal close.
            status, _, _ = dashboard.post(
                "/api/v1/apply", {"selected_fields": 1, "sample_period_ms": 100}
            )
            self.assertEqual(status, 202)
            dashboard.wait_for(lambda events, _process: sum(
                event["type"] == "control" and event.get("status") == "APPLIED"
                for event in events
            ) >= 4)
            status, _, _ = dashboard.post("/api/v1/close")
            self.assertEqual(status, 202)
            events = dashboard.wait_finished()
            self.assertFalse(any(event["type"] == "failure" for event in events))
            self.assertGreaterEqual(sum(
                event["type"] == "actual" and event["status"] == "STOPPED"
                for event in events
            ), 2)
            self.assertTrue(any(
                event["type"] == "control" and event.get("command") == "CLOSE"
                for event in events
            ))
            encoded = json.dumps(events)
            for forbidden in ("192.0.2.10", "DO_NOT_SHOW", str(self.config), "ssid", "mac"):
                self.assertNotIn(forbidden, encoded)

    def test_sse_replays_startup_and_static_assets_show_controls(self):
        with RunningDashboard(str(self.host), str(self.config)) as dashboard:
            status, headers, body = dashboard.request("GET", "/")
            self.assertEqual(status, 200)
            self.assertIn("default-src 'self'", headers["Content-Security-Policy"])
            for marker in (b'id="connect"', b'id="apply"', b'id="stop"', b'id="close"'):
                self.assertIn(marker, body)
            status, _, script = dashboard.request("GET", "/app.js")
            self.assertEqual(status, 200)
            self.assertIn(
                b"lastDesired.version === lastActual.observed_desired_version",
                script,
            )
            self.assertIn(b'event.type === "dashboard_snapshot"', script)
            dashboard.post("/api/v1/connect")
            dashboard.wait_catalog()
            events = dashboard.sse_events(1)
            snapshot = events[0]["data"]
            self.assertEqual(snapshot["type"], "dashboard_snapshot")
            self.assertTrue(snapshot["host_running"])
            self.assertEqual(
                [item["type"] for item in snapshot["events"]],
                ["bootstrap", "session", "catalog", "control"],
            )
            self.assertFalse(any(
                item["type"] == "sample" for item in snapshot["events"]
            ))

    def test_fixed_control_routes_authorization_and_body_validation(self):
        with RunningDashboard(str(self.host), str(self.config)) as dashboard:
            status, _, _ = dashboard.request(
                "POST", "/api/v1/connect", {"Content-Length": "0"}
            )
            self.assertEqual(status, 403)
            status, _, _ = dashboard.post("/api/v1/start")
            self.assertEqual(status, 404)
            dashboard.post("/api/v1/connect")
            dashboard.wait_catalog()
            status, _, _ = dashboard.post(
                "/api/v1/apply", {"selected_fields": 0, "sample_period_ms": 100}
            )
            self.assertEqual(status, 422)
            status, _, _ = dashboard.post(
                "/api/v1/apply", {"selected_fields": 1, "sample_period_ms": 101}
            )
            self.assertEqual(status, 422)
            status, _, _ = dashboard.post(
                "/api/v1/apply", {"selected_fields": 1024, "sample_period_ms": 100}
            )
            self.assertEqual(status, 422)
            status, _, _ = dashboard.post("/api/v1/stop")
            self.assertEqual(status, 409)

    def test_impossible_events_and_control_variants_are_closed(self):
        self.assertIsNone(normalize_host_event({
            "type": "actual", "status": "RUNNING", "state_version": 1,
            "observed_desired_version": 1,
            "apply_result": "APPLIED", "effective_enabled": True,
            "data_active": False, "selected_fields": 3,
            "sample_period_ms": 1000, "production_epoch_known": True,
        }))
        self.assertEqual(normalize_host_event({
            "type": "control", "status": "REJECTED",
            "reason": "UNSUPPORTED_PARAMETERS", "secret": "discard",
        }), {
            "type": "control", "status": "REJECTED",
            "reason": "UNSUPPORTED_PARAMETERS",
        })
        self.assertIsNone(normalize_host_event({
            "type": "control", "status": "REJECTED", "reason": "UNKNOWN",
        }))

    def test_identical_new_desired_does_not_replace_old_actual_version(self):
        desired = normalize_host_event({
            "type": "desired", "status": "PUBLISHED", "version": 2,
            "enabled": True, "selected_fields": 3, "sample_period_ms": 500,
        })
        actual = normalize_host_event({
            "type": "actual", "status": "RUNNING", "state_version": 7,
            "observed_desired_version": 1, "apply_result": "APPLIED",
            "effective_enabled": True, "data_active": True,
            "selected_fields": 3, "sample_period_ms": 500,
            "production_epoch_known": True,
        })
        self.assertIsNotNone(desired)
        self.assertIsNotNone(actual)
        missing_version = dict(actual)
        missing_version.pop("observed_desired_version")
        self.assertIsNone(normalize_host_event(missing_version))

        with RunningDashboard(str(self.host), str(self.config)) as dashboard:
            with dashboard.state._condition:
                dashboard.state._append(desired)
                dashboard.state._append(actual)
            _, snapshot = dashboard.state.initial_sse(0)
            self.assertIsNotNone(snapshot)
            restored = {event["type"]: event for event in snapshot["events"]}
            self.assertEqual(restored["desired"]["version"], 2)
            self.assertEqual(restored["actual"]["observed_desired_version"], 1)
            self.assertEqual(
                restored["desired"]["selected_fields"],
                restored["actual"]["selected_fields"],
            )

    def test_no_lifetime_event_limit(self):
        burst = self.make_host("burst_host", r'''#!/usr/bin/env python3
import json, sys
def emit(value): print(json.dumps(value, separators=(",", ":")), flush=True)
emit({"type":"bootstrap","status":"CONNECTED","locator_redacted":True})
emit({"type":"session","status":"READY","remote_identity_known":True,
      "identity_match":True,"session_id_known":True,
      "limits":{"max_frame_bytes":4096,"max_runtime_payload_bytes":2048,
      "max_registry_entries":16}})
emit({"type":"catalog","status":"ACCEPTED","generation":1,"endpoint_count":1,
      "route_count":3,"capability":{"supported_fields":3,"min_period_ms":100,
      "max_period_ms":10000,"period_step_ms":100}})
emit({"type":"control","status":"READY","mode":"INTERACTIVE"})
emit({"type":"desired","status":"PUBLISHED","version":1,"enabled":True,
      "selected_fields":3,"sample_period_ms":100})
emit({"type":"desired_delivery","status":"ACCEPTED","version":1})
emit({"type":"actual","status":"RUNNING","state_version":1,
      "observed_desired_version":1,"apply_result":"APPLIED",
      "effective_enabled":True,"data_active":True,"selected_fields":3,
      "sample_period_ms":100,"production_epoch_known":True})
base = {"selected_fields":3,"present_fields":3,"gap":False,"missing_before":0,
        "link_state":True,"signal_mbm":-5000,"rx_bytes":None,"tx_bytes":None,
        "rx_packets":None,"tx_packets":None,"rx_dropped":None,"tx_dropped":None,
        "rx_errors":None,"tx_errors":None}
for value in range(1, 4201):
    emit({"type":"sample","ordinal":value,"sequence":value,
          "source_time_ns":value*100000000,**base})
for line in sys.stdin:
    if line == '{"command":"close"}\n':
        emit({"type":"session","status":"CLOSED","reason":"NORMAL"})
        emit({"type":"control","command":"CLOSE","status":"COMPLETE"})
        raise SystemExit(0)
''')
        with RunningDashboard(str(burst), str(self.config)) as dashboard:
            dashboard.post("/api/v1/connect")
            dashboard.wait_for(lambda events, process: process is not None and any(
                event.get("sequence") == 4200 for event in events
            ), "continuous Host was stopped at the legacy event cap")
            with dashboard.state._condition:
                original_process = dashboard.state._process
                self.assertIsNotNone(original_process)
                self.assertLessEqual(len(dashboard.state._events), 512)
                self.assertGreater(dashboard.state._events[0][0], 1)
                snapshot_watermark = dashboard.state._next_sequence - 1

            # A new page starts at cursor zero after lifecycle events were
            # evicted.  It receives one atomic state baseline, no historical
            # samples, and does not replace the persistent Host child.
            replay_message = dashboard.sse_events(1)[0]
            self.assertEqual(replay_message["id"], snapshot_watermark)
            replay = replay_message["data"]
            self.assertEqual(replay["type"], "dashboard_snapshot")
            self.assertTrue(replay["host_running"])
            self.assertEqual(
                [event["type"] for event in replay["events"]],
                ["bootstrap", "session", "catalog", "control", "desired",
                 "desired_delivery", "actual"],
            )
            self.assertFalse(any(
                event["type"] == "sample" for event in replay["events"]
            ))
            lagged = dashboard.sse_events(1, after=1)[0]["data"]
            self.assertEqual(lagged["type"], "dashboard_snapshot")
            self.assertEqual(lagged["events"], replay["events"])
            with dashboard.state._condition:
                self.assertIs(dashboard.state._process, original_process)

            status, _, _ = dashboard.post(
                "/api/v1/apply", {"selected_fields": 3, "sample_period_ms": 200}
            )
            self.assertEqual(status, 202)
            dashboard.post("/api/v1/close")
            events = dashboard.wait_finished()
            self.assertFalse(any(event["type"] == "failure" for event in events))

    def test_startup_timeout_malformed_output_and_unexpected_exit(self):
        slow = self.make_host("slow_host", "#!/bin/sh\nsleep 2\n")
        with RunningDashboard(str(slow), str(self.config), startup_timeout=0.05) as dashboard:
            dashboard.state.connect()
            events = dashboard.wait_finished()
            self.assertEqual(events[-1]["stage"], "host_startup_timeout")

        malformed = self.make_host(
            "malformed_host", "#!/bin/sh\nprintf '%s\\n' 'not-json'\n"
        )
        with RunningDashboard(str(malformed), str(self.config)) as dashboard:
            dashboard.state.connect()
            events = dashboard.wait_finished()
            self.assertEqual(events[-1]["stage"], "host_output")

        nonzero = self.make_host("nonzero_host", "#!/bin/sh\nexit 7\n")
        with RunningDashboard(str(nonzero), str(self.config)) as dashboard:
            dashboard.state.connect()
            events = dashboard.wait_finished()
            self.assertEqual(events[-1]["stage"], "host_exit")


if __name__ == "__main__":
    unittest.main()
