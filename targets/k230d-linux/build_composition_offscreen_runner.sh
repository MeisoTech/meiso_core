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
    -DMEISO_K230_LINUX_TARGET=1 \
    -I"$repo_root/include" \
    -I"$repo_root/profiles/composition-scene-lab-v0" \
    "$repo_root/targets/k230d-linux/composition_offscreen_runner.c" \
    "$repo_root/profiles/composition-scene-lab-v0/composition_scene.c" \
    "$repo_root/profiles/composition-scene-lab-v0/scene_store.c" \
    "$repo_root/profiles/composition-scene-lab-v0/composition_coordinator.c" \
    "$repo_root/profiles/composition-scene-lab-v0/composition_reference_renderer.c" \
    "$repo_root/src/meiso_resource.c" \
    "$repo_root/src/meiso_blob.c" \
    "$repo_root/src/meiso_session.c" \
    -o "$output"
