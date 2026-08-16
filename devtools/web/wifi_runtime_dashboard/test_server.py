#!/usr/bin/env python3
"""Deterministic tests for the Wi-Fi Runtime dashboard boundary."""

from __future__ import annotations

import http.client
import json
from pathlib import Path
import stat
import sys
import tempfile
import threading
import time
import unittest

from server import create_server, normalize_host_event


FAKE_HOST = r'''#!/usr/bin/env python3
import json
import sys

if len(sys.argv) != 3 or sys.argv[1] != "--config":
    raise SystemExit(2)

def emit(value):
    print(json.dumps(value, separators=(",", ":")), flush=True)

emit({"type":"bootstrap","status":"CONNECTED","locator_redacted":True,
      "connect_ipv4":"192.0.2.10","config":sys.argv[2],"ssid":"DO_NOT_SHOW"})
emit({"type":"session","status":"READY","remote_identity_known":True,
      "identity_match":True,"session_id_known":True,"peer_id":"DO_NOT_SHOW",
      "limits":{"max_frame_bytes":4096,"max_runtime_payload_bytes":2048,
                "max_registry_entries":16}})
emit({"type":"catalog","status":"ACCEPTED","generation":1,
      "endpoint_count":1,"route_count":3,"endpoint_id":"DO_NOT_SHOW",
      "capability":{"supported_fields":3,"min_period_ms":100,
                    "max_period_ms":10000,"period_step_ms":100}})
emit({"type":"desired","status":"PUBLISHED","version":1,"enabled":True,
      "selected_fields":3,"sample_period_ms":1000})
emit({"type":"desired_delivery","status":"ACCEPTED","version":1})
emit({"type":"actual","status":"RUNNING","state_version":1,
      "apply_result":"APPLIED","effective_enabled":True,"data_active":True,
      "selected_fields":3,"sample_period_ms":1000,"production_epoch_known":True})
emit({"type":"sample","ordinal":1,"sequence":1,"selected_fields":3,
      "present_fields":3,"gap":False,"missing_before":0,"link_state":True,
      "signal_mbm":-5500,"rx_bytes":None,"tx_bytes":None,"rx_packets":None,
      "tx_packets":None,"rx_dropped":None,"tx_dropped":None,
      "rx_errors":None,"tx_errors":None,"mac":"DO_NOT_SHOW"})
emit({"type":"desired","status":"PUBLISHED","version":2,"enabled":False,
      "selected_fields":3,"sample_period_ms":1000})
emit({"type":"desired_delivery","status":"ACCEPTED","version":2})
emit({"type":"actual","status":"STOPPED","state_version":2,
      "apply_result":"APPLIED","effective_enabled":False,"data_active":False,
      "selected_fields":0,"sample_period_ms":0,"production_epoch_known":False})
emit({"type":"session","status":"CLOSED","reason":"NORMAL"})
emit({"type":"run","status":"COMPLETE","requested_samples":1,
      "received_samples":1})
'''


class RunningDashboard:
    def __init__(self, host: str, config: str, *, timeout: float = 180.0):
        self.server, self.state = create_server(
            host, config, host_timeout_seconds=timeout
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

    def request(self, method: str, path: str, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=3)
        connection.request(method, path, headers=headers or {})
        response = connection.getresponse()
        body = response.read()
        result = response.status, dict(response.getheaders()), body
        connection.close()
        return result

    def start(self):
        return self.request(
            "POST",
            "/api/v1/start",
            {
                "Origin": self.server.expected_origin,
                "X-Meiso-CSRF": self.state.csrf_token,
                "Content-Length": "0",
            },
        )

    def wait_finished(self):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            with self.state._condition:
                if self.state._process is None:
                    return
            time.sleep(0.01)
        raise AssertionError("fake Host did not finish")

    def sse_events(self, count: int):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=3)
        connection.request("GET", "/api/v1/events?after=0")
        response = connection.getresponse()
        if response.status != 200:
            raise AssertionError(f"unexpected SSE status: {response.status}")
        if response.getheader("Content-Type") != "text/event-stream; charset=utf-8":
            raise AssertionError("unexpected SSE content type")
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

    def test_projection_drops_sensitive_fields_and_preserves_null(self):
        sample = normalize_host_event(
            {
                "type": "sample", "ordinal": 1, "sequence": 2,
                "selected_fields": 3, "present_fields": 3, "gap": False,
                "missing_before": 0, "link_state": True, "signal_mbm": -6200,
                **{name: None for name in (
                    "rx_bytes", "tx_bytes", "rx_packets", "tx_packets",
                    "rx_dropped", "tx_dropped", "rx_errors", "tx_errors")},
                "ssid": "secret", "locator": "198.51.100.1", "peer_id": "full",
            }
        )
        self.assertIsNotNone(sample)
        self.assertIsNone(sample["rx_bytes"])
        self.assertNotIn("ssid", sample)
        self.assertNotIn("locator", sample)
        self.assertNotIn("peer_id", sample)

    def test_fixed_host_start_and_sanitized_sse(self):
        with RunningDashboard(str(self.host), str(self.config)) as dashboard:
            self.assertEqual(
                dashboard.state.argv,
                (str(self.host.resolve()), "--config", str(self.config.resolve())),
            )
            status, _, body = dashboard.start()
            self.assertEqual(status, 202)
            self.assertEqual(json.loads(body), {"started": True})
            dashboard.wait_finished()
            events = dashboard.sse_events(12)
            payloads = [item["data"] for item in events]
            self.assertEqual(
                [event["type"] for event in payloads],
                ["bootstrap", "session", "catalog", "desired", "desired_delivery",
                 "actual", "sample", "desired", "desired_delivery", "actual",
                 "session", "run"],
            )
            sample = payloads[6]
            self.assertIsNone(sample["rx_bytes"])
            encoded = json.dumps(payloads)
            for forbidden in ("192.0.2.10", "DO_NOT_SHOW", str(self.config), "ssid", "mac"):
                self.assertNotIn(forbidden, encoded)

    def test_loopback_assets_and_start_authorization(self):
        with RunningDashboard(str(self.host), str(self.config)) as dashboard:
            self.assertEqual(dashboard.server.server_address[0], "127.0.0.1")
            status, headers, body = dashboard.request("GET", "/")
            self.assertEqual(status, 200)
            self.assertIn("default-src 'self'", headers["Content-Security-Policy"])
            self.assertIn(dashboard.state.csrf_token.encode(), body)
            status, _, _ = dashboard.request("POST", "/api/v1/start", {"Content-Length": "0"})
            self.assertEqual(status, 403)
            status, _, _ = dashboard.request("POST", "/api/v1/control", {
                "Origin": dashboard.server.expected_origin,
                "X-Meiso-CSRF": dashboard.state.csrf_token,
                "Content-Length": "0",
            })
            self.assertEqual(status, 404)

    def test_invalid_sample_is_rejected(self):
        self.assertIsNone(normalize_host_event({
            "type": "sample", "ordinal": 1, "sequence": 1,
            "selected_fields": 3, "present_fields": 3, "gap": False,
            "missing_before": 0, "link_state": True, "signal_mbm": -50000,
            **{name: None for name in (
                "rx_bytes", "tx_bytes", "rx_packets", "tx_packets",
                "rx_dropped", "tx_dropped", "rx_errors", "tx_errors")},
        }))
        invalid_presence = {
            "type": "sample", "ordinal": 1, "sequence": 1,
            "selected_fields": 3, "present_fields": 1, "gap": False,
            "missing_before": 0, "link_state": True, "signal_mbm": -5000,
            **{name: None for name in (
                "rx_bytes", "tx_bytes", "rx_packets", "tx_packets",
                "rx_dropped", "tx_dropped", "rx_errors", "tx_errors")},
        }
        self.assertIsNone(normalize_host_event(invalid_presence))

    def test_impossible_desired_and_actual_are_rejected(self):
        self.assertIsNone(normalize_host_event({
            "type": "desired", "status": "PUBLISHED", "version": 2,
            "enabled": False, "selected_fields": 0, "sample_period_ms": 0,
        }))
        self.assertIsNone(normalize_host_event({
            "type": "actual", "status": "RUNNING", "state_version": 1,
            "apply_result": "APPLIED", "effective_enabled": True,
            "data_active": False, "selected_fields": 3,
            "sample_period_ms": 1000, "production_epoch_known": True,
        }))

    def test_complete_requires_equal_counts_and_zero_exit(self):
        self.assertIsNone(normalize_host_event({
            "type": "run", "status": "COMPLETE",
            "requested_samples": 2, "received_samples": 1,
        }))
        host = self.make_host(
            "nonzero_host",
            "#!/bin/sh\nprintf '%s\\n' '{\"type\":\"run\",\"status\":\"COMPLETE\",\"requested_samples\":1,\"received_samples\":1}'\nexit 1\n",
        )
        with RunningDashboard(str(host), str(self.config)) as dashboard:
            dashboard.state.start()
            dashboard.wait_finished()
            events = [event for _, event in dashboard.state.events_after(0, timeout=0)]
            self.assertEqual(
                events,
                [{"type": "failure", "status": "FAILED", "stage": "host_exit"}],
            )

    def test_timeout_and_invalid_output_are_explicit_failures(self):
        slow = self.make_host("slow_host", "#!/bin/sh\nsleep 2\n")
        with RunningDashboard(str(slow), str(self.config), timeout=0.05) as dashboard:
            dashboard.state.start()
            dashboard.wait_finished()
            events = [event for _, event in dashboard.state.events_after(0, timeout=0)]
            self.assertEqual(events[-1]["stage"], "host_timeout")

        malformed = self.make_host(
            "malformed_host", "#!/bin/sh\nprintf '%s\\n' 'not-json'\n"
        )
        with RunningDashboard(str(malformed), str(self.config)) as dashboard:
            dashboard.state.start()
            dashboard.wait_finished()
            events = [event for _, event in dashboard.state.events_after(0, timeout=0)]
            self.assertEqual(events[-1]["stage"], "host_output")

    def test_zero_exit_without_complete_is_explicit_failure(self):
        empty = self.make_host("empty_host", "#!/bin/sh\nexit 0\n")
        with RunningDashboard(str(empty), str(self.config)) as dashboard:
            dashboard.state.start()
            dashboard.wait_finished()
            events = [event for _, event in dashboard.state.events_after(0, timeout=0)]
            self.assertEqual(events[-1]["stage"], "missing_complete")


if __name__ == "__main__":
    unittest.main()
