# Composition scene lab v0

> 状态：`C0 CODEC IMPLEMENTED / C1 RESOURCE SEMANTIC VERIFIED / C2 PORTABLE COORDINATOR SEMANTIC VERIFIED / R1 PORTABLE BLOB CARRIAGE VERIFIED / K1a HOST OFFSCREEN REFERENCE VERIFIED / TARGET BUILD VERIFIED / TARGET NOT RUN`。
> 本页固定首个 Edge-native scene 实验的 composite Profile、Resource Graph、字节布局、容量和验收；不重定义
> [Resource](../../docs/resource.md)、[Runtime](../../docs/runtime.md)或
> [Composition](../../docs/composition.md)语义，也不是产品 Profile。

## 问题与边界

Host 前端创建 canonical triangle mesh，修改 object transform 与 camera；K230D Edge 在本地保留
一个有界 scene world、离屏渲染，并以 Stream 返回结果 metadata 和小尺寸 raw thumbnail。

本实验完成后的最终验收范围是：

- Host Blob、Host State、Edge actual State、Edge Stream 组成一个真实 Edge-native scene；
- 完整 world snapshot 在 Edge 原子应用，mesh/buffer/object storage 全部有界；
- scene 与现有 Wi-Fi endpoint 在同一 Session/catalog/TX scheduler 中互不混淆；
- 一个真实 K230D off-screen result 随 mesh/transform/camera 改变。

本实验不证明 physical display、stereo、lens correction、完整 framebuffer 回传、MediaStream、
durable world persistence、通用 scene graph、任意 material/shader、animation、lighting 或产品性能。

## Exact composite Session Profile

Session 只有一个 `MeisoProfileRef`。本实验 composite ProfileId 已冻结为
`{0x4b3233305f53434e, 0x5f4c41425f563030}`（ASCII `K230_SCN/_LAB_V00`），并有两个不可混用的
exact version：

| ProfileRef | Runtime wire | 用途 |
|---|---|---|
| `K230_SCN/_LAB_V00:v1` | MRT0 v2 | C0--C2 compatibility；仍拒绝 `IMMUTABLE/Blob` route |
| `K230_SCN/_LAB_V00:v2` | MRT0 v3 | R1 显式 opt-in；允许本页 Host→Edge Mesh Blob carriage |

v2 不覆写或 fallback 到 v1；Profile/version mismatch 在 Session handshake 失败。两版都不是同时
协商两个 Profile，而是精确包含：

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
| `max_frame_bytes` | 至少 8236（8192 payload + 32-byte Runtime prefix + 12-byte Session header） |
| `max_registry_entries` | 至少 12 |
| Runtime `max_endpoints` | 至少 3 |
| Runtime `max_endpoint_history` | 至少 3 |
| Runtime `max_routes` | 至少 9 |
| Runtime `max_capability_bytes` | 至少三 endpoint capability 编码总和 |
| Blob simultaneous staging transfers | 1 |

若 literal catalog/State/Stream/Blob frame 在 codec 冻结后超过上述值，应减小 Profile payload 或
显式发布新 Profile version，不能在 target 上静默提高限制。

## Combined Resource Graph

Endpoint/Resource/schema 使用独立的 `WIFI_TLM/*` 与 `K230_SCN/*` namespace。所有 composition
identifier 的 high word 分别为 `K230_SCN` 或 `K230_SCH`；low word 如下，均按 8-byte ASCII packed
为非零 `u64`。任何 endpoint/resource/schema collision 使完整 catalog reject。

| endpoint | facet | owner/kind | ResourceId | schema | 方向 |
|---|---|---|---|---|---|
| `WIFI_TLM/ENDPOINT` | desired | Host/State | 既有 | 既有 | Host→Edge |
|  | actual | Edge/State | 既有 | 既有 | Edge→Host |
|  | data | Edge/Stream | 既有 | 既有 | Edge→Host |
| `PRESENT_` | desired | Host/State | `PLAN_DES` | `PLAN_V00:v1` | Host→Edge |
|  | actual | Edge/State | `PLAN_ACT` | `PSTAT_00:v1` | Edge→Host |
| `SCENE___` | desired | Host/State | `SCN_DES\0` | `SCENE_00:v1` | Host→Edge |
|  | actual | Edge/State | `SCN_ACT\0` | `SSTAT_00:v1` | Edge→Host |
|  | immutable | Host/Blob | `MESH_BLB` | `MESH_V00:v1` | Host→Edge |
|  | data | Edge/Stream | `RES_STR\0` | `RRES_V00:v1` | Edge→Host |

Presentation/Scene capability schema low words are `PCAP_V00`/`SCAP_V00`，版本均为 1。C0 builder
只构造上述 2 endpoints/6 routes；combined integration 必须再加入既有 Wi-Fi 1 endpoint/3 routes，
并运行 identifier disjoint check。combined catalog canonical encoded size 为 920 bytes。

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

Runtime v2 继续拒绝 `IMMUTABLE/Blob` route；R1 的 strict MRT0 v3 handle 在 Session opaque Runtime
payload 内加入 Host-owned Blob carriage 与 owner/direction 校验。v3 支持同 catalog 多条 Blob
route，但每个 handle 全局只有一个 transfer；`STREAM_SAMPLE` 仍只支持 Edge→Host。v2 literal bytes、
六类 frame meaning、Core API/source 与 Session wire 均不变。R1 已证明 portable catalog/Blob
carriage；R2a 已证明portable combined builder与独立fair scheduler；R2b0已证明真实AF_INET loopback
happy path，R2b1证明same-Session lifecycle，R2b2证明same-Session catalog replacement，R2b3已证明
explicit same-process RST/new-Session recovery，R2b4已证明max-single-Blob/resource boundary，R2b5
已证明wrong-Profile fail-closed及分方向backpressure/fairness slice；其余negative matrix与target
interoperability留待后续/K1。

## C0 canonical wire

所有整数使用 big-endian，长度必须 exact，表中未分配字节必须为零。`float32` 要求 IEEE-754
binary32；encoder/decoder 均拒绝 NaN、±Inf、负零和 subnormal。FOV 单位为 degree；quaternion
顺序 `xyzw`、平方范数误差不超过 `2^-12`，canonical hemisphere 依 `w,z,y,x` 第一个非零分量为正。
codec 不分配 heap，不以 `sizeof(C struct)` 作为 wire size。

Capability 是 catalog inline literal：

| payload | bytes | offsets / meaning |
|---|---:|---|
| `PCAP` | 32 | `0 magic:u32, 4 version:u16, 6 required_layers:u16=1, 8 width:u16=320, 10 height:u16=240, 12 color:u16=RGBA8888, 14 depth:u16=DEPTH32, 16 max_hz:u16=10, 18 reserved:2, 20 max_offline_lease_ms:u32=5000, 24 reserved:8` |
| `SCAP` | 52 | `0 magic:u32, 4 version:u16, 6 flags:u16=0, 8 objects:u32=32, 12 unique_meshes:u32=8, 16 blob_max:u32=262144, 20 published_slots:u32=16, 24 staging:u32=1, 28 chunk:u32=4096, 32 resident_world:u32=2097152, 36 resident_total:u32=4194304, 40 thumb_w:u16=64, 42 thumb_h:u16=48, 44 thumb_format:u16=RGB565, 46 max_hz:u8=10, 47 result_slots:u8=4, 48 jobs:u8=2, 49 target_sets:u8=2, 50 reserved:2` |

State/Blob/Stream Profile suffix：

| suffix | bytes | fixed header / records |
|---|---:|---|
| `PLN0` PresentationPlan | 128 | `0 magic:u32, 4 version:u16, 6 flags:u16, 8 plan_generation:u64, 16 capability_generation:u64, 24 candidate_sha256:32, 56 scene_endpoint:16, 72 source:u8, 73 geometry:u8, 74 required:u8=1, 75 token_echo_present:u8, 76 target_w/h:u16×2, 80 color/depth:u16×2, 84 max_hz:u16, 86 reserved:2, 88 offline_lease_ms:u32, 92 reserved:4, 96 token_echo:32` |
| `PST0` PresentationStatus | 160 | `0 magic:u32, 4 version:u16, 6 reserved:2, 8 capability_generation:u64, 16 candidate_generation:u64, 24 candidate_sha256:32, 56 active_generation:u64, 64 active_sha256:32, 96 candidate_state:u8, 97 active_state:u8, 98 reason:u16, 100 token_present:u8, 101 reserved:3, 104 token:32, 136 target_w/h:u16×2, 140 color/depth:u16×2, 144 scene_endpoint:16` |
| `MSH0` Mesh | `64 + vertices + indices`, max 262144 | `0 magic:u32, 4 version:u16, 6 flags:u16, 8 vertex_count:u32, 12 index_count:u32, 16 vertex_stride:u32=12|16, 20 vertex_bytes:u32, 24 index_bytes:u32, 28 reserved:4, 32 bounds_min:f32×3, 44 bounds_max:f32×3, 56 reserved:8`; records are BE `xyz[f32]` plus optional `RGBA8`, then BE `u32` indices |
| `SCN0` SceneDesired | `112 + 112*object_count`, max 3696 | `0 magic:u32, 4 version:u16, 6 flags:u16=0, 8 count:u32, 12 reserved:4, 16 camera:96`; each object is `ObjectId:16, ContentId:36, space:u8, flags:u8, reserved:2, space_epoch:u64, position:f32×3, quat:f32×4, scale:f32×3, reserved:8` |
| `SST0` SceneStatus | 112 | `0 magic:u32, 4 version:u16, 6 reason:u16, 8 active_plan:u64, 16 world:u64, 24 objects:u32, 28 meshes:u32, 32 blobs:u32, 36 target_slots:u32, 40 blob_bytes:u64, 48 resident_bytes:u64, 56 target_bytes:u64, 64 jobs:u32, 68 result_depth:u32, 72 last_epoch:u64, 80 last_sequence:u64, 88 reserved:24` |
| `RRS0` RenderResult | 6304 | `0 magic:u32, 4 version:u16, 6 code:u16=COMPLETE, 8 evidence:u32, 12 objects/meshes:u16×2, 16 applied_owner:u64, 24 applied_version:u64, 32 active_plan:u64, 40 world:u64, 48 source_clock_epoch:u64, 56 start_ns:u64, 64 end_ns:u64, 72 target_w/h:u16×2, 76 color/depth:u16×2, 80 logical_color/depth_row_bytes:u32×2=1280, 88 full_target_sha256:32, 120 thumb_w/h:u16×2, 124 thumb_format:u16=RGB565, 126 reserved:2, 128 thumb_row_bytes:u32=128, 132 thumb_bytes:u32=6144, 136 reserved:24, 160 thumbnail:6144` |

Camera record 为 `space:u8, projection:u8, reserved:6, space_epoch:u64, clock_epoch:u64,
monotonic_ns:u64, position:f32×3, quat:f32×4, vertical_fov:f32, near:f32, far:f32,
viewport_xywh:u16×4, reserved:16`。同一 SceneDesired 的 camera/object 必须使用同一非零 WORLD
epoch。`ContentId` record 为 `algorithm:u16,digest_size:u8,reserved:u8,SHA-256:32`。

`RGB565` pixel 是 BE `u16`，bit 15..11/10..5/4..0 分别为 R/G/B。RenderResult 的 SHA-256 域
是恰好 `320*240*4` 个可见、row-major RGBA8888 bytes，不含 padding/depth；两个 logical row
bytes 描述 canonical layout，K230 DMA/physical stride 仍为 backend 私有。Stream dependency v0 必须
为空，因此最大 Runtime envelope 是 `12 + 144 + 6304 = 6460` bytes；SceneDesired 最大 envelope
为 3812 bytes。

`candidate_sha256` 是 canonical 128-byte `PLN0` 的 SHA-256，计算时 digest 字段、
`token_echo_present` 与 token bytes 全部清零；token echo 不改变 candidate identity。codec 只验证
shape/非零，Composition coordinator 必须复算。`capability_generation` 必须等于承载相应 endpoint
capability 的 Runtime `catalog_generation`。

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
- absence 从新 active world 删除 object；C0/C1 使用的 Core BlobStore 不提供 eviction，已发布但
  未引用 mesh 在同一 store lifetime 内继续占 slot。第 17 个不同 ContentId 明确
  `BLOB_CAPACITY`；cache 命中或 duplicate publish 都不恢复 desired、plan 或授权；
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
handles/job/output 不可移入新 world。产品集成最终须按PresentationPlan定义处理Session loss与offline
lease；当前C2/K1a和offline runner没有Session-loss signal，不执行lease。Edge restart后world、
production epoch和local handles全新。

## Mesh Blob v0

Mesh 是有限 immutable Blob，不是 State 或 Stream。C0 codec 已冻结 canonical big-endian
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

Scene Runtime actual 与 suffix 的最小合法组合如下；未列组合由 coordinator 拒绝，codec 本身不
承担跨字段状态机：

| phase | Runtime canonical actual | SceneStatus suffix |
|---|---|---|
| disabled | `APPLIED, effective=false, STOPPED, epoch=0, reason=DISABLED` | 可保留旧 world 统计，但不再产 result |
| preparing | `PENDING, effective=false, STARTING, epoch=0, reason=APPLYING` | `WAITING_BLOB` 或明确 capacity/validation reason |
| running | `APPLIED, PRESENT, effective=true, RUNNING, epoch>0, reason=NONE` | `READY` 且 plan/world 非零 |
| reconfigure pending | `PENDING, effective=true, RUNNING, old epoch, reason=APPLYING` | 仍描述旧 applied world；candidate 不冒充 active |
| rejected with old world | `REJECTED, effective=true, RUNNING, old epoch` | 明确失败 reason，旧 world/result 可继续 |
| failed without usable world | `FAILED, effective=false, FAILED, epoch=0` | 明确 `INVALID_SCENE/RENDERER_FAILED/capacity` |

Presentation candidate 顺序是 `PREPARING -> PREPARED(token) -> ACTIVATION_PENDING(same token) ->
ACTIVE`。PREPARED/ACTIVATION_PENDING 都要求非零 token、`reason=NONE`；Composition 在同一 frame
transaction 原子切换 active plan 和 READY_CANDIDATE world，随后才允许 Scene actual 进入 RUNNING。
Presentation/Scene actual 是两条独立 Core State，但 coordinator 必须保证上述联合边界。

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
full off-screen target SHA-256
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

logical_color_row_bytes = logical_depth_row_bytes = 320 * 4
B_targets_private = K_target * (
    align(private_color_stride * 240, A_buffer)
  + align(private_depth_stride * 240, A_buffer))

B_blob_arena = meiso_blob_store_memory_required(
    descriptor.max_payload = B_blob_max,
    published_slot_count = P_blob)
// v0 storage model approximately includes (P_blob + staging + verification)
// full payload regions; implementation uses the API result, not this approximation.

B_compositor_total = B_session_arena + B_runtime_arena
                   + B_plan_and_scene_state
                   + B_blob_arena
                   + B_mesh_resident_total_max
                   + B_targets_private
                   + J_render * B_job
                   + B_renderer_scratch
```

v0 capability admission 要求 backend 的 private color/depth stride 也可取 1280，且两个 target set
的已分配 bytes 不超过 1228800；仅 allocation 起始地址可以额外对齐。需要更大 row padding 的 backend
必须拒绝本 Profile，而不是在 `SceneStatus.target_bytes` 中发布超出 capability 的值。

C1/K1 启动前必须记录每一项 exact bytes、alignment、memory domain 与合计；任何 arena/CMA/heap
申请失败都在启用 scene 前失败。不能先接受 desired 再依赖无界 malloc。`O_max`、`M_max`、
`B_blob_max`、两个 resident-mesh budget 与 target layout 任一改变都要求新 capability generation；
超限 desired/Blob 不改变旧 active world。一个已占满 8 个 mesh 的 active world 切换到 8 个完全
不同 mesh 的 candidate 必须在上述 16 slots/4 MiB worst-case budget 内成功，不能把 Profile 内合法
snapshot 因隐藏双代开销拒绝；该切换后 store 已用满，继续引入新 ContentId 可合法返回
`BLOB_CAPACITY`。后续若增加 eviction，必须另行冻结与 active/candidate/published visibility 的关系。

## K1a Host synthetic renderer 与 offline runner evidence

public commit `c3b4770` 加入portable `composition_reference_renderer`。它借用caller-owned BlobStore，
自身caller arena固定两个job及两个完整320×240 RGBA8888/implementation-depth32 target set；当前macOS
arm64最小arena 1,268,872 bytes/alignment 8，其中target 1,228,800 bytes、metadata 40,072 bytes。
control-scope改变会清queue/targets/latest result，`retire_world`精确清退同scope旧world；render只写
inactive target，失败保留旧complete与head。complete views借用arena，evidence flags恒为exact `0x3`。

该backend读取每个已验证canonical triangle-list Mesh的全部bytes和完整SceneDesired，但并不按三角形
几何光栅化；color/depth/64×48 thumbnail只是source-commit-pinned synthetic oracle，pixel算法不是Profile
wire合同。它不拥有SceneStore/Coordinator/Stream/Session/Runtime、heap/thread/socket/driver/native handle
或CMA；RenderResult进入external Stream仍由后续integration负责。Actions run `32326481615`四个job全部
success，证据标签为`HOST_OFFSCREEN_REFERENCE_VERIFIED`。

public commit `b9ec6a3` 的offline runner执行一次`baseline_cube_once`：一个cube Blob、一个object/world、
一个Plan和一次render；receipt显式`full_lifecycle=false`。当前macOS五个caller allocation依次为
BlobStore 4,720,304（`cpu_heap_core_blob`）、SceneStore 31,656（`cpu_heap_profile`）、Coordinator
1,520（`cpu_heap_profile`）、renderer 1,268,872（`cpu_heap_reference_targets`）、runner scratch
14,720 bytes（`cpu_heap_runner`），均alignment 8，合计6,037,072 bytes。checked allocator的零后续
allocation只限定runner wrapper scope；数值不含stack/libc/OS、Session/Runtime/scheduler/State/Stream/TCP、
GPU/CMA。

native runner可标`HOST_OFFSCREEN_REFERENCE_VERIFIED`。tracked cross脚本已在linux/amd64 Ubuntu 22.04
container中用官方K230 SDK默认Xuantie toolchain两次fresh、byte-exact构建neutral
`k230d_linux_candidate`，故标`TARGET_BUILD_VERIFIED`。83,152-byte ELF的SHA-256为
`05accb66e621c0bef981e78312fae851a8f0cb8b233e1a9c89e4d648340972f8`；它尚未执行，没有board
identity或target NDJSON receipt，必须同时标`TARGET_NOT_RUN`，不能自证
`TARGET_OFFSCREEN_REFERENCE_VERIFIED`。runner不连接Wi-Fi，不执行offline lease，也不覆盖
move/camera/delete/disable、320×240以外分辨率、performance或physical output。

## R2a TX fairness 与 combined builder evidence（完成）

public commit `5d92ff6` 在 Runtime--SDK 层增加 caller-owned `meiso_runtime_scheduler`，绑定本地 peer
的 exact Runtime/Session pair 并成为该 pair 唯一 TX owner；它不修改 Core、Session wire 或 direct
Runtime API。Composition Profile v2 builder 以 no-partial-output 语义选择 exact Session limits
`8236/8192/12`，按 Wi-Fi/Presentation/Scene 顺序生成 3 endpoint、9 route 与 canonical 920-byte
catalog。Profile v1 与 MRT0 v2 不变。

R2a scheduler 已实现并冻结以下 lab contract：

- Host与Edge各自只调度本peer的outbound队列，并分别计successful TX slot；不得把full-duplex两个
  方向合成一个全局slot序列。Host序列包含Host control与Blob chunk，Edge序列包含Edge control、
  Wi-Fi sample与RenderResult；peer接收或另一方向的发送不消耗本方向round/bound；
- control queues 与每条 data/Blob route 都固定容量；
- 每个peer的control burst最多连续4个本方向成功TX slot，随后若本方向data/blob pending必须进入
  本方向data round；
- data round对本方向每个ready route最多发送一帧；Host Blob transfer每轮最多一个chunk；
- `WIFI_TLM/DATA_STR` 与 `K230_SCN/RES_STR` 都 ready 时，任一路由等待不超过
  `4 + N_edge_ready_routes` 个Edge outbound成功TX slot；Host ready Blob route对应使用
  `4 + N_host_ready_routes` 个Host outbound成功TX slot；
- local TX busy不消耗该peer的round/sequence。

确定性vector证明Host单Blob data route等待 `<=4+1=5` 个Host成功slot；Edge Wi-Fi与RenderResult同时
ready时各等待 `<=4+2=6` 个Edge成功slot。`data_probe_due`使nonbusy control reject后下一step先探测
data；successful slot只表示direct Runtime send返回`OK`且frame已排入Session outbound，不表示socket
write、peer receive、application commit或时延上界。`TX_BUSY`对完整Scheduler/Runtime/Session arena、
source、output与pending frame原字节零推进。

State `OFFER`形成commit obligation；Blob仅在接收`COMMIT`后Runtime snapshot为
`publish_pending && !result_pending`时形成Core publish obligation。length/SHA precheck失败直接产生
terminal `RESULT`且没有publish obligation。active catalog generation替换清binding/round并进入
`REBIND_REQUIRED`；Session terminal永久进入`SESSION_TERMINAL`并exact-once通知sources，旧对象不能
rebind，必须新建Session/Runtime/Scheduler。

public commit `5d92ff6` 的fresh Debug CTest为24/24，strict C11/C++17通过，clang analyzer对scheduler、
builder、test为0 finding。该结果只证明portable/in-process调度与Profile builder；source queue的
deadline/drop/gap policy仍由Profile integration负责。R2b0现已覆盖真实AF_INET loopback happy path，
R2b1覆盖same-Session lifecycle，R2b2覆盖same-Session catalog replacement，R2b3覆盖explicit
same-process RST/new-Session recovery，R2b4覆盖max-single-Blob/resource boundary，R2b5覆盖
wrong-Profile fail-closed、分方向真实backpressure与5/6-slot fairness；自动resume、fresh Edge及未覆盖的
negative/stress matrix仍待验证；K1a已有Host synthetic renderer，但K1 target仍须验证K230D，
Hz/latency/jitter/throughput/performance也未测量。

## R2b0 AF_INET combined semantic evidence（完成）

public commit `7e53bc9` 使用真实`127.0.0.1` AF_INET `SOCK_STREAM`；Host和Edge各自以nonblocking
POSIX adapter驱动自己的R2a scheduler。每次transport I/O限制为17 bytes，双方Runtime frame均实际
跨越多次`WRITE`/`READ_PROGRESS`。同一个Profile v2/MRT0 v3 Session接受exact 3 endpoint、9 route、
920-byte catalog，并完成以下happy-path transcript：

- cube Mesh Blob经Runtime reassembly与generic Core publish bridge提交，再显式`mark_visible`；
- Scene desired与PresentationPlan State经C2 prepare/token/activate进入`ACTIVE`；
- Wi-Fi、Scene、Presentation actual分别使用production epoch `77`、`42`、`41`；
- `WIFI_TLM/DATA_STR`在exact route上产生epoch `177`/sequence `1`，`K230_SCN/RES_STR`在exact
  route上产生epoch `417`/sequence `1`。

fresh Debug与ASan+UBSan CTest均为25/25，clang analyzer为0 finding；GitHub Actions run
`32316527393`四个job全部success。状态仅为
`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED / TRANSPORT_VERIFIED`（loopback happy path）。该证据
不证明wrong Profile、catalog replacement、reconnect、backpressure、fair-contention stress、K230/K1、
renderer/present、Hz/latency/performance、transport security或physical output。

## R2b1 same-Session lifecycle evidence（完成）

public commit `9daa7f9` 沿用R2b0同一个真实TCP Session、exact catalog、Plan 1及已发布cube：

- world 1/RenderResult sequence 1；Scene desired v2 pending时old-world exact sequence 2；TRS world 2/
  sequence 3；camera world 3/sequence 4；第二个共享cube对象world 4/sequence 5；删除首对象world 5/
  sequence 6；各结果的world/version/object/mesh字段均逐项验证；
- 每个desired/actual State均完成offer→replica commit→accept→publisher confirmed闭环并保持byte-exact；
  Plan 1的PlanRef/digest不变；Scene actual production epoch `43..46`分别在actual前拒绝new epoch、
  actual后拒绝old epoch，且拒绝不消费RenderResult sequence；
- 全程不重传cube Blob，BlobStore count/bytes/publications/duplicates、Host/Edge transfer high-water及
  transfer idle flags前后不变；
- 8-byte prefix-only Presentation desired v3先进入disable pending；`drained=false`返回`WAITING_DRAIN`
  且Coordinator/SceneStore arena raw zero-diff，`drained=true`再原子退役；
- Presentation与Scene actual均byte-exact进入`STOPPED`，SceneStatus稳定为reason `NONE`且无active
  plan/world/object；RenderResult随后得到`ROUTE_NOT_ACTIVE`，Wi-Fi仍以epoch `77`发送sequence `2`。

fresh Debug与ASan+UBSan full CTest均25/25，clang analyzer 0 finding；GitHub Actions run
`32317542841`四个job全部success。`9daa7f9`已push到`main`与`rain/core-session-v0`。状态仍只到
`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED / TRANSPORT_VERIFIED`（loopback same-Session happy
lifecycle），不证明catalog replacement、RST/reconnect、max-Blob、wrong Profile、backpressure、
K230/K1、renderer/present、性能、security或physical output。

## R2b2 same-Session catalog replacement evidence（完成）

public commit `369ff4d` 沿用R2b1的真实TCP Session：

- catalog g2 offer只建立staged/pending catalog，active g1、3/9 shape、双方scheduler phase/generation与
  全部source boundary不变；accept后Host/Edge active catalog原子切到g2；
- 双方scheduler为每个source exact-once报告`CATALOG_CHANGED`并进入`REBIND_REQUIRED`；重复step不重报，
  stale g1 route不能bind，所有route-bound source重绑g2后才恢复READY；两端各一个stale g1 queued
  work得到`SOURCE_INVALID`，Session outbound保持空；
- SessionId、Session handle、fd、peer/incarnation、limits与`READY`全程不变；Runtime清除
  Wi-Fi/Presentation/Scene actual与data gate，但保留production high-water `77/41/46`；
- fresh capability-generation-2 coordinator推进SceneStore control scope并清world/candidate/visibility；
  旧coordinator API、old token/world activation与old-world result ref均fail closed；
- Core Blob cache继续包含cube，但Scene desired State v6先得到`MISSING_BLOB`；g2 Blob transfer经Core
  返回`DUPLICATE`并显式`mark_visible`后才prepare；
- 新State闭环的desired version为Wi-Fi/Scene/Plan `2/6/6`，actual version为Wi-Fi/Presentation/Scene
  `2/3/7`；Plan/capability generation均为2，Plan desired v4建立candidate/new token，v5 old-token
  echo mismatch保持candidate不变，v6 new-token echo激活world 6；actual production epoch为
  `88/51/52`；旧production epoch `77/46`与旧Stream epoch `177/417`分别被对应gate拒绝；新的
  Wi-Fi epoch `178`与RenderResult epoch `418`均从sequence 1无gap恢复。

fresh Debug与ASan+UBSan full CTest均25/25，clang analyzer 0 finding；Actions run `32318799466`
四个job全部success。`369ff4d`已push到`main`与`rain/core-session-v0`。状态只到
`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED / TRANSPORT_VERIFIED`（loopback same-Session catalog
replacement），不证明RST/reconnect、max-Blob、wrong Profile、backpressure、K230/K1、renderer/
present、性能、security或physical output。

## R2b3 explicit RST/new-Session recovery evidence（完成的 slice）

public commits `9df5268`、`e4a726f`与`43ae3ce` 已push到`main`与`rain/core-session-v0`：

- Runtime refresh在READY且SessionId/role/limits exact时保持完整arena byte-for-byte；terminal首次原子清
  catalog、pending State/Blob与Edge scratch，保留Blob transfer-id high-water并返回`CORE_NOT_READY`，
  以后永久`STALE_SESSION`。live mismatch与verifier reentry有独立vector；Scheduler在poll/TX前refresh，
  清理后才exact-once通知`SESSION_TERMINAL`；
- g2 ACTIVE后distinct Mesh完成`BEGIN/READY/CHUNK`但不发`COMMIT`；Edge adapter abort、`SO_LINGER`
  RST/close后Host read检测loss，两端Session均为`FAILED/TRANSPORT_LOST`。未完成Blob未进入Core，旧
  Runtime/Scheduler/route/epoch fail closed，SceneStore旧active world仍保持；
- 新AF_INET连接使用不同SessionId及全新Session/Runtime/Scheduler/coordinator arenas；同process Core
  cache与SceneStore保留；新coordinator开始新control scope后才清world/visibility并使旧coordinator/
  token/world失效，desired、visibility、world及actual/data gate均不自动继承。显式提高State
  delivery generation、重发cube并取得Core `DUPLICATE`、再次`mark_visible`后，以Plan generation 3/
  new token、Wi-Fi/Presentation/Scene actual epoch `99/61/62`及Wi-Fi/RenderResult epoch `179/419`
  sequence 1恢复；
- Linux CI首次暴露测试把scratch假设为arena tail而忽略padding；`43ae3ce`改为用真实COMMIT
  Runtime-scratch borrow校准offset，并在同配置新fixture重验范围，产品实现未改。Actions run
  `32321051775`四个job全部success。

状态为`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED / TRANSPORT_VERIFIED`（loopback explicit
same-process reconnect）。这不是自动resume，也不证明fresh Edge process、wrong Profile、max-Blob、
backpressure、K230/K1、renderer/present、性能、安全transport或physical output。

## R2b4 max-Blob/resource-boundary evidence（完成）

public commit `a392850` 已push到`main`与`rain/core-session-v0`，沿用R2b3重连后的真实AF_INET Session：

- canonical `MSH0` total exact 262144 bytes：64-byte header、1 vertex、65517 indices、262080-byte
  vertex+index区域；ContentId SHA-256为
  `418a5b882e82eafe440a720a040cc8e2e36ee4cefdd1c7cbe6114e1df091cf8f`；Runtime SHA、Profile verifier
  与decode shape全部一致；
- 262145-byte `BEGIN`在Host Runtime返回`PAYLOAD_CAPACITY`。source记录rejected completion且scheduler
  `data_probe_due`置位，但successful slots、Session outbound、Runtime transfer/high-water、两端
  Runtime/Session、Edge scheduler、Core BlobStore与SceneStore均不变；
- 合法transfer exact 64×4096-byte `CHUNK`，BEGIN的唯一`RESULT`为`READY`，COMMIT的terminal
  `RESULT`为`PUBLISHED`；每个CHUNK后Edge result source completion不变，即无逐块ACK；
- 第32块后Edge反向发送Wi-Fi production epoch `99`/Stream epoch `179`/sequence `2`及old-world
  RenderResult production epoch `62`/Stream epoch `419`/sequence `2`。Host 64个Blob data slots与Edge
  两个反向data slots按peer独立；
- COMMIT经Runtime scratch、Core bridge与Mesh verifier发布，显式`mark_visible`后Scene desired v8在既有
  Plan generation 3下swap；Scene actual v9/epoch `62` invalid transition拒绝，v10/epoch `63`提交，旧
  production epoch `62` result拒绝，RenderResult epoch `419`/sequence `3`引用新world。

R2b4 resource ledger严格分层：逻辑Mesh total为262144、active resident vertex+index为262080；Core
BlobStore中cube 304 + max Mesh 262144，共2 blobs/262448 logical bytes。当前macOS arm64五arena（alignment
8）为Host Runtime 17,384、Edge Runtime 279,528（含262,144 scratch）、2-slot Mesh BlobStore
1,049,056、SceneStore 31,656、Coordinator 1,520，合计1,379,144 bytes。target/job为0。该合计不含
Session、scheduler、POSIX adapter、State/Stream、Host external asset buffer、TCP kernel、GPU/CMA；2-slot是
integration fixture配置，不能外推为SCAP 16-slot/8+8 candidate aggregate evidence。

Actions run `32322475279`四个job全部success。状态是
`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED / TRANSPORT_VERIFIED`（loopback max-single-Blob/resource
boundary）；不证明自动resume、fresh Edge、wrong Profile、backpressure、fair-contention stress、K230/K1、
renderer/present、Hz/latency/jitter/throughput/performance、安全transport或physical output。

## R2b5 adversarial TCP evidence（完成的 slice）

public commit `6488fef` 已push到`main`与`rain/core-session-v0`：

- Composition Profile v1和Wi-Fi Profile分别连接Composition Profile v2时，Edge在真实AF_INET
  handshake中以`PROFILE_MISMATCH`失败；两端都没有SessionId，catalog/Runtime frame均未开始，failure
  frame后Host也fail closed。匹配的Profile v2正例仍到达READY；
- Edge→Host RenderResult与Host→Edge max-Blob `CHUNK`分别在nonblocking adapter上取得真实
  `WRITE_WOULD_BLOCK`。Session保持READY/outbound pending时，下一次scheduler step精确返回`TX_BUSY`，
  Scheduler/Runtime/Session/POSIX-adapter arena、sources、output及poll/completion原字节不变；
- 接收端排空后，全部已排队RenderResult或Blob CHUNK按payload/offset各接收一次；Blob以显式ABORT
  终结，Core BlobStore仍为空；
- Host方向同时ready的4个control与Blob data在5个successful slots内完成，Blob是第5个；Edge方向
  4个control及Wi-Fi/RenderResult在6个successful slots内完成，两条data属于同一冻结round。每段中
  对端scheduler successful-slot计数不变。

Actions run `32324328746`四个job全部success。上述两个backpressure/fairness方向在同一Session内依次
验证，不是simultaneous socket saturation，不给出wall-clock、latency、jitter、throughput或performance
界，也不证明无穷ready source、自动resume、fresh Edge、SCAP 16-slot aggregate、K230/K1、renderer/
present、安全transport或physical output。

## C0 executable evidence

[`composition_scene.h`](composition_scene.h) 与 [`composition_scene.c`](composition_scene.c) 是 C0
唯一 codec 实现；`meiso_composition_scene_test` 冻结：

- exact Profile/endpoint/resource/schema IDs、32/52-byte capabilities、6 composition routes 和与 Wi-Fi
  namespace disjoint；
- Plan/PresentationStatus/Mesh/SceneDesired/SceneStatus/RenderResult literal bytes；每个 fixed codec 的
  every-prefix truncation、trailing byte、reserved-byte mutation、capacity 与 no-partial-output；
- independently calculated Plan SHA-256 preimage/digest vector、binary32/quat/camera/mesh bounds/index、
  32-object/8-mesh/262144-byte Mesh/6304-byte result boundaries；
- compile-time 920-byte combined catalog、3812-byte Scene State、6460-byte result Stream 与 8236-byte
  Session frame envelope。

该C0证据只证明 portable Host codec/wire oracle；C2另行证明coordinator的digest/token/state-machine
enforcement，R1再证明Profile v2/MRT0 v3 Blob carriage。C0本身不证明Host frontend、K230D
store/renderer或board interop；Profile v1/MRT0 v2仍必须拒绝本Profile的IMMUTABLE route。

## C1 Resource fixture evidence

[`scene_store.h`](scene_store.h) 与 [`scene_store.c`](scene_store.c) 是无heap、无driver的
caller-owned Resource fixture；测试不创建Session/Runtime handle，也不走Session/Runtime wire。
commit `4bccc12` 的 `meiso_composition_scene_resource_test` 使用真实Core primitive验证：

- canonical Mesh用完整bytes的SHA-256作为ContentId；commit前不可见，bad digest或invalid Mesh不可
  授权；new desired失败会淘汰旧candidate但不改变old active；
- Host desired StatePublisher→Edge StateReplica→SceneStore，Edge SceneStatus StatePublisher→Host
  StateReplica，RenderResult进入4-slot `MeisoStream`；没有mock Resource合同；
- cube→TRS move→camera move→second Mesh→shared Mesh→delete保持精确owner/version/world identity；
  disable按expected active ref原子退役并停止render；mesh/camera/TRS各自独立改变full-target SHA-256
  与thumbnail；
- 新State owner清active/candidate/observation、store内last-result引用与per-owner Blob visibility；底层
  immutable cache可留，但只有新owner重新publish或verified duplicate后才能重新引用；world
  generation在同一Edge store lifetime内保持单调。外部Stream queue/history不由store清理；
- 33 objects、9 unique meshes、16 publication slots、8+8 distinct logical mesh bytes、State
  duplicate/stale/conflict、late Blob、output alias、Stream duplicate/stale/epoch mismatch与DROP_OLDEST
  都有确定性vector。

当前macOS arm64 ABI记录的exact caller arenas为：BlobStore 4,720,304 bytes、desired
publisher/replica 3,920/3,856、actual publisher/replica 384/320、result Stream 32,272、scene store
31,656；均要求8-byte alignment，合计4,792,712 bytes。它们均由Host test process heap提供且
CPU-visible，没有GPU/CMA arena。`resident_mesh_bytes` 是每world引用的
canonical vertex+index逻辑字节，8+8边界为4,193,280 bytes；
CPU fixture从BlobStore零拷贝读取。fake renderer逐pixel流式计算320×240 target SHA-256和64×48
RGB565 thumbnail，不持有renderer target或job，所以SceneStatus中target/job如实为`0/0`。

fresh strict build与ASan+UBSan全套均CTest 22/22。该C1证据不证明独立4 MiB GPU resident arena、
2个target set/CMA allocation、Composition token/coordinator、Runtime Blob carriage、Session、K230D
backend或display；coordinator与Blob carriage分别由下一节C2/R1验证。

## C2 Portable coordinator evidence

public commit `4210ddf` 的[`composition_coordinator.h`](composition_coordinator.h)与
[`composition_coordinator.c`](composition_coordinator.c)在Core Resource之上、renderer SDK seam之前
加入caller-owned portable coordinator；Core source/API与Session wire均未改变。它验证：

- Runtime desired shape精确为disabled的8-byte canonical prefix/零Profile suffix，或enabled的同一prefix
  加exact 128-byte `PLN0`；每owner/control-scope `plan_generation`不复用，同generation Plan除token echo
  外不可变，State version只按PREPARING/terminal/PREPARED/active规则推进；
- Plan digest复算C0 canonical preimage，独立literal SHA-256 golden不以被测API为oracle；backend report
  必须回交exact `PlanRef(owner/version/generation/digest)`，late callback与arena alias零副作用拒绝；
- token绑定完整SessionId、SceneStore scope generation、Edge/desired owner、serial、capability、PlanRef、
  Scene ref、prepare time、selected config与caller nonce。它是deterministic binding而非authentication；
  caller保证nonce全生命周期fresh，coordinator仅额外检测最近一次成功nonce；
- fresh Session即使沿用同一desired owner，也由首次`begin_owner`推进SceneStore control-scope generation并
  使旧coordinator失效；old world/token/result ref/Blob visibility不能复活，immutable Blob cache与world
  generation high-water只保留为未授权的process-local cache/counter；
- prepare/reject/capacity/caller-reported local deadline保留old active；PREPARED后的scene supersede会令
  activation拒绝旧token、候选回PREPARING并要求新token；SceneStore activate成功后才原子提交active；
- disable先取消candidate并等待producer drain；`producer_drained=false`对observation/status/stats/active/
  world/time零副作用，drained=true才在frame boundary退役plan/world。`offline_lease_ms`只验证/保留，
  因C2无Session-loss signal而不执行hold/expiry。

8组vectors覆盖独立digest、Runtime shape/order、async/late、token/scene/generation、same-owner新Session、
arena/alias/time、parallel Session scope invalidation及success/swap/status/disable；fresh normal与
ASan+UBSan全套均CTest 22/22，strict相关targets通过warnings-as-errors。macOS arm64 coordinator arena
为1,520 bytes/alignment 8；C2完整fixture为4,794,968 bytes，memory domain仍是Host test process的
CPU-visible heap。

状态因此为`COMPLETE / PORTABLE COORDINATOR SEMANTIC VERIFIED`。C2证据范围止于portable
coordinator；Blob carriage由下一节R1单独验证。C2未验证Session transport、K230D build/run、
真实renderer/backend、GPU/target/job/CMA allocation、offline lease执行或physical output。C2是single-thread/
non-reentrant，coordinator/opaque SceneStore arena必须由caller保持不重叠；这些P2限制不改变Core。

## R1 Runtime v3 Blob carriage evidence

public commit `a8a777a` 在 portable `meiso_runtime` 增加严格 MRT0 v3；Profile v1仍选择MRT0 v2，
`meiso_composition_scene_profile_v2()`才选择MRT0 v3。v3保留type 1..6的body合同并增加Host→Edge
`BLOB_BEGIN(7) / CHUNK(8) / COMMIT(9) / ABORT(10)`与Edge→Host`BLOB_RESULT(11)`；完整offset、
RESULT value与phase规则由[Runtime negotiation core v0](../../docs/runtime-core-v0.md)冻结。

R1固定4096-byte chunk、zero Blob、非final chunk exact 4096、short-final；BEGIN唯一等待READY，
CHUNK无逐块ACK，COMMIT/ABORT等待terminal RESULT。Runtime生成Session-scoped nonzero transfer ID；
多个合法Blob route共享一个全局slot。Session或catalog discontinuity清Runtime transfer与Edge scratch，
但不删除已发布Core Blob cache；新Session/control scope仍须显式建立SceneStore visibility。仅catalog
replacement时外部visibility是否重建由后续Profile/SceneStore integration定义，Runtime不持有它，
R1未验证。

COMMIT先在Runtime scratch上检查total与SHA-256，再由generic synchronous bridge执行Core BlobStore
stage/copy/`commit_verified`。Runtime SHA与Mesh Profile verifier在同一个immutable Core verification
snapshot上成立，只有Core `PUBLISHED/DUPLICATE`映射wire success；外部已有staging不被覆盖/abort，
本bridge创建但失败的candidate才被强制清理。Core publication本身不激活scene object/world。

`meiso_runtime_blob_wire_test`覆盖五类独立literal frame、0/4096/4097/262144边界、reserved/truncation、
gap/overlap/replay、async terminal RESULT、abort-before-READY/late READY、TX_BUSY/alias/reentry、
multi-route global slot、catalog/Session invalidation、Core external staging/forced abort、same-snapshot
combined verifier、duplicate publication后显式visibility，以及v3 State/Stream/Blob共存。fresh Debug、
strict与ASan+UBSan均CTest 23/23；clang analyzer对Runtime/composition/test为0 finding。

macOS arm64 exact arenas均alignment 8：Host Runtime 14,904 bytes；Edge Runtime 277,048 bytes，其中
262,144 bytes为完整reassembly scratch；Mesh BlobStore 4,720,304；SceneStore 31,656；Coordinator
1,520。上述五项合计5,045,432 bytes，不含Session/State/Stream。若按C2完整fixture 4,794,968 bytes
再加入两端Runtime，则为5,086,920 bytes，仍不含两端Session。两种口径都只表示Host test process
CPU-visible heap，不是K230 RAM、GPU/CMA、renderer target或physical allocation。

R1状态为`COMPLETE / PORTABLE BLOB CARRIAGE SEMANTIC-WIRE VERIFIED`；该提交自身不证明fair
scheduler。后续R2a已证明portable combined builder/scheduler，R2b0又证明loopback happy-path
AF_INET combined transport，R2b1证明same-Session lifecycle，R2b2证明same-Session catalog
replacement，R2b3证明explicit same-process RST/new-Session recovery；它们仍不证明自动resume、
fresh Edge、其余negative matrix、Host editor、K230D build/run、
renderer/backend、offline lease执行或physical output。

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
object sharing same mesh → delete first → scene disable`。cube publish只验证Blob可见；从one object
到delete的每个scene snapshot步骤，Host收到新applied SceneDesired version和不同或按generator预期
相同的result SHA-256/thumbnail；disable验证STOPPED且不再产生result。共享mesh不重复占published
Blob slot。全过程同时保持Wi-Fi Stream，且没有跨endpoint gate、epoch或payload routing。
