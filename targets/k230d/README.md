# K230D Resource Core conformance target

> Status: the frozen source has been cross-built and host-inspected as a RISC-V
> ELF and full-media image; the candidate with its locked boot-critical prefix,
> payload identity, and documented mutable-FAT boundary is `BOARD_VERIFIED` for the
> Resource semantic core v0 runner only.

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

## Host-inspected target snapshot

The 2026-08-13 offline candidate binds the payload source to
`add9b8ac49a86cb0fb10b8f8cff7cbb8f3739b27`. The source is retained as a
verified complete Git bundle in addition to the clean checkout. The source's
publication state after evidence capture is not part of the board result and
does not alter the artifact identity.

The exact 20-project CanMV v1.8 graph was built from a fresh derived output
directory with the graph's x86_64-hosted RISC-V musl compiler. The builder was
Ubuntu 24.04 x86_64, which is a recorded and successfully qualified deviation
from the upstream direct-build documentation's Ubuntu 20.04 host, not a claim
that the two host environments are identical.

- `meiso_conformance`: 325472 bytes; SHA-256
  `04b085c813f85414ce0f8284f54a8e24fe1649e113429a0df50273edb1f23e91`;
- ELF64 little-endian RISC-V `EXEC`, `lp64d`, RVC and RVV 1.0; static, entry
  `_start == 0x200000000`, RX/RW LOAD separation, no dynamic dependency or
  unresolved non-local symbol;
- `.text = 166048`, `.data = 560`, `.bss = 934896`; emitted stack-usage data
  records a largest single Meiso/test function frame of 1152 bytes, not a
  runtime peak;
- the fresh full `.img` is 650117120 bytes with SHA-256
  `bbe269ff33b8d43c1f6974ec6e0fc641347e3db9ed22ed826c23db528e577a45`;
- read-only extraction of `/sdcard/app/meiso_conformance` from that image is
  byte-identical to the inspected ELF;
- the generated `.kdimg` is 8630272 bytes with SHA-256
  `3514b5c2bd6905fdce755446b50331a47b0d51871383b872b1fd68489ebb8258`,
  but its table contains only `rtt` and `rtapp`. It is therefore
  `NO_MEISO_PAYLOAD / NOT_A_MEISO_OTA_CANDIDATE`.

All fourteen Meiso Core/test/runner objects were freshly compiled under the
application's warnings-as-errors flags with zero Meiso warning or error. The
pinned SDK and toolchain emitted 1421 reviewed upstream warnings during the
complete fresh system build; those warnings are recorded separately and are not
rewritten as Meiso diagnostics. None of these host inspections is a board
execution result.

## Board execution snapshot

The candidate described above, with the documented post-write FAT metadata
mutation below, booted on a `CanMV-K230D-Zero V1.1`. Captured output covered
U-Boot SPL, DDR training, OpenSBI, RT-Smart/CanMV, and nncase; a fixed marker
probe established that the interactive native loader was MSH.
One manual invocation of `/sdcard/app/meiso_conformance` reported `rc=0` for
`resource`, `state`, `stream`, `action`, `blob`, `descriptor_set`, and
`four_kind`, ending with `passed=7 failed=0 first_failed_suite=none`.

After a physical power cycle, a second manual process run produced the same
summary and output digest. Only the second conformance invocation has a retained
complete raw RX capture; it does not include the preceding boot. Capture was not armed before power-on, and opening the serial device may
have changed control lines, so this is not an undisturbed cold-boot trace and
does not prove LinkSession reset or autostart behavior.

The development card received the complete source-length image write and sync.
No whole-media equality was captured before automount. A later macOS
read-write automount changed writable FAT metadata, so whole-media equality was
not preserved; later comparison proved only the first 115343360-byte boot-critical
prefix and the conformance payload byte-identical to the source candidate. This acceptance is specific to a freely
reflashable development card and is not a release-media reproducibility claim.

The operator reported gradual, acceptable warming. No calibrated numeric
temperature, ambient condition, power, soak,
or thermal-fault evidence was collected. `BOARD_VERIFIED` therefore covers only
this deterministic Resource Core runner, not Link, Composition, MediaStream,
capability/platform adapters, or a product Profile.

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
toolchain. This candidate froze the archive and installed compiler identities;
every later promotion build must recheck both before using the toolchain. A
host-only conformance run does not require either download.

## SDK overlay

Set four task-specific variables. `MEISO_PUBLIC_COMMIT` must come from the
candidate lock, not from an unreviewed moving checkout. It is accepted only as
the exact 40-character lowercase commit of the clean checkout. Use an absolute
`MEISO_PUBLIC_ROOT` made only of ASCII letters, digits, `_`, `.`, `/`, and `-`;
the restricted path keeps compiler flags and provenance checks unambiguous.

```sh
SDK_ROOT=/path/to/locked/k230_rtos_sdk
SDK_TOOLCHAIN_DIR=/path/to/locked/k230_toolchains
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
  SDK_TOOLCHAIN_DIR="$SDK_TOOLCHAIN_DIR" \
  MEISO_PUBLIC_ROOT="$MEISO_PUBLIC_ROOT" \
  MEISO_PUBLIC_COMMIT="$MEISO_PUBLIC_COMMIT" \
  app-distclean
make -C "$SDK_ROOT" \
  SDK_TOOLCHAIN_DIR="$SDK_TOOLCHAIN_DIR" \
  SKIP_TOOLCHAIN_CHECK=1 \
  MEISO_PUBLIC_ROOT="$MEISO_PUBLIC_ROOT" \
  MEISO_PUBLIC_COMMIT="$MEISO_PUBLIC_COMMIT" \
  app
```

`SKIP_TOOLCHAIN_CHECK=1` only bypasses the SDK's unconditional check for the
separate XuanTie/Linux toolchain: this Zero config consumes the locked prebuilt
U-Boot and OpenSBI payloads, while the application still uses the explicit
`SDK_TOOLCHAIN_DIR` RT-Smart compiler. Use the bypass only after verifying the
RT-Smart archive/compiler identities and the prebuilt U-Boot/OpenSBI hashes;
never use it to ignore a missing or mismatched RT-Smart compiler.

The standalone ELF is staged at:

```text
output/k230d_canmv_bpi_zero_meiso_conformance_defconfig/images/sdcard/app/meiso_conformance
```

The application Makefile deliberately uses the C compiler, C11, the locked
RT-Smart linker script, and static musl linkage. It excludes the HelloWorld
template's OpenMP, MPP, OpenCV, nncase, HAL aggregation, third-party aggregation,
C++, strip, and `libm` settings. Inspect the unstripped ELF with the locked
`file`, `readelf`, `size`, and `nm` tools before packaging it.

The commands above are an ordinary application build. Promotion additionally
requires a controlled runbook to inventory the old derived output, move that
exact directory recoverably outside `output/`, recreate it from the reviewed
derived config, and prove that the new directory contained no object, ELF, or
image before the build. This avoids publishing a destructive generic cleanup
recipe and prevents stale outputs from satisfying a packaging script that did
not regenerate them. After that fresh-output preflight, build the full image
with the same variables and no explicit goal:

```sh
make -C "$SDK_ROOT" \
  SDK_TOOLCHAIN_DIR="$SDK_TOOLCHAIN_DIR" \
  SKIP_TOOLCHAIN_CHECK=1 \
  MEISO_PUBLIC_ROOT="$MEISO_PUBLIC_ROOT" \
  MEISO_PUBLIC_COMMIT="$MEISO_PUBLIC_COMMIT"
```

The complete media image is the unique top-level `*.img` under:

```text
output/k230d_canmv_bpi_zero_meiso_conformance_defconfig/
```

The exact Zero `*.kdimg` is a distinct burning-tool/OTA artifact containing only
`rtt` and `rtapp`; it does not contain the SDCARD FAT payload. Record it as a
negative packaging result, not as a Meiso deployment candidate. Never raw write
a `.kdimg` to an SD card.

## Offline promotion gate

`HOST_VERIFIED_BOARD_UNVERIFIED` requires all of the following:

- the source and owning contract commits are frozen, the checkout is clean, and
  the exact source is recoverable from a reviewed public ref or complete verified
  Git bundle;
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
- the Meiso application compiles under warnings-as-errors with zero Meiso
  diagnostic, pinned upstream full-build warnings are separately counted and
  reviewed, and ELF architecture, entry point, sections, size, static linkage,
  undefined symbols, and emitted `.su` stack reports are reviewed;
- ELF strings contain neither `MEISO_PUBLIC_ROOT` nor the current SDK/build root;
  Meiso source locations are normalized to the public `meiso_glass` prefix, while
  any pinned toolchain DWARF path is recorded as toolchain provenance;
- the exact ELF is present in `images/sdcard/app/meiso_conformance` and the ELF,
  fresh full `.img`, and optional compressed full image each have recorded names,
  byte counts, and SHA-256;
- read-only extraction proves that the full image contains that exact ELF, while
  the `.kdimg` partition table is recorded as `NO_MEISO_PAYLOAD` and is excluded
  from candidate activation;
- image inspection confirms there is no startup hook, `main.py`, daemon, or
  network activation added by this target.

Do not report vector counts or runtime peak storage: the current shared tests
expose suite results, while ELF section sizes provide only a host-side static
storage bound.

## Board activation boundary

Packaging the ELF does not prove that the observed CanMV REPL can execute it.
First boot remains RX-only. Run `/sdcard/app/meiso_conformance` manually at most
once per separately authorized boot/process attempt, only after the current prompt has been independently proven to be MSH (or
another reviewed native ELF loader) and the operator has authorized that run.
The recorded candidate passed this gate through an MSH marker probe and manual
invocation. A future candidate must repeat the gate rather than inherit this
result. Static source/image inspection shows that this target adds no autostart,
MSH builtin, startup script, persistent configuration, background task, or network
service; retained serial evidence does not independently prove runtime autostart
absence across the full cold-boot interval.
