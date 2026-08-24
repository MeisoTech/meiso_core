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
    -I"$repo_root/adapters/posix" \
    -I"$repo_root/devtools" \
    -I"$repo_root/profiles/wifi-telemetry-v0" \
    -I"$repo_root/profiles/device-telemetry-v0" \
    -I"$repo_root/profiles/composition-scene-lab-v0" \
    -I"$repo_root/targets/k230d-linux" \
    "$repo_root/targets/k230d-linux/composition_debug_edge.c" \
    "$repo_root/targets/k230d-linux/composition_edge_app.c" \
    "$repo_root/targets/k230d-linux/wifi_linux_adapter.c" \
    "$repo_root/targets/k230d-linux/device_telemetry_linux_adapter.c" \
    "$repo_root/targets/k230d-linux/camera_v4l2_adapter.c" \
    "$repo_root/devtools/composition_debug_peer.c" \
    "$repo_root/profiles/composition-scene-lab-v0/composition_media_posix_tcp.c" \
    "$repo_root/profiles/composition-scene-lab-v0/composition_media_transport.c" \
    "$repo_root/profiles/composition-scene-lab-v0/composition_session_v3.c" \
    "$repo_root/profiles/composition-scene-lab-v0/composition_scene.c" \
    "$repo_root/profiles/composition-scene-lab-v0/scene_store.c" \
    "$repo_root/profiles/composition-scene-lab-v0/composition_coordinator.c" \
    "$repo_root/profiles/composition-scene-lab-v0/composition_cpu_raster_renderer.c" \
    "$repo_root/profiles/wifi-telemetry-v0/wifi_telemetry.c" \
    "$repo_root/profiles/device-telemetry-v0/device_telemetry.c" \
    "$repo_root/src/meiso_resource.c" \
    "$repo_root/src/meiso_state.c" \
    "$repo_root/src/meiso_stream.c" \
    "$repo_root/src/meiso_blob.c" \
    "$repo_root/src/meiso_descriptor_set.c" \
    "$repo_root/src/meiso_action.c" \
    "$repo_root/src/meiso_session.c" \
    "$repo_root/src/meiso_runtime.c" \
    "$repo_root/src/meiso_runtime_scheduler.c" \
    "$repo_root/adapters/posix/meiso_posix_tcp.c" \
    -pthread -lm -o "$output"
