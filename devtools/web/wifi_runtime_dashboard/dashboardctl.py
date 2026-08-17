#!/usr/bin/env python3
"""Start and stop the local Wi-Fi Runtime dashboard outside a terminal PTY."""

from __future__ import annotations

import argparse
import http.client
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from typing import Any


PORT = 49_999
ADDRESS = "127.0.0.1"
DASHBOARD_DIRECTORY = Path(__file__).resolve().parent
SERVER = DASHBOARD_DIRECTORY / "server.py"
REPOSITORY = DASHBOARD_DIRECTORY.parents[2]
STATE_DIRECTORY = REPOSITORY / "logs" / "wifi_runtime_dashboard"
PID_FILE = STATE_DIRECTORY / "dashboard.pid"
LOG_FILE = STATE_DIRECTORY / "dashboard.log"
START_TIMEOUT_SECONDS = 8.0
STOP_TIMEOUT_SECONDS = 8.0


class ControlError(Exception):
    """A bounded dashboard lifecycle operation could not complete."""


def _pid_state() -> dict[str, Any] | None:
    try:
        raw = json.loads(PID_FILE.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ControlError(f"invalid PID file: {PID_FILE}") from error
    legacy_keys = {"pid", "port", "server"}
    current_keys = legacy_keys | {"host_binary", "config_path"}
    if (
        not isinstance(raw, dict)
        or set(raw) not in (legacy_keys, current_keys)
        or type(raw["pid"]) is not int
        or raw["pid"] <= 1
        or raw["port"] != PORT
        or raw["server"] != str(SERVER)
        or (
            set(raw) == current_keys
            and (
                not isinstance(raw["host_binary"], str)
                or not raw["host_binary"]
                or not isinstance(raw["config_path"], str)
                or not raw["config_path"]
            )
        )
    ):
        raise ControlError(f"invalid PID file: {PID_FILE}")
    return raw


def _process_command(pid: int) -> str | None:
    result = subprocess.run(
        ("ps", "-p", str(pid), "-o", "command="),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        return None
    command = result.stdout.strip()
    return command or None


def _is_dashboard_process(state: dict[str, Any]) -> bool:
    command = _process_command(state["pid"])
    return (
        command is not None
        and str(SERVER) in command
        and f"--port {PORT}" in command
    )


def _healthy() -> bool:
    connection = http.client.HTTPConnection(ADDRESS, PORT, timeout=0.5)
    try:
        connection.request("GET", "/", headers={"Host": f"{ADDRESS}:{PORT}"})
        response = connection.getresponse()
        response.read()
        return (
            response.status == 200
            and response.getheader("Server", "").startswith(
                "MeisoWifiRuntimeDashboard/1"
            )
        )
    except OSError:
        return False
    finally:
        connection.close()


def _write_pid(pid: int, host_binary: Path, config_path: Path) -> None:
    STATE_DIRECTORY.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(STATE_DIRECTORY, 0o700)
    temporary = PID_FILE.with_suffix(".pid.new")
    temporary.write_text(
        json.dumps(
            {
                "pid": pid,
                "port": PORT,
                "server": str(SERVER),
                "host_binary": str(host_binary),
                "config_path": str(config_path),
            },
            separators=(",", ":"),
        )
        + "\n",
        encoding="utf-8",
    )
    os.chmod(temporary, 0o600)
    temporary.replace(PID_FILE)


def _remove_pid(expected_pid: int | None = None) -> None:
    try:
        if expected_pid is not None:
            state = _pid_state()
            if state is None or state["pid"] != expected_pid:
                return
        PID_FILE.unlink()
    except FileNotFoundError:
        pass


def _wait_for_exit(pid: int, timeout_seconds: float) -> bool:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        try:
            waited, _status = os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            waited = 0
        if waited == pid:
            return True
        if _process_command(pid) is None:
            return True
        time.sleep(0.05)
    try:
        waited, _status = os.waitpid(pid, os.WNOHANG)
    except ChildProcessError:
        waited = 0
    if waited == pid:
        return True
    return _process_command(pid) is None


def _direct_children(parent_pid: int) -> list[tuple[int, int, str, str]]:
    result = subprocess.run(
        ("ps", "-axww", "-o", "pid=,ppid=,pgid=,stat=,command="),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise ControlError("could not enumerate dashboard children")
    children: list[tuple[int, int, str, str]] = []
    for line in result.stdout.splitlines():
        fields = line.strip().split(None, 4)
        if len(fields) != 5:
            continue
        try:
            pid, observed_parent, process_group = map(int, fields[:3])
        except ValueError:
            continue
        if observed_parent == parent_pid:
            children.append((pid, process_group, fields[3], fields[4]))
    return children


def _hard_stop(state: dict[str, Any]) -> None:
    server_pid = state["pid"]
    try:
        os.kill(server_pid, signal.SIGSTOP)
    except ProcessLookupError:
        return

    try:
        children = _direct_children(server_pid)
        live_children = [child for child in children if "Z" not in child[2]]
        expected_host = state.get("host_binary")
        expected_config = state.get("config_path")
        expected_command = (
            f"{expected_host} --config {expected_config} --interactive"
            if expected_host is not None and expected_config is not None
            else None
        )
        if len(live_children) > 1 or any(
            child_pid <= 1
            or process_group != child_pid
            or expected_command is None
            or not command.endswith(expected_command)
            for child_pid, process_group, _status, command in live_children
        ):
            raise ControlError(
                "dashboard hard stop refused: Host child identity did not match"
            )
        for child_pid, process_group, _status, _command in live_children:
            try:
                os.killpg(process_group, signal.SIGKILL)
            except ProcessLookupError:
                pass
        os.kill(server_pid, signal.SIGKILL)
        if not _wait_for_exit(server_pid, 2.0):
            raise ControlError("dashboard did not stop within the bounded timeout")
        for child_pid, _process_group, _status, _command in live_children:
            if not _wait_for_exit(child_pid, 2.0):
                raise ControlError("native Host child did not stop")
    except BaseException:
        try:
            os.kill(server_pid, signal.SIGCONT)
        except ProcessLookupError:
            pass
        raise


def _stop(*, quiet: bool = False) -> int:
    state = _pid_state()
    if state is None:
        if not quiet:
            print("dashboard: STOPPED")
        return 0
    pid = state["pid"]
    if not _is_dashboard_process(state):
        _remove_pid(pid)
        if not quiet:
            print("dashboard: STOPPED (removed stale PID file)")
        return 0
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        _remove_pid(pid)
        if not quiet:
            print("dashboard: STOPPED")
        return 0
    if not _wait_for_exit(pid, STOP_TIMEOUT_SECONDS):
        _hard_stop(state)
        if not _wait_for_exit(pid, 2.0):
            raise ControlError("dashboard did not stop within the bounded timeout")
    _remove_pid(pid)
    if not quiet:
        print("dashboard: STOPPED")
    return 0


def _start(host_binary: str, config_path: str) -> int:
    host = Path(host_binary).expanduser().resolve(strict=True)
    config = Path(config_path).expanduser().resolve(strict=True)
    if not host.is_file() or not os.access(host, os.X_OK):
        raise ControlError("Host binary must be executable")
    if not config.is_file():
        raise ControlError("config must be a regular file")

    state = _pid_state()
    if state is not None:
        if _is_dashboard_process(state) and _healthy():
            raise ControlError("dashboard is already RUNNING")
        if _is_dashboard_process(state):
            raise ControlError("dashboard process exists but is not healthy; stop it first")
        _remove_pid(state["pid"])
    if _healthy():
        raise ControlError(f"port {PORT} is already occupied by a dashboard")

    STATE_DIRECTORY.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(STATE_DIRECTORY, 0o700)
    log_descriptor = os.open(
        LOG_FILE,
        os.O_WRONLY | os.O_CREAT | os.O_APPEND,
        0o600,
    )
    os.chmod(LOG_FILE, 0o600)
    null_descriptor = os.open(os.devnull, os.O_RDONLY)
    try:
        os.write(
            log_descriptor,
            f"\n=== dashboard start {time.strftime('%Y-%m-%d %H:%M:%S')} ===\n".encode(
                "ascii"
            ),
        )
        argv = (
            sys.executable,
            str(SERVER),
            "--host-binary",
            str(host),
            "--config",
            str(config),
            "--port",
            str(PORT),
        )
        pid = os.posix_spawn(
            sys.executable,
            argv,
            os.environ.copy(),
            file_actions=(
                (os.POSIX_SPAWN_DUP2, null_descriptor, 0),
                (os.POSIX_SPAWN_DUP2, log_descriptor, 1),
                (os.POSIX_SPAWN_DUP2, log_descriptor, 2),
                (os.POSIX_SPAWN_CLOSE, null_descriptor),
                (os.POSIX_SPAWN_CLOSE, log_descriptor),
            ),
            setsid=True,
        )
    finally:
        os.close(null_descriptor)
        os.close(log_descriptor)
    try:
        _write_pid(pid, host, config)
    except OSError:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        _wait_for_exit(pid, 2.0)
        raise

    deadline = time.monotonic() + START_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        try:
            waited, _status = os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            waited = 0
        if waited == pid or _process_command(pid) is None:
            _remove_pid(pid)
            raise ControlError(f"dashboard exited during startup; inspect {LOG_FILE}")
        if _healthy():
            print(f"dashboard: RUNNING pid={pid} http://{ADDRESS}:{PORT}/")
            print(f"dashboard log: {LOG_FILE}")
            return 0
        time.sleep(0.05)

    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    _wait_for_exit(pid, 2.0)
    _remove_pid(pid)
    raise ControlError(f"dashboard startup timed out; inspect {LOG_FILE}")


def _status() -> int:
    state = _pid_state()
    if state is None:
        print("dashboard: STOPPED")
        return 1
    if not _is_dashboard_process(state):
        print(f"dashboard: STALE pid={state['pid']}")
        return 1
    if _healthy():
        print(f"dashboard: RUNNING pid={state['pid']} http://{ADDRESS}:{PORT}/")
        print(f"dashboard log: {LOG_FILE}")
        return 0
    print(f"dashboard: UNHEALTHY pid={state['pid']}")
    print(f"dashboard log: {LOG_FILE}")
    return 2


def _logs(lines: int) -> int:
    try:
        content = LOG_FILE.read_text(encoding="utf-8", errors="replace").splitlines()
    except FileNotFoundError:
        print("dashboard: no log")
        return 1
    for line in content[-lines:]:
        print(line)
    return 0


def _arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Manage the fixed loopback Wi-Fi Runtime dashboard."
    )
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("start", "restart"):
        command = commands.add_parser(name)
        command.add_argument("--host-binary", required=True)
        command.add_argument("--config", required=True)
    commands.add_parser("status")
    commands.add_parser("stop")
    logs = commands.add_parser("logs")
    logs.add_argument("--lines", type=int, default=80)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = _arguments(sys.argv[1:] if argv is None else argv)
    try:
        if arguments.command == "status":
            return _status()
        if arguments.command == "stop":
            return _stop()
        if arguments.command == "logs":
            if not 1 <= arguments.lines <= 2_000:
                raise ControlError("--lines must be in 1..2000")
            return _logs(arguments.lines)
        if arguments.command == "restart":
            _stop(quiet=True)
        return _start(arguments.host_binary, arguments.config)
    except (ControlError, OSError) as error:
        print(f"dashboardctl: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
