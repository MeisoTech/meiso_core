# K230D DRM topology admission evidence — 2026-09-05

## Outcome

This non-normative evidence note summarizes one native topology observation made
without explicit userspace display-write calls. The owning behavior and safety
contract remains the parent [K230D target README](../README.md). The observation
closes only that non-committing topology gate for one controlled K230D boot. The
probe completed normally, but the policy result was
`ADMISSION_BLOCKED / ZPOS_UNPROVABLE`. No plane was selected and no display write
was authorized.

The canonical sanitized receipt was retained with the controlled run evidence
outside the public Git tree, as required by repository hygiene policy. It is 2021
bytes, ends with one newline, and has SHA-256:

```text
163c9111089d4385f75dd4ead87ad5c3e3006e8ab5db879e9e794ed7d5393674
```

## Exact implementation and fresh cross-build

The implementation was committed before this evidence was sealed. A fresh source
directory was created from `git archive` of that exact commit; no object or CMake
state from the earlier board build was reused.

| item | value |
|---|---|
| implementation commit | `07a365696a32a294be31805df6a78d873267e022` |
| implementation tree | `d0ea7faa7a931080f2ce6119ca7e0cf648097b27` |
| `git archive` SHA-256 | `de239243acb71da40cbeb94c512e3053b8dc26018383a076d47b8355729684b2` |
| extracted source files | 421 |
| extracted source manifest SHA-256 | `5df5ab11abc7200f34f7e6e883a0f746384e26c558f3a2d2a3d0407794d169e6` |
| CMake | `3.28.3` |
| complete CMake version-output SHA-256 | `d3d5b682f3f5ef5b24a79ed8cf4b8c5868fc950dad48cab1be0e3c060a77f8ee` |
| `CMakeCache.txt` SHA-256 | `9772a3cd1b8a235e4f2d19ec51a9e5b47782214917a7884335047c266b10839e` |
| executable `link.txt` SHA-256 | `c9fea907ab2f20c885e610f8ec72484f24e02fb948ce361d319b0fc153e3c067` |
| executable `flags.make` SHA-256 | `e8a3fca503d3a733e95570b6846118b4db6a37dad40723e0b47f9d44a18336e8` |
| native translator `flags.make` SHA-256 | `ce623dca1f397cf8c4233cbf1ee307cfda2d7d2585b28a3dc33cd0e69791a555` |

The source manifest is the SHA-256 of newline-terminated `sha256␠␠./relative-path`
lines for every regular file, with paths sorted under `LC_ALL=C`.

The configure contract was Release, tests off, applications and K230D Linux apps
on, the topology runner explicitly on, the repository K230D Xuantie toolchain
file, and explicit target libdrm header/library locators. The locators were
deployment-local and are intentionally not recorded here.

The compiler identity was:

```text
riscv64-unknown-linux-gnu-gcc
riscv64-unknown-linux-gnu-gcc (Xuantie-900 linux-6.6.0 glibc gcc Toolchain V3.0.2 B-20250410) 14.1.1 20240710
```

The compiler executable SHA-256 was
`76382afdcb7433d0ce2d58c50b20f2716bb4b533f8fea219c6121ac37225f75f`;
the full version-output SHA-256 was
`85d6298decdf27706513eabc2498de4b7636e2020f6facea3b1ec5c98a34db84`.
Here the sysroot is the toolchain's target-side header and runtime-file root. Its
`libc.so.6` was 1666592 bytes with SHA-256
`e4fee415b0de9afc46699cf1c246d1ee1c024325807bce5403d1a72ce6b93a6f`;
its dynamic loader was 157264 bytes with SHA-256
`cc210395272c657d5ef9bf1e1c8ab89264effdac357d9b9493cff079d18d7a1f`.

All nine target object depfiles (compiler-emitted source/header dependency lists)
were normalized into content-hash/relative-path lines. Backslash-newline
continuations were joined; unique absolute dependency
tokens were sorted under `LC_ALL=C`; the source, toolchain and libdrm-header roots
were replaced by `source/`, `toolchain/` and `libdrm-headers/`; and each
newline-terminated line used `sha256␠␠normalized-path`. Their combined manifest
contained 124 files and had SHA-256
`670c57caaf368952055b4988816cf3e86e427870d3e9021af4b514ca0bde54e5`:

| dependency subset | files | manifest SHA-256 |
|---|---:|---|
| exact repository sources/headers used | 12 | `c5e4222e73965223b63157fb095203a3d2ad45c226f5226b67918b1337722e66` |
| compiler/sysroot headers named by depfiles | 107 | `afed3dc6fef1ef90242352a795f007743a0a773592577c4d7e0d3a8462f769ef` |
| target libdrm headers named by depfiles | 5 | `4a33eb8b571b3779d93e1dcc7cbd41f28928ab61eeef1aba1d38a576d0b6cc16` |

Compiler depfiles are not a complete toolchain or link closure: they do not by
themselves enumerate compiler subprocesses, assembler/linker binaries, CRT or
libgcc objects, specs, linker scripts, archive members, or runtime-transitive
libraries. The separately recorded compiler, flags, link command, sysroot runtime
files and target libdrm identities narrow that gap but do not turn this note into
a hermetic build manifest.

The complete staged libdrm header tree contained 338 files; its normalized
manifest SHA-256 was
`b27e794998385928437c3f603ba78fc6bc467a1495272268234ebf6080eeeac1`.
The exact target `libdrm.so.2.124.0` used for linking was 83872 bytes with
SHA-256 `34ef3d671e5164d2701089a1d7fc21c1517e3ef1775aedb1bfdc95827acc611b`.

The fresh build produced a 42648-byte ELF with SHA-256:

```text
e9c5941806be848e58c12c57fe839da1711729e95847d6706e1be3bca780657f
```

That hash is byte-for-byte identical to the artifact deployed for the board
observation. The ELF is 64-bit little-endian RISC-V with RVC compressed
instructions and the LP64D 64-bit/double-float ABI. It uses interpreter
`/lib/ld-linux-riscv64-lp64d.so.1` and has only `libdrm.so.2` and `libc.so.6` as
loader-declared `NEEDED` libraries. Its direct libdrm imports are
the expected version/cap/node/master queries, resource/property getters, matching
free functions, and `drmSetClientCap`. It has no direct import of
`drmSetMaster`, `drmDropMaster`, atomic allocation/commit, legacy modeset,
framebuffer creation, generic DRM ioctl, or mapping APIs.

This is a controlled build attestation, not a cryptographic signature or a claim
that another environment will reproduce the same bytes without the identified
toolchain, sysroot, headers, and library.

## Single board observation

The actual device probe was started once. It exited 0 before its external
15-second watchdog fired, wrote an empty stderr, and wrote exactly the one
canonical sanitized receipt summarized above. The observed facts were:

- primary DRM node verified; universal-plane and atomic enumeration caps set;
- `canaan-drm` 1.0.0;
- one connector, one CRTC, and seven planes, all translated completely;
- one selected active route from connected DSI connector 48 to CRTC 46
  (canonical index 0), current mode 1920x1080;
- canonical topology SHA-256
  `4924deeef96f4f828accd9b85f104256acc011f1895bdeb0e48f88d2c3f5f96b`;
- zero planes whose observed `CRTC_ID` equalled 46, two unused RGB565 overlays,
  and zero candidates with a provable immutable zpos;
- `minimum_rgb565_bytes=0` is an unset sentinel because the zpos gate blocked
  before the mode/buffer-envelope gates; it is not a zero-byte requirement or a
  buffer-capacity pass;
- all 119 acquired libdrm objects released, and the device fd consumed and closed;
- the fd was observed as DRM master while open, but the program made no explicit
  set-master or drop-master call;
- zero explicit KMS mutation calls, zero framebuffer-create or dumb-create calls,
  no property write, no atomic TEST_ONLY, no real commit, no modeset, no latch,
  and no input frame.

The same-boot outer observer recorded these exact before/after comparisons:

| observation | before | after |
|---|---|---|
| DRM node holder (process with the device fd open) count | 0 | 0 |
| connector sysfs snapshot SHA-256 | `22a564c30a18f7fce85e91b93854ab6956c0f9d3c57db0a94276350e32d94bae` | same |
| framebuffer sysfs observation | empty/unavailable (`e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`) | empty/unavailable |
| kernel-log snapshot SHA-256 | `738812e7edc84eb888af54fd00bec820a49a2f6706a8872a236fdeb3fd512790` | same |
| kernel-log line count | 356 | 356 |

The redacted boot-identity SHA-256 remained
`b5e729c9bdc587d88f8008a790f1518a133767dd15695a37676f8a1c7387e6b5`.
The deployed program and resolved board `libdrm.so.2` still matched the ELF and
library hashes above after the run.

Before that observation, one wrapper attempt exited 127 because the board's
BusyBox image did not provide a `timeout` applet. The probe binary was not
started: stdout was empty, stderr was exactly 23 bytes (`sh: timeout: not found`),
and the holder, boot, connector, empty framebuffer-observation input, kernel-log,
deployed-ELF, and resolved-libdrm checks remained unchanged. It is classified as
`NOT_EXECUTED / binary_not_started`, not as another device probe and not as a
failed result that was retried to select a passing sample.

## Interpretation and remaining hard gate

`active_plane_count=0` means only that no enumerated plane's observed `CRTC_ID`
equalled CRTC 46 at that moment. It does not prove that the CRTC or panel was off,
that the screen was black, that scanout had stopped, or that taking over a plane
would be safe.

The topology is a normalized sequence of fd-visible libdrm reads, not an atomic
kernel snapshot, authentication, or a stability guarantee. Equal nonempty outer
hashes show no persistent difference in the selected connector and kernel-log
surfaces. The empty framebuffer observation supplies no framebuffer-state
evidence. None of these checks proves absence of a transient implicit-master or
last-close effect, nor pixel continuity. The running kernel image is not
cryptographically bound here to a particular published vendor-source commit.

Most importantly, two unused RGB565 overlays do not make either one admissible.
Without a driver-exposed immutable zpos, their relative order is unprovable, so
the safe result remains `ADMISSION_BLOCKED / ZPOS_UNPROVABLE`. Plane IDs,
enumeration order, overlay type, and RGB565 support must not be used as substitutes.

This evidence is for a DSI route, not HDMI. It proves no frame ingest, framebuffer,
TEST_ONLY, real commit/page-flip/event, readback, restore, Display Actual RUNNING,
panel pixel, optical, or stereo result. The next display-write slice remains
blocked until z-order is made authoritative or a separately specified and
reviewed exclusive-single-plane policy is authorized and proven.
