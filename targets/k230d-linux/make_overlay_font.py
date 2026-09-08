#!/usr/bin/env python3
"""Build Meiso CJK16 from the pinned GNU Unifont 17.0.05 HEX font.

Input: https://unifoundry.com/pub/unifont/unifont-17.0.05/font-builds/unifont-17.0.05.hex.gz
The generated font remains under SIL OFL 1.1; install its notice and license.
The bank has an 8-byte magic and 65536 records of 16 big-endian 16-bit rows.
Eight-pixel glyphs occupy the high byte. Host geometry selects 8-pixel Basic
Latin or 16-pixel CJK cells; this is not a shaping engine.
"""
import argparse
import gzip
import hashlib
from pathlib import Path

SOURCE_SHA256 = "2ae5311c8e123e9e85f5331cd012aa99757071df23243f1487fdbf8f3acd86be"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source = args.source.read_bytes()
    if hashlib.sha256(source).hexdigest() != SOURCE_SHA256:
        parser.error("source is not the pinned Unifont 17.0.05 HEX archive")
    bank = bytearray(b"MCJK16\r\n") + bytearray(65536 * 32)
    for line in gzip.decompress(source).decode("ascii").splitlines():
        code, value = line.split(":")
        cp = int(code, 16)
        if cp > 65535:
            continue
        rows = bytes.fromhex(value)
        if len(rows) == 16:
            rows = b"".join(bytes((row, 0)) for row in rows)
        if len(rows) != 32:
            parser.error("unexpected glyph dimensions")
        bank[8 + cp * 32:8 + (cp + 1) * 32] = rows
    with args.output.open("xb") as output:
        output.write(bank)
    print(f"bytes={len(bank)} sha256={hashlib.sha256(bank).hexdigest()}")


if __name__ == "__main__":
    main()
