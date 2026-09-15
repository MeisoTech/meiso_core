#!/bin/sh
set -eu

if [ "$#" -ne 2 ] || [ -z "${MEISO_K230_LINUX_CC:-}" ]; then
    echo "internal usage: MEISO_K230_LINUX_CC=/path/to/compiler $0 TARGET OUTPUT" >&2
    exit 2
fi

target=$1
output=$2

case "$target" in
    meiso_k230d_camera_v4l2_capture_once|\
    meiso_k230d_composition_edge_service|\
    meiso_k230d_drm_single_plane_test_only_once|\
    meiso_k230d_drm_topology_admission_once|\
    meiso_composition_offscreen_runner|\
    meiso_four_kind_lab|\
    meiso_k230d_wifi_runtime_edge)
        ;;
    *)
        echo "unsupported K230D Linux CMake target: $target" >&2
        exit 2
        ;;
esac

# Requesting a historical executable explicitly opts into its archived stack.
legacy_option=OFF
case "$target" in
    meiso_k230d_composition_edge_service|meiso_composition_offscreen_runner|\
    meiso_four_kind_lab|meiso_k230d_wifi_runtime_edge)
        legacy_option=ON ;;
esac

if [ -z "$output" ] || [ -d "$output" ]; then
    echo "output must name a file: $output" >&2
    exit 2
fi
output_parent=$(dirname -- "$output")
if [ ! -d "$output_parent" ]; then
    echo "output parent directory does not exist: $output_parent" >&2
    exit 2
fi
if [ -L "$output" ]; then
    echo "refusing to overwrite a symbolic-link output: $output" >&2
    exit 2
fi
if [ -e "$output" ] && [ ! -f "$output" ]; then
    echo "refusing to overwrite a non-regular output: $output" >&2
    exit 2
fi
if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake is required to build K230D Linux targets" >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
temporary_root=$(CDPATH= cd -- "${TMPDIR:-/tmp}" && pwd)
build_dir=$(mktemp -d "$temporary_root/meiso-k230d-cmake.XXXXXX")

cleanup()
{
    if [ -n "${build_dir:-}" ] && [ -d "$build_dir" ] &&
       [ ! -L "$build_dir" ]; then
        case "$build_dir" in
            "$temporary_root"/meiso-k230d-cmake.*)
                rm -rf -- "$build_dir"
                ;;
        esac
    fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

if [ "$target" = meiso_k230d_drm_topology_admission_once ] ||
   [ "$target" = meiso_k230d_drm_single_plane_test_only_once ]; then
    if [ -z "${MEISO_K230D_LIBDRM_INCLUDE_DIR:-}" ] ||
       [ -z "${MEISO_K230D_LIBDRM_LIBRARY:-}" ]; then
        echo "DRM native build requires explicit libdrm include and library paths" >&2
        exit 2
    fi
    drm_topology_option=OFF
    drm_test_only_option=OFF
    if [ "$target" = meiso_k230d_drm_topology_admission_once ]; then
        drm_topology_option=ON
    else
        drm_test_only_option=ON
    fi
    cmake \
        -S "$repo_root" \
        -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DMEISO_BUILD_APPLICATIONS=ON \
        -DMEISO_BUILD_LEGACY_COMPOSITION="$legacy_option" \
        -DMEISO_BUILD_K230D_LINUX_APPS=ON \
        -DMEISO_BUILD_K230D_DRM_TOPOLOGY_ADMISSION_RUNNER="$drm_topology_option" \
        -DMEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RUNNER="$drm_test_only_option" \
        -DMEISO_K230D_LIBDRM_INCLUDE_DIR="$MEISO_K230D_LIBDRM_INCLUDE_DIR" \
        -DMEISO_K230D_LIBDRM_LIBRARY="$MEISO_K230D_LIBDRM_LIBRARY" \
        -DCMAKE_TOOLCHAIN_FILE="$repo_root/cmake/toolchains/k230d-linux-xuantie.cmake"
else
    cmake \
        -S "$repo_root" \
        -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DMEISO_BUILD_APPLICATIONS=ON \
        -DMEISO_BUILD_LEGACY_COMPOSITION="$legacy_option" \
        -DMEISO_BUILD_K230D_LINUX_APPS=ON \
        -DCMAKE_TOOLCHAIN_FILE="$repo_root/cmake/toolchains/k230d-linux-xuantie.cmake"
fi
cmake --build "$build_dir" --target "$target"

artifact=$build_dir/$target
if [ ! -f "$artifact" ] || [ -L "$artifact" ]; then
    echo "CMake target did not produce the expected executable: $artifact" >&2
    exit 1
fi
cmake -E copy "$artifact" "$output"
