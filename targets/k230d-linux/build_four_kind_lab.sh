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
    -I"$repo_root/include" \
    -I"$repo_root/adapters/posix" \
    -I"$repo_root/profiles/four-kind-lab-v0" \
    "$repo_root/devtools/four_kind_lab/four_kind_lab.c" \
    "$repo_root/profiles/four-kind-lab-v0/lab_protocol.c" \
    "$repo_root/src/meiso_resource.c" \
    "$repo_root/src/meiso_state.c" \
    "$repo_root/src/meiso_stream.c" \
    "$repo_root/src/meiso_blob.c" \
    "$repo_root/src/meiso_descriptor_set.c" \
    "$repo_root/src/meiso_action.c" \
    "$repo_root/src/meiso_session.c" \
    "$repo_root/adapters/posix/meiso_posix_tcp.c" \
    -o "$output"
