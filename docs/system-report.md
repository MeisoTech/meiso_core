# 系统进展

> 日期：2026-08-20
> 状态：公开、已脱敏的 evidence report；不是产品规范，不包含原始日志、地址、凭据、
> 唯一设备标识或本机路径。

## 架构重基线

产品规范现已直接使用 `State / Stream / Action / Blob`、Virtual Edge 和 transport-
independent Session boundary。旧公共 API、旧私有 wire 和旧术语没有兼容承诺。新的
portable Resource semantic core、Session Core、Runtime negotiation core 与板级实验各自只有下文明确列出的 evidence
范围，不能互相替代。

规范入口是 [docs/index.md](index.md)。本报告不规定 platform、runtime、sensor、frontend、
transport 或实现顺序。

## 当前公共代码

截至 payload source 提交 `add9b8ac49a86cb0fb10b8f8cff7cbb8f3739b27`，
`meiso_resource_core` 已实现四种 primitive、共同值类型、
descriptor validation 与 fixed immutable descriptor snapshot。下表中的“已证明”只表示当前
portable in-process 实现通过了对应 Host fixture，不表示 remote、target 或产品 Runtime 已经
成立。证据捕获时 source 由 verified Git bundle 保留；commit 的后续发布状态不属于本报告的
运行证据，也不改变 artifact identity。

| 内容 | 当前 Host evidence | 尚未证明 |
|---|---|---|
| common / descriptor set | typed identity、SHA-256 ContentId 形状、descriptor 校验；fixed-capacity register/seal/value-copy lookup、duplicate/conflict/capacity | Runtime catalog/capability lifecycle、generation/permission/schema negotiation、wire 或 runtime dispatch |
| State | bounded publisher/replica、完整 snapshot、owner/version、duplicate/conflict/stale/wrap、best-effort/convergent、delivery generation 与 ACK/actual 分离 | Session adapter、真实物理 application、remote backend、线程安全或持久化 |
| Stream | `STRICT_MONOTONIC`、producer/epoch/sequence、bounded queue/history、gap/stale/conflict、drop/reject 与 deadline admission | reorder/reliable Profile、clock estimator、MediaStream external-buffer/codec/recovery 或 remote delivery |
| Action | bounded caller/executor、ActionId dedup/conflict、delivery ACK/executor acceptance/terminal result 分离、deadline、indeterminate 与 no eviction | 真实 operation、persistent dedup/exactly-once、Session 自动恢复、cancel/progress/DAG |
| Blob | bounded staging、callback verification、完整后原子 publish、duplicate/conflict/capacity/abort、zero length 与 reentrancy guard | Core 内置 hash、chunk/resume、persistence、eviction、授权或 Session transfer |
| four-kind fixture | 同一 descriptor snapshot 下四类真实 public contract 的独立初始化、正交状态以及逐类容量失败隔离 | Resource Runtime、generic dispatcher、LinkSession、remote backend 或产品 Profile |

当前 `main` 另实现 `meiso_session_core`：caller-owned arena、显式 big-endian wire v1、
`OPEN / ACCEPT / CONFIRM / READY`、exact protocol/Profile、bounded limits、一次性 Session
和 READY 前 Runtime-message gate。两个独立 arena 经同一 codec 完成 loopback；旧 Session、
错误 identity/version/Profile/limits、畸形 frame、提前 Runtime、close 和 transport loss 均有
明确失败。`session_four_kind` fixture 进一步用 test-only envelope 证明四类跨 peer 测试消息
只有在当前 SessionId/READY、已声明 ResourceId 且 kind 匹配后才进入 typed primitive；
旧 Session copy、未声明 ID 与错误 kind 均被拒绝。

该 fixture 的 Resource envelope 明确是 test-only；它自身没有 catalog generation 或正式
Runtime route codec，也没有认证、安全 transport 或 K230D Session evidence。因此它只支持
`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED`，不构成 `TRANSPORT_VERIFIED`。

当前 Session source tree 在 AppleClang 17、CMake 4.2、Ninja 1.13 的 clean out-of-tree
Debug、Release 与 ASan/UBSan 配置下完成 warnings-as-errors build 和 CTest，并通过
AppleClang 静态分析与 portable-library forbidden-symbol 审计。测试 inventory 以该
source tree 的
`ctest --test-dir <build> -N` 与实际 `ctest --output-on-failure` 输出为准；本报告不把测试总数
固化为长期事实。

GitHub Actions run `32289451531` 对 source
`5a53790d44be9a8097e37ebab05bf8f2cb9b8e0e` 完成四个 blocking job：Ubuntu 24.04 GCC Debug、
macOS 15 AppleClang Release、Ubuntu 24.04 Clang ASan+UBSan，以及 Python warnings-as-errors/
Node.js 的两个 Dashboard 与 Markdown link gate；全部成功。每个 native job 先单独执行 retained
four-kind integration fixture，再执行完整 CTest。该 CI 证明对应 hosted runner/source 的 build、
Host tests 与 sanitizer 路径，不证明 K230D cross-build、board execution、target-in-loop、Windows、
长稳或产品性能。

当前 portable `meiso_runtime` 依赖 `meiso_core`，不把 Runtime 反向纳入 Core。Source v0
永久绑定 READY Session，使用双 bank 原子 catalog；endpoint 内联 capability，route 绑定
EndpointId/incarnation、Core Resource descriptor 与 facet。MRT0 v2 catalog 接受Host-owned desired
State、Edge-owned actual State和Edge-owned data Stream；strict MRT0 v3另接受Host-owned
`IMMUTABLE/Blob` Host→Edge route。v2/v3不混用、不fallback，不存在subscription control plane。

Desired/actual 经 `MeisoStateOffer` 传递，delivery acceptance 保持为 Core State 进展；Host
只有在 actual 被 `StateReplica` 接受为 `RECEIVE_NEW` 并回交 Runtime 后才打开 production
gate。Desired/actual receive candidate 均以有界副本绑定同一次 Core State event，commit 时拒绝
相同 State identity 下的 payload 偷换；desired payload 的有界副本包含完整 canonical prefix 与
Profile suffix。Gate 绑定当前 desired reference、effective Profile 与 production epoch；desired
disable 本身不关门，`STOPPING` 的旧 epoch 仍可发送，`STOPPED` 才关闭。停止后重启、不连续
Profile/desired 变化必须使用严格更新的 epoch。

Bounded endpoint history 以 `(EndpointId, opaque incarnation)` pair 计数且不驱逐。Endpoint
暂时离开 catalog 不会退役 current pair，同 pair 返回时继承 epoch high-water；任意从未见过的
非零 incarnation 都能成为新 current 并退役旧 pair，退役 pair 返回为 `STALE_ENDPOINT`，历史
不足为 `CAPACITY`。Catalog 激活仍清空 desired/actual/gate。

`meiso_runtime_test` 覆盖 canonical State payload、v0 catalog/bundle 约束、StateReplica commit
gate、desired/actual payload exact match、disable/stop、epoch 与 endpoint identity history
continuity/retirement/capacity、catalog replacement 与 Core terminal；
`meiso_runtime_session_test` 使用真实 Session、StatePublisher/Replica 和 Stream primitive 验证
desired→actual→sample、disable cut、reconfigure/new epoch 与新 Session 重建 catalog；continuous 用例在
同一 READY Session 内验证 enable、旧 epoch 在重配置接受前继续、新 epoch 切换、
disable 中的 `STOPPING -> STOPPED`、stop 后 Session 仍 READY 以及再次 enable。
`meiso_runtime_wire_test` 以独立 literal golden bytes 冻结 wire v2 的 catalog、accept、
reject、State offer/accept 与 Stream sample 六类 frame，并覆盖每个截断点、reserved 字段和
malformed 时的公开状态不变。

public commit `a8a777a` 的`meiso_runtime_blob_wire_test`又冻结MRT0 v3：Composition Profile
v1→MRT0 v2、Profile v2→MRT0 v3；Host→Edge BEGIN/CHUNK/COMMIT/ABORT 与Edge→Host RESULT；
4096-byte chunk、全局单transfer、Runtime生成的Session-scoped ID、无per-chunk ACK、terminal
phase/prefix以及Session/catalog discontinuity。Edge先在完整Runtime scratch核对length/SHA-256，
再由generic synchronous bridge将同一bytes stage/copy/commit至Core BlobStore；Runtime SHA与caller
Profile verifier作用于同一个immutable Core verification snapshot。只有Core PUBLISHED/DUPLICATE
映射wire success；publication后仍须显式建立SceneStore/SDK visibility。

v3 vectors覆盖五类独立literal frame、0/4096/4097/262144 bytes、gap/overlap/replay、abort-before-
READY/late READY、async terminal RESULT、TX_BUSY/alias/reentry、multi-route global slot、Core external
staging ownership/forced cleanup、duplicate后visibility，以及State/Stream/Blob共存。fresh Debug、strict、
ASan+UBSan均CTest 23/23，clang analyzer对Runtime/composition/test为0 finding。这支持portable Runtime
`IMPLEMENTED / HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED`；该R1 evidence不证明transport、fair
scheduler或target，后续R2a scheduler evidence见本报告Composition段。精确边界见
[Runtime negotiation core v0](runtime-core-v0.md)。

Portable Runtime仍未实现Action、MediaStream、Edge→Host/并行/resumable Blob、产品endpoint
direction/QoS/permission schema、typed dispatcher与Host orchestration/SDK。R2a 已在 direct Runtime
之外增加 Composition Profile v2 专用的 caller-owned fair scheduler；它不是 Core、wire 或通用产品
QoS。下述Wi-Fi slice在Core外组合一个具体Profile、config/probe adapter与direct-IP target；它不把
这些平台选择提升为portable Core能力。

当前 POSIX platform layer 另实现 connected-stream adapter：借用 caller-owned、already-
connected、nonblocking `SOCK_STREAM` fd 和 Session；RX/TX 各由 `max_frame_bytes` 限制，
每 step 至多一次 `recv/send` 且不超过 `max_io_bytes_per_step`。adapter 不创建、连接、监听或
关闭 socket，不解析 IP/DNS/config，不拥有 event loop、认证或 Runtime route。

`meiso_posix_tcp_test` 使用真实 macOS `AF_INET` loopback TCP 验证 nonblocking requirement、
read would-block、write backpressure 不提前确认、一字节 handshake/full-duplex、连续 Runtime
frame、非法长度、半帧 EOF、adapter capacity、partial-prefix terminal 和 reset/no-SIGPIPE。
macOS Dev Tool 另以 `poll`、7-byte step 和每阶段 5-second watchdog 完成 handshake、双向
opaque Runtime payload 与正常 close。因此证据为
`TRANSPORT_VERIFIED(macOS 15.5 arm64, AF_INET loopback TCP, max_frame=256)`；它不证明
remote/direct-IP、Wi-Fi、K230D、bootstrap policy、TLS/auth、Runtime dispatch 或产品 Profile。

## Direct-IP Wi-Fi Runtime slice

当前 `main` 以 [`wifi-telemetry-v0`](../profiles/wifi-telemetry-v0/README.md) 组合 Session Core、
Runtime wire v2、Core State/Stream 与 POSIX adapter。真实本机 `AF_INET` 测试完成 Core READY、
完整 catalog accept、Host desired enable、Edge actual RUNNING、88-byte匿名 Wi-Fi sample、
disable cut、actual STOPPED、unsupported desired 和新 Session 不继承 gate/actual；它使用
synthetic telemetry，不证明 remote network、Linux sysfs 或 K230D。

同一测试另在 `RUNNING` 与首个 sample 后以真实 TCP RST 终止连接：两端 Session 均进入
`FAILED/TRANSPORT_LOST`，旧 Runtime route 与 production epoch 失效；fresh fixture 不替 Host
自动重发 desired。
新 `AF_INET` 连接必须重新完成 Session、catalog、desired 与 actual；旧 epoch 被拒绝，只有新
production epoch 的 sample 才重新进入 Core Stream。该向量仍是本机 synthetic carrier evidence，
不是 K230D network-loss 或 reconnect target evidence。

macOS Host foreground Dev Tool 从私有配置读取 numeric IPv4 locator 与 local/expected
PeerId，只按 accepted Profile 输出 allowlisted NDJSON；不带 flag 时保留有界 one-shot，
`--interactive` 在同一 Core Session 内接受精确 compact NDJSON
`apply(enabled=true/false)` 与 `close`。它已通过
warnings-as-errors build，但不是 SDK/Virtual Edge。K230D Linux Edge target 从私有
interface/rate/field envelope 与 Linux probe 交集生成 catalog，并在单连接中处理多个
desired；production 中 source continuity 无法证明时，它排空已物化数据并以
`SHUTDOWN` 结束当前 Session。

2026-08-16，Edge source commit `2ac960c` 以 Xuantie GCC 14.1.1 交叉构建为 91,128-byte
RISC-V ELF（SHA-256 `24b02ff30ca9f1caa0542a997bdea93785d23766fdd22e06a9aee8a83a2b76f6`），
并在 K230D Buildroot Linux 6.6.36 上与 macOS Host 完成一次真实 Wi-Fi direct-IP 运行。双方
依次得到 Core READY、1 endpoint/3 route catalog accept、desired v1 enable、actual RUNNING、
真实匿名 Wi-Fi sample、desired v2 disable、actual STOPPED 与 normal close，双方 rc=0。第一轮
选择 6 fields/500 ms 并收到 5 个连续无 gap 样本；第二轮只选择 link+signal/1000 ms 并收到
3 个连续样本，其他字段保持 absent，证明 desired/actual 确实改变 producer 的字段形状与 cadence。
公开摘要不保留 locator、SSID、MAC、PeerId、凭据或原始 counters。该结果可标
`BOARD_VERIFIED`，但只覆盖这两个手动 one-shot 参数组合与上述 transcript。

2026-08-17，新 Edge 以 Xuantie toolchain 交叉构建为 95,360-byte RISC-V ELF
（SHA-256 `fe8e20272917d04b4b90a565c8c9c4041a9062d717810c811fd6c7c517ce4dbc`）。
macOS Host 与 K230D 在同一 Core Session 内以 `selected_fields=0x03f` / `500 ms` 启动，
再运行中改为 `0x003` / `1000 ms`，两段合计接收 5 个真实 sample。脱敏 transcript
精确经过 `STARTING/PENDING -> RUNNING/APPLIED`、保留旧 effective/epoch 的
`RUNNING/PENDING`、新 epoch 的 `RUNNING/APPLIED`、`STOPPING/PENDING -> STOPPED/APPLIED`，
停止后 Session 仍 READY；最后 Host/Edge normal close，双方 rc=0。这项 `BOARD_VERIFIED`
只覆盖该 artifact、参数组合和
transcript，不证明性能、长稳、安全 transport、自动重连或 SDK。

当前 loopback-only Wi-Fi Runtime Dashboard 以固定 argv 启动同一 native Host，向它交付
`connect/apply/stop/close` 固定意图，并投影 Host 已校验的脱敏 NDJSON。desired/actual 仍是
Core State，sample 仍是 Core Stream；Web 不解析 wire，不是 Runtime/SDK/Virtual Edge，也不实现
subscription 或第二套 truth。

2026-08-17 的当前 Dashboard 实机验证先显示 Core READY/catalog，再在同一 native
Host/K230D Session 内显示 `0x03f/500 ms` 持续 sample，运行中改为
`0x003/1000 ms` 后新 epoch 的 Stream sequence 重新起算。浏览器 reload 后同一
Session/sequence 继续、control 状态恢复，且不重放历史 sample；disable 依次显示
`STOPPING -> STOPPED`，Session 仍 READY，然后同 Session 再 start 到新 epoch。它只证明
Web projection/control 与 native 持续链路的集成，不扩展协议或 target 证据范围。本次
页面 attempt 尚未请求 normal close，不声明其 Host/Edge rc；前述独立 native run 才支持
normal close 双方 rc=0。2026-08-16 的早期页面另曾完整投影一次 3-sample
K230D one-shot transcript，不能替代当前 interactive control evidence。

2026-08-16 的 target fault run 在 actual `RUNNING` 且已交换 10 个 sample 时强制终止 Host；Host 与
one-shot Edge 均非零退出，Edge 未伪装成 `STOPPED` 或 normal close。板与 Wi-Fi 未重启，fresh
Edge/Host 随即建立新 Session，并重新完成 catalog、desired、actual、5 个 sample、`STOPPED` 与
normal close，双方 rc=0。这是显式 process restart recovery evidence，不证明自动 reconnect、
resume、旧 desired/gate 继承、OS/network outage 或长稳。

2026-08-16 另一次 bounded cadence run 只选择 link+signal，以 100 ms desired 周期连续接收 sequence
1..64；全部 `gap=false/missing_before=0`，actual 精确确认参数，随后 `STOPPED`、normal close，
Host/Edge rc=0。这个约 6.4-second 窗口不是 latency/jitter、吞吐、deadline 或长稳 evidence。

Direct IP 是 locator，不是 identity；plain TCP 上的 PeerId/Profile exact check 不是认证、机密性
或完整性。不存在 subscription：Host 以 desired State 停止生产，已物化样本必须进入 Core
Stream。完整操作规则与 evidence matrix 见
[Direct-IP Wi-Fi Runtime v0 实装规则书](wifi-runtime-direct-ip-v0.md)。

## K230D Linux four-kind integration CI

保留该 Profile 的首要目的，是在同一固定 transcript 中纵向联测 Session Core、POSIX
connected-stream adapter 与四类 Resource primitive；它与正式 Runtime v2 CI 互补，但不定义
产品协商。2026-08-16 的一次受控实验把同一 Session Core、POSIX connected-stream adapter
和固定 [`four-kind-lab-v0`](../profiles/four-kind-lab-v0/README.md) fixture 运行在 macOS Host
与 K230D 大核 Linux Edge 之间。Edge ELF 为 96104 bytes，SHA-256
`41a7a7b3930d2b738c42fb127d2628b7b3b623a74952ca061b41703f247f738b`；它从维护通道临时部署到
`/tmp`，没有刷机、安装服务或 autostart。

一次 Wi-Fi plain-TCP Session 中，Host 与 Edge 均返回成功；State、Stream、Action、Blob
分别通过 `3/3`、`3/3`、`4/4`、`3/3` 检查。Host 正常本地关闭，Edge 观察到 peer close。
State/Stream 展示了真实 Wi-Fi interface/carrier、脱敏计数与 signal bucket；K230 片上 junction
temperature 经 vendor conversion 后为 `59.002 °C ±3 °C`。Action 只重置进程内 telemetry
window，Blob 是 401-byte 固定脱敏 manifest。camera 与 IMU 没有被注册为可用 endpoint。

本地 Dashboard 只渲染 native Host probe 的固定 NDJSON allowlist，不解析 Meiso wire。该结果
证明一个有界四语意数据交换切片可以在当前 Wi-Fi 环境运行；该 fixture 早于且没有使用
`meiso_runtime`，不能作为正式 catalog/desired/actual evidence。它也没有测吞吐、延迟分布、
长稳、重连、认证、加密、SDK/Virtual Edge 或 NuttX bridge。

## 代理平台实验

一个 640×480 lab workload 曾在 i.MX8MM Edge proxy 与 Orin Host 间得到：

- H.264 media、Host decode/detection、source-frame identity 与 Edge-side bounded correction；
- 验收窗口约 27 fps 的 encode/decode/preview 进展；
- 媒体发送失败时，capture、encode 与本地 preview 继续进展；
- desired 与 actual 分离、ACK 丢失重试、session 重发、lease safe state；
- bounded history、payload integrity、Dashboard 和资源额度停止行为。

这些结果只证明当时 adapter/workload 的形状和失败门。Hantro、RTP/UDP、Linux process、
设备节点、私有 packet、Orin 计算路径和约 27 fps 数字都不是新架构约束。

## Host handoff 与 compositor 设计状态

当前 [Host 集成回执 v0](host-integration-receipt-v0.md) 只整理已经实现/验证的 direct-IP
Session→Runtime→Resource 顺序、Host handle 生命周期与协议入口，没有新增 wire 或 SDK API。
Host 本地 object/compositor 模型应通过 adapter 映射到 accepted catalog、desired/actual State、
Stream 与未来 Composition Resource；不能把本地 C struct、Dashboard NDJSON 或 preliminary
remote encoding 直接作为 Edge wire。

[Edge local compositor v0 开发方案](edge-local-compositor-v0.md) 已选定 Runtime--SDK 边界、
plan prepare/activate authority、renderer/present seam、K230D Linux 首发与选择性源码迁移规则。
[composition-scene-lab-v0](../profiles/composition-scene-lab-v0/README.md) 当前完成C0--C2与R1的Host侧
分层证据：同一composite合同包含Wi-Fi、presentation与scene endpoint；Mesh用Host Blob，camera/
object snapshot用Host State，Edge world/capacity用actual State，固定thumbnail用Edge Stream。
commit `3b9ef023edffba1c2e652c778723768f1bc7c8cf` 冻结 exact IDs、32/52-byte capability、2 个新
endpoint/6 条新 route、PresentationPlan/Status、canonical Mesh、SceneDesired/Status 与 6304-byte
RenderResult。literal/negative/max-boundary Host test 验证 920-byte combined catalog、3812-byte State、
6460-byte Stream、与Wi-Fi namespace disjoint及独立计算的Plan SHA-256 preimage/digest；它只构成C0
codec/wire oracle。

commit `4bccc12` 的 `scene_store.h/.c` 与 `meiso_composition_scene_resource_test` 又完成 C1 Resource
fixture：真实
`MeisoBlobStore`、Host/Edge StatePublisher/Replica 与 `MeisoStream` 贯通 cube commit、完整 scene
snapshot、candidate、显式 frame-boundary activate、SceneStatus 和 deterministic RenderResult；新 State
owner 会清 world、candidate、store内last-result引用与Blob visibility，只有重新 publish 或 verified
duplicate 后才可引用底层 cache；外部Stream queue/history不由store清理。
missing/invalid Blob、新 desired supersede、33 objects、9 meshes、16-slot publication、8+8 distinct
logical mesh boundary、State duplicate/stale/conflict、4-slot Stream overflow与world/Blob授权
no-resurrection均有确定性vector。

该 evidence 支持 `C0 CODEC IMPLEMENTED / HOST PROFILE-WIRE VERIFIED` 与
`C1 RESOURCE SEMANTIC VERIFIED / LOGICAL ADMISSION ONLY`。当前macOS arm64 ABI中，Blob、desired
publisher/replica、actual publisher/replica、result Stream、scene metadata arena依次为
4,720,304 / 3,920 / 3,856 / 384 / 320 / 32,272 / 31,656 bytes；均要求8-byte alignment，合计
4,792,712 bytes，memory domain是Host test process的CPU-visible heap，不含GPU/CMA arena。CPU
fixture零拷贝借用已发布 Blob，
`resident_mesh_bytes` 是 canonical vertex+index 的逻辑引用量；fake renderer流式生成 hash/thumbnail，
所以如实报告 target/job `0/0`。它没有证明独立 4 MiB GPU resident arena、renderer target/CMA分配。

public commit `4210ddf` 的`composition_coordinator.h/.c`与扩展后的SceneStore完成C2，状态为
`COMPLETE / PORTABLE COORDINATOR SEMANTIC VERIFIED`。Coordinator借用caller-owned SceneStore并绑定
exact READY SessionId；fresh coordinator首次`begin_owner`推进monotonic control-scope generation，
same-owner新Session也会清world/candidate/result引用与Blob visibility并使旧coordinator所有API失效。
immutable Blob cache和world-generation high-water可保留，但不能跨scope授权复活。

C2只接受decoded Runtime desired的两种exact形状：disabled为canonical 8-byte prefix且无Profile suffix，
enabled为同一prefix加128-byte`PLN0`。Plan generation为每owner/scope不复用high-water；coordinator复算
C0 digest，backend completion必须带exact `PlanRef(owner/version/generation/digest)`。prepared token绑定
完整SessionId、scope generation、Edge/desired owner、serial、capability、PlanRef、Scene ref、prepare
time、selected config与caller nonce；它是deterministic binding，不是authentication，nonce全生命周期
fresh仍是caller前置条件。late report、wrong token、same-generation conflict与arena alias不替换candidate/
active；PREPARED后scene supersede使旧token失效并回PREPARING，old active保留。SceneStore activation是
transaction最后一个fallible调用；disable只有在producer drained后才在frame boundary退役plan/world，
未drain的`WAITING_DRAIN`对observation/status/stats/active/world/time零副作用。

8组deterministic vectors覆盖independent C0 digest、Runtime shape/order、async failure/late completion、
token/scene/generation、same-owner new Session、arena/alias/time、parallel Session scope invalidation与
success/swap/status/disable。fresh normal及ASan+UBSan全套CTest均22/22，相关targets另过strict
conversion/shadow/prototype warnings-as-errors。Coordinator arena为1,520 bytes/alignment 8；包含
BlobStore、SceneStore、Coordinator、Scene desired、Presentation desired/actual State pair及result Stream
的C2 fixture合计4,794,968 bytes，同样只来自macOS arm64 Host test process CPU-visible heap。

public commit `a8a777a`完成R1 portable Blob carriage。Composition Profile v1保持MRT0 v2并拒绝
IMMUTABLE route；Profile v2显式选择strict MRT0 v3。多个Host-owned Blob route共享一个全局transfer；
Session/catalog discontinuity清Runtime transfer与Edge scratch，已发布Core cache可保留。新Session/
control scope必须重建visibility；catalog-only replacement的外部visibility策略由后续Profile/SceneStore
integration定义，Runtime不持有它，R1未验证该策略。
Runtime→Core publish bridge、owner/direction、phase/result、boundary与failure atomicity均有确定性vector。

macOS arm64下Host Runtime为14,904 bytes，Edge Runtime为277,048 bytes（含262,144-byte scratch），
alignment均为8。BlobStore、SceneStore、Coordinator与两端Runtime五项合计5,045,432 bytes，不含
Session/State/Stream；若以C2完整fixture 4,794,968 bytes再加两端Runtime，则为5,086,920 bytes，仍
不含两端Session。两者都是Host process CPU-visible heap口径，不是K230/GPU/CMA allocation。

public commit `5d92ff6` 完成 R2a portable combined builder 与 TX scheduler。Profile v2 builder 在失败时
保持所有输出不变，并固定 selected Session limits `8236/8192/12`、3 endpoint、9 route 与 canonical
920-byte catalog。每个 peer 的 caller-owned scheduler 独立拥有本地 Runtime/Session TX：control source
持久 RR，最多连续 4 个成功 control slot；data round 冻结成员，每个 ready source 本轮至多一帧；
`data_probe_due` 防止 nonbusy Runtime-rejected control head 持续饿死 data。Host 单 Blob data route 的确定性等待上界
为 `<=5` 个 Host outbound 成功 slot；Edge 的 Wi-Fi/RenderResult 两条 data route 同时 ready 时各为
`<=6` 个 Edge outbound 成功 slot。successful slot只表示direct Runtime send返回`OK`且frame已经排入
Session outbound，不表示socket write、peer receive、application commit或时延上界。

R2a vector 还验证 `TX_BUSY` 对完整 Scheduler/Runtime/Session arena、source、output 与 pending frame
原字节零推进；State `OFFER`必须commit；Blob仅在接收`COMMIT`后Runtime snapshot为
`publish_pending && !result_pending`时必须经Core BlobStore publish wrapper完成，length/SHA precheck
失败直接产生terminal `RESULT`且没有publish obligation。active catalog generation变化清binding/round
并进入`REBIND_REQUIRED`；Session terminal永久进入`SESSION_TERMINAL`并exact-once通知sources，必须新建
Session/Runtime/Scheduler。public commit `5d92ff6` 的 fresh Debug CTest 为 24/24，strict C11/C++17 通过，clang analyzer 对
scheduler/builder/test 为 0 finding。Core/Session/direct Runtime/Profile v1 wire/API tracked diff 为零。

public commit `7e53bc9` 又完成R2b0的真实loopback happy path：Host与Edge各用nonblocking POSIX
adapter连接`127.0.0.1`上的`AF_INET/SOCK_STREAM`，每次transport I/O限制为17 bytes，因此双方
Runtime frame均实际跨越多次`WRITE`/`READ_PROGRESS`。同一Profile v2/MRT0 v3 Session使用exact
3 endpoint、9 route、920-byte catalog与双方R2a scheduler；cube Mesh Blob经Runtime接收、Core
publish及显式`mark_visible`后，Scene/Plan State驱动C2进入`ACTIVE`。实际观测的production epoch为
Wi-Fi actual `77`、Scene actual `42`、Presentation actual `41`；`WIFI_TLM/DATA_STR` Stream为epoch
`177`/sequence `1`，`K230_SCN/RES_STR` Stream为epoch `417`/sequence `1`。

fresh Debug与ASan+UBSan CTest均为25/25，clang analyzer为0 finding。GitHub Actions run
`32316527393`已完成，四个job全部success。

public commit `9daa7f9` 已push到`main`与`rain/core-session-v0`；它在同一真实TCP Session、catalog、
Plan 1和已发布cube上继续完成R2b1 lifecycle。
所有desired/actual State均通过offer、replica commit、accept与publisher confirmation闭环，并保持
byte-exact。transcript依次固定：world 1/RenderResult sequence 1；Scene desired v2 pending期间old-world
exact sequence 2；TRS world 2/sequence 3；camera world 3/sequence 4；第二个共享cube对象world 4/
sequence 5；删除首对象后的world 5/sequence 6。Plan 1的PlanRef与digest始终不变；Scene actual
production epoch `43..46`分别在actual提交前拒绝new epoch、提交后拒绝old epoch，且拒绝不消费
Stream sequence。

整个reconfigure链没有重传Blob：BlobStore count/bytes/publications/duplicates、Host/Edge transfer
high-water及idle flags前后不变，publication仍只有一次。最后发送8-byte prefix-only Presentation
desired v3；未drain时`WAITING_DRAIN`并验证Coordinator与SceneStore arena raw zero-diff，drained后
原子退役。Presentation与Scene actual均byte-exact进入`STOPPED`，SceneStatus稳定为reason `NONE`、
无active plan/world/object；随后RenderResult得到`ROUTE_NOT_ACTIVE`，而Wi-Fi仍以production epoch
`77`发送sequence `2`。fresh Debug与ASan+UBSan full CTest均25/25，clang analyzer 0 finding；GitHub
Actions run `32317542841`四个job全部success。

public commit `369ff4d` 已push到`main`与`rain/core-session-v0`，并在同一真实TCP Session上完成R2b2。
generation 2 catalog offer只建立staged/pending值：Host/Edge active仍为generation 1，3 endpoint/
9 route及两端scheduler generation、phase、所有source boundary均不变。Host accept后两端active catalog
原子切到generation 2；双方scheduler每个source只收到一次`CATALOG_CHANGED`并进入
`REBIND_REQUIRED`，重复step不重复通知。所有route-bound source必须绑定g2 route，绑定g1被拒；
两端各一个stale g1 queued work得到`SOURCE_INVALID`，Session outbound保持空，未写socket。

整个replacement保持同一SessionId、Session handle、fd、local/remote peer identity、incarnation、
negotiated limits与`READY`，没有close/failure。Runtime清除Wi-Fi/Presentation/Scene actual与data gate，
但保留production epoch high-water `77/41/46`。fresh capability-generation-2 coordinator在同一Session
下推进SceneStore control scope，清world/candidate/visibility；旧coordinator的observation/candidate/
active/status/stats均不可再取，旧token/world activation及旧world result引用被拒。

Core Blob cache仍保留cube，但不恢复新scope visibility，因此Scene desired State v6第一次prepare得到
`MISSING_BLOB`。g2 Blob transfer完整走wire/Core bridge并返回Core `DUPLICATE`，随后显式
`mark_visible`才允许prepare。新的State闭环使用Wi-Fi/Scene/Plan desired version `2/6/6`与Wi-Fi/
Presentation/Scene actual version `2/3/7`；Plan generation/capability generation均为2，新prepared
token与旧token不同：Plan desired v4建立candidate/new token，v5回显old token得到mismatch且candidate
不变，v6回显new token激活world 6。新actual production epoch为`88/51/52`；旧production epoch `77/46`在scheduler
侧拒绝，旧Stream epoch `177/417`在新production gate下到达后由Stream拒绝。新的exact
`WIFI_TLM/DATA_STR` epoch `178`与`K230_SCN/RES_STR` epoch `418`均从sequence `1`无gap恢复。

fresh Debug与ASan+UBSan full CTest均25/25，clang analyzer 0 finding；GitHub Actions run
`32318799466`四个job全部success。

public commits `9df5268`、`e4a726f`与`43ae3ce` 已push到`main`与`rain/core-session-v0`，完成R2b3的
explicit same-process RST/reconnect slice。Runtime refresh在READY且SessionId/role/limits exact时完整
byte-preserve；unavailable/unknown/non-READY首次原子清两代catalog、pending State/Blob与Edge scratch，
保留Blob transfer-id high-water并返回`CORE_NOT_READY`，随后永久`STALE_SESSION`。live identity/role/
limits mismatch首次即stale；verifier reentry为`CONFLICT`且arena零变化。Scheduler在poll/TX前自动
refresh，先清Runtime再exact-once通知`SESSION_TERMINAL`。

真实transcript在g2 ACTIVE后开始distinct Mesh transfer：`BEGIN/READY/CHUNK`完成但无`COMMIT`；Edge
adapter abort并以`SO_LINGER` RST/close中止，Host read观察transport loss。双方Session均进入
`FAILED/TRANSPORT_LOST`；旧Runtime/Scheduler/route/epoch fail closed，未完成的第二Blob未进入Core，
SceneStore旧active world仍保持。新AF_INET连接使用不同SessionId及新的Session/Runtime/Scheduler/
coordinator arenas；新coordinator开始新control scope后才清world/visibility并使旧coordinator/token/
world失效。同process Core cache与SceneStore保留，但desired、visibility、world和actual/data gate均不
自动继承。显式提高State delivery generation、重发cube并取得Core `DUPLICATE`、再次
`mark_visible`后，以Plan generation 3/new token、Wi-Fi/Presentation/Scene actual epoch `99/61/62`
与Wi-Fi/RenderResult epoch `179/419` sequence 1恢复。Linux CI首次暴露测试对arena tail padding的错误假设；`43ae3ce`改为从真实
COMMIT Runtime-scratch borrow校准offset并在同配置fixture重验范围，产品实现未变。GitHub Actions run
`32321051775`四个job全部success。

R2b0--R2b3上述证据严格标记为
`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED / TRANSPORT_VERIFIED`（loopback same-Session happy
lifecycle/catalog replacement及explicit same-process reconnect）；它没有覆盖自动resume、fresh Edge、
wrong Profile、max-Blob、backpressure、fair-contention stress，也没有证明K230/K1 build/run、
Hz/latency/jitter/throughput/performance、Host editor、renderer backend、
GPU/target/job/CMA、transport security、offline lease执行或physical output。`offline_lease_ms`在C2只被校验/保留；C2没有
Session-loss signal，backend`DEADLINE`只是caller报告的本地attempt结果。本轮也没有合并外部Host
compositor、旧本地素材、vendor SDK或二进制。

## K230D lab evidence

Resource semantic core 的 target build 状态为 `HOST_VERIFIED`，identity-gated candidate 在
`CanMV-K230D-Zero V1.1` 上的 execution 状态为 `BOARD_VERIFIED`。同一七套 Host 测试源码
已经在 exact CanMV v1.8 20-project graph 上 fresh cross-build；生成的 325472-byte static
RISC-V ELF 经 host inspection，并从 fresh full-media `.img` 的
`/sdcard/app/meiso_conformance` 只读抽取后逐字节复核。完整镜像中的 payload SHA-256 为
`04b085c813f85414ce0f8284f54a8e24fe1649e113429a0df50273edb1f23e91`。

该 target application 在 warnings-as-errors 下有零 Meiso diagnostic；完整 pinned SDK fresh
build 的 1421 条上游/toolchain warnings 已单独计数和审阅。生成的 `.kdimg` 分区表只有 `rtt` 与 `rtapp`，不含
SDCARD FAT 或 Meiso ELF，因此是 `NO_MEISO_PAYLOAD / NOT_A_MEISO_OTA_CANDIDATE`。

candidate `.img` 的准确 source length 已完成写入并 sync 到一张可反复刷写的开发卡。没有取得
自动挂载前的 whole-media equality；macOS 随后的读写自动挂载改变了 writable FAT metadata，
因此 post-write whole-media digest 不再等于 source image。后续比较只证明前 115343360 bytes
的 boot-critical prefix 与 `/sdcard/app/meiso_conformance` payload 逐字节一致。这个明确记录的
开发卡接受条件不等于 release-media reproducibility，也不把 host inspection 或 USB 枚举本身
算成板端通过。

带上述 FAT 变更边界的 candidate 从 TF/SD 启动，capture 实证 U-Boot SPL、DDR training、OpenSBI、
RT-Smart/CanMV 与 nncase runtime；固定 marker probe 证明交互入口为 MSH。人工运行
`/sdcard/app/meiso_conformance` 后，`resource / state / stream / action / blob /
descriptor_set / four_kind` 七套 suite 均返回 `rc=0`，最终报告 `passed=7`、`failed=0`、
`first_failed_suite=none`。物理断电重启后再次人工运行得到相同摘要与输出 digest；只有第二次
conformance invocation 保留完整原始 RX capture，该 capture 不包含此前的上电过程。候选基于
[官方 CanMV v1.8 Zero release](https://github.com/kendryte/canmv_k230/releases/tag/v1.8)。对应
[Zero 官方规格](https://www.kendryte.com/k230_canmv/en/main/userguide/boards/canmv_k230d.html)
声明 128 MiB LPDDR4，但本次启动并未测得可供 Meiso 使用的容量或预算。这个结果只证明
准确镜像实例能够启动并在一个 fresh process 中执行 Resource semantic core v0 runner。

本次没有取得从上电前开始的完整无干扰 cold-boot capture；串口工具打开设备时是否改变
control line 也未知。因此证据不声明运行时 autostart absence、完整重启时序或 LinkSession
reset；静态 image inspection 只证明 Meiso target 没有新增 startup hook。操作者报告板体缓慢
升温且可接受，但没有校准数值、环境条件、功耗或 soak，所以这不是 thermal validation。

当前公开 evidence 尚未证明该板实例的 camera module、display panel、touch、microphone、
IMU、codec/KPU workload、clock quality、usable memory budget、持续温度、功耗或任何产品
Profile。上面的 Wi-Fi lab 只证明一个固定 plain-TCP 数据路径，不证明产品 network stack；
firmware 中包含 driver/API 也不等于物理 endpoint 存在。

系统通过通用 capability/endpoint discovery 表达这些组件，不为 K230D Zero 或某种 sensor
建立 Core 特例。CanMV、native runtime、SDK sample 或其他镜像都可以用于后续实验；结果
必须按具体版本和条件记录。

## 已验证的负面结论

- 把每帧 raw camera 当成可靠完整 transfer 会造成高带宽与旧数据阻塞，不适合连续 Stream；
- per-frame ACK/retry 会让过期媒体妨碍实时进展；
- 软件 JPEG 在当时代理 workload 中未达到目标帧率；
- driver start/success 不能证明首个真实 sample 或 physical output；
- 在现有 CPU workload 尚有余量时先建设通用 GPU/DLA framework，没有证据支持其复杂度；
- private wire 之间已经出现不兼容语义，不能作为新公共 wire 的迁移基础。

负面结论同样带平台与 workload 范围；它们用于设计 failure vector，不用于禁止所有未来
实现。

## 未实现与未知

- 稳定的 Host-facing `EdgeHandle`/Virtual Edge API；
- product Runtime 的 endpoint facet/direction/QoS/permission schema、通用 hotplug、Host
  orchestration 与 typed dispatcher；
- discovery、安全 transport、time estimator 与 reconnect/resume 参数；
- Wi-Fi slice 的 network-loss、source-continuity-loss、reconnect、catalog update 与长期板端 evidence；
- MediaStream 与 Resource lifecycle/telemetry 的公共集成；
- Composition的自动resume、fresh Edge、wrong-Profile、max-Blob、backpressure与fair-contention
  fault/recovery，Host editor、renderer/present backend与真实presentation endpoint；R2b0已有loopback
  happy-path semantic/wire/transport evidence，R2b1已有same-Session lifecycle evidence，R2b2已有
  same-Session catalog replacement evidence，R2b3已有explicit same-process RST/new-Session evidence，R2a已有
  portable builder/fair scheduler evidence，C2
  coordinator与R1 Runtime v3 Blob carriage也已有Host evidence，但没有target interoperability、
  offline lease执行或physical output；
- Wi-Fi Profile 的 perf、fault、recovery、memory、thermal 与安全验收，以及其他产品 Profile；
- K230D 自动化 cold-boot/restart evidence、target-in-loop CI、fuzz、并发、板端资源预算和长期
  fault/soak/thermal evidence。

这些 gap 是无序能力集合。实际下一项由当前产品问题、可用硬件、风险和可执行验收选择，
不在本报告中固化开发路线。
