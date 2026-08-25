# K230 debug dashboard

Loopback-only Python/vanilla WebGL devtool. Its default mode remains a recorded
local scene draft; an explicit option can start exactly one native
`CompositionDebugHost` child and project that child's typed local API.

```text
default:
browser <- fixed JSON commands / SSE -> Python RECORDED_LOCAL draft

explicit native mode:
browser <- editor intents / sanitized SSE -> Python bridge
                                              | strict Host NDJSON
                                              | CMFB/v1 socketpair
                                              v
                                      CompositionDebugHost -> K230

explicit offline simulator mode:
browser <- same intents / sanitized SSE -> Python bridge
                                              |
                                              v
                                CompositionDebugHost --simulator
                                              |
                                   caller-owned EdgeHandle
```

The Python and browser layers never parse or emit Meiso wire.

## Truth boundary

The browser owns editor-only handles (`draft_object`), a complete local scene
snapshot, and requested render/camera/telemetry intent. It does not allocate or
accept input for State versions, ContentIds, Meiso ObjectIds, plan/world
generations, production epochs, Stream sequences, Peer/Session identity,
bindings, nonces, or activation tokens.

In native mode the bridge converts every scene mutation into a complete strict
`set_scene` command. It converts XYZ Euler degrees to the Host's Hamilton
`qz*qy*qx` quaternion; the native service owns scene-space/time epochs and the
fixed target viewport. Native command batches are built and admitted to one
bounded local queue candidate-first. A later child failure is projected as a
terminal event; the bridge does not promise transaction rollback across
multiple already-enqueued native commands.

Only an exact allowlist of typed native output is copied into snapshots/SSE.
Read-only evidence can include State/plan/world generations, production epoch,
frame sequence/drop counters, effective media parameters, Wi-Fi/device samples,
SceneStatus and RenderResult evidence. Peer/Session identity, binding material,
nonces, tokens, ContentIds, Meiso ObjectIds and raw wire are never projected.

CMFB/v1 records arrive on a caller-created AF_UNIX socketpair passed as the
native service's `--frame-fd`. The bridge validates and caches the newest whole
render and camera record separately. It never publishes a partial record.

## Run

Recorded/local mode:

```sh
python3 devtools/web/k230_debug_dashboard/server.py
```

Explicit native mode requires both options and starts one child without a
shell:

```sh
python3 devtools/web/k230_debug_dashboard/server.py \
  --native-host /absolute/path/to/composition_debug_host \
  --native-config /absolute/path/to/composition-host.conf
```

Open `http://127.0.0.1:50001/`. The listener is fixed to `127.0.0.1`;
`--port` only selects another loopback port. Omitting either native option is an
error, while omitting both preserves `RECORDED_LOCAL` behavior.

The same native service can instead run the fixed offline simulator without a
remote config or any socket:

```sh
python3 devtools/web/k230_debug_dashboard/server.py \
  --native-host /absolute/path/to/composition_debug_host \
  --native-simulator
```

The backend is shown as `NATIVE_SIMULATOR`. It executes the same bounded editor
commands and projects synthetic Wi-Fi/device/Composition truth, but it keeps
device connection, Session and TCP media evidence absent. It does not publish
CMFB frames; the RenderResult is the SceneStore synthetic offscreen result.

## Fixed API

```text
GET  /api/v1/snapshot
GET  /api/v1/events?after=<event id>
GET  /api/v1/frames/render
GET  /api/v1/frames/camera
POST /api/v1/command
```

Frame endpoints return the newest complete record as
`application/vnd.meiso.cmfb`, or `404 FRAME_UNAVAILABLE`. They are loopback-only
and read-only.

`POST` requires the page Origin and generated CSRF token. Accepted commands and
their exact JSON shapes are:

```text
scene.object.add        asset=builtin:cube
scene.object.delete     draft_object
scene.object.select     draft_object
scene.object.transform  draft_object, position, rotation_deg, scale
scene.camera.set        position, rotation_deg, vertical_fov_deg, near, far
intent.render.set       enabled, max_hz
intent.camera.set       enabled, fps
intent.telemetry.set    enabled, sample_period_ms, selected_fields
intent.device.set       enabled, sample_period_ms, selected_fields
```

`scene.object.select` is editor-local. Every other scene edit queues the full
current scene snapshot in native mode. The only asset import presently exposed
is `builtin:cube`; adding OBJ or another editor import does not change Meiso
wire ownership.

Unknown keys, non-finite values, invalid ranges, unknown assets and missing
objects are rejected without changing the sticky revision or event ring. The
SSE ring holds the newest 256 complete projections.

## Verify

```sh
python3 devtools/web/k230_debug_dashboard/test_server.py
python3 devtools/web/k230_debug_dashboard/test_native_bridge.py
node --test devtools/web/k230_debug_dashboard/test_model.mjs
node --check devtools/web/k230_debug_dashboard/static/app.js
python3 -m py_compile \
  devtools/web/k230_debug_dashboard/server.py \
  devtools/web/k230_debug_dashboard/native_bridge.py
```

Recorded/local UI preview is not RenderResult evidence. Native CMFB caching and
typed service projection do not make the browser a source of protocol truth.

The bounded K230D run recorded in
[`k230d-composition-debug-e2e-v0.md`](../../../docs/k230d-composition-debug-e2e-v0.md)
used this fixed Web API and the sole native Host child to carry camera,
telemetry and scene-transform intents. The page projection is deliberately
refreshed from a deduplicated native snapshot after Actual/media transitions;
native SSE updates do not overwrite an in-progress form unless the browser
revision changed. That receipt is local bridge evidence, not a remote Web
control plane or a claim that browser state is protocol truth.
