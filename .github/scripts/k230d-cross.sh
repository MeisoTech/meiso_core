#!/usr/bin/env bash
# PRODUCT build gate only. No deployment, board access, or BSP/image build.
# Download identities are owned by the k230d job env in workflows/ci.yml.
set -euo pipefail
source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="${K230D_CI_WORK:-$source_root/build/k230d-ci}"
mkdir -p "$work/downloads"
work="$(cd "$work" && pwd)"
export LC_ALL=C TZ=UTC
export SOURCE_DATE_EPOCH="$(git -C "$source_root" show -s --format=%ct HEAD)"

fetch() {
    local name="$1" url="$2" digest="$3" file="$work/downloads/$1"
    if [[ ! -f "$file" ]]; then
        curl --fail --location --silent --show-error --connect-timeout 20 \
            --max-time 1200 --retry 2 "$url" -o "$file.partial"
        printf '%s  %s\n' "$digest" "$file.partial" | sha256sum -c -
        mv "$file.partial" "$file"
    fi
    printf '%s  %s\n' "$digest" "$file" | sha256sum -c -
}
fetch toolchain.tar.gz "$XUANTIE_URL" "$XUANTIE_SHA256"
fetch libdrm.tar.xz "$LIBDRM_URL" "$LIBDRM_SHA256"
fetch jpeg.tar.gz "$JPEG_URL" "$JPEG_SHA256"
mkdir -p "$work/toolchain" "$work/src/libdrm" "$work/src/jpeg" "$work/sysroot"
tar -xf "$work/downloads/toolchain.tar.gz" --strip-components=1 -C "$work/toolchain"
tar -xf "$work/downloads/libdrm.tar.xz" --strip-components=1 -C "$work/src/libdrm"
tar -xf "$work/downloads/jpeg.tar.gz" --strip-components=1 -C "$work/src/jpeg"
compiler="$work/toolchain/bin/riscv64-unknown-linux-gnu"
"$compiler-gcc" --version | head -1 | tee "$work/compiler-version.txt"
grep -q 'V3.0.2 B-20250410' "$work/compiler-version.txt"
export MEISO_K230_LINUX_CC="$compiler-gcc"
export PATH="$work/toolchain/bin:$PATH"
jobs="${K230D_CI_JOBS:-2}"

# Minimal extension to the compiler's own libc sysroot: the same libdrm 2.4.124
# and IJG JPEG 9f used by the existing Zero build. No vendor rootfs/binaries.
cat > "$work/cross.ini" <<CROSS
[binaries]
c = '$compiler-gcc'
ar = '$compiler-ar'
strip = '$compiler-strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'linux'
cpu_family = 'riscv64'
cpu = 'c908'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = ['-march=rv64gcv_xtheadc', '-mabi=lp64d', '-ffile-prefix-map=$work=.']
c_link_args = ['-march=rv64gcv_xtheadc', '-mabi=lp64d']
CROSS
meson setup "$work/drm-build" "$work/src/libdrm" --cross-file "$work/cross.ini" \
    --prefix=/usr --libdir=lib --buildtype=release --auto-features=disabled \
    -Ddefault_library=shared -Dtests=false -Dinstall-test-programs=false \
    > "$work/drm-config.log" 2>&1
meson compile -C "$work/drm-build" -j "$jobs" > "$work/drm-build.log" 2>&1
DESTDIR="$work/sysroot" meson install -C "$work/drm-build" > "$work/drm-install.log" 2>&1
mkdir -p "$work/jpeg-build"
(
    cd "$work/jpeg-build"
    CC="$compiler-gcc" AR="$compiler-ar" RANLIB="$compiler-ranlib" \
        CFLAGS="-O2 -march=rv64gcv_xtheadc -mabi=lp64d -ffile-prefix-map=$work=." \
        "$work/src/jpeg/configure" --host=riscv64-unknown-linux-gnu \
        --prefix=/usr --enable-shared --disable-static > "$work/jpeg-config.log" 2>&1
    make -j "$jobs" > "$work/jpeg-build.log" 2>&1
    make DESTDIR="$work/sysroot" install > "$work/jpeg-install.log" 2>&1
)

cmake -S "$source_root" -B "$work/edge-build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$source_root/cmake/toolchains/k230d-linux-xuantie.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DMEISO_BUILD_K230D_VIDEO_SERVICE=ON \
    -DCMAKE_C_FLAGS="-ffile-prefix-map=$source_root=. -ffile-prefix-map=$work=." \
    -DCMAKE_FIND_ROOT_PATH="$work/sysroot;$work/toolchain/sysroot" \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DMEISO_DISPLAY_DRM_INCLUDE_DIR="$work/sysroot/usr/include" \
    -DMEISO_DISPLAY_DRM_UAPI_INCLUDE_DIR="$work/sysroot/usr/include/libdrm" \
    -DMEISO_DISPLAY_DRM_LIBRARY="$work/sysroot/usr/lib/libdrm.so" \
    -DJPEG_INCLUDE_DIR="$work/sysroot/usr/include" \
    -DJPEG_LIBRARY="$work/sysroot/usr/lib/libjpeg.so" > "$work/edge-config.log" 2>&1
cmake --build "$work/edge-build" --target \
    meiso_display_ingress_video_edge meiso_display_ingress_edge_launch \
    -j "$jobs" > "$work/edge-build.log" 2>&1
cmake --install "$work/edge-build" --component K230dVideoRuntime \
    --prefix "$work/install" > "$work/edge-install.log" 2>&1

# Verify the installed product, not merely an object/library compile.
for name in meiso_display_ingress_video_edge meiso_display_ingress_edge_launch; do
    binary="$work/install/bin/$name"
    test -x "$binary"
    "$compiler-readelf" -h -l "$binary" > "$work/$name.elf.txt"
    grep -q 'RISC-V' "$work/$name.elf.txt"
    grep -q 'double-float ABI' "$work/$name.elf.txt"
    grep -q '/lib/ld-linux-riscv64-lp64d.so.1' "$work/$name.elf.txt"
done
test -x "$work/install/bin/meiso-edge-reconnect"
test -x "$work/install/share/meiso/init.d/S90meiso-edge"
test -f "$work/install/share/meiso/edge.conf.example"
ninja -C "$work/edge-build" -t commands meiso_display_ingress_video_edge > "$work/edge-commands.txt"
if grep -Eq '/devtools/|host_fixture_input|k230d-drm' "$work/edge-commands.txt"; then
    echo 'Product Edge unexpectedly depends on a reference/research source or Host fixture' >&2
    exit 1
fi
# CMake tracks every source for package provenance, including unbuilt research.
# Query actual targets instead of mistaking those configure inputs for a target.
ninja -C "$work/edge-build" -t targets all > "$work/edge-targets.txt"
if grep -Eq '^(meiso_k230d_drm_|libmeiso_k230d_drm_)' "$work/edge-targets.txt"; then
    echo 'DRM research unexpectedly entered the default build graph' >&2
    exit 1
fi
(
    cd "$work/install"
    find . -type f -print0 | sort -z | xargs -0 sha256sum > "$work/SHA256SUMS"
    tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 \
        --numeric-owner -cf - . | gzip -n > "$work/K230dVideoRuntime.tar.gz"
)
{
    printf 'source_revision=%s\n' "$(git -C "$source_root" rev-parse HEAD)"
    printf 'xuantie_sha256=%s\nlibdrm_sha256=%s\njpeg_sha256=%s\n' \
        "$XUANTIE_SHA256" "$LIBDRM_SHA256" "$JPEG_SHA256"
    cat "$work/compiler-version.txt"
} > "$work/build-info.txt"
cat "$work/SHA256SUMS"
