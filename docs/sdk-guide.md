# SDK 设计与开发指南

> 状态：实现与验证指南。稳定语义以[系统设计](system.md)、[Resource 契约](resource.md)、
> [Runtime 契约](runtime.md)、[Link 契约](link.md)和[Composition 契约](composition.md)为准。

## SDK 边界

```text
Host application / service / frontend
    ↓
Host SDK: EdgeHandle / Virtual Edge API
    ↓
Host Runtime: Resource Graph / policy / control
    ↓
Core: identity / Session / Resource delivery
    ↕ transport adapter
Edge Core
    ↑
Edge Runtime: Resource Graph / policy / control
    ↑
capability / platform / endpoint adapters

```

这个分层不要求 daemon、多进程或通用 plugin framework。第一份实现可以在单进程中证明
契约；只有真实 lifecycle、isolation 或 deployment 需求出现时才拆进程。

SDK 不把 socket、packet、driver handle 或 platform buffer 暴露为 Host application 的
业务接口。反过来，Core 也不要求所有 payload 复制进通用 byte container；adapter 可以
提供满足相同 Resource lifecycle 的 zero/low-copy path。

| 层 | 拥有 | 不拥有 |
|---|---|---|
| Core | identity、handshake、Session、bounded wire、Resource delivery primitive | Resource 声明、sensor/control、Virtual Edge |
| Runtime | catalog/endpoint instance、desired/actual 关联、production route、policy/control/adapter | peer handshake、application facade |
| Host SDK | Virtual Edge、typed async API、application view | socket/IP、platform adapter、physical truth |

Bootstrap 可以解析本地 direct-IP/TCP 配置并向 Core transport adapter 注入 typed
options；该配置不是 SDK 业务 API 或 Core 协议，IP 也不是 peer identity。

Host SDK 不从板卡型号推断 sensor 清单。Host Runtime 只把当前 Session 已接受的
catalog、Edge actual 与有效 route 投影给 Virtual Edge。App 显式提交 Host desired State；SDK
的本地 observe/subscribe 只控制 Host 内 callback、retention 与 view，不产生跨端 request，也不
自动修改 desired。它不能让 Edge 产生数据后选择不发送。

## 数据如何选择

| 业务含义 | Resource |
|---|---|
| 可替换的当前事实、configuration、desired/actual、capability/status | State |
| 追加的 sensor sample、input event、telemetry 或 deadline-bound result | Stream |
| 连续 raw/encoded camera、audio 或 projection | MediaStream（Stream 专业化） |
| 请求另一端执行有副作用的操作并观察明确结果 | Action |
| 有限、不可变、完整校验后才能使用的 asset/snapshot | Blob |

不要为了覆盖四种类型虚构业务。大型当前事实仍是引用 Blob 的 State；已经发生的 button
event 是 Stream；连续 frame 不是 Blob；desired State 的 ACK 不是 Action result。

## Host SDK / Virtual Edge

Host-facing API 对 simulated、loopback、recorded 和 remote backend 使用同一业务语义。
最小 facade 应能表达：

- Session/online/freshness snapshot；
- capability 与 Resource discovery；
- State read/watch/submit 及 delivery/actual 分离；
- Stream subscribe/publish endpoint；
- Action invoke/result；
- Blob lookup/transfer；
- Composition plan/status（当 capability 存在）。

第一版不需要一次实现所有 operation。未实现能力必须通过 capability 缺失或 explicit
unsupported 表达，不能留一个会静默成功的空方法。

API 不承诺 transport 数量。backend 看到一个 transport-independent Session seam；具体
Profile/adapter 自己选择 transport arrangement。本规范不定义 Host switching、跨 Session
authority handoff 或 topology migration，也不对这些能力作支持或兼容承诺。

### Composition Profile v3 首个 `EdgeHandle` 切片

当前可执行需求是本地编辑器和 Host 应用在不接触 debug/transport 对象的前提下使用
Composition Profile v3。首个公开 facade 因此只选择该 Profile 已经实装的操作，不建设通用
backend plugin 或任意 schema dispatcher：

- application header 只暴露 opaque `EdgeHandle`、Session/freshness snapshot、accepted catalog 的
  endpoint/resource value view、typed desired command、typed actual/Stream/Composition snapshot、
  mesh put 与 external-media latest/release；
- Host integration 以独立 adapter 把一个 live Composition HostApp 投影为 handle，并继续独占
  socket、readiness、pump、media connection 与 backend lifetime；
- 当前 capability 包含 State read/submit、Stream read、Host→Edge Blob put、Composition 与
  external-media read；State watch、Stream publish、Action invoke、Blob lookup 均明确 absent；
- handle 在 handshake/catalog 阶段不发布 resource graph；Profile v3 READY 后才一次性发布 exact
  6 endpoint/18 resource view。terminal/catalog discontinuity 清除 live view，旧 handle 永久失效；
- command receipt、desired delivery、Actual observed/applied、inline sample、Core Blob result、Scene
  visibility、render result 与 media frame 分字段表达，任何前一阶段都不冒充后一阶段。

macOS Composition Debug Host 现在也沿用这条边界：编辑器命令、typed truth、mesh 与 external-media
borrow/release 只调用公开 `MeisoEdgeHandle` API；进程内 HostApp 仅负责 facade 初始化、readiness/pump、
control/media fd、one-use media slot 与 abort。NDJSON/CMFB 仍是 machine-local bridge，不成为第二套
SDK 或 wire owner。

同一个 Debug Host 还必须提供显式 `--simulator` 启动模式，作为本地编辑器的真实离线入口，而不是只在
单元测试中直接调用 simulator。该模式不读取远端 config、不创建 control/media socket，也不伪造
Session、TCP binding 或 physical frame；它以固定 Composition Profile v3 catalog、caller-owned arena 和
进程本地 synthetic sample 初始化同一个 opaque `MeisoEdgeHandle`，再由相同 NDJSON command、typed truth
和 snapshot 投影驱动 Mesh→Scene→Presentation→RenderResult。默认远端 CLI 与输出保持兼容；Dashboard
必须显式选择 `NATIVE_SIMULATOR`，不得把该模式显示成 `NATIVE_HOST`、设备连接或 media transport bound。
每个子进程是 fresh simulator lifetime，close/EOF 后旧 handle 不可恢复。离线模式不生成 CMFB frame；
RenderResult 仅为 SceneStore synthetic offscreen/hash/thumbnail evidence。

该切片的 conformance vector 必须使用真实 `AF_INET` HostApp/Edge fixture，并同时证明：pre-READY
本地排队不制造在线 resource；READY 后 exact discovery 与 typed Wi-Fi State/Stream 闭环；未支持
operation 的 capability 为 false；media borrow 只能以 facade key exact release；control terminal
后 snapshot 不再暴露旧 capability/data，所有 mutation fail closed。另有一个 application-only
translation unit 只 include public SDK/Profile header，保证业务代码无法依赖 fd、Session/Runtime
pump 或 devtools type。该 evidence 是 Host facade/loopback evidence，不证明 physical Edge、自动
reconnect、Action、任意 Profile 或跨 backend ABI。

### Deterministic simulator / replay backend

第二个 backend 必须继续返回同一种 opaque `MeisoEdgeHandle`，应用不得通过 cast、private include、
backend tag 或测试旁路区别 remote HostApp 与 simulator。它只需要实现实际用于本地编辑器测试的
Composition 子集，但 `operations` 与每个 unsupported 返回值必须诚实一致。

Simulator 使用 caller-owned arena、显式 activate/step、调用方提供的 source time 与完整 media
access unit；不读 wall clock、不建线程/socket、不分配 heap。每次 step 最多完成一个 queued desired，
并把 local receipt、delivery accepted、Actual observed/applied、Stream sample 与 media availability
分阶段公开。terminal 会清 live graph、typed truth 与 borrow，旧 handle 不可复活；新的 run 必须新 arena、
新 nonzero continuity serial。Recorded sample/frame 只证明 deterministic replay，不证明 physical Edge、
fresh sensor 或 transport。

Composition simulation也是config显式opt-in。启用后，caller arena同时容纳固定slot数的BlobStore、
SceneStore、C2 coordinator、一个最大Mesh pending buffer和两路media buffer；`put_mesh`先candidate-copy，
step再publish并显式mark-visible。Scene与Presentation沿真实codec/PlanRef/token-echo/C2 frame-boundary推进，
active world通过SceneStore fake renderer发布带`OFFSCREEN_COMPLETE|THUMBNAIL_INCLUDED`的deterministic
RenderResult。Scene replacement、missing-Blob等待、capacity、disable、epoch exhaustion与render media
world/plan identity都fail closed。这个闭环服务离线编辑器状态机验证，不是CPU raster像素golden、网络、
K230、camera、physical display或性能证据。

DeviceTelemetry simulation是config显式opt-in：调用方提供nonzero production epoch与一份canonical sample；
SET_DEVICE按Wi-Fi之后、media之前的固定step优先级产生desired/Actual/inline Stream，并对未选字段归零。
未启用时SET_DEVICE明确`unsupported`。这让离线设备面板可重复测试，但sample仍是synthetic，不是fresh
sensor truth。

固定v1 artifact由[Composition Edge deterministic trace](composition-edge-trace-v1.md)定义。它使用
caller-owned writer/reader arena和最多16 MiB的canonical in-memory bytes；CONFIG、activate、三种
supported desired、step/time、完整frame与terminal都有closed record kind和CRC32C。reader init先验证
整份trace，随后每次只推进一个record；失败不推进cursor。codec不打开path或持fd，应用也不能在replay
lifetime中旁路修改同一simulator handle。v1固定在Wi-Fi与两路media，要求DeviceTelemetry和Composition
opt-in都关闭且新增config suffix为canonical zero；后续simulator能力不会静默改变既有trace格式。

## 一个 Edge、多个本地 runtime

一个 Edge implementation 可以横跨多个 core、OS 或进程。对外 Core 管理公开
peer/boot/Session identity；Edge Runtime 管理 capability/Runtime catalog 与 endpoint
lifecycle。内部 runtime 通过 platform-private 的有界 bridge 交付 sample、status 或本地 operation。

内部 bridge 至少明确 schema/version、producer incarnation、clock/stream epoch、capacity、
overflow 和 restart behavior。外层 adapter 保留 source identity/time，并在内部 endpoint 断开
时更新 actual status。它不得把内部 RPC/queue 当成第二套公开 Resource 或 Link wire。

## 独立 Host 开发

Host 开发包只依赖公开 Profile、schema/wire、Virtual Edge API、deterministic simulator
和 conformance vectors；不依赖板级 SDK、驱动 header 或内部 IPC。从 simulator 切换到
physical Edge 时，Host 业务 API 不改变，只替换 backend 与已声明的 Profile/configuration。

允许独立实现的 contract bundle 必须同时冻结 protocol/schema version、上限、golden
encoding/failure vectors 和一条完整 Session trace。只有概念文档或某个 C struct 不足以让
另一端独立实现 wire。

## Endpoint adapter 模式

runtime 发现的 sensor、input、audio、media、presentation、codec 或 compute endpoint 使用
统一 capability/descriptor 模式，而不是在 Core 增加设备类型。

一个 endpoint adapter 只公开它确实具备的组合：

```text
configuration / desired    -> State（可选）
actual status / capability -> State
samples / events           -> Stream 或 MediaStream（可选）
side-effect operation      -> Action（可选）
immutable resource         -> Blob（可选）
```

driver API 存在、module 可 import 或 device object 已注册都只证明 runtime exposure。只有
真实 sample、physical output 或 effect/result 通过验收后，才把 endpoint 标为 physically
verified。

型号、format、sample rate、axis、calibration、clock relationship、memory domain 和 native
handle 留在 descriptor/adapter/Profile。camera、IMU、microphone、display 只是实例，不是
固定系统结构，也没有规范规定的实现先后。

## Portable core 约束

适合进入 portable core 的行为应：

- 不依赖 OS、socket、线程、driver 或具体 runtime；
- 在受限目标上允许调用方提供固定容量 storage；
- 用显式状态码和终态，不靠日志猜测成功；
- 明确 owner、version/epoch、lifetime 和借用 buffer 的有效期；
- 对 overflow、duplicate、conflict、wrap、disconnect 和 resource exhaustion 有测试；
- 不把 scheduling、locking、persistence 或 transport identity 偷藏在 primitive 中。

现有 newest-wins 实验已经证明一组可复用行为：完整 snapshot、严格 version、pending、
精确 ACK、重复/冲突、新 Session 重新发布和无隐式 heap。这些行为可转成 State conformance
vector；现有文件名、C symbol 和 ABI 不因此自动晋升。

## Identity 与时间

从 loopback 开始，测试与 trace 使用相同逻辑 identity：

```text
peer / boot / session
resource / state version
stream epoch / sample sequence / FrameKey
action identity / Blob content identity
clock epoch / source time / receive time / uncertainty
presentation generation
```

`FrameKey` 至少区分 producer clock epoch、stream epoch/instance 与 sequence。Host 本地帧号
不能替代 source identity。缺少正确 epoch、时间、calibration 或 freshness 时，消费方按
契约拒绝/隐藏，不能用最近一次任意值补齐。

跨端时间只在 offset/uncertainty 满足当前 Profile 时可比较。测试必须覆盖 reboot、epoch
变化、wrap、late arrival 与 uncertainty 超限。

## Bounded 与实际应用

- capture、Stream、codec、Link、history 和 physical output 是独立的有界消费者；一个
  consumer 失败不能无限反压其他路径；
- 每个 queue/history/retry/cache 声明 capacity、overflow policy 与 counters；
- best-effort Stream 允许丢弃过期数据，但必须保持后续进展；
- State/Action/Blob 不能在资源不足时静默伪装成功；
- “已应用”由真实进展证明，例如首个 sample、实际寄存器/设备状态、Action result 或
  presentation activation；adapter call 返回 success 本身不够；
- persistence、database、Dashboard 和文件布局是 integration，不进入 portable ABI。

## Runtime、镜像与构建选择

SDK 架构不规定 MicroPython、native C/C++、RTOS、Linux、system image 或 firmware build
路线。每个 platform work unit 选择最短且可复现的实现路径；需要制作或刷写镜像时，把
provisioning 作为该工作单元的正常实现步骤，并提供准确目标、来源/integrity、恢复路径、
供电条件、预期信号和写后验证。

实现便利性不能扩大证据范围：Host build 不证明 target ABI，runtime API 不证明物理
endpoint，模拟 output 不证明真实 scanout，另一块 SoC 的性能不证明当前 Profile。

## 验证环境

| 环境 | 证明 | 不证明 |
|---|---|---|
| pure unit | Resource primitive、identity、version/epoch、bounded storage | serialization、peer 或 hardware |
| in-process loopback | Host/Edge contract 与 Virtual Edge state machine | process/transport failure |
| local multi-process | serialization、buffer lifetime、crash/backpressure boundary | physical link/hardware |
| synthetic remote | Session、time、loss/reorder/reconnect | driver 和真实性能 |
| platform adapter | 一个真实 endpoint 的 sample/output/effect 与失败 | 其他 endpoint 或其他平台 |
| integrated Profile | 声明组合的端到端正确性、资源、时延和恢复 | 未声明组合 |

这些是可选择的验证环境，不是组件实施顺序。每个工作单元选择最小环境来回答当前问题；
相同 conformance vector 应尽可能用于 simulated、loopback 和 physical backend。

## 一个工作单元

1. 记录 observed baseline、真实问题与非目标；
2. 区分 `FACT / INFERENCE / PROPOSAL / UNKNOWN`；
3. 若公共行为改变，先修改最小权威规范和 conformance/failure vector；
4. 选择最小 implementation/Profile/configuration；
5. build/run 并检查真实输出、bounded counters 与生成物；
6. 记录证据范围，协调规范、报告和状态；
7. 分仓库显式暂存并提交，不把原始设备证据或本机信息带入公开仓。

组件、frontend、runtime 或 platform 的排期保存在当前计划中，不写入产品规范。当前公共
代码成熟度与已知证据见[系统进展](system-report.md)。
