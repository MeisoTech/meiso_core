#!/usr/bin/env python3
"""Fail when a repository-facing Markdown link points at a missing local file."""

from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parents[1]
LINK = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
SCAN_ROOTS = (
    ROOT / "docs",
    ROOT / "profiles",
    ROOT / "devtools" / "web",
)


def markdown_files() -> list[Path]:
    files = [ROOT / "README.md"]
    for directory in SCAN_ROOTS:
        files.extend(directory.rglob("*.md"))
    return sorted(set(files))


def local_target(raw: str) -> str | None:
    target = raw.strip()
    if target.startswith("<") and target.endswith(">"):
        target = target[1:-1]
    if not target or target.startswith("#"):
        return None
    if re.match(r"^[A-Za-z][A-Za-z0-9+.-]*:", target):
        return None
    target = target.split("#", 1)[0].split("?", 1)[0]
    return unquote(target) or None


def main() -> int:
    failures: list[str] = []
    for source in markdown_files():
        text = source.read_text(encoding="utf-8")
        for match in LINK.finditer(text):
            target = local_target(match.group(1))
            if target is None:
                continue
            resolved = (source.parent / target).resolve()
            if not resolved.exists():
                line = text.count("\n", 0, match.start()) + 1
                failures.append(
                    f"{source.relative_to(ROOT)}:{line}: missing {match.group(1)}"
                )
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"Markdown links OK ({len(markdown_files())} files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
