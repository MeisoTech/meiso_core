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

## 实施切片

1. **Contract fixture**：定义 Composition Profile/source contract 与 golden/failure vectors；Core
   文件和 ABI 零改动。
2. **Synthetic control plane**：portable Composition coordinator + fake renderer，证明双代
   prepare/token/activate/supersede/new Session。
3. **Off-screen renderer**：接入许可明确的 stereo cut/reprojection；recorded frame/pose/depth，
   注入 stale、deadline、capacity、device failure、partial stereo。
4. **K230D local presentation**：固定 buffer pool、Linux accelerator seam、安全层、DRM atomic
   display boundary；分别记录 candidate/fence/commit/vblank。
5. **Remote projection**：补 Runtime Blob/MediaStream route；Host adapter 把 plan/unit 映射为
   Meiso State/Blob/MediaStream，不暴露 Host 本地 object/socket ABI。
6. **Profile gate**：冻结 format/modifier/stride、CMA/job budget、deadline/fallback、disconnect/
   recovery、thermal/soak，完成 physical presentation 后才声明 capability。

首个 workload 选 `REMOTE_PROJECTION + Edge local safety layer`；它最直接复用 Host renderer 和
现有 K230 backend。`EDGE_NATIVE` 等 control plane、asset 与 scanout 稳定后再加入。

## 验收门

- 所有现有 Core/Session/Runtime tests 原样通过；Core source/API 无 compositor 分支；
- wrong Session/incarnation/catalog/capability/plan digest/token/State version 零副作用拒绝；
- prepare/reject/supersede 时旧 active 不闪断，partial stereo/partial layer 永不 present；
- stale frame/pose/depth epoch、deadline、buffer exhaustion 与 device loss 有明确 drop/failure；
- 新 Session token 失效，Edge restart no-resurrection，offline lease 不被重传续期；
- safety layer 始终可覆盖 Host layer；候选、fence、commit、vblank evidence 不合并成一个布尔值；
- macOS synthetic Host 与 K230D 使用同一 plan/status/frame fixture；
- physical display 前只标 `HOST_SEMANTIC/WIRE` 或 target off-screen evidence，不标 presentation
  `BOARD_VERIFIED`。
