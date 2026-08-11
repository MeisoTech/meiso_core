# Resource 契约

> 状态：产品级技术契约。定义 `State / Stream / Action / Blob` 的稳定语义；不定义 wire、
> C ABI、字段 offset、transport 或具体 sensor 清单。

## 共同规则

Resource 是 Host 与 Edge 之间可发现、可授权、可观察的业务事实或交互端点。每个
Resource descriptor 至少在概念上声明：

- 稳定的 resource identity、kind 与 schema/version；
- truth owner、允许的读写方向与 lifecycle scope；
- capability、权限与当前可用状态；
- bounded size、queue/history/retry 行为和 overflow policy；
- 适用的 freshness、deadline、delivery 或 integrity 要求。

descriptor 是契约信息，不要求所有 payload 都进入同一个复制器或内存路径。大媒体帧
可以走 codec/buffer 专用数据面；它仍共享同一 Resource identity、Session、时间、能力、
lifecycle 和 telemetry。

每个事实只有一个 truth owner。desired 与 actual 是两个不同 Resource 或两个明确区分的
事实方向；Host desired 的本地提交、Link 接受和 Edge actual application 不能合并成一个
布尔值。

## State

State 表示“当前事实”。一次更新提交完整、不可变的 snapshot；新 version 取代旧 version，
调用者不依赖中间变化全部可见。

State 的稳定规则：

- owner 在同一 `(resource identity, owner incarnation)` 内产生单调、不可混淆的 version；
  跨 owner incarnation 的 version 不直接比较，同一作用域内同 version 同内容是幂等、
  不同内容是冲突；
- replica 只能在完整验证 snapshot 后原子替换当前副本；
- QoS/Profile 明确 State 是需要最终收敛，还是允许 best-effort latest；
- 需要收敛的 State 在资源不足或 session 结束时必须暴露失败，不能静默丢弃最终 truth；
- transport ACK 只证明协议层接受，不证明对应 desired 已物理应用；actual State 由实际
  truth owner 发布；
- 大型 State 可以引用 Blob，但不能因为 payload 大就改变 State 的当前事实语义。

常见模式是 Host-owned desired State 与 Edge-owned actual State 配对。这个模式不是全局
方向规则；Host world model、Edge capability 或双方 telemetry 都可以按各自 truth owner
声明独立 State。

## Stream

Stream 表示追加的时序 sample 或已经发生的 event。每个 sample 属于明确的 stream epoch，
并携带足以判断顺序、来源时间、freshness 与依赖关系的 identity。

Stream 的稳定规则：

- sequence 只在自己的 stream epoch 内解释；新 producer incarnation 或无法连续恢复时
  建立新 epoch；
- sample 使用 sender monotonic time 与 clock epoch；跨端比较必须同时携带时间估计及
  uncertainty；
- QoS/Profile 声明 deadline、history、loss、ordering 与 backpressure 行为；
- queue/history 有界，过期或允许丢失的 sample 不得阻塞更新数据；
- 需要可靠到 deadline 的 event 可以报告缺口或失败，但不因此成为 Action；
- button、gesture 和 sensor sample 是已发生事实，属于 Stream，而不是请求系统执行副作用。

## MediaStream

MediaStream 是 Stream 的 codec-aware 专业化，不是独立于 Meiso 的野生通道。它遵守
Stream 的 identity、epoch、timestamp、deadline、bounded backpressure 与 lifecycle，并
额外声明 codec/raw format、dimensions、dependency/recovery information 和关联的
Composition source。

MediaStream 必须注册在同一 Resource Registry 中，共享 Session、capability、权限、时钟、
调度预算与 telemetry。编码、packetization、hardware buffer、recovery point 和 transport
arrangement 由 Profile/adapter 决定；这不要求媒体 payload 经过通用 State/Blob 复制路径。

连续 camera、audio 或 projection frame 是 MediaStream。它们不能因为“大”就变成 Blob，
也不能要求每帧最终送达。

## Action

Action 表示请求另一端执行有副作用的操作。它不是 State 更新，也不是已经发生的输入
event。

第一版稳定语义只要求：

- action identity、target、operation、arguments 与可选 deadline；
- operation 声明幂等策略或明确不可安全重试；
- 接收方按 action identity 去重；
- 调用者可观察 `accepted / succeeded / failed / rejected / expired / indeterminate` 中的
  明确状态；
- transport ACK 不能冒充 Action result；断线后无法证明副作用是否发生时必须返回
  `indeterminate`，不能伪装为失败后自动重试。

Core 不提供通用 DAG、分布式事务、全局顺序、持久 exactly-once 或工作流引擎。只有真实
operation 和可执行验收证明需要时，Profile 才增加 ordering、progress、cancel 或依赖。

## Blob

Blob 表示有限、不可变、按 content identity 标识的完整内容。只有全部内容通过长度与
integrity 验证后，接收方才原子发布 Blob；部分内容不能被上层当作有效资源。

分块、重试、断点续传、缓存与持久化是 delivery/Profile 行为，不是新的顶层 Resource。
资源不足、取消或 session 结束时必须得到明确终态。State 或 Composition 可以按 content
identity 引用 Blob；缓存命中不代表当前授权或当前 State。

## Sensor 与 Endpoint 模式

Sensor 不是第五种 Resource。runtime 通过 capability/endpoint discovery 声明实际存在的
sensor、input、audio、media、display、codec 或其他 endpoint。一个具体 endpoint 通常由：

- configuration/desired 与 actual status 的 State；
- sample/event 的 Stream 或 MediaStream；
- 仅在确有副作用调用时出现的 Action；
- 仅在确有不可变资源时出现的 Blob。

camera、IMU、microphone 只是 endpoint 例子。型号、format、sample rate、axis、calibration、
clock relationship 和 platform handle 属于 descriptor、adapter 或 Profile，不进入 Core，
也不产生固定的实现顺序。

## 验收条件

Resource Runtime 合格需要证明：

- 每个声明支持的 Resource kind，在每个已实现的 local、simulated、recorded 或 remote
  backend 中都使用相同业务语义与适用的 conformance vector；本规范不要求一个实现同时
  提供全部 Resource kind 或 backend；
- owner、version/epoch、freshness、终态和实际应用状态不会混淆；
- 所有 storage、queue、history、retry 和 cache 在过载时仍有界、可观察；
- session 中断、peer 重启、过期、重复、冲突和资源耗尽都有明确结果；
- MediaStream 使用统一 Resource lifecycle，但不被迫复制进不合适的通用 payload 路径；
- capability 不存在时显式报告 unavailable/rejected，不生成假数据或静默降级。
