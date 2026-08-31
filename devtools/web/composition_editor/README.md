# Composition Editor

Loopback-only Python/vanilla WebGL devtool. Its default mode remains a recorded
local scene draft; an explicit option can start exactly one native
`CompositionHostBridge` child and project that child's typed local API.

```text
default:
browser <- fixed JSON commands / SSE -> Python RECORDED_LOCAL draft

explicit native mode:
browser <- editor intents / sanitized SSE -> Python bridge
                                              | strict Host NDJSON
                                              | CMFB/v1 socketpair
                                              v
                                      CompositionHostBridge -> K230

explicit offline simulator mode:
browser <- same intents / sanitized SSE -> Python bridge
                                              |
                                              v
                                CompositionHostBridge --simulator
                                              |
                                   caller-owned EdgeHandle
```

The Python and browser layers never parse or emit Meiso wire.

`RECORDED_LOCAL` has no Session, so its `backend.profile` is JSON `null`.
An accepted native `service.ready` identifies the service as `composition`.

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

Remote native mode has one deliberately narrow process supervisor. A typed
control-transport, Session, or catalog terminal closes the old child, clears
all old native truth and cached frames, and retries a fresh child with
exponential backoff from 100 ms to 5 s. The fresh child gets a fresh arena and
Session; after it starts, the bridge queues a complete bootstrap derived from
the newest browser-owned scene and intent snapshot. Local edits remain
candidate-first and are accepted as `NATIVE_RECONNECT_PENDING` while no child
is live. Generation guards reject late events and frames from every retired
child. Internal/local failures and simulator terminals remain sticky instead
of entering a restart loop. Terminal codes use the closed `HOST_RUNTIME_*`
vocabulary; `HOST_RUNTIME_INTERNAL_TERMINAL` remains sticky because it carries
no trustworthy recovery class. This
is not Session resume, old-handle revival,
generic SDK/backend reconnect, Edge process restart, discovery, or target
network-recovery evidence.

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
python3 devtools/web/composition_editor/server.py
```

Explicit native mode requires both options and starts one child without a
shell:

```sh
python3 devtools/web/composition_editor/server.py \
  --native-host /absolute/path/to/composition_host_bridge \
  --native-config /absolute/path/to/composition-host.conf
```

Open `http://127.0.0.1:50001/`. The listener is fixed to `127.0.0.1`;
`--port` only selects another loopback port. Omitting either native option is an
error, while omitting both preserves `RECORDED_LOCAL` behavior.

If the remote child reports one of the three continuity terminals above, the
same Composition Editor process owns the bounded retry loop. `native.reconnect` events
distinguish attempt, retry, and fresh-child-started from an authoritative
`service.ready`/snapshot; a started replacement is not displayed as Session
READY until its own native truth proves that state.

The same native service can instead run the fixed offline simulator without a
remote config or any socket:

```sh
python3 devtools/web/composition_editor/server.py \
  --native-host /absolute/path/to/composition_host_bridge \
  --native-simulator
```

The backend is shown as `NATIVE_SIMULATOR`. It executes the same bounded editor
commands and projects synthetic Wi-Fi/device/Composition truth, but it keeps
device connection, Session and TCP media evidence absent. Its caller-owned CPU
raster renderer publishes a complete CMFB/v1 `render` record (80-byte header
plus a 320x240 RGBA8888 payload); the payload SHA-256 equals the same-world
RenderResult full-target hash. It publishes no `camera` frame and makes no
driver, scanout, physical-display, target, or cross-target pixel claim.

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

Intent 参数只接受当前 Profile 的静态 envelope：render `max_hz=1..10`；camera
`fps=1..30`；Wi-Fi telemetry 的 `sample_period_ms=100..10000`（100 ms 步长）、
`selected_fields=1..1023`；Device telemetry 的 period 范围和步长相同，
`selected_fields=1..15`。即使 `enabled=false`，参数也会先验证再保存，便于后续启用
时保持同一份 intent。

这些检查不代表 Editor 已投影当前连接 catalog 中可能更窄的
`supported_fields` 子集；动态 capability 仍由 native runtime 的 actual/rejection
truth 最终裁决。

`scene.object.select` is editor-local. Every other scene edit queues the full
current scene snapshot in native mode. The only asset import presently exposed
is `builtin:cube`; adding OBJ or another editor import does not change Meiso
wire ownership.

Unknown keys, non-finite values, invalid ranges, unknown assets and missing
objects are rejected without changing the sticky revision or event ring. The
SSE ring holds the newest 256 complete projections.

## Verify

```sh
python3 devtools/web/composition_editor/test_server.py
python3 devtools/web/composition_editor/test_native_bridge.py
node --test devtools/web/composition_editor/test_model.mjs
node --check devtools/web/composition_editor/static/app.js
python3 -m py_compile \
  devtools/web/composition_editor/server.py \
  devtools/web/composition_editor/native_bridge.py
```

Recorded/local UI preview is not RenderResult evidence. Native CMFB caching and
typed service projection do not make the browser a source of protocol truth.

The fresh bounded K230D gate defined in
[`k230d-composition-e2e.md`](../../../docs/k230d-composition-e2e.md) will use
this fixed Web API and the sole native Host child to carry camera, telemetry
and scene-transform intents. Until that page records a fresh receipt, earlier
runs do not verify the current identity or Editor path. The page projection is
deliberately refreshed from a deduplicated native snapshot after Actual/media
transitions; native SSE updates do not overwrite an in-progress form unless
the browser revision changed. A future receipt is local bridge evidence, not
a remote Web control plane or a claim that browser state is protocol truth.
