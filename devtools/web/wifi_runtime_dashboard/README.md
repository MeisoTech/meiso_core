# Wi-Fi Runtime dashboard

Loopback-only Dev Tool for the formal `wifi-telemetry-v0` direct-IP slice. It
starts the already-built macOS `meiso_wifi_runtime_host` and visualizes that
Host's sanitized NDJSON. It does not implement Core, Runtime, Profile, wire,
transport, Runtime control, or an SDK API.

```text
browser <- SSE <- 127.0.0.1 server.py -> fixed native Host -> Meiso TCP -> Edge
```

## Run

Use a local config ignored by Git (normally `*.local.conf`). The server never
reads its contents or sends its path to the browser.

```sh
python3 devtools/web/wifi_runtime_dashboard/server.py \
  --host-binary /absolute/path/to/meiso_wifi_runtime_host \
  --config /absolute/path/to/wifi-host.local.conf
```

Open the printed random-port URL and choose **Start one run**. The command is
fixed at startup as:

```text
<host-binary> --config <config>
```

It is executed with `shell=False`. The server always binds `127.0.0.1`; there
is no remote bind or browser-supplied argument. The Host performs desired
enable, sample receipt, desired disable, actual `STOPPED`, and normal Session
close. The page provides no extra lifecycle or sensor control.

## Display contract

Only the Host's existing NDJSON types are accepted:

```text
bootstrap session catalog desired desired_delivery actual sample run failure
```

The page separates Host-owned desired State from Edge-owned actual State and
shows `actual.data_active` as the production gate. A sample is interpreted
under the last active actual effective field mask and period, not under desired
alone. Sequence, gap/missing count, link, signal and eight counters are shown.
JSON `null` remains unavailable (`—`); it is never converted to zero.

The allow-list discards all unknown keys before the SSE ring. Locator/config
path, full PeerId/EndpointId/SessionId, SSID, BSSID, MAC, credentials, raw
frames and stderr never enter the browser. The fixed local API is:

```text
POST /api/v1/start
GET  /api/v1/events?after=<SSE event id>
```

The event ring is bounded to 512 entries. SSE reconnect uses its event id.
This page is evidence presentation only; `run=COMPLETE` is exposed only after
the native Host reports equal requested/received counts and exits with `rc=0`.
Timeout, malformed output, missing completion, or nonzero exit becomes an
explicit Dashboard failure; this does not create a second protocol verdict.

## Verify

The deterministic fake Host checks the fixed argv, localhost/static boundary,
start authorization, SSE order, null preservation, closed projection, and
absence of an extra control endpoint.

```sh
python3 devtools/web/wifi_runtime_dashboard/test_server.py
node --check devtools/web/wifi_runtime_dashboard/static/app.js
```
