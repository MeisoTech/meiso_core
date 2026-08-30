# Four-kind internal fixture

> Status: bounded lab Profile, executable codec contract, and retained vertical integration CI. It
> is not a product Profile, security transport, application Runtime, SDK API, discovery protocol,
> or sensor catalog.

This fixture predates the formal [Runtime contract](../../../docs/runtime.md). Its fixed Registry and
one-shot order are intentionally retained as a cross-layer CI path and as bounded board/data-exchange
evidence. Product endpoints must use the Runtime catalog/desired/actual/route negotiation and must
not extend `M4L1` into a parallel Runtime.

## Question and evidence boundary

This Profile answers one question: after a real Wi-Fi TCP connection and Core Session `READY`, can a
macOS Host and the K230D Linux Edge execute one truthful, bounded transcript through the existing
`State / Stream / Action / Blob` primitives?

In CI it answers the same question without claiming product negotiation: can the four public
Resource primitives, Session gate, POSIX connected-stream adapter, fixed codec/order validator and
normal/failure lifecycle still operate together? This is complementary to the formal Runtime v2
tests. The four-kind target stays blocking in normal and sanitizer configurations; a failure must be
fixed rather than skipped merely because the Profile is lab-only.

The observed endpoint combines Wi-Fi network telemetry and the K230 on-die temperature source. The
temperature value is the corrected vendor conversion with an explicit uncertainty; the raw register
or conversion code remains platform evidence and is not frozen into this wire. Camera and IMU are
unavailable in this Profile. Wi-Fi is a network-observation endpoint, not a physical sensor. No SSID,
IP address, MAC address, interface name, credential, key, device identifier, or raw private log is a
payload field.

The Profile proves only the declared lab transcript under its recorded board, image, network and
build conditions. Plain TCP plus `PeerId` exact-match is not authentication, confidentiality,
integrity or replay protection; use is restricted to the controlled lab network.

## Fixed Profile selection and limits

| item | value |
|---|---|
| Profile id | high `0x464f55525f4b494e`, low `0x445f4c41425f5630` (`FOUR_KIND_LAB_V0`) |
| Profile version | `1` |
| Core protocol | exact `1.0` |
| Runtime envelope | `M4L1`, wire version `1`, big-endian |
| Registry generation | exactly `1` for this one-shot Session |
| Registry entries | exactly `4` |
| maximum Runtime message | `512` bytes |
| selected Session limits | frame `640`, Runtime payload `512`, Registry entries `4` |
| lab TCP port | `27640` |
| Host PeerId | `0x4d344c315f484f53:0x545f4c41425f5630` (`M4L1_HOST_LAB_V0`) |
| Edge PeerId | `0x4d344c315f454447:0x455f4c41425f5630` (`M4L1_EDGE_LAB_V0`) |

The two public PeerIds identify lab protocol roles, not a device, account or unique board. Every
transport connection creates a fresh Core handle, nonce and SessionId. v0 has no resume,
catalog update, automatic Action replay or Blob continuation. A new process also creates a new Edge
incarnation, State owner scope, Stream epoch and source clock epoch.

## Fixed descriptors

All descriptors use owner role `EDGE`, schema version `1`, ResourceId high
`0x4d344c315f524553` (`M4L1_RES`) and SchemaId high `0x4d344c315f534348` (`M4L1_SCH`). Their low
words are `1..4` in the following order.

| low | kind | schema and maximum payload | meaning |
|---:|---|---|---|
| 1 | State | `edge.actual.v1`, 24 bytes | interface/carrier observations, latest corrected temperature and process-local sample-window id |
| 2 | Stream | `edge.telemetry.v1`, 80 bytes | one timestamped network-counter/optional-signal/temperature sample |
| 3 | Action | `telemetry.reset_window.v1`, 8-byte maximum result | reset only this process's telemetry baselines; no device/network reconfiguration |
| 4 | Blob | `edge.capability.manifest.v1`, exactly 401 bytes | immutable fixed manifest returned by the Edge |

The Action targets the Stream Resource, has no arguments or deadline, uses `RETRY_NONE`, and is
deduplicated by ActionId. It must not reset the Wi-Fi interface, routing, driver, thermal hardware,
Core Session or operating system counters. Success returns the new nonzero window id as one big-endian
`u64`; delivery acceptance, executor acceptance, terminal result and subsequent observations remain
distinct.

## Typed payloads

`EdgeState` is 24 bytes:

| offset | size | field |
|---:|---:|---|
| 0 | 8 | nonzero process-local `window_id` |
| 8 | 1 | flags: bit 0 interface present, 1 carrier present, bit 2 reserved zero, bit 3 temperature present |
| 9 | 3 | zero |
| 12 | 4 | signed corrected milli-Celsius |
| 16 | 4 | temperature uncertainty in milli-Celsius |
| 20 | 4 | zero |

`TelemetrySample` is 80 bytes:

| offset | size | field |
|---:|---:|---|
| 0 | 8 | matching nonzero `window_id` |
| 8 | 8 | elapsed monotonic nanoseconds since that process-local window began |
| 16..63 | 6 x 8 | RX bytes, TX bytes, RX packets, TX packets, RX dropped, TX dropped since window start |
| 64 | 4 | optional signed Wi-Fi signal in mBm |
| 68 | 4 | optional signed corrected milli-Celsius |
| 72 | 4 | temperature uncertainty in milli-Celsius |
| 76 | 1 | flags: bit 0 signal present, bit 1 temperature present |
| 77 | 3 | zero |

Absent optional values are canonical zero. Present signal is within `[-20000, 0]` mBm; present
temperature is within the K230 measurement range `[-40000, 125000]` milli-Celsius with canonical
untrimmed uncertainty `3000` milli-Celsius.
The two network flags are direct interface/carrier observations. They do not assert or infer an
assigned address or default route; establishment of the direct-IP Session does not make either
claim.
Counters are monotonic within one window and may reset only after the accepted Action creates a new
window id. The Stream source time is a separate producer monotonic time/clock epoch. Without a clock
mapping, the Host records receive time separately and does not calculate fictional one-way latency.

The current kernel temperature path performs an unbounded kernel-side poll. Therefore the Edge may
enable temperature only after one separately bounded, controlled preflight has returned a valid
corrected sample; the LinkSession/telemetry critical path consumes a cached optional sample and does
not synchronously poll thermal sysfs. Preflight failure or an out-of-domain conversion is encoded as
temperature absent, not a fabricated value. Fixing the kernel poll is outside this Profile.

## `M4L1` envelope

Every Session Runtime payload begins with this 12-byte header:

| offset | size | field |
|---:|---:|---|
| 0 | 4 | ASCII `M4L1` |
| 4 | 1 | wire version `1` |
| 5 | 1 | message type |
| 6 | 2 | zero |
| 8 | 4 | exact total envelope bytes, header included |

Every body begins with `registry_generation:u32 == 1`. Integers are big-endian; C structs are never
copied to wire. Resource and schema ids are two `u64`; owner scope is role `u8`, seven zero bytes,
then incarnation `u64`. ContentId is algorithm `u16`, digest size `u8`, one zero byte and the 32-byte
SHA-256 digest. Decoders require exact message size, zero reserved bytes, canonical fixed descriptor,
ResourceId, schema, owner role and manifest values before a typed primitive may mutate.

| type | value | sender | body after generation |
|---|---:|---|---|
| `REGISTRY` | 1 | Edge | count `u32 == 4`, then four fixed 44-byte descriptors |
| `REGISTRY_ACCEPT` | 2 | Host | accepted count `u32 == 4` |
| `STATE_ACTUAL` | 3 | Edge | State ResourceId, owner scope, nonzero version, `EdgeState` |
| `STATE_ACCEPT` | 4 | Host | State ResourceId, matching owner scope and version |
| `STREAM_METRICS` | 5 | Edge | Stream ResourceId, producer scope, epoch, sequence, source clock epoch/time, `TelemetrySample` |
| `ACTION_RESET` | 6 | Host | Action ResourceId, ActionId, expected executor, fixed Stream target, fixed Action schema, `RETRY_NONE` |
| `ACTION_DELIVERY_ACCEPT` | 7 | Edge | Action ResourceId, ActionId and executor scope |
| `ACTION_RESULT` | 8 | Edge | Action ResourceId, ActionId, executor, Action state, seven zero bytes, result window id |
| `BLOB_MANIFEST` | 9 | Edge | Blob ResourceId, fixed ContentId, length and exact manifest bytes |
| `BLOB_ACCEPT` | 10 | Host | Blob ResourceId and matching ContentId |
| `LAB_CLOSE` | 11 | Host | status `PASS` or `FAIL`, three zero bytes |

The fixed manifest is canonical UTF-8 text ending in LF. Its byte length is `401`; SHA-256 is
`7932cd21131b0eec0953082cd537915c89c2327ebb1d6c7077ab88ac6a6fc9e8`.

## Required transcript

```text
Core READY
  Edge -> REGISTRY
  Host -> REGISTRY_ACCEPT
  Edge -> STATE_ACTUAL
  Host -> STATE_ACCEPT
  Edge -> STREAM_METRICS
  Host -> ACTION_RESET
  Edge -> ACTION_DELIVERY_ACCEPT
  Edge -> ACTION_RESULT(ACCEPTED)
  Edge -> ACTION_RESULT(SUCCEEDED, new_window_id)
  Edge -> BLOB_MANIFEST
  Host -> BLOB_ACCEPT
  Host -> LAB_CLOSE(PASS)
Core CLOSE(NORMAL)
```

The Host keeps the accepted Stream sample queued through Action terminal result, records that the
sample was received, then pops it and separately records consumption. This is the executable proof
that Stream receive is not application consumption. For Blob, the Host first starts staging and
records `published=false`, copies the fixed bytes, runs `commit_verified`, records
`published=true`, and only then sends `BLOB_ACCEPT`. Transport delivery never substitutes for State
acceptance, Action result or Blob publication.

## Failure semantics

- A Runtime message before Core `READY`, before Registry acceptance, from the wrong transcript
  position/sender, or with malformed/cross-kind/undeclared/capacity-invalid data is not dispatched.
  The endpoint records a bounded local reason and requests Core close `PROTOCOL_ERROR`.
- Codec/order failure is sticky for that attempt. No retry, fallback parser, field repair or partial
  primitive mutation is allowed.
- Connect, accept, Core handshake, Registry and each semantic phase use caller-enforced monotonic
  deadlines. Timeout is a failed attempt, not an unavailable Resource silently reported as success.
- State remains invisible until a complete validated snapshot is received. Convergent delivery ends
  explicitly failed if transport is lost before `STATE_ACCEPT`.
- Stream source absence is expressed by presence flags, not invented values. Queue overflow/drop and
  receive/consume remain separately observable.
- After `ACTION_DELIVERY_ACCEPT`, disconnect without a terminal result becomes `INDETERMINATE`; the
  request is not automatically replayed on a new Session. Duplicate ActionId never resets twice.
- Blob bytes remain invisible during staging. Integrity, length or transport failure aborts staging;
  a new Session starts a new transfer. A previously verified immutable publication may remain cached
  but is not current authorization.
- `LAB_CLOSE(PASS)` is valid only after the exact successful transcript and is the only input that
  makes the order validator return `COMPLETE`. `LAB_CLOSE(FAIL)` is diagnostic only: observing it
  returns `UNEXPECTED` and latches the unsuccessful terminal, so it can never convert failure to
  completion. An endpoint that already latched a semantic failure may emit `LAB_CLOSE(FAIL)` without
  advancing the failed order validator. Outer Core close remains authoritative for Session lifecycle.

JSON/HTML output, socket bootstrap, IP configuration, polling, filesystem probes, vendor thermal
conversion, SHA-256 implementation, UI and evidence storage are platform/Dev Tool responsibilities,
not part of this codec.

## Build boundary

The Profile codec is strict C11, allocation-free and independent of sockets, JSON and Session. A
Host-native contract test needs only the public headers, `src/core/meiso_resource.c`, this codec and its
test:

```sh
cc -std=c11 -Wall -Wextra -Wpedantic -Werror \
  -Iinclude -Iprofiles/internal/four_kind \
  src/core/meiso_resource.c \
  profiles/internal/four_kind/lab_protocol.c \
  profiles/internal/four_kind/lab_protocol_message.c \
  profiles/internal/four_kind/lab_protocol_order.c \
  tests/test_four_kind_lab_protocol.c -o /tmp/four-kind-lab-protocol-test
/tmp/four-kind-lab-protocol-test
```

For K230D Linux, run the same command with the image SDK's RISC-V Linux C compiler/sysroot in place
of `cc`; do not use the RT-Smart compiler or host headers. The endpoint executable additionally links
the existing Core Session/POSIX adapter and its platform observation adapter. Direct-IP connect/listen,
fixed port `27640`, deadlines and exact lab PeerIds belong to that endpoint bootstrap, outside Core.
