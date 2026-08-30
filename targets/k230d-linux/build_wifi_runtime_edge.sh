#!/bin/sh
set -eu

if [ "$#" -ne 1 ] || [ -z "${MEISO_K230_LINUX_CC:-}" ]; then
    echo "usage: MEISO_K230_LINUX_CC=/path/to/compiler $0 OUTPUT" >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$script_dir/_build_cmake_target.sh" \
    meiso_k230d_wifi_runtime_edge "$1"
