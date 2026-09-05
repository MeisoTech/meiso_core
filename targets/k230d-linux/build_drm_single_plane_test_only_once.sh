#!/bin/sh
set -eu

if [ "$#" -ne 1 ] || [ -z "$1" ]; then
    echo "usage: $0 OUTPUT" >&2
    exit 2
fi
if [ -z "${MEISO_K230_LINUX_CC:-}" ] ||
   [ -z "${MEISO_K230D_LIBDRM_INCLUDE_DIR:-}" ] ||
   [ -z "${MEISO_K230D_LIBDRM_LIBRARY:-}" ]; then
    echo "compiler and explicit target libdrm paths are required" >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
"$script_dir/_build_cmake_target.sh" \
    meiso_k230d_drm_single_plane_test_only_once "$1"
