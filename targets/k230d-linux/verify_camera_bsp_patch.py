#!/usr/bin/env python3
"""Static contract check for the public K230D Zero camera BSP patch.

With no arguments this validates the checked-in artifact itself. With
``--sdk-root`` it additionally performs a read-only ``patch --dry-run``
against an unpacked K230 Linux SDK root.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


PATCH_NAME = "0001-k230d-zero-gc2093-vvcam-min-buffers.patch"
EXPECTED_PATHS = (
    "buildroot-overlay/configs/BPI-CanMV-K230D-Zero_defconfig",
    "buildroot-overlay/package/vvcam/v4l2/video/vvcam_video_register.c",
)
REQUIRED_FRAGMENTS = (
    '-BR2_PACKAGE_VVCAM_DEF_SENSOR="imx335"',
    '+BR2_PACKAGE_VVCAM_DEF_SENSOR="gc2093"',
    "+    queue->min_buffers_needed = 2;",
    "Linux 6.6 vb2 raises the allocated REQBUFS count to this minimum",
    "request and queue at least four capture buffers",
    "not a repair for synchronization inside",
)


class ContractError(RuntimeError):
    pass


def verify_artifact(patch_path: Path) -> None:
    patch = patch_path.read_text(encoding="utf-8")
    paths: list[str] = []

    if "GIT binary patch" in patch or "Binary files " in patch:
        raise ContractError("binary patch content is forbidden")
    for line in patch.splitlines():
        if line.startswith("diff --git a/"):
            fields = line.split()
            if len(fields) != 4 or not fields[2].startswith("a/") or not fields[3].startswith("b/"):
                raise ContractError("malformed diff header")
            old_path = fields[2][2:]
            new_path = fields[3][2:]
            if old_path.startswith("/") or "/../" in f"/{old_path}/":
                raise ContractError("unsafe diff path")
            if old_path != new_path:
                raise ContractError("rename is forbidden")
            paths.append(old_path)

    if tuple(paths) != EXPECTED_PATHS:
        raise ContractError("patch path whitelist mismatch")
    for fragment in REQUIRED_FRAGMENTS:
        if patch.count(fragment) != 1:
            raise ContractError("required contract fragment missing or duplicated")

    if "+    queue->min_queued_buffers" in patch:
        raise ContractError("Linux 6.6 uses min_buffers_needed")
    if "isp_media_server" in "\n".join(
        line for line in patch.splitlines() if line.startswith(("diff --git ", "--- ", "+++ "))
    ):
        raise ContractError("closed-source server must not be patched")


def verify_sdk_dry_run(patch_path: Path, sdk_root: Path) -> None:
    if not sdk_root.is_dir():
        raise ContractError("SDK root is not a directory")
    for relative in EXPECTED_PATHS:
        candidate = sdk_root / relative
        if not candidate.is_file():
            raise ContractError("SDK baseline file is missing")

    completed = subprocess.run(
        [
            "patch",
            "--batch",
            "--forward",
            "--dry-run",
            "--silent",
            "-p1",
            "-i",
            str(patch_path.resolve()),
        ],
        cwd=sdk_root,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise ContractError("patch does not apply cleanly to SDK baseline")


def main() -> int:
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument(
        "--sdk-root",
        type=Path,
        help="optional unpacked k230_linux_sdk root for read-only dry-run",
    )
    args = parser.parse_args()

    patch_path = Path(__file__).resolve().parent / "patches" / PATCH_NAME
    try:
        verify_artifact(patch_path)
        if args.sdk_root is not None:
            verify_sdk_dry_run(patch_path, args.sdk_root.resolve())
    except (ContractError, OSError) as error:
        print(f"camera_bsp_patch_contract: FAIL: {error}", file=sys.stderr)
        return 1

    mode = "artifact+sdk-dry-run" if args.sdk_root is not None else "artifact"
    print(f"camera_bsp_patch_contract: PASS ({mode})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
