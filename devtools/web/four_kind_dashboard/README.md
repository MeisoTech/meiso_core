# Four-kind lab dashboard

This directory contains a loopback-only development UI for a native Meiso Host
probe. It displays sanitized Session, fixture Registry, aggregate `EdgeState` /
`TelemetrySample`, and `State / Stream / Action / Blob` verification events. It is not an
application-facing SDK, a Runtime Registry implementation, a Meiso transport, or
a product HTTP protocol.

The browser never connects to the Edge and never parses Session or Resource
wire. The native probe owns direct-IP bootstrap, the POSIX adapter, Session,
test-only fixture routing, and the final verification verdict:

```text
browser -> 127.0.0.1 HTTP -> server.py -> native probe -> Meiso TCP -> Edge
```

## Run

Python 3 and a browser are the only dashboard dependencies. Pass the native
probe as an argument array after `--probe-command`; no shell evaluates it. A
literal `{test}` argument is replaced with `state`, `stream`, `action`, `blob`,
or `all`. If the placeholder is absent, the server appends `--test <kind>`.

```sh
python3 devtools/web/four_kind_dashboard/server.py \
  --probe-command /path/to/meiso_four_kind_lab \
  --host \
  --config /path/to/gitignored-lab-config \
  --test '{test}'
```

The server always binds `127.0.0.1` on an OS-selected random port and prints the
URL. It has no option to bind another address or request a stable port. Locator,
credential, SSID, IP, MAC, full PeerId, full SessionId, raw logs, and target
configuration must remain outside this directory and outside Git.

The UI does not send lifecycle commands to the native probe. Each probe owns its
bounded transcript and normal Core close. A hard 120-second local watchdog
terminates the isolated probe process group and records `FAIL` so a stuck probe
or inherited stdout cannot leave the dashboard permanently running.

## Native probe event contract

The probe writes one UTF-8 JSON object per newline on stdout. A line is limited
to 8192 bytes and a run to 4096 events. Unknown event types, invalid UTF-8/JSON,
overlong lines, and excess events terminate the run as `FAIL`. stderr is not
shown by the dashboard; diagnostics intended for the UI must use the bounded
schema below.

Every event has a `type` in:

```text
session registry state stream action blob verification
```

Common optional fields are closed enums: `layer`, `direction`, `phase`,
`status`, `test`, plus bounded numeric `monotonic_ns` and `payload_size`.
Type-specific safe fields are:

| Type | Accepted fields |
|---|---|
| `session` | `state`, `failure`, identity-known/match booleans, `session_id_known`, three negotiated numeric `limits` |
| `registry` | `state`, numeric `entries/capacity`, `source=fixture/runtime/unknown` |
| `state` | `name=EdgeState`, version, window id, interface/carrier booleans, corrected on-die temperature plus untrimmed uncertainty, verified |
| `stream` | `name=TelemetrySample`, epoch/sequence/source time, window id/elapsed time, RX/TX byte/packet/drop counters, Host-derived signal bucket, corrected on-die temperature plus untrimmed uncertainty, verified |
| `action` | exact `telemetry.reset_window` operation (only the probe's process-local counter window), delivery/accept/executed/verified booleans, closed result code, numeric result value |
| `blob` | exact `edge.capability.manifest` label, size, integrity/published/duplicate/verified booleans |
| `verification` | test, `PASS/FAIL/INCOMPLETE`, bounded check counts |

All other fields are discarded before storage. In particular the server never
retains addresses, SSIDs, MACs, arbitrary messages, credentials, full peer IDs,
full Session IDs, nonces, digests, paths, raw sysfs temperature, or raw wire
frames. Wi-Fi is a verified network telemetry endpoint, not a physical sensor.
The K230 on-die temperature is the currently verified physical sensor; only its
explicitly corrected `[-40000, 125000]` milli-Celsius value and canonical `3000`
milli-Celsius untrimmed uncertainty are accepted. The fixed manifest marks
camera and IMU `UNAVAILABLE`; microphone and display remain `UNVERIFIED` rather
than being inferred from unrelated device nodes.

The Resource projection follows `four-kind-lab-v0` exactly except for the
optional Wi-Fi signal: the native Host converts wire `signal_mbm` into the
closed `UNKNOWN / POOR / FAIR / GOOD / EXCELLENT` bucket before emitting it.
`UNKNOWN` means absent; the present ranges are `POOR < -8000`, `FAIR -8000..-7001`,
`GOOD -7000..-6001`, and `EXCELLENT -6000..0` mBm. The dashboard never receives
the exact signal value. Dashboard-generated
`process` events describe only local probe lifecycle and are not probe event
types or Resource messages.

`PASS` is never inferred from process exit or received bytes. A requested test
must emit its own `verification` event with `result=PASS` and exit zero. Missing
verdicts become `INCOMPLETE`; malformed output, nonzero exit, or a `FAIL` verdict
becomes `FAIL`. Terminal verdicts are single-assignment: a duplicate or
conflicting terminal verdict fails the entire run, and a failure is latched.
Each verdict names one Resource test, has `status == result`, and supplies a
nonzero bounded check total; `PASS` requires every check to have passed.

## Local HTTP boundary

The fixed local API is:

```text
GET  /api/v1/snapshot
GET  /api/v1/events?after=<cursor>&limit=<1..256>
POST /api/v1/tests            {"kind":"state|stream|action|blob|all"}
```

POST requires the exact loopback `Origin`, a per-process CSRF token supplied in
the served page, JSON content type, and a body of at most 4096 bytes. All
requests require the exact random-port `Host`. The service sends no CORS
permission and applies `no-store`, CSP, `nosniff`, frame denial, referrer and
permissions headers. Static files use an explicit route table rather than a
general filesystem handler.

The in-memory event ring holds at most 512 sanitized events; the browser renders
at most 256 rows. Nothing is persisted or exported. Raw board evidence belongs
in a separately authorized ignored evidence directory, not in this dashboard.

## Verify

The test creates its fake probe only inside a temporary directory. It covers
the loopback bind, random port, fixed assets, security headers, Host/Origin/CSRF,
request/body bounds, fixed command spawning, positive four-kind render data,
sensitive-field removal, malformed probe output, removed lifecycle control
surface, probe watchdog, and event-ring bounds.

```sh
python3 devtools/web/four_kind_dashboard/test_server.py
```
