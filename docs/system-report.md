# 系统进展

> 日期：2026-08-13
> 状态：公开、已脱敏的 evidence report；不是产品规范，不包含原始日志、地址、凭据、
> 唯一设备标识或本机路径。

## 架构重基线

产品规范现已直接使用 `State / Stream / Action / Blob`、Virtual Edge 和 transport-
independent Session boundary。旧公共 API、旧私有 wire 和旧术语没有兼容承诺。新的
portable Resource semantic core、legacy fixture 与板级实验各自只有下文明确列出的 evidence
范围，不能互相替代。

规范入口是 [docs/index.md](index.md)。本报告不规定 platform、runtime、sensor、frontend、
transport 或实现顺序。

## 当前公共代码

截至 payload source 提交 `add9b8ac49a86cb0fb10b8f8cff7cbb8f3739b27`，
`meiso_resource_core` 已实现四种 primitive、共同值类型、
descriptor validation 与 fixed immutable descriptor snapshot。下表中的“已证明”只表示当前
portable in-process 实现通过了对应 Host fixture，不表示 remote、target 或产品 Runtime 已经
成立。

| 内容 | 当前 Host evidence | 尚未证明 |
|---|---|---|
| common / descriptor set | typed identity、SHA-256 ContentId 形状、descriptor 校验；fixed-capacity register/seal/value-copy lookup、duplicate/conflict/capacity | capability registry lifecycle、generation/permission/schema negotiation、wire 或 runtime dispatch |
| State | bounded publisher/replica、完整 snapshot、owner/version、duplicate/conflict/stale/wrap、best-effort/convergent、delivery generation 与 ACK/actual 分离 | Session adapter、真实物理 application、remote backend、线程安全或持久化 |
| Stream | `STRICT_MONOTONIC`、producer/epoch/sequence、bounded queue/history、gap/stale/conflict、drop/reject 与 deadline admission | reorder/reliable Profile、clock estimator、MediaStream external-buffer/codec/recovery 或 remote delivery |
| Action | bounded caller/executor、ActionId dedup/conflict、delivery ACK/executor acceptance/terminal result 分离、deadline、indeterminate 与 no eviction | 真实 operation、persistent dedup/exactly-once、Session 自动恢复、cancel/progress/DAG |
| Blob | bounded staging、callback verification、完整后原子 publish、duplicate/conflict/capacity/abort、zero length 与 reentrancy guard | Core 内置 hash、chunk/resume、persistence、eviction、授权或 Session transfer |
| four-kind fixture | 同一 descriptor snapshot 下四类真实 public contract 的独立初始化、正交状态以及逐类容量失败隔离 | Resource Runtime、generic dispatcher、LinkSession、remote backend 或产品 Profile |

该提交在 AppleClang 17、CMake 4.2、Ninja 1.13 的 clean out-of-tree Debug、Release 与
ASan/UBSan 配置下完成 warnings-as-errors build 和 CTest，并通过 AppleClang 静态分析与
portable-library forbidden-symbol 审计。测试 inventory 以该提交的
`ctest --test-dir <build> -N` 与实际 `ctest --output-on-failure` 输出为准；本报告不把测试总数
固化为长期事实。Core fixture 与下面的 legacy fixture 必须分开解释。

## Legacy evidence

以下模块早于当前 Resource 规范，继续保留为可执行 behavior/workload evidence。其文件名、
C symbol、slot layout 和 ABI 不是新的 Resource semantic core，也没有兼容承诺。

| 内容 | 已证明 | 未证明 |
|---|---|---|
| `meiso_latest_state` | 固定容量完整 snapshot、newest-wins、pending、精确 ACK、duplicate/conflict、version wrap gate 与新 session 重发 | 新 State ABI、Resource Runtime、Session identity、wire、scheduler、concurrency、persistence 或 actual application |
| `meiso_slice` | 确定性 frame identity、pose/freshness/uncertainty gate、lease、session isolation 与 fail-closed workload | Virtual Edge、真实 MediaStream、platform adapter 或产品 Composition |
| synthetic demo | 固定 camera-like frame、模拟姿态/检测与 PPM 输出 | 真实 camera、sensor、codec、network、display 或 target performance |

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

Resource semantic core 的 target build 状态为 `HOST_VERIFIED_BOARD_UNVERIFIED`，K230D
conformance execution 状态仍为 `NOT_RUN`。同一七套 Host 测试源码已经在 exact CanMV v1.8
20-project graph 上 fresh cross-build；生成的 325472-byte static RISC-V ELF 经 host inspection，
并从 fresh full-media `.img` 的 `/sdcard/app/meiso_conformance` 只读抽取后逐字节复核。完整镜像
中的 payload SHA-256 为
`04b085c813f85414ce0f8284f54a8e24fe1649e113429a0df50273edb1f23e91`。

该 target application 在 warnings-as-errors 下有零 Meiso diagnostic；完整 pinned SDK fresh
build 的 1421 条上游/toolchain warnings 已单独计数和审阅。生成的 `.kdimg` 分区表只有 `rtt` 与 `rtapp`，不含
SDCARD FAT 或 Meiso ELF，因此是 `NO_MEISO_PAYLOAD / NOT_A_MEISO_OTA_CANDIDATE`。Host 生成、
ELF inspection、镜像内 payload equality 或 USB 枚举都不能晋升 `NOT_RUN`；以下既有启动证据
也仍只属于官方 runtime recovery baseline。

一块 `CanMV-K230D-Zero V1.1` 已使用
[官方 CanMV v1.8 Zero release](https://github.com/kendryte/canmv_k230/releases/tag/v1.8)
从 TF/SD 完整启动到 CanMV REPL；启动链实证 U-Boot SPL、DDR training、OpenSBI、
RT-Smart/CanMV 与 nncase runtime。对应
[Zero 官方规格](https://www.kendryte.com/k230_canmv/en/main/userguide/boards/canmv_k230d.html)
声明 128 MiB LPDDR4，但本次启动并未测得可供 Meiso 使用的容量或预算。这个结果只证明
准确镜像实例能够启动。

当前公开 evidence 尚未证明该板实例的 camera module、display panel、touch、microphone、
IMU、codec/KPU workload、network data path、clock quality、usable memory budget、持续温度、
功耗或任何产品 Profile。firmware 中包含 driver/API 也不等于物理 endpoint 存在。

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
- application-facing Resource Runtime、capability/endpoint registry lifecycle 与 backend
  orchestration；
- 生产 LinkSession handshake、安全 transport、公共 wire、time/reconnect 参数；
- capability/endpoint discovery 和真实 platform adapter；
- MediaStream 与 Resource lifecycle/telemetry 的公共集成；
- Composition API、prepared generation 与真实 presentation endpoint；
- 任一完整、声明并验证过的产品 Profile；
- K230D conformance execution、CI、fuzz、并发、板端资源预算和长期 fault/soak evidence。

这些 gap 是无序能力集合。实际下一项由当前产品问题、可用硬件、风险和可执行验收选择，
不在本报告中固化开发路线。
