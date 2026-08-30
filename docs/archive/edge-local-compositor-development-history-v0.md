# Edge local compositor v0 开发历史

> **ARCHIVED / NON-AUTHORITATIVE.** 本页保留旧开发流水；下文状态与“当前”均只对应
> 归档截止点，不描述现行 SDK 身份或 target evidence。
>
> 状态：`DESIGN / C0-C2 HOST FIXTURE COMPLETE / R1 PORTABLE BLOB CARRIAGE VERIFIED / K1a HOST OFFSCREEN REFERENCE VERIFIED / TARGET BUILD VERIFIED / TARGET NOT RUN`。这是
> Composition 的实现计划，不是新产品规范或target evidence；通用语义由
> [Composition 契约](../composition.md)拥有。

## 决策

Edge compositor 新增在 **Edge Runtime 与 renderer/platform SDK 之间**。Core、Session wire 和
`State / Stream / Action / Blob` primitive 不改，也不增加 compositor 专用 Core message。
public commit `4210ddf` 在 Profile/Runtime--SDK 层加入 portable coordinator 与 SceneStore
control-scope seam；commit `a8a777a` 在 portable Runtime 加入严格 MRT0 v3 Blob carriage。两者都
没有修改 Core 或 Session wire。

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

当前 portable Runtime source v0 承载 desired/actual State、Edge-owned Stream，以及MRT0 v3中
Host-owned `IMMUTABLE/Blob` 的Host→Edge carriage；仍没有Action、MediaStream、Host→Edge
continuous data route或多State facet。Blob由Runtime scratch经generic bridge同步发布到Core
BlobStore，Profile/SceneStore visibility另行授权；这些扩展均不改Core。

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
[`composition-scene-lab-v0`](../../profiles/composition/README.md)。这里的“保留 world”
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

当前 Session handshake 只协商一个 exact `MeisoProfileRef`。Composition Profile v1选择MRT0 v2，
Profile v2才显式选择MRT0 v3 Blob carriage；两版不fallback。Wi-Fi telemetry 与 scene 在同一
Session 并存时，不能分别协商`wifi-telemetry-v0`和一个scene Profile；首个板测必须使用
`composition-scene-lab-v0:v2` composite Profile，其catalog原子包含：

- 现有 `WIFI_TLM/ENDPOINT`；
- Composition presentation endpoint；
- Edge-native scene endpoint。

单独 scene Session 或 in-memory fixture 只可作为过渡证据，不能满足“Wi-Fi 与 compositor
并存”验收。combined catalog 的 EndpointId、ResourceId、route 和 schema 必须互异；Wi-Fi 与
scene 分别拥有 desired/actual、production epoch 与 data gate。v0 catalog generation 更新仍会
使整份 catalog binding 失效，Host 必须重新接受 catalog 并分别重发两组 desired；这不允许实现
把两个 endpoint 的 gate 或 actual 合并。

每个peer Session handle各有一个outbound TX slot；R1 direct Runtime不包含scheduler，R2a 已用独立
Runtime--SDK scheduler 分别调度两个方向：Host outbound是Host control+Blob chunk，Edge outbound是
Edge control+Wi-Fi sample+scene result。每方向control burst最多4个成功slot，冻结data round后每个
ready route至多一次发送机会；Host Blob chunk不能连续占满Host TX，full-duplex不能合并成全局slot
序列。确定性上界为Host单data route `<=5`、Edge两条data route各 `<=6` 个本方向成功slot；过载仍须
以gap/drop/capacity暴露，不能静默饿死Wi-Fi或result。

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

- K1a CPU reference/off-screen backend已在Host冻结source-pinned像素/transaction golden，K230D
  official cross-build已验证但cross-run仍未执行；
- VPU/V4L2 解码，NonAI2D 做必要的 NV12→RGB/resize，VGLite/RVV 做 warp/raster/blend；
- dma-buf/CMA/cache/fence 由 local buffer adapter 管理，DRM/KMS adapter 负责 atomic present；
- VGLite 当前 one-submitted-command-buffer 与全局 DMA/finish 风险要求单一 owner、固定容量队列
  和 watchdog/failure，不允许多 service 直接争用；
- 当前reference backend只实现并验证Profile固定的320×240 off-screen target；1920×1080未实现/未
  验证，任何480×800 panel或其他物理scanout也没有evidence。candidate、fence、DRM accepted、vblank
  与实际display必须分开记录。

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

状态：`COMPLETE`（Profile codec / Host wire vectors；不是 Runtime/target interoperability）。

- 固定 composite Session ProfileRef、三个 endpoint、九条 route、schema、方向、payload/
  object/buffer 上限与 literal golden vectors；引用而不复制 Wi-Fi Profile 语义；
- 固定 `SceneDesired` 全 snapshot、`SceneStatus` 与 `RenderResult` 字段；mesh v0 只允许一个
  bounded canonical triangle format；
- 固定错误分类、fair scheduler slot bound，以及 in-memory、loopback、K230D 三种 evidence 标签。

证据：commit `3b9ef023edffba1c2e652c778723768f1bc7c8cf` 冻结 2 个新 endpoint、6 条新 route、
32/52-byte capability、六类 Profile suffix 与 literal/negative/max-boundary test；combined 3 endpoint/
9 route catalog 为 920 bytes，Wi-Fi namespace disjoint，Core API/source 无变更。Runtime v2 仍拒绝
IMMUTABLE route，故 C0 不证明 remote combined catalog。

### C1 — Resource-only in-memory fixture

状态：`COMPLETE / RESOURCE SEMANTIC VERIFIED / LOGICAL ADMISSION ONLY`。

- 直接使用 `MeisoBlobStore`、Host StatePublisher/Edge StateReplica、Edge StatePublisher/Host
  StateReplica 与 `MeisoStream`；不引入 Session 或 Runtime wire；
- 发布一个 cube mesh Blob，再发布引用它的 camera/object snapshot；fake renderer 生成由 mesh、
  transform、camera 决定的 deterministic result/thumbnail；
- scene store固定每 world `32 objects / 8 unique meshes`，借用 caller-owned 16-slot BlobStore，并以
  logical vertex+index bytes执行 active+candidate 4 MiB admission；result queue固定4 slots；
- CPU fixture零拷贝借用 Blob bytes，streaming fake renderer 不持有 target/job，因此 truthfully报告
  target/job `0/0`。后续K1a reference renderer已另行预分配两个CPU target set；GPU resident、CMA与
  target backend arena仍须在真实backend中另验。

完成条件：Blob 未 commit 时 scene 不应用；完整 world 原子替换；结果引用精确 applied State
owner/version；duplicate/stale/conflict/capacity/overflow vectors 全部通过；scene store自身不调用heap，
fixture只在caller一侧分配测试arena，且不使用socket或driver。

实现证据：`scene_store.h/.c` 提供 bounded owner/Blob visibility、canonical SHA-256 Mesh verifier、
prepare/observation、CAS activate、disable、status与deterministic fake render；
`meiso_composition_scene_resource_test` 直接串起Core Blob/State/Stream。fresh strict与ASan+UBSan全套
均22/22。当前macOS arm64 ABI中C1全部caller arena合计4,792,712 bytes、alignment均为8；其中store
arena为31,656 bytes。memory domain是Host test process的CPU-visible heap；logical resident边界不另
分配GPU/CMA arena；external Stream的Session边界清理仍留给后续Runtime integration。

### C2 — Portable Composition coordinator

状态：`COMPLETE / PORTABLE COORDINATOR SEMANTIC VERIFIED`（public commit `4210ddf`；Host
in-memory semantics，不是 Runtime/Session wire、renderer 或 target evidence）。

- `composition_coordinator.h/.c` 借用 caller-owned SceneStore；自身 arena 1,520 bytes、alignment 8，
  无内部 heap/socket/driver。C2 完整 fixture 含 BlobStore、SceneStore、coordinator、Scene desired、
  Presentation desired/actual State pair 与 RenderResult Stream，共4,794,968 bytes，均为macOS arm64
  Host test process的CPU-visible heap；
- desired 直接取一次 Core StateReplica commit 解码后的 Runtime value：`enabled=false` 只接受canonical
  8-byte Runtime prefix且Profile suffix为0；`enabled=true`只接受exact 128-byte `PLN0` suffix。
  `plan_generation` 是每owner/control scope不复用high-water；同generation内容不可变，State version与
  token echo只按明确阶段推进；
- coordinator复算C0 canonical Plan SHA-256；test含独立literal digest，不用被测API生成oracle。
  backend completion携带exact `PlanRef(owner/version/generation/digest)`，late report不能命中新candidate；
- prepared token确定性绑定完整SessionId、SceneStore control-scope generation、Edge/desired owner、
  serial、capability generation、PlanRef、Scene world ref、prepare time、selected config与caller nonce。
  token不是authentication；nonce全生命周期fresh是caller前置条件，实现只额外拦截最近一次成功nonce；
- fresh coordinator的首次`begin_owner`总是推进SceneStore control-scope generation；即使owner相同，旧
  coordinator所有query/report/activate API也立即失效。world/candidate/result引用与Blob visibility清空，
  immutable BlobStore cache和world-generation high-water保留但不能越scope授权复活；
- prepare/reject/capacity/local backend deadline、wrong token与supersede保持old active。PREPARED后
  Scene candidate改变会在activation boundary拒绝旧token、回到PREPARING并要求新token；SceneStore
  activate是transaction最后一个可能失败的调用，成功后才提交active plan；
- `enabled=false`先形成disable intent并取消candidate；`producer_drained=false`返回
  `WAITING_DRAIN`且observation/status/stats/active/world/time均不变，只有drained=true的frame boundary
  才同时退役plan与world。`offline_lease_ms`只校验并保留；C2没有Session-loss signal，不计算或执行
  lease，backend `DEADLINE`只是caller报告的本地prepare/render结果。

8组deterministic vectors覆盖：独立C0 digest；Runtime shape/order；async failure与late completion；
token/scene supersede/generation guards；same-owner new Session no-resurrection；API arena/alias/time；
distinct-arena parallel Session scope invalidation；success/swap/status/disable。fresh normal与ASan+UBSan
全套CTest均22/22，相关targets额外通过strict conversion/shadow/prototype warnings-as-errors。

C2证据范围止于portable coordinator；Host→Edge Blob carriage由下节R1单独验证。C2未验证
Session transport、K230D build/run、真实renderer/target/job/CMA、offline lease执行或physical output。
P2边界是single-thread/non-reentrant caller discipline、opaque SceneStore arena不重叠前置条件与caller保证
nonce全局fresh；均不提升本阶段evidence等级。

### R1 — Runtime Blob carriage 与 route direction

状态：`COMPLETE / PORTABLE BLOB CARRIAGE SEMANTIC-WIRE VERIFIED`（public commit `a8a777a`；
不是scheduler、transport或target evidence）。

- Profile v1→MRT0 v2；Profile v2→strict MRT0 v3。v2六类frame/literal/meaning原样保留，v3增加
  BEGIN/CHUNK/COMMIT/ABORT/RESULT五类frame；不混用、不fallback；
- 只允许Host-owned `IMMUTABLE/Blob` Host→Edge；多个route共享一个全局transfer。Runtime生成
  Session-scoped ID，BEGIN唯一READY，CHUNK固定4096且无逐块ACK，COMMIT/ABORT返回terminal RESULT；
- Edge Runtime完整scratch先核对length/SHA-256，再由generic bridge在同一Core verification snapshot
  上执行SHA+Profile verifier与BlobStore atomic publish；publication后仍须显式SceneStore visibility；
- Session/catalog discontinuity清Runtime transfer/scratch，published Core cache可保留；新Session/
  control scope不复活desired/world/visibility。catalog-only replacement的外部visibility策略留给后续
  Profile/SceneStore integration，Runtime不持有它，R1未验证；Core/Session wire无改动。

独立literal、truncation/reserved、0/4096/4097/262144、offset、phase/result、TX_BUSY、alias/reentry、
multi-route、Core staging ownership/cleanup、duplicate/visibility与State/Stream coexist vectors通过；fresh
Debug、strict、ASan+UBSan均CTest 23/23，clang analyzer 0 finding。macOS arm64 Host/Edge Runtime
arena为14,904/277,048 bytes、alignment 8，Edge值含262,144-byte scratch。

R1提交自身不实现fair scheduler；control burst、round-robin与direction-local no-starvation由下节
R2a的独立Runtime--SDK对象证明。

### R2a — Portable combined builder 与 fair TX scheduler（完成）

public commit `5d92ff6` 提供 exact Profile v2 combined builder 与 caller-owned scheduler；后者绑定一个
本地 peer 的 exact Runtime/Session pair，是该 pair 唯一 TX owner，不修改 Core、Session wire 或
direct Runtime API。

- builder 无部分输出地选择 `8236/8192/12`，按 Wi-Fi/Presentation/Scene 顺序建立3 endpoint、9 route
  与 canonical 920-byte catalog；
- control source持久RR且burst最多4个成功slot；data round冻结本轮ready membership，每个source至多
  一次发送机会，Host Blob每轮至多一个chunk；`data_probe_due`使nonbusy control reject后优先探测data；
- Host与Edge独立计本方向successful slot；Host单Blob route等待 `<=5`，Edge Wi-Fi/RenderResult同时
  ready时各等待 `<=6`；local `TX_BUSY`不推进Scheduler、Runtime、Session、source或本方向round；
- successful slot只表示direct Runtime send返回`OK`且frame已排入Session outbound，不表示socket write、
  peer receive、application commit或时延上界；
- State `OFFER`要求commit；Blob仅在接收`COMMIT`后Runtime snapshot为
  `publish_pending && !result_pending`时要求Core publication。length/SHA precheck失败直接产生terminal
  `RESULT`，没有publish obligation；
- active catalog generation变化清binding/round并进入`REBIND_REQUIRED`；Session terminal永久进入
  `SESSION_TERMINAL`并exact-once通知sources，旧对象不能rebind，必须新建Session/Runtime/Scheduler。

public commit `5d92ff6` 的fresh Debug CTest为24/24，strict C11/C++17通过，clang analyzer对
scheduler/builder/test为0 finding。该证据是portable/in-process语义门，不证明AF_INET combined
transport、K230/K1、Hz/latency/performance或renderer/physical output。

### R2b0 — Combined Session/catalog AF_INET happy path（完成）

public commit `7e53bc9` 在真实`127.0.0.1` AF_INET `SOCK_STREAM`上连接Host与Edge；两端均使用
nonblocking POSIX adapter，并把每次I/O限制为17 bytes，使双方Runtime frame实际经历多次`WRITE`/
`READ_PROGRESS`。一个Profile v2/MRT0 v3 Session使用exact 3 endpoint、9 route、920-byte catalog与
双方R2a scheduler，贯通：

- cube Mesh Blob → Runtime scratch → Core BlobStore publish → 显式SceneStore `mark_visible`；
- Scene/Plan State → C2 `ACTIVE`；
- Wi-Fi actual production epoch `77`、Scene `42`、Presentation `41`；
- exact `WIFI_TLM/DATA_STR` route上的epoch `177`/sequence `1`，以及exact `K230_SCN/RES_STR`
  route上的epoch `417`/sequence `1`。

fresh Debug与ASan+UBSan CTest均为25/25，clang analyzer为0 finding；GitHub Actions run
`32316527393`四个job全部success。结论严格为
`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED / TRANSPORT_VERIFIED`（loopback happy path），不是
K230、renderer、physical output、性能或安全证据。

### R2b1 — Same-Session lifecycle transcript（完成）

public commit `9daa7f9` 沿用R2b0的同一个真实TCP Session、catalog、Plan 1及cube publication，完成：

- world 1/RenderResult sequence 1；desired v2 pending时old-world exact sequence 2；TRS world 2/
  sequence 3；camera world 3/sequence 4；第二个共享cube对象world 4/sequence 5；删除首对象world 5/
  sequence 6；
- 所有desired/actual State闭环且byte-exact；PlanRef/digest不变；actual epoch `43..46`分别验证actual
  前new epoch拒绝与actual后old epoch拒绝，拒绝不消费sequence；
- Blob count/bytes/publication/duplicate/transfer high-water及idle状态前后不变，没有Blob重传；
- prefix-only Presentation desired v3先返回`WAITING_DRAIN`且Coordinator/SceneStore raw zero-diff；
  drained后退役，Presentation与Scene发布exact `STOPPED` actual，SceneStatus为stable `NONE`；
- RenderResult随后得到`ROUTE_NOT_ACTIVE`，Wi-Fi仍以epoch `77`发送sequence `2`。

fresh Debug与ASan+UBSan full CTest均25/25，clang analyzer 0 finding；GitHub Actions run
`32317542841`四个job全部success。该证据只覆盖same-Session happy lifecycle。

### R2b2 — Same-Session catalog replacement（完成）

public commit `369ff4d` 在R2b1同一真实TCP Session内完成：

- g2 offer只staged，active g1与scheduler boundary不变；accept后两端atomic active g2；
- 双方scheduler每个source exact-once `CATALOG_CHANGED/REBIND_REQUIRED`，所有route source重绑g2；
  stale g1 queued work为`SOURCE_INVALID`且无Session outbound/socket output；
- SessionId、handle、fd、peer/incarnation、limits与`READY`不变；Runtime gate清除，production high-water
  `77/41/46`保留；
- fresh capability-generation-2 coordinator推进scope并清world/visibility，旧coordinator/token/world
  fail closed；Core Blob cache保留，但Scene v6先`MISSING_BLOB`，g2 transfer获Core `DUPLICATE`后显式
  `mark_visible`；
- Plan generation 2使用新token，new actual epoch为Wi-Fi/Presentation/Scene `88/51/52`；旧production/
  Stream epoch拒绝，新Wi-Fi epoch `178`与RenderResult epoch `418`均从sequence 1恢复。

fresh Debug与ASan+UBSan full CTest均25/25，clang analyzer 0 finding；Actions run `32318799466`
四个job全部success。该证据只覆盖same-Session catalog replacement。

### R2b3 — Explicit RST/new-Session recovery（完成的 slice）

public commits `9df5268`、`e4a726f`与`43ae3ce` 已push到`main`与`rain/core-session-v0`：

- READY exact refresh保持Runtime byte-for-byte；terminal首次在scheduler poll/TX前原子清catalog、pending
  State/Blob与Edge scratch并返回`CORE_NOT_READY`，保留Blob transfer-id high-water，后续`STALE_SESSION`；
  identity/role/limits mismatch与verifier reentry均有独立fail-closed vector；terminal callback exact-once；
- g2 ACTIVE后发送distinct Mesh `BEGIN/READY/CHUNK`但不`COMMIT`；Edge执行adapter abort、`SO_LINGER`
  RST与close，Host read检测loss，两端Session均为`FAILED/TRANSPORT_LOST`；第二个Blob未进入Core，旧
  Runtime/Scheduler/route/epoch不可复用，SceneStore旧active world仍保持；
- 新AF_INET连接使用新SessionId和全新Session/Runtime/Scheduler/coordinator arenas；同process Core cache
  与SceneStore保留；新coordinator开始新control scope后才清world/visibility并使旧coordinator/token/
  world失效，desired、visibility、world及gate均不自动继承。显式提高State delivery generation、
  重发cube并取得Core `DUPLICATE`、再次`mark_visible`后，以Plan generation 3/new token、Wi-Fi/
  Presentation/Scene actual epoch `99/61/62`、Wi-Fi/RenderResult epoch `179/419` sequence 1恢复；
- Linux CI首次发现scratch-tail padding测试假设；`43ae3ce`改用COMMIT Runtime-scratch borrow校准offset，
  并在同配置新fixture重验范围，Runtime产品代码不变。Actions run `32321051775`四个job全部success。

该slice是显式same-process reconnect，不是自动resume，也不证明fresh Edge process、scene-only/wifi-only
wrong Profile、max-Blob、backpressure、fair-contention stress、K230/K1、renderer/present、性能、安全
transport或physical output。

### R2b4 — Max Blob与resource boundary（完成）

public commit `a392850` 沿用R2b3的新AF_INET Session：Host本地拒绝262145-byte Mesh `BEGIN`且无
wire/Runtime transfer/Core/Scene副作用；随后把exact 262144-byte canonical `MSH0`作为64个4096-byte
`CHUNK`发送，Edge只在BEGIN后返回`READY`、COMMIT后返回`PUBLISHED`，没有逐块ACK。第32块后Edge
自己的outbound scheduler继续发送Wi-Fi epoch `179`/sequence `2`与old-world RenderResult epoch `419`/
sequence `2`；Core publish与显式visibility后，scene swap在Plan generation 3内产生新world，actual v10/
production epoch `63`开放RenderResult sequence `3`。这验证Blob与反向Wi-Fi/result分别走两个peer的
独立scheduler，没有改变Core或compositor ownership。

macOS arm64的当前五arena ledger为Host/Edge Runtime `17,384/279,528` bytes（Edge含262,144-byte
scratch）、2-slot Mesh BlobStore `1,049,056`、SceneStore `31,656`、Coordinator `1,520`；均alignment 8，
合计`1,379,144`。逻辑published bytes为cube 304 + max Mesh 262,144 = 262,448，active world的
`resident_mesh_bytes=262080`，target/job为0。它不含Session/scheduler/adapter/State/Stream、Host asset
buffer、TCP kernel、GPU/CMA；2-slot只是fixture，不验证SCAP 16-slot aggregate。Actions run
`32322475279`四个job全部success。

R2b4仍只是loopback Host semantic/wire/transport evidence；不证明自动resume、fresh Edge、wrong Profile、
backpressure、fair-contention stress、K230/K1、renderer/present、Hz/latency/throughput、安全transport或
physical output。

### R2b5 — Adversarial TCP Profile/backpressure/fairness（完成的 slice）

public commit `6488fef` 在真实AF_INET上验证：Composition v1与Wi-Fi Profile连接Composition v2均在
handshake中`PROFILE_MISMATCH`并fail closed，没有SessionId、catalog或Runtime frame；匹配v2正例仍
READY。Edge→Host RenderResult与Host→Edge Blob `CHUNK`分别把nonblocking adapter推进到
`WRITE_WOULD_BLOCK`，随后scheduler `TX_BUSY`对Scheduler/Runtime/Session/adapter arena、source、
output和poll/completion原字节零推进；排空后已排队frame exact-once到达，Blob再显式ABORT且Core保持空。

方向公平性按peer独立验证：Host的4个control后Blob data精确在第5个successful slot；Edge的4个
control后Wi-Fi/RenderResult精确在第5/6个slot且属于同一冻结data round，每段对端scheduler slot计数
不动。两方向是依次制造的饱和阶段，不证明simultaneous full-duplex saturation、wall-clock/latency/
throughput/performance、无穷ready source、K230/K1、renderer/present或安全transport。Actions run
`32324328746`四个job全部success。

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

- K1a已完成最小CPU/reference backend的Host evidence；后续才替换经许可和provenance检查的
  RVV/VGLite backend；
- reference renderer固定320×240 color/depth target、两个target slot与两个job；其macOS caller arena
  是CPU heap，不是GPU/CMA。严格区分Blob publication、world prepare、render complete和external
  thumbnail Stream emitted；
- 在当前 direct-IP carrier 上运行 composite Profile，捕获 object/camera 改动导致不同真实
  thumbnail/SHA-256，并保持 Wi-Fi Stream 连续。

完成条件：exact target binary/source receipt、board-side capacity counters、至少 create→move→
camera move→delete→disable 的 transcript；不得把 off-screen 结果标为 scanout/physical evidence。

K1a Host证据：public commit `c3b4770` 的portable synthetic renderer使用caller-owned arena，无内部
heap/thread/socket/driver/native handle；两target共1,228,800 bytes，当前macOS最小renderer arena
1,268,872 bytes/alignment 8。两个job、control-scope reset、exact-world retire、inactive-target atomic
commit与literal color/depth/thumbnail golden已验证，evidence flags exact为`0x3`；Actions run
`32326481615`四个job全部success。它消费canonical triangle-list Mesh的完整bytes，但pixel/depth算法只是
source-commit-pinned synthetic oracle，不证明geometry-correct rasterization或Profile wire output。

public commit `b9ec6a3` 的offline runner只执行`baseline_cube_once`，不含完整lifecycle或external Stream。
其当前macOS五arena依次为BlobStore 4,720,304、SceneStore 31,656、Coordinator 1,520、renderer
1,268,872、scratch 14,720 bytes，均alignment 8且合计6,037,072 bytes；memory domains分别为Core Blob
CPU heap、两项Profile CPU heap、reference-target CPU heap与runner CPU heap。该数值不含stack/libc/OS、
Session/Runtime/scheduler/State/Stream/TCP或GPU/CMA。native标签为
`HOST_OFFSCREEN_REFERENCE_VERIFIED`。官方Xuantie toolchain已两次fresh、byte-exact构建83,152-byte
candidate ELF（SHA-256 `05accb66e621c0bef981e78312fae851a8f0cb8b233e1a9c89e4d648340972f8`），
故build为`TARGET_BUILD_VERIFIED`；没有board identity或target NDJSON receipt，runtime仍为
`TARGET_NOT_RUN`。`offline_lease_ms`只随Plan验证/保留，没有Session-loss执行。

### K1-camera — K230D physical capture source（完成的独立 slice）

K230D Linux V4L2 adapter已在准确板上产生`THREE_FRAME_CAPTURE_RECEIPT`：既有GC2093 server保持
存活，4个MMAP buffer取得3帧progressive NV12 `320x240/stride320/115200 bytes`。raw field ANY只在
`G_FMT`确认progressive后继承；driver sequence 0标为absent，adapter使用synthetic effective sequence
`1/2/3`且drop为0。退出时phase为`STOPPED`，mapped/queued ledger均为0。artifact/receipt SHA-256分别为
`eda3eb1d8eeb94832e8867197a7c5e1a54241e29c0d87a6b1901723d8b90d4df`与
`86de6baf0b2797713217113035e5bd28cc963504b38c8f17e4867cb0a18eef33`。

该slice只证明camera source的bounded physical capture。后续独立combined run已经在当前source上
覆盖MediaTransport、Runtime desired/actual、同一Session的render+camera、Host local Web bridge和
zero-ledger shutdown，见
[K230D Composition 正式化前联调证据](k230d-composition-pre-formal-e2e.md)。两层证据必须分别保留；
combined run仍不证明encoder、吞吐、长稳或physical presentation，也不能提升上述历史K1a offline
candidate artifact的`TARGET_NOT_RUN`。

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
