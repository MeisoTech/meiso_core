#!/usr/bin/env python3
"""Process-level lifecycle test for the detached dashboard controller."""

from __future__ import annotations

import http.client
from pathlib import Path
import re
import socket
import tempfile
import time
import unittest

import dashboardctl
from test_server import FAKE_HOST


class DashboardControllerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        directory = Path(self.temporary.name)
        self.host = directory / "meiso_wifi_runtime_host"
        self.host.write_text(FAKE_HOST, encoding="utf-8")
        self.host.chmod(0o700)
        self.config = directory / "wifi-host.local.conf"
        self.config.write_text("ignored test config\n", encoding="utf-8")

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind((dashboardctl.ADDRESS, 0))
            port = listener.getsockname()[1]
        self.originals = (
            dashboardctl.PORT,
            dashboardctl.STATE_DIRECTORY,
            dashboardctl.PID_FILE,
            dashboardctl.LOG_FILE,
            dashboardctl.STOP_TIMEOUT_SECONDS,
        )
        state_directory = directory / "state"
        dashboardctl.PORT = port
        dashboardctl.STATE_DIRECTORY = state_directory
        dashboardctl.PID_FILE = state_directory / "dashboard.pid"
        dashboardctl.LOG_FILE = state_directory / "dashboard.log"

    def tearDown(self) -> None:
        try:
            dashboardctl._stop(quiet=True)
        finally:
            (
                dashboardctl.PORT,
                dashboardctl.STATE_DIRECTORY,
                dashboardctl.PID_FILE,
                dashboardctl.LOG_FILE,
                dashboardctl.STOP_TIMEOUT_SECONDS,
            ) = self.originals
            self.temporary.cleanup()

    def request(self, method: str, path: str, headers=None) -> tuple[int, bytes]:
        connection = http.client.HTTPConnection(
            dashboardctl.ADDRESS, dashboardctl.PORT, timeout=2
        )
        connection.request(method, path, headers=headers or {})
        response = connection.getresponse()
        body = response.read()
        status = response.status
        connection.close()
        return status, body

    @staticmethod
    def child_pids(parent_pid: int) -> list[int]:
        result = dashboardctl.subprocess.run(
            ("ps", "-ax", "-o", "pid=,ppid="),
            stdout=dashboardctl.subprocess.PIPE,
            text=True,
            check=True,
        )
        children = []
        for line in result.stdout.splitlines():
            pid_text, parent_text = line.split()
            if int(parent_text) == parent_pid:
                children.append(int(pid_text))
        return children

    def test_detached_start_status_restart_and_bounded_stop(self) -> None:
        self.assertEqual(dashboardctl._start(str(self.host), str(self.config)), 0)
        first_state = dashboardctl._pid_state()
        self.assertIsNotNone(first_state)
        first_pid = first_state["pid"]
        self.assertTrue(dashboardctl._healthy())
        self.assertEqual(dashboardctl._status(), 0)

        self.assertEqual(dashboardctl._stop(quiet=True), 0)
        self.assertIsNone(dashboardctl._process_command(first_pid))
        self.assertEqual(dashboardctl._start(str(self.host), str(self.config)), 0)
        second_state = dashboardctl._pid_state()
        self.assertIsNotNone(second_state)
        second_pid = second_state["pid"]
        self.assertNotEqual(first_pid, second_pid)

        status, index = self.request("GET", "/")
        self.assertEqual(status, 200)
        match = re.search(rb'<meta name="meiso-csrf" content="([^"]+)">', index)
        self.assertIsNotNone(match)
        token = match.group(1).decode("ascii")
        status, _ = self.request(
            "POST",
            "/api/v1/connect",
            {
                "Host": f"{dashboardctl.ADDRESS}:{dashboardctl.PORT}",
                "Origin": f"http://{dashboardctl.ADDRESS}:{dashboardctl.PORT}",
                "X-Meiso-CSRF": token,
                "Content-Length": "0",
            },
        )
        self.assertEqual(status, 202)

        deadline = time.monotonic() + 2
        children: list[int] = []
        while time.monotonic() < deadline:
            children = self.child_pids(second_pid)
            if children:
                break
            time.sleep(0.02)
        self.assertEqual(len(children), 1)

        # Freeze the server so its SIGTERM handler cannot run.  This forces the
        # bounded hard fallback to identify and kill the separate Host process
        # group before killing the dashboard server itself.
        dashboardctl.STOP_TIMEOUT_SECONDS = 0.05
        dashboardctl.os.kill(second_pid, dashboardctl.signal.SIGSTOP)
        self.assertEqual(dashboardctl._stop(quiet=True), 0)
        self.assertIsNone(dashboardctl._process_command(second_pid))
        for child in children:
            self.assertIsNone(dashboardctl._process_command(child))
        self.assertFalse(dashboardctl.PID_FILE.exists())


if __name__ == "__main__":
    unittest.main()
