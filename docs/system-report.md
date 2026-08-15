# 系统进展

> 日期：2026-08-16
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

当前分支另实现 `meiso_session_core`：caller-owned arena、显式 big-endian wire v1、
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

当前分支另实现 portable `meiso_runtime`；`meiso_runtime` 依赖 `meiso_core`，不把 Runtime
反向纳入 Core。它永久绑定一个 READY Session，并实现双-bank 原子 catalog、
`ANNOUNCED / HOST_SUBSCRIPTION` route、Host desired、Edge actual、严格 request id、route
epoch，以及 Session/catalog/endpoint/resource/kind/schema/request/epoch 的完整数据门。
Catalog 使用单一 bounded frame；被拒绝的 generation 保留 monotonic high-water；Host 容量
较小时仍能发送 capacity reject；Core 终止使所有 route 失效。

`meiso_runtime_test` 已验证 arena/limits、accept/reject/update、非对称容量、TX-busy retry、
desired 与 actual 分离、route failure 和 Core close；`meiso_runtime_session_test` 经真实 Session
frame 和 test-only Stream payload 验证 Runtime route 后进入 Stream primitive，并拒绝 stale generation 与旧 Session
message。`meiso_runtime_wire_test` 另以独立 literal golden bytes 冻结六类 frame，并覆盖每个
截断点、trailing byte、reserved 字段与 big-endian decode。该结果支持
`IMPLEMENTED / HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED`；不构成 transport、target 或产品
Profile evidence。准确边界见
[Runtime negotiation core v0](runtime-core-v0.md)。

这只是正式 Runtime 的 negotiation 子集：没有 endpoint facet/direction/QoS/permission 产品
schema、真实 config/probe/hotplug adapter、typed dispatcher、Host orchestration/SDK、MediaStream、
target service 或 K230D Runtime evidence。

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

## K230D Linux four-kind lab

2026-08-16 的一次受控实验把同一 Session Core、POSIX connected-stream adapter 和固定
[`four-kind-lab-v0`](../profiles/four-kind-lab-v0/README.md) fixture 运行在 macOS Host 与
K230D 大核 Linux Edge 之间。Edge ELF 为 96104 bytes，SHA-256
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
- product Runtime 的 endpoint facet/direction/QoS/permission schema、adapter-driven
  config/probe/hotplug、Host orchestration 与 typed dispatcher；
- 产品 direct-IP bootstrap/config ownership、安全 transport、time estimator 与
  reconnect/resume 参数；
- capability/endpoint discovery、真实 platform adapter 与 Runtime target evidence；
- MediaStream 与 Resource lifecycle/telemetry 的公共集成；
- Composition API、prepared generation 与真实 presentation endpoint；
- 任一完整、声明并验证过的产品 Profile；
- K230D 自动化 cold-boot/restart evidence、CI、fuzz、并发、板端资源预算和长期
  fault/soak/thermal evidence。

这些 gap 是无序能力集合。实际下一项由当前产品问题、可用硬件、风险和可执行验收选择，
不在本报告中固化开发路线。
