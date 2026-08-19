# Composition scene lab v0

> 状态：`EXPERIMENT CONTRACT / NOT IMPLEMENTED / NO INTEROPERABILITY CLAIM`。
> 本页固定首个 Edge-native scene 实验的 selected Profile、Resource Graph、容量和验收；不重定义
> [Resource](../../docs/resource.md)、[Runtime](../../docs/runtime.md)或
> [Composition](../../docs/composition.md)语义，也不是产品 Profile。

## 问题与边界

Host 前端创建 canonical triangle mesh，修改 object transform 与 camera；K230D Edge 在本地保留
一个有界 scene world、离屏渲染，并以 Stream 返回结果 metadata 和小尺寸 raw thumbnail。

本实验只证明：

- Host Blob、Host State、Edge actual State、Edge Stream 组成一个真实 Edge-native scene；
- 完整 world snapshot 在 Edge 原子应用，mesh/buffer/object storage 全部有界；
- scene 与现有 Wi-Fi endpoint 在同一 Session/catalog/TX scheduler 中互不混淆；
- 一个真实 K230D off-screen result 随 mesh/transform/camera 改变。

本实验不证明 physical display、stereo、lens correction、完整 framebuffer 回传、MediaStream、
durable world persistence、通用 scene graph、任意 material/shader、animation、lighting 或产品性能。

## Exact composite Session Profile

Session 只有一个 `MeisoProfileRef`。本实验必须新增并冻结一个 composite ProfileRef，符号名为
`K230_SCN/_LAB_V00:v1`；实现工作单元在写 codec 前给它分配唯一 128-bit ProfileId，并以 literal
handshake golden bytes 固定。它不是同时协商两个 Profile，而是精确包含：

1. [`wifi-telemetry-v0`](../wifi-telemetry-v0/README.md) 的 endpoint/schema/行为；
2. 本页的 presentation endpoint；
3. 本页的 scene endpoint。

wifi-only 或 scene-only peer 与 composite peer 建立 Session 必须得到
`MEISO_SESSION_FAILURE_PROFILE_MISMATCH`。单独 Session 只可用于 C1/C2 过渡 fixture，不计入
combined board acceptance。

首片 selected Session/Runtime limits：

| limit | 值 / 下界 |
|---|---:|
| `max_runtime_payload_bytes` | 8192 |
| `max_frame_bytes` | 至少 `8192 + MEISO_SESSION_WIRE_HEADER_SIZE` |
| `max_registry_entries` | 至少 12 |
| Runtime `max_endpoints` | 至少 3 |
| Runtime `max_endpoint_history` | 至少 3 |
| Runtime `max_routes` | 至少 9 |
| Runtime `max_capability_bytes` | 至少三 endpoint capability 编码总和 |
| Blob simultaneous staging transfers | 1 |

若 literal catalog/State/Stream/Blob frame 在 codec 冻结后超过上述值，应减小 Profile payload 或
显式发布新 Profile version，不能在 target 上静默提高限制。

## Combined Resource Graph

Endpoint/Resource/schema 使用独立的 `WIFI_TLM/*` 与 `K230_SCN/*` namespace。下列符号名在 C0
转为非零 128-bit constant；任何 collision 使完整 catalog reject。

| endpoint | facet | owner/kind | ResourceId | schema | 方向 |
|---|---|---|---|---|---|
| `WIFI_TLM/ENDPOINT` | desired | Host/State | 既有 | 既有 | Host→Edge |
|  | actual | Edge/State | 既有 | 既有 | Edge→Host |
|  | data | Edge/Stream | 既有 | 既有 | Edge→Host |
| `K230_SCN/PRESENT` | desired | Host/State | `K230_SCN/PLAN_DES` | `K230_SCH/PLAN_V00:v1` | Host→Edge |
|  | actual | Edge/State | `K230_SCN/PLAN_ACT` | `K230_SCH/PSTAT_00:v1` | Edge→Host |
| `K230_SCN/SCENE` | desired | Host/State | `K230_SCN/SCN_DES` | `K230_SCH/SCENE_00:v1` | Host→Edge |
|  | actual | Edge/State | `K230_SCN/SCN_ACT` | `K230_SCH/SSTAT_00:v1` | Edge→Host |
|  | immutable | Host/Blob | `K230_SCN/MESH_BLB` | `K230_SCH/MESH_V00:v1` | Host→Edge |
|  | data | Edge/Stream | `K230_SCN/RES_STR` | `K230_SCH/RRES_V00:v1` | Edge→Host |

两个新 endpoint 的 capability 都内联于 catalog；source v0 不增加第二份 capability State。
Presentation capability 只允许一个 required `EDGE_NATIVE Projection` layer，引用 scene endpoint；
Scene capability 固定本页 object/mesh/target/thumbnail 上限。

`PresentationPlan` 仍按 Composition 的双代/prepare/token/activate 规则传递，首片候选只包含一个
layer：source=`EDGE_NATIVE`、geometry=`Projection`、binding=`K230_SCN/SCENE`、单 camera、
320×240 target。Host 可以允许 `USE_SAFETY_FRAME`，不能声明未实现的 representation downgrade。
Plan State version、plan generation 与 scene State version 各自独立；scene 更新不制造新 plan，只有
layer/binding/target/capability 改变才建立新 plan generation。

Blob route 在 catalog 接受后即可 staging/commit，不受 scene production gate 控制，否则资产无法在
world prepare 前到达；Blob publication 本身也不激活 object。只有 `SceneStatus` actual 经 Core State
语义提交为 `RUNNING` 后，`K230_SCN/RES_STR` data gate 才开放。

当前 Runtime wire v2 会拒绝 `IMMUTABLE/Blob` route，且 `STREAM_SAMPLE` 固定 Edge→Host。C1/C2
因此只做 in-memory primitive；R1 新增 Runtime wire version（实现时冻结，预期 `MRT0 v3`），在
Session 的 opaque Runtime payload 内加入 Blob carriage 和 owner/direction 校验。不得把新 frame
追加到 v2；v2 literal bytes、六类 frame meaning 与 Core 均不变。combined remote catalog 在 R1
前不得宣称可接受。

## Host State 与 Edge retained world

`SceneDesired` 是 Host-owned、完整、不可变 State snapshot；Runtime canonical desired prefix 的
`enabled` 控制 scene/result producer，Profile suffix 至少包含：

```text
scene flags
camera: space + space epoch + source clock/time + pose + projection + viewport
object_count
objects[object_count]:
  ObjectId
  Mesh ContentId (SHA-256)
  WORLD space + space epoch
  position xyz / normalized quaternion xyzw / positive scale xyz
  visibility flags
```

该 State 使用 convergent/latest snapshot 行为：Host 可在 pending delivery 时用更新的 camera/
transform snapshot 替换旧 pending version，Edge 不承诺渲染每个编辑中间态。每个公开 result 必须
指出它实际应用的 owner/version；没有 result 不能被 Host 推断为已渲染。

规则：

- `ObjectId` 只在 Host scene owner incarnation 内稳定；同 snapshot 重复或零 ID malformed；
- State 是全量 snapshot，不传 create/update/delete log；新 snapshot 未激活前旧 world 不变；
- object transform/camera 数值必须 finite，quaternion 可验证归一，scale 为正，projection/near/far/
  viewport 落在 capability；错误不能部分应用；
- ContentId 不存在时 candidate 为 `WAITING_BLOB`，不是用 placeholder 静默成功；迟到 Blob 只唤醒
  仍为 current desired 的 candidate；
- absence 从新 active world 删除 object；unreferenced mesh 可按本地 eviction policy 释放，但 cache
  命中不恢复 desired、plan 或授权；
- same ContentId/same bytes 幂等，same ContentId/different bytes 是 integrity conflict；Blob 全长与
  SHA-256 commit 前不可见。

Edge world object 生命周期：

```text
EMPTY
  -> WAITING_BLOB / VALIDATING
  -> PREPARING_BUFFERS
  -> READY_CANDIDATE
  -> Composition-controlled atomic ACTIVE(world_generation++)
  -> RETIRING -> RELEASED
```

最多同时存在 `one active + one candidate`。初次进入 ACTIVE 必须与 PresentationPlan activation
是同一 Composition transaction；plan 已 ACTIVE 时，后续 world swap 也只能发生在其控制的完整
frame boundary。该边界之前新 desired 只能是 observed/pending，不能成为新 applied/RUNNING truth。
Scene desired supersede 取消旧 candidate；其已 commit Blob 可以留在有界 cache，但 renderer
handles/job/output 不可移入新 world。Session loss 按 PresentationPlan offline lease 处理；Edge
restart 后 world、production epoch 和 local handles 全新。

## Mesh Blob v0

Mesh 是有限 immutable Blob，不是 State 或 Stream。C0 codec 必须冻结 canonical big-endian
header、truncation/reserved rules 与 literal cube vector；语义面固定为：

```text
triangle list
position: finite float32 xyz
optional vertex color: canonical RGBA8
index: uint32
object-local right-handed coordinates
vertex_count / index_count / exact byte length / local bounds
```

`index_count` 必须非零且为 3 的倍数；每个 index 小于 `vertex_count`；bounds 必须由同一 bytes
验证，不能信任未验证 sidecar。v0 不接受 texture、skin、animation、shader、compression、NaN/
Inf、外部文件路径或 native GPU buffer descriptor。

## SceneStatus 基础信息 State

`SceneStatus` 使用 Runtime canonical actual prefix，因此 observed/applied desired、apply result、
availability、effective enabled、production status/reason 与 `production_epoch` 不另造字段。Profile
suffix 是 Edge truth，至少报告：

```text
active plan generation
world_generation
active object_count / unique_mesh_count
published_blob_count / published_blob_bytes
resident_mesh_bytes
target_slots_used / target_bytes
render_jobs_in_flight / result_queue_depth
last_result stream epoch / sequence
stable reason code
```

`RUNNING` 只在所有 active object Blob/renderer buffer 就绪、plan 已 active 且 result producer 已
建立非零 scene production epoch 后发布。`WAITING_BLOB / OBJECT_CAPACITY / BLOB_CAPACITY /
MESH_BYTES_CAPACITY / BUFFER_CAPACITY / INVALID_SCENE / RENDERER_FAILED` 必须可区分。新 desired 被
拒绝时 actual 仍可精确指出旧 applied desired/world 正在 RUNNING。

Wi-Fi 与 scene 的 actual、production epoch/high-water/data gate 完全独立；二者可使用相同数值但
identity 不同。disable scene 不停止 Wi-Fi，disable Wi-Fi 不改变 active world。

## RenderResult Stream

首片 result 是 Edge-owned Core Stream，不是 framebuffer truth 或 Action result。每个 sample
绑定 scene `production_epoch`，并在 payload 中至少携带：

```text
applied SceneDesired owner incarnation + State version
active plan generation + world_generation
result code / evidence flags
object_count + unique_mesh_count
render start/end source monotonic time
color/depth target extent and format
full off-screen target checksum
thumbnail width=64 / height=48 / format=RGB565 / exact 6144 bytes
```

Profile 最大 cadence 为 10 Hz；queue slots 为 4，overflow `DROP_OLDEST`，下一 sample 必须通过
Stream gap/missing count 暴露。固定 thumbnail 不得改尺寸、压缩或追加完整 frame。完整持续画面
是后续 MediaStream，使用新 capability/Profile version。

Evidence flags 至少区分 `OFFSCREEN_COMPLETE / THUMBNAIL_INCLUDED / FENCE_COMPLETE /
DISPLAY_COMMIT_ACCEPTED / VBLANK_OBSERVED / PHYSICAL_VERIFIED`。K1 只允许前两项；没有观察就必须
为零。

## Fixed capacities 与公式

首片固定：

| symbol | value |
|---|---:|
| `O_max` objects | 32 |
| `M_max` unique mesh ContentIds | 8 |
| `B_blob_max` per mesh Blob | 256 KiB |
| `P_blob` published Blob slots | 16（active 8 + candidate 8 worst case） |
| `T_blob` simultaneous staging transfers | 1 |
| `C_blob` wire chunk payload | 4096 bytes |
| `B_mesh_resident_per_world_max` | 2 MiB |
| `B_mesh_resident_total_max` | 4 MiB（active + candidate） |
| render extent | 320×240 |
| color format | RGBA8888 |
| depth format | 32-bit implementation-local depth |
| `K_target` color/depth target sets | 2 |
| `J_render` in-flight render jobs | 2 |
| `Q_result` result Stream slots | 4 |
| thumbnail | 64×48 RGB565 = 6144 bytes |

实现必须从编码常量计算 State max payload，不能依赖 `sizeof(C struct)`：

```text
B_scene_profile_max = B_scene_header + B_camera + O_max * B_object_record
B_scene_state_max   = MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE
                    + B_scene_profile_max

B_mesh_resident(world) = sum over unique meshes referenced by world(
    align(vertex_count * vertex_stride, A_gpu)
  + align(index_count * 4, A_gpu))
  <= B_mesh_resident_per_world_max

B_mesh_resident(active) + B_mesh_resident(candidate)
  <= B_mesh_resident_total_max

stride_color = align(320 * 4, A_scanout)
stride_depth = align(320 * 4, A_depth)
B_targets = K_target * (
    align(stride_color * 240, A_buffer)
  + align(stride_depth * 240, A_buffer))

B_blob_arena = meiso_blob_store_memory_required(
    descriptor.max_payload = B_blob_max,
    published_slot_count = P_blob)
// v0 storage model approximately includes (P_blob + staging + verification)
// full payload regions; implementation uses the API result, not this approximation.

B_compositor_total = B_session_arena + B_runtime_arena
                   + B_plan_and_scene_state
                   + B_blob_arena
                   + B_mesh_resident_total_max
                   + B_targets
                   + J_render * B_job
                   + B_renderer_scratch
```

C1/K1 启动前必须记录每一项 exact bytes、alignment、memory domain 与合计；任何 arena/CMA/heap
申请失败都在启用 scene 前失败。不能先接受 desired 再依赖无界 malloc。`O_max`、`M_max`、
`B_blob_max`、两个 resident-mesh budget 与 target layout 任一改变都要求新 capability generation；
超限 desired/Blob 不改变旧 active world。一个已占满 8 个 mesh 的 active world 切换到 8 个完全
不同 mesh 的 candidate 必须在上述 16 slots/4 MiB worst-case budget 内成功，不能把 Profile 内合法
snapshot 因隐藏双代开销拒绝。

## TX fairness

一个 Session 当前只有一个 TX slot。target integration 使用以下 lab scheduler contract：

- control queues 与每条 data/Blob route 都固定容量；
- control burst 最多连续 4 个成功 TX slot，随后若 data/blob pending 必须进入 data round；
- data round 对每个 ready route最多发送一帧；Blob transfer 每轮最多一个 chunk；
- `WIFI_TLM/DATA_STR` 与 `K230_SCN/RES_STR` 都 ready 时，任一路由等待不超过
  `4 + N_ready_routes` 个成功 TX slot；
- TX busy 不消耗 round/sequence；deadline 到期的 result 依 Stream policy 丢弃并报告 gap；Blob
  不允许用无限 retry queue 延长等待。

Host/Edge loopback 用 deterministic TX-busy injection 证明 slot bound；K230D mixed run 同时传一个
最大 Blob、10 Hz thumbnail 与 1 Hz Wi-Fi sample，Wi-Fi 无 scheduler-induced silent starvation。

## Required sequence

```text
Core Session READY under exact composite ProfileRef
  -> Edge offers one combined catalog (3 endpoints / 9 routes)
  -> Host atomically accepts catalog
  -> Host publishes and commits Mesh Blob(s)
  -> Host publishes complete SceneDesired State
  -> Edge validates/prepares world; SceneStatus remains non-producing
  -> Host publishes one EDGE_NATIVE PresentationPlan
  -> Edge PREPARED token / Host token echo / Edge ACTIVE
  -> Edge actual reports applied scene + RUNNING + scene production_epoch
  -> Edge RenderResult Stream references exact applied scene version
  -> Wi-Fi samples continue on independent epoch/gate
```

Plan 可早于 Blob/scene 到达，但只能保持 preparing；它不能让 incomplete world active。实现 transcript
必须分别记录 State delivery acceptance、SceneStatus applied、plan prepared/active 与首个 result，
不能压成一个 success。

## Failure vectors 与验收

必须执行：

- wrong Session ProfileRef；duplicate/colliding EndpointId/ResourceId/schema；partial catalog；
- Blob oversize、bad length/hash、chunk gap/overlap/out-of-order、exact duplicate、conflicting duplicate、
  Session loss mid-transfer、published-slot exhaustion、8-mesh active→8 distinct-mesh candidate；
- SceneDesired unknown ContentId、duplicate/zero ObjectId、33 objects、9 unique meshes、resident bytes
  over 2 MiB、invalid index/float/quaternion/scale/camera/space epoch；
- desired duplicate/stale/conflict/supersede；Blob 为已 supersede candidate 迟到；renderer prepare/
  job/target capacity failure；partial candidate；
- result duplicate/stale/epoch mismatch/deadline expiry/queue overflow；fixed thumbnail truncation/trailing
  bytes/format mismatch；
- independent Wi-Fi/scene enable、disable、reconfigure、production epoch reuse；combined catalog replace
  后旧 route/desired/actual/sample；
- deterministic mixed TX load 和 TX-busy injection，验证 scheduler slot bound 与 gap counters；
- new Session 必须 reaccept catalog，并重发 referenced Blob、both desired/plan；duplicate Blob 可幂等
  接受，但 Host 不依赖 Edge process cache。Edge restart/new incarnation 丢失 process-local Blob store，
  no-resurrection。

最小 success transcript 是：`cube publish → one object → object move → camera move → second
object sharing same mesh → delete first → scene disable`。每一步 Host 收到新 applied SceneDesired
version 和不同或按 oracle 预期相同的 result checksum/thumbnail；共享 mesh 不重复占 published Blob
slot。全过程同时保持 Wi-Fi Stream，且没有跨 endpoint gate、epoch 或 payload routing。
