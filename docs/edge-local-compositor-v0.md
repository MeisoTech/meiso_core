# Edge local compositor v0 开发方案

> 状态：`DESIGN / NOT_IMPLEMENTED`。这是 Composition 的实现计划，不是新产品规范或 target
> evidence；通用语义由[Composition 契约](composition.md)拥有。

## 决策

Edge compositor 新增在 **Edge Runtime 与 renderer/platform SDK 之间**。Core、Session wire 和
`State / Stream / Action / Blob` primitive 不改，也不增加 compositor 专用 Core message。

当前不整仓合并外部 `looking_glass`：

- Host/SDC compositor 保留在 Host 项目，作为 logical scene、representation candidate 与
  `PresentationPlan` producer；
- Edge 只选择性移植 provenance/license 清晰的 transactional stereo cut、reprojection、格式
  转换与 K230 backend 代码/测试；
- vendor SDK、firmware、captures、模型、大型 build output 与旧本地快照不进入本仓；
- 两端只通过版本化 Meiso Composition Profile 与 Resource 互操作，不共享本地 object ABI、
  socket framing、DMA handle 或 GPU command struct。

外部 Host 项目当前的 compositor control-plane、peripheral-resource mirror 与 presentation-unit
可以作为 adapter 输入，但不能替代 Edge 的 capability/actual truth、prepared token、activation
或 physical presentation authority。

## 分层与唯一 authority

```text
Host app / Host compositor planner
  -> Host SDK: complete PresentationPlan + assets / projection
  -> Host Runtime
  -> unchanged Core Session + Resource delivery
  -> Edge Runtime: accepted catalog + typed routes
  -> Edge Composition Runtime
       PlanManager / SourceResolver / AdmissionPolicy / FrameScheduler
       -> Renderer SDK seam -> K230 renderer backend
       -> PresentAdapter -> display boundary / scanout
       -> PresentationStatus + telemetry
```

| 层 | 拥有 | 明确不拥有 |
|---|---|---|
| Core | peer/Session identity、bounded delivery、通用 Resource 语义 | layer、plan、renderer、display |
| Edge Runtime | catalog、Composition endpoint/routes、Resource validation、desired/actual 关联 | GPU command、scanout policy |
| Composition Runtime | plan validate/prepare/token/activate、layer order、降级、安全层、hold-last、最终 display authority | Session wire、socket、driver ABI |
| Renderer SDK/backend | plan resources 与完整 frame candidate；准确 cost/failure | prepared token、active plan、Status publication |
| PresentAdapter | local buffer/fence、atomic display commit、vblank/scanout observation | Host plan mutation、协议 identity |

network RX 不能直接修改 active display state。所有远端输入先通过当前 Session、accepted catalog、
Resource owner/version/schema/epoch，再进入 Composition。若 renderer 拆进程，IPC 只是有界的
platform-private bridge，不是第二套公开 Runtime。

## Composition endpoint bundle

Presentation 不是第五种 Resource；下表是待 Composition Profile 冻结的最小 endpoint bundle，
不得覆盖产品规范：

| facet | Resource / truth owner | 内容 |
|---|---|---|
| capability | `CompositionCapabilities` State，Edge | capability generation、source/layer/format/correction、display timing、layer/plan/job/buffer 上限；大型表由 State 引用 Blob |
| desired | `PresentationPlan` State，Host | 完整不可变候选、plan generation/digest、layer/source/dependency、required/preferred/degrade、offline lease、activation echo |
| actual | `PresentationStatus` State，Edge | observed/applied plan、candidate 与 active 正交状态、selected config、reason、prepared token、activation boundary |
| native state | application State，Host | `EDGE_NATIVE` layer 的业务 truth；不是 renderer 私有对象 |
| immutable | Blob，Host/Edge 依 Profile | texture、font、mesh topology、calibration、有限资源包 |
| projection config/status | Host State / Edge State | codec、extent、projection、stream epoch、decoder/recovery 状态 |
| projection data | MediaStream，Host→Edge | frame/presentation unit、source pose/projection、deadline、key/delta dependency |
| telemetry | Stream，Edge→Host | candidate、fence、commit、vblank、drop/fallback 等逐帧事实 |
| operation | Action，可选 | self-test、calibrate、diagnostic capture/reset；plan activation 不用 Action |

Host logical layer 与 Edge-resident layer 使用不同 namespace。Host 每次发布完整
`PresentationPlan` snapshot；稳定 `LayerKey` 只用于缓存和关联，不能用跨端
`CREATE/UPDATE/DELETE` 日志维持隐藏 truth。连续 frame 是 MediaStream；有限不可变内容才是 Blob。

当前 portable Runtime source v0 只承载 desired/actual State 与 Edge-owned Stream，尚无通用
Blob/MediaStream、Host→Edge data route或多 State facet。因此第一片先做本地/synthetic
Composition coordinator；后续扩展 Runtime carriage，仍不改 Core。

## 首个正式实验：Edge-native scene

首个 workload 改为 `EDGE_NATIVE` 的有界 3D scene，而不是先做 Remote Projection。Host 前端
编辑 mesh、object transform 与 camera；Edge 在本地保留一个 scene snapshot、建立 renderer
资源并离屏渲染：

```text
Host mesh bytes ----------------------------> Mesh Blob
Host camera + complete object snapshot -----> SceneDesired State
Edge applied world/capacity/current use ----> SceneStatus actual State
Edge bounded render result + 64×48 thumbnail -> RenderResult Stream
```

完整实验合同见
[`composition-scene-lab-v0`](../profiles/composition-scene-lab-v0/README.md)。这里的“保留 world”
只表示同一 Edge incarnation 内跨 State 更新复用已校验 Blob/GPU buffer，并在新 snapshot 准备时
继续使用旧 active world；它不是磁盘持久化。Session 重建后 Host 必须重新发布 plan 与 scene
State；Edge reboot 后不能从缓存恢复 active truth。

Scene 使用一个 Host-owned `SceneDesired State`，其中同时携带 camera、完整 object 列表、每个
object 的 transform 与 mesh `ContentId`。它不是增量 CRUD 日志：同一 snapshot 内 absence 表示
删除，新 snapshot 只有在所有 ContentId 已发布、对象/内存容量和 renderer prepare 全部通过后
才原子替换 active world。缺 Blob、容量不足或 malformed transform 时旧 world 保持不变。

首片 `RenderResult Stream` 回传固定小尺寸 raw thumbnail 和结果 metadata，用来证明真实 scene
变化、State application 与 renderer 输出相关；它不冒充完整 framebuffer 或 MediaStream。
完整画面回传后续走 MediaStream，不能扩大该 Stream payload。

### 一个 Session、一个 composite Profile

当前 Session handshake 只协商一个 exact `MeisoProfileRef`。Wi-Fi telemetry 与 scene 在同一
Session 并存时，不能分别协商 `wifi-telemetry-v0` 和一个 scene Profile；首个板测必须使用
`composition-scene-lab-v0` 定义的新 composite Profile，其 catalog 原子包含：

- 现有 `WIFI_TLM/ENDPOINT`；
- Composition presentation endpoint；
- Edge-native scene endpoint。

单独 scene Session 或 in-memory fixture 只可作为过渡证据，不能满足“Wi-Fi 与 compositor
并存”验收。combined catalog 的 EndpointId、ResourceId、route 和 schema 必须互异；Wi-Fi 与
scene 分别拥有 desired/actual、production epoch 与 data gate。v0 catalog generation 更新仍会
使整份 catalog binding 失效，Host 必须重新接受 catalog 并分别重发两组 desired；这不允许实现
把两个 endpoint 的 gate 或 actual 合并。

当前 Session 只有一个 TX slot。Runtime/target integration 必须在其上提供有界公平调度：control
帧允许有限优先 burst；每个 ready data/Blob route 每轮至多提交一帧；Blob chunk 不能连续占满
TX。Wi-Fi sample、scene result 和 Blob transfer 同时 pending 时，每条 route 都必须在声明的
slot bound 内获得一次提交机会，过载以 gap/drop/capacity 暴露，不能静默饿死 Wi-Fi。

## Plan 生命周期

candidate 与 active 必须正交；准备新 plan 时旧 plan 继续显示：

```text
candidate: EMPTY -> VALIDATING -> PREPARING -> PREPARED | REJECTED
                                             -> ACTIVATION_PENDING
active:    OLD_ACTIVE -------------------------------> NEW_ACTIVE
                                                    -> old RETIRING -> RELEASED
```

1. Host plan 绑定当前 capability generation、非零 plan generation 与 immutable candidate digest；
2. Edge 原子验证 layer、Resource 引用、budget、dependency、deadline 和允许降级范围；
3. renderer `PLAN_RESOURCES_READY` 只产生本地 candidate，不等于 Composition `PREPARED`；
4. 所有依赖就绪后 Edge 发布 `PREPARED`。`prepared_token` 绑定 Session、Edge incarnation、
   capability generation、plan generation、candidate digest 与 selected config；
5. Host 用新的 plan State version 回显 token；同 plan generation 只能改变激活字段，不能换内容；
6. Edge 在显示边界原子切换并发布 `ACTIVE`；旧 generation 等 in-flight job/fence 完成后释放；
7. prepare/activate 前失败保留旧 active；激活后只使用 plan 明示且本地安全允许的 fallback；
8. 新 Session 使旧 token/remote job 失效。同 Edge incarnation 与 lease 内可暂留画面；Edge
   重启后 active truth 不从缓存复活。

State version、plan generation、capability generation、MediaStream epoch/sequence、render attempt、
buffer/fence handle 必须分别建模。

## Renderer seam

建议最小接口，而不是通用 plugin framework：

```text
probe_platform() -> capabilities / current budget

prepare_plan(context, immutable_plan, admission)
  -> PLAN_RESOURCES_READY(local_plan, selected_config, cost)
   | REJECTED_*

render_frame(local_plan, immutable_job)
  -> RENDER_CANDIDATE_READY(buffer_lease, fence, dependency_set)
   | DROPPED_STALE | MISSED_DEADLINE | CAPACITY
   | PLATFORM_FAILED | CANCELLED_SUPERSEDED

retire_plan(local_plan)
release_candidate(buffer_lease)
```

`RenderJob` 至少绑定 active plan ref、capability generation、selected-config digest、projection
config State version、source FrameKey、pose/depth dependency key、target display time、deadline/
uncertainty、render attempt 与本地 input lease。

renderer 只返回 **完整 candidate 或失败**。左右眼/多 layer transactional cut 未完成时整帧不可
见；hold-last 与 safety fallback 由 Composition 决策。`RENDER_CANDIDATE_READY` 不证明 cache
flush、fence、DRM commit、vblank 或物理像素。

## K230D Linux backend

首发固定大核 Linux；K230D 整板仍是一个 Edge，NuttX 只可作为私有 sensor plane：

- CPU reference/off-screen backend 先冻结像素/transaction golden；
- VPU/V4L2 解码，NonAI2D 做必要的 NV12→RGB/resize，VGLite/RVV 做 warp/raster/blend；
- dma-buf/CMA/cache/fence 由 local buffer adapter 管理，DRM/KMS adapter 负责 atomic present；
- VGLite 当前 one-submitted-command-buffer 与全局 DMA/finish 风险要求单一 owner、固定容量队列
  和 watchdog/failure，不允许多 service 直接争用；
- 当前 1920×1080 主要是 off-screen evidence，不能冒充 480×800 panel physical scanout；
  candidate、fence、DRM accepted、vblank 与实际 display evidence 分开记录。

初始 deployment 可用一个 Linux service 内的 Runtime I/O、Composition control、独占 renderer
worker 与 present thread。只有 hang/isolation evidence 要求时才拆 renderer process。RT-Smart
后端后置：它只能作为跑同一 conformance 的替代 target，或作为 Linux authority 下的私有
worker；不能同时拥有第二套 plan/actual/display truth。

## 选择性迁移表

| 来源 | 决策 | 进入 Meiso 的方式 |
|---|---|---|
| Host logical layer/compositor planner | `ALIGN` | 留在 Host；通过 Host SDK 生成完整 plan/candidates |
| Host peripheral resource service | `ADAPT` | 只镜像 Edge catalog/capability/actual；Host lease 不替代 prepared token |
| K230 transactional stereo cut / LOD | `ADAPT` | 抽成 bounded renderer algorithm；移除 per-call heap 与激活权 |
| reprojection/reference scenes | `ADAPT` | 变成 synthetic/recorded conformance vectors与 Host reference oracle |
| K230 VGLite/VPU/NonAI2D/DRM probes | `ADAPT` | target backend 与独立 evidence；不进入 portable Runtime |
| external wire/object/transport planes | `DO_NOT_MERGE` | 以 Meiso Session/Runtime/Profile 为唯一跨端协议 |
| vendor SDK、firmware、LFS binaries、captures、models | `DO_NOT_MERGE` | 外部 pin/ignored artifact；公开仓只留可复现 source 与脱敏摘要 |
| 未合并 Host branch | `DEFER` | 合入 Host 默认分支并冻结 adapter seam 后再做互操作，不复制到 Edge |

任何源码迁移前先确认 author/license、exact source commit、clean diff 与 third-party boundary；每个
算法/backend 单独小提交，不能用 26 GB 旧目录或整仓 copy 作为 provenance。

## Ordered work units

每个工作单元单独提交并以自己的测试闭环；后项不得用 stub 绕过前项失败。外部 renderer 源码
迁移、physical scanout 与完整 MediaStream 不属于前四项。

### C0 — 冻结 composite lab contract

- 固定 composite Session ProfileRef、三个 endpoint、九条 route、schema、方向、payload/
  object/buffer 上限与 literal golden vectors；引用而不复制 Wi-Fi Profile 语义；
- 固定 `SceneDesired` 全 snapshot、`SceneStatus` 与 `RenderResult` 字段；mesh v0 只允许一个
  bounded canonical triangle format；
- 固定错误分类、fair scheduler slot bound，以及 in-memory、loopback、K230D 三种 evidence 标签。

完成条件：文档/parser/link/diff-check 通过；所有 ID 在 composite catalog 内唯一；现有 Wi-Fi
Profile 和 Core API 无变更。

### C1 — Resource-only in-memory fixture

- 直接使用 `MeisoBlobStore`、Host StatePublisher/Edge StateReplica、Edge StatePublisher/Host
  StateReplica 与 `MeisoStream`；不引入 Session 或 Runtime wire；
- 发布一个 cube mesh Blob，再发布引用它的 camera/object snapshot；fake renderer 生成由 mesh、
  transform、camera 决定的 deterministic result/thumbnail；
- scene store 固定每 world `32 objects / 8 unique meshes`，并为 active+candidate worst case 保留
  `16 Blob slots / 4 MiB resident mesh / 2 render targets / 4 result queue slots`；所有 arena 由
  调用者提供。

完成条件：Blob 未 commit 时 scene 不应用；完整 world 原子替换；结果引用精确 applied State
owner/version；duplicate/stale/conflict/capacity/overflow vectors 全部通过，无 heap、socket 或 driver。

### C2 — Portable Composition coordinator

- 加入 presentation plan、scene world 与 renderer-plan 三个独立 lifecycle；实现
  `old active + one preparing candidate`；
- `SceneStatus` 正交报告 observed/applied desired、world generation、对象/Blob/buffer current use、
  reason 与 result production epoch；
- fake renderer 注入 prepare failure、partial output、deadline 与 supersede；只有完整 candidate
  才能进入 result Stream，plan token/activation 仍由 Composition 拥有。

完成条件：missing Blob/capacity/renderer failure 保留旧 world；disable 在 producer drain 后才
`STOPPED`；new Session/Edge incarnation vectors 不复活 token、world 或 result epoch。

### R1 — Runtime Blob carriage 与 route direction

- 保持 Session Core 不变，新增 Runtime wire version（实现时冻结，预期 `MRT0 v3`）承载
  Host-owned `IMMUTABLE/Blob` route；不得把新 frame 追加到 `MRT0 v2`。v2 的六类 frame、literal
  bytes 和含义原样保留；
- transfer 使用 content identity、总长、连续 offset、固定 chunk、一个有界 staging transfer 和
  commit/abort；完整长度与 hash 验证前不得发布；Session loss 必须 abort staging；
- route direction 从 descriptor owner/facet 验证。本实验只要求 Host→Edge Blob 与 Edge→Host
  result Stream，不能顺便声明未测试的任意双向 Stream；
- 加入单 TX 公平 scheduler：control burst 有上限，Blob 每轮一个 chunk，Wi-Fi/scene data route
  round-robin；所有 pending/retry storage 固定容量。

完成条件：每个 truncation、reserved、offset overlap/gap、duplicate/conflict/hash/length/capacity、
wrong Session/catalog/endpoint/resource/owner/direction 均零部分 publication；现有 Runtime v2
golden bytes 原样通过；Core source/API 零 diff。

### R2 — Combined Session/catalog loopback

- Host/Edge 使用一个 composite ProfileRef 完成一次 Session handshake；Edge 原子 offer 三个
  endpoint/九条 route 的 catalog；
- 同一 event loop 内完成 Mesh Blob→SceneDesired、PresentationPlan prepare/activate→SceneStatus
  RUNNING→RenderResult，同时 Wi-Fi endpoint 继续 desired/actual/sample；
- 分别启停、重配两 endpoint，证明 production epoch、gate、State version 和 Stream epoch 不交叉；
- catalog replacement 后两组 desired 都必须重发；旧 route 和旧 epoch 全部拒绝。

完成条件：scene-only 或 wifi-only Profile 对 composite peer 得到 exact `PROFILE_MISMATCH`；混合
负载 scheduler vector 证明 Blob/result 不饿死 Wi-Fi，Wi-Fi 流量也不阻断 scene control。

### H1 — Host editor/devtool

- Host tool 只通过 typed Profile API 创建/校验 mesh、发布 Blob、提交完整 SceneDesired、观察
  SceneStatus/RenderResult；不暴露 socket packet 或 K230 native handle；
- UI 至少支持 create/delete object、选择 mesh、修改 position/orientation/scale、camera pose/
  projection、enable/disable，以及同时观察 Wi-Fi；
- thumbnail 明确标注 off-screen/result Stream、applied scene State version 与 gap，不能伪装为
  physical display capture。

完成条件：同一输入 fixture 可驱动 in-memory、local TCP 和 recorded backend；刷新 UI 不重放
Blob/desired 或制造第二个 truth owner。

### K1 — K230D Linux off-screen target

- 先用最小 CPU/reference backend，再替换经许可和 provenance 检查的 RVV/VGLite backend；
- 固定 320×240 color/depth target、两个 target slot、mesh/GPU/CMA 上限；严格区分 Blob
  publication、world prepare、render complete 和 thumbnail emitted；
- 在当前 direct-IP carrier 上运行 composite Profile，捕获 object/camera 改动导致不同真实
  thumbnail/checksum，并保持 Wi-Fi Stream 连续。

完成条件：exact target binary/source receipt、board-side capacity counters、至少 create→move→
camera move→delete→disable 的 transcript；不得把 off-screen 结果标为 scanout/physical evidence。

### K2 — Physical present 与完整 media（后置）

- 只有 K1 稳定后才接 DRM/display boundary、安全层、vblank 与物理 panel evidence；
- 完整或持续 framebuffer 回传改用 MediaStream，并继承 active plan、epoch、deadline、recovery
  与 bounded backpressure；不得扩大 `RenderResult Stream` 充当视频通道；
- 最后再做 format/modifier/stride、CMA、thermal、disconnect/recovery 与 soak Profile gate。

完成条件：candidate/fence/commit/vblank/physical capture 分层验收；未完成前不声明 presentation
`BOARD_VERIFIED`。

## 验收门

- 所有现有 Core/Session/Runtime tests 原样通过；Core source/API 无 compositor 分支；
- wrong Session/incarnation/catalog/capability/plan digest/token/State version 零副作用拒绝；
- prepare/reject/supersede 时旧 active 不闪断，partial stereo/partial layer 永不 present；
- stale frame/pose/depth epoch、deadline、buffer exhaustion 与 device loss 有明确 drop/failure；
- 新 Session token 失效，Edge restart no-resurrection，offline lease 不被重传续期；
- safety layer 始终可覆盖 Host layer；候选、fence、commit、vblank evidence 不合并成一个布尔值；
- macOS synthetic Host 与 K230D 使用同一 plan/status/frame fixture；
- 同一 exact composite Profile/Session 下 catalog 同时含 Wi-Fi 与 scene；两 endpoint 的 desired、
  actual、production epoch、data gate、Stream sequence 与 failure counter 独立；
- 单 TX 混合负载满足 scheduler slot bound；Blob chunk 与 thumbnail 不能饿死 Wi-Fi；
- scene 总对象、unique mesh、Blob published/staging、resident mesh bytes、target/job/result queue
  均有固定上限，超限保持旧 active world 并发布明确 reason；
- physical display 前只标 `HOST_SEMANTIC/WIRE` 或 target off-screen evidence，不标 presentation
  `BOARD_VERIFIED`。
