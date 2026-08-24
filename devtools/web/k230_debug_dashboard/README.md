# K230 debug dashboard — local draft MVP

Loopback-only Python/vanilla WebGL Host devtool for editing a local scene draft
before a native composite Host is available.

```text
browser <- fixed JSON commands / SSE -> Python recorded-local backend
                                         (no Meiso Session or K230 connection)
```

## Truth boundary

This MVP is deliberately **not** a K230 simulator and does not parse or emit
Meiso wire. The server owns one sticky local draft. The browser may add, delete,
select and transform built-in cubes; edit the scene camera; and record render,
camera and Wi-Fi telemetry intents. It never allocates a Meiso `State` version,
`ContentId`, `ObjectId`, plan/world generation, production epoch or Stream
sequence.

The UI therefore always shows:

- backend `RECORDED_LOCAL` / proof `LOCAL_DRAFT_ONLY`;
- device not connected, Session/catalog absent and native Host unbound;
- RenderResult Stream and Wi-Fi telemetry unbound;
- camera MediaStream unsupported by the current composite Profile;
- every cross-device identity as `not allocated`.

Changing a local intent cannot make any binding or actual-device status become
successful. A future native Host must replace the backend behind the same fixed
browser commands and publish its own sanitized desired-delivery/actual/Stream
events.

## Run

```sh
python3 devtools/web/k230_debug_dashboard/server.py
```

Open `http://127.0.0.1:50001/`. For an alternate foreground port:

```sh
python3 devtools/web/k230_debug_dashboard/server.py --port 55425
```

The listener is fixed to `127.0.0.1`; `--port` cannot change the bind address.

## Fixed API

```text
GET  /api/v1/snapshot
GET  /api/v1/events?after=<event id>
POST /api/v1/command
```

`POST` requires the page Origin and generated CSRF token. The accepted command
names and their exact JSON shapes are:

```text
scene.object.add        asset=builtin:cube
scene.object.delete     draft_object
scene.object.select     draft_object
scene.object.transform  draft_object, position, rotation_deg, scale
scene.camera.set        position, rotation_deg, vertical_fov_deg, near, far
intent.render.set       enabled, max_hz
intent.camera.set       enabled, fps
intent.telemetry.set    enabled, sample_period_ms, selected_fields
```

`draft_object` is a server-owned editor handle, not a Meiso `ObjectId`. Unknown
keys, non-finite values, invalid ranges, unknown assets and missing objects are
rejected without changing the sticky revision or event ring. The SSE ring holds
the newest 256 complete snapshots; ordinary `GET /api/v1/snapshot` is read-only.

## Verify

```sh
python3 devtools/web/k230_debug_dashboard/test_server.py
node --test devtools/web/k230_debug_dashboard/test_model.mjs
node --check devtools/web/k230_debug_dashboard/static/app.js
```

The tests freeze the local/unbound truth, all mutation commands, no-partial
rejection, bounded SSE, refresh behavior, CSRF/Origin/static boundaries and the
absence of Meiso identity allocation in browser commands.

## Explicitly later

- native composite Host and direct-IP TCP;
- Blob/MSH0 publication, SceneDesired/PresentationPlan and desired delivery;
- K230 actual State, Wi-Fi samples and RenderResult thumbnails;
- camera MediaStream, which requires a declared Profile route;
- reconnect, catalog replacement and target evidence.
