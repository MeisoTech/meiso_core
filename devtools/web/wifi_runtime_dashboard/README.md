# Wi-Fi Runtime dashboard

Loopback-only Dev Tool for the formal `wifi-telemetry-v0` direct-IP slice. It
keeps one native macOS Host process alive and gives the browser four bounded
operations: connect, apply/start desired, stop production, and normal close.
The browser never implements Core, Runtime, Profile, wire, or transport.

```text
browser <- SSE / fixed control API -> server.py <-> native Host -> Meiso TCP -> Edge
```

## Run

Use a Git-ignored local config. The server never reads its contents or exposes
its path to the browser.

```sh
python3 devtools/web/wifi_runtime_dashboard/server.py \
  --host-binary /absolute/path/to/meiso_wifi_runtime_host \
  --config /absolute/path/to/wifi-host.local.conf
```

The CLI binds only `127.0.0.1` and defaults to port `49999`; use `--port N` to
select another loopback port. The fixed child argv is:

```text
<host-binary> --config <config> --interactive
```

It is executed with `shell=False`. Locator, config path, identities and other
process arguments cannot come from the browser.

## Lifecycle and parameters

1. **Connect** starts the Host, establishes Core Session, accepts Runtime
   catalog, and leaves the Session open.
2. **Start / apply** publishes `enabled=true` desired with one or more selected
   Wi-Fi fields and a period. It may be repeated while running.
3. **Stop** publishes `enabled=false`; Edge actual must reach `STOPPED`. Session
   and catalog stay alive.
4. **Normal close** makes the Host stop production if necessary, performs Core
   normal close, and exits.

The page derives enabled fields and period limits from the accepted catalog.
The server additionally constrains the Profile envelope to field mask
`0x001..0x3ff` and period `100..10000 ms` in `100 ms` units. Commands sent to
the Host are exact compact NDJSON:

```json
{"command":"apply","enabled":true,"selected_fields":63,"sample_period_ms":500}
{"command":"apply","enabled":false,"selected_fields":63,"sample_period_ms":500}
{"command":"close"}
```

There is no subscription. A stop disables production through desired/actual;
it does not discard produced Stream samples.

## Browser API and display contract

```text
POST /api/v1/connect
POST /api/v1/apply   {"selected_fields":N,"sample_period_ms":N}
POST /api/v1/stop
POST /api/v1/close
GET  /api/v1/events?after=<SSE event id>
```

The SSE projection accepts only these Host event types:

```text
bootstrap session catalog desired desired_delivery actual sample control run failure
```

The UI separates Host desired from Edge actual. `MATCHED` requires equal
effective enabled/fields/period, `APPLIED`, and
`desired.version == actual.observed_desired_version`; a same-value newer
desired therefore cannot match stale actual. Continuous evidence includes
sequence, gap count, browser arrival cadence, Edge monotonic `source_time_ns`,
signal history, and deltas between successive counters. JSON `null` stays
unavailable; it is never converted to zero.

The sanitizer discards unknown keys before the bounded 512-event ring. Locator,
full PeerId/EndpointId/SessionId, SSID, BSSID, MAC, credentials, raw frames and
stderr never enter the browser. There is no lifetime event cap: the deque bounds
memory while a valid Host may run indefinitely. Only startup-to-catalog and
requested normal-close have timeouts; an active link has no total-duration
watchdog.

On a new SSE page (`after=0`) or a cursor older than the ring, the server sends
one atomic `dashboard_snapshot`: reset metadata plus sticky sanitized Session,
catalog, interactive-control, latest desired/delivery, and latest actual state.
Historical samples are deliberately excluded. The client clears its sample
count, arrival/counter baselines, chart and LIVE indication, then waits for the
next new sample. A refresh neither replaces the Host child nor starts a second
Session.

## Verify

```sh
python3 devtools/web/wifi_runtime_dashboard/test_server.py
node --check devtools/web/wifi_runtime_dashboard/static/app.js
```

The fake interactive Host tests fixed argv and command shapes, connect/apply/
reconfigure/stop/close, desired/observed-actual version identity, dynamic
parameter validation, SSE sanitization, and sticky refresh recovery after a
4,200-event stream beyond the removed one-shot cap.
