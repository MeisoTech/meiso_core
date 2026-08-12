# K230D Resource Core conformance target

> Status: source integration prepared; target build and board execution are not
> yet verified.

This target builds the same seven current Resource Core suites used on the host:
`resource`, `state`, `stream`, `action`, `blob`, `descriptor_set`, and
`four_kind`. Each test source is compiled once more with its `main` symbol
renamed, then linked behind the platform-neutral conformance runner. It does not
include the legacy `meiso_slice` or `meiso_latest_state` sources or tests.

The Resource Core itself has no heap, socket, thread, clock, device, `stdio`, or
`libm` dependency. The conformance process uses the RT-Smart userspace C runtime
and `stdio` only for deterministic reporting; it does not use heap, sockets,
threads, clocks, devices, or `libm`. The K230D build moves the large test arenas
to static storage; host tests retain automatic storage. The target runner is
therefore deliberately single-threaded, non-reentrant, and intended to run once
per process.

## Locked SDK boundary

The initial integration is for the official CanMV v1.8 graph:

- K230 RTOS SDK commit
  `8c71e02ab098d69b9b7576bba9679b5455ad4a0d`;
- manifest commit `9271de746a3ec4eb1a0753e1b354e104567983b9`;
- base config `configs/k230d_canmv_bpi_zero_defconfig`;
- RT-Smart libraries commit
  `7d76be21ddaed3e617fe5fe78464846fa1fc7c82`.

The full resolved graph, official recovery image digests, and flash safety gates
remain outside this public source target. Do not replace these commits with a
mutable branch head.

The SDK toolchain named by this graph is an x86_64 Linux-hosted RISC-V musl
toolchain. Download the SDK and toolchain only when a supported builder is ready
and the public source commit has been frozen. A host-only conformance run does
not require either download.

## SDK overlay

Set three task-specific variables. `MEISO_PUBLIC_COMMIT` must come from the
candidate lock, not from an unreviewed moving checkout. It is accepted only as
the exact 40-character lowercase commit of the clean checkout. Use an absolute
`MEISO_PUBLIC_ROOT` made only of ASCII letters, digits, `_`, `.`, `/`, and `-`;
the restricted path keeps compiler flags and provenance checks unambiguous.

```sh
SDK_ROOT=/path/to/locked/k230_rtos_sdk
MEISO_PUBLIC_ROOT=/path/to/clean/meiso_glass
MEISO_PUBLIC_COMMIT=<full-public-source-commit>
DERIVED_CONFIG=k230d_canmv_bpi_zero_meiso_conformance_defconfig
```

Confirm the SDK graph is at the locked commits before making an overlay. Then
copy only the application integration files; the Makefile compiles Core and test
sources directly from `MEISO_PUBLIC_ROOT` and rejects a dirty or different
commit.

```sh
install -d "$SDK_ROOT/src/applications/meiso_conformance"
install -m 0644 \
  "$MEISO_PUBLIC_ROOT/targets/k230d/sdk-overlay/src/applications/meiso_conformance/Kconfig" \
  "$SDK_ROOT/src/applications/meiso_conformance/Kconfig"
install -m 0644 \
  "$MEISO_PUBLIC_ROOT/targets/k230d/sdk-overlay/src/applications/meiso_conformance/Makefile" \
  "$SDK_ROOT/src/applications/meiso_conformance/Makefile"
git -C "$SDK_ROOT" apply --check \
  "$MEISO_PUBLIC_ROOT/targets/k230d/sdk-overlay/apps.mk.patch"
git -C "$SDK_ROOT" apply \
  "$MEISO_PUBLIC_ROOT/targets/k230d/sdk-overlay/apps.mk.patch"
```

Create a derived defconfig rather than changing the locked official base. The
only semantic addition is the reviewed fragment; the config-name replacement
gives the candidate its own output directory.

```sh
awk -v derived="$DERIVED_CONFIG" '
  /^CONFIG_BOARD_CONFIG_NAME=/ {
    print "CONFIG_BOARD_CONFIG_NAME=\"" derived "\""
    next
  }
  { print }
' \
  "$SDK_ROOT/configs/k230d_canmv_bpi_zero_defconfig" \
  "$MEISO_PUBLIC_ROOT/targets/k230d/config/meiso_conformance.fragment" \
  > "$SDK_ROOT/configs/$DERIVED_CONFIG"
```

Review that derived file against the official base. Its accepted diff is one
config-name replacement plus
`CONFIG_APP_ENABLE_MEISO_CONFORMANCE=y`. Start the candidate from a clean app
build, then build from the SDK root:

```sh
make -C "$SDK_ROOT" "$DERIVED_CONFIG"
make -C "$SDK_ROOT" \
  MEISO_PUBLIC_ROOT="$MEISO_PUBLIC_ROOT" \
  MEISO_PUBLIC_COMMIT="$MEISO_PUBLIC_COMMIT" \
  app-distclean
make -C "$SDK_ROOT" \
  MEISO_PUBLIC_ROOT="$MEISO_PUBLIC_ROOT" \
  MEISO_PUBLIC_COMMIT="$MEISO_PUBLIC_COMMIT" \
  app
```

The standalone ELF is staged at:

```text
output/k230d_canmv_bpi_zero_meiso_conformance_defconfig/images/sdcard/app/meiso_conformance
```

The application Makefile deliberately uses the C compiler, C11, the locked
RT-Smart linker script, and static musl linkage. It excludes the HelloWorld
template's OpenMP, MPP, OpenCV, nncase, HAL aggregation, third-party aggregation,
C++, strip, and `libm` settings. Inspect the unstripped ELF with the locked
`file`, `readelf`, `size`, and `nm` tools before packaging it.

After the ELF gate passes, build the full image with the same variables and no
explicit goal:

```sh
make -C "$SDK_ROOT" \
  MEISO_PUBLIC_ROOT="$MEISO_PUBLIC_ROOT" \
  MEISO_PUBLIC_COMMIT="$MEISO_PUBLIC_COMMIT"
```

The complete media image is the unique top-level `*.img` under:

```text
output/k230d_canmv_bpi_zero_meiso_conformance_defconfig/
```

The `*.kdimg` is a distinct burning-tool/OTA artifact. Record the name, byte
count, and SHA-256 of the ELF, full `.img`, and `.kdimg` separately. Never raw
write a `.kdimg` to an SD card.

## Offline promotion gate

`HOST_VERIFIED_BOARD_UNVERIFIED` requires all of the following:

- the public source and owning contract commits are frozen and the checkout is
  clean;
- Debug, Release, warnings-as-errors, ASan/UBSan, analyzer, short-enum, the seven
  independent suites, and the aggregate host runner pass;
- aggregate host output starts with `MEISO_CONFORMANCE_BEGIN format=1
  source_commit=UNLOCKED_HOST_BUILD suites_total=7`; the K230D ELF contains the
  full locked source commit and
  emits that commit in the same field when it is eventually run;
- aggregate output reports every suite's return code and ends with either
  `MEISO_CONFORMANCE_PASS` or
  `MEISO_CONFORMANCE_FAIL`, including passed/failed totals and
  `first_failed_suite`;
- the exact locked SDK graph and derived-config diff are recorded;
- the target build has no warnings, and ELF architecture, entry point,
  sections, size, static linkage, undefined symbols, and emitted `.su` stack
  reports are reviewed;
- ELF strings contain neither `MEISO_PUBLIC_ROOT` nor the SDK/build root; source
  locations are normalized to the public `meiso_glass` prefix;
- the exact ELF is present in `images/sdcard/app/meiso_conformance` and the ELF,
  full `.img`, and `.kdimg` each have recorded names, byte counts, and SHA-256;
- image inspection confirms there is no startup hook, `main.py`, daemon, or
  network activation added by this target.

Do not report vector counts or runtime peak storage: the current shared tests
expose suite results, while ELF section sizes provide only a host-side static
storage bound.

## Board activation boundary

Packaging the ELF does not prove that the observed CanMV REPL can execute it.
First boot remains RX-only. Run `/sdcard/app/meiso_conformance` manually exactly
once only after the current prompt has been independently proven to be MSH (or
another reviewed native ELF loader) and the operator has authorized that run.
If the board still ends at CanMV REPL, activation is blocked. This target adds no
autostart, MSH builtin, startup script, persistent configuration, background
task, or network service.
