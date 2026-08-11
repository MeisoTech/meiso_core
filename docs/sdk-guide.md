# SDK 设计与开发指南

> 状态：实现与验证指南。稳定语义以[系统设计](system.md)、[Resource 契约](resource.md)、
> [Link 契约](link.md)和[Composition 契约](composition.md)为准。

## SDK 边界

```text
Host application / service / frontend
    ↓
EdgeHandle / Virtual Edge API
    ↓
portable Resource Runtime
    ↓
LinkSession backend interface
    ↓
transport and platform adapters

Edge Resource Runtime
    ↓
capability / policy / endpoint adapters
```

这个分层不要求 daemon、多进程或通用 plugin framework。第一份实现可以在单进程中证明
契约；只有真实 lifecycle、isolation 或 deployment 需求出现时才拆进程。

SDK 不把 socket、packet、driver handle 或 platform buffer 暴露为 Host application 的
业务接口。反过来，Core 也不要求所有 payload 复制进通用 byte container；adapter 可以
提供满足相同 Resource lifecycle 的 zero/low-copy path。

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

## Virtual Edge 实现

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
