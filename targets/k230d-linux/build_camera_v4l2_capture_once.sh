#!/bin/sh
set -eu

if [ "$#" -ne 1 ] || [ -z "${MEISO_K230_LINUX_CC:-}" ]; then
    echo "usage: MEISO_K230_LINUX_CC=/path/to/compiler $0 OUTPUT" >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
output=$1

"$MEISO_K230_LINUX_CC" \
    -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
    -march=rv64gcv_xtheadc -mabi=lp64d \
    -Wl,--dynamic-linker=/lib/ld-linux-riscv64-lp64d.so.1 \
    -DMEISO_K230_LINUX_TARGET=1 \
    -I"$repo_root/include" \
    -I"$repo_root/profiles/wifi-telemetry-v0" \
    -I"$repo_root/profiles/device-telemetry-v0" \
    -I"$repo_root/profiles/composition-scene-lab-v0" \
    -I"$repo_root/targets/k230d-linux" \
    "$repo_root/targets/k230d-linux/camera_v4l2_capture_once.c" \
    "$repo_root/targets/k230d-linux/camera_v4l2_adapter.c" \
    -o "$output"
