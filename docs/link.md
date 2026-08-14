# Link 契约

> 状态：产品级技术契约。定义 identity、Session、time、delivery、failure 与 transport-
> independent boundary；Resource 业务语义见 [Resource 契约](resource.md)。

## 分层

```text
Application / Composition
  -> Virtual Edge / Resource Runtime
  -> LinkSession
  -> Profile-selected transport implementation
```

Application 不感知 socket、port、packet、MTU 或 transport topology。Resource Runtime
解释 State/Stream/Action/Blob；LinkSession 在 peer 之间维护可验证的生命周期、时间与交付
边界；transport implementation 只负责当前 Profile 所需的数据移动。

## LinkSession

LinkSession 的公共责任是：

- peer、protocol/version、session identity、liveness 与关闭；
- capability 与 Resource Registry snapshot 的协商/可见性；
- delivery acceptance、去重、flow control、backpressure 与失败状态；
- clock offset、drift/uncertainty 的持续估计；
- 短暂中断恢复的证据，或无法证明连续性时建立新 Session；
- 所有 queue、retry、reassembly 和恢复状态的边界与 telemetry。

Session ready 不表示每个 Resource endpoint 都 running。sensor、Stream、decoder、presentation
或其他 endpoint 有自己的 State/lifecycle；它们通过 capability 和 Resource 状态表达。

## Transport 边界

LinkSession 的公开 identity、time、Resource delivery 和 failure 语义不依赖具体 transport。
Profile 选择 transport arrangement；本规范不规定其类型、数量、是否并行、是否可切换或
是否迁移。

当前 Core 不公开 binding cardinality，也不承诺通用 transport-topology scheduler。实现可以
从满足当前 Profile 的最小 transport adapter 开始。未来媒体、功耗或物理链路需求可以扩展
adapter，但不得改变 Resource 语义。

## Identity

以下 identity 必须明确区分：

- peer/device identity：谁参与通信；
- boot/incarnation identity：哪一次 peer 运行；
- Session identity：哪一次双方已协商的关系；
- Resource identity 与 State version；
- stream epoch 与 sample sequence；
- action identity；
- Blob content identity；
- Composition capability/plan generation。

一个字段不能代替另一层 identity。Host 本地帧号不能替代 Edge stream identity；连接地址
不能替代 peer/session identity；transport 重连也不能自动证明仍是原 Session。

一个 peer 内部可以有多个 producer clock/runtime。内部 producer 重启时，它的 producer
incarnation 必须更新，受影响的 Stream 使用新 stream epoch；只有 source clock continuity
无法证明时才使用新 clock epoch。只要对外 peer/boot 与有界 Session 状态的连续性仍可
证明，不必仅因内部 runtime 重启而新建 LinkSession。peer Resource Runtime 重启、整机
重启或无法证明连续性时，必须使用新 boot/incarnation 并建立新 Session。

## 建立与恢复

一个实现的 handshake 至少要在使用业务 Resource 前完成：

1. 发现或选择候选 peer；发现不等于可信 Session；
2. 建立 Profile 要求的认证和安全 transport；
3. 协商 protocol/schema、peer/boot identity 与资源上限；
4. 判定恢复已有 Session 还是创建新 Session；
5. 建立 clock estimate 与 uncertainty；
6. 交换 capability 与 Resource Registry snapshot；
7. 协调 owner 当前 State，并分别启动选中的 endpoint。

只有实现能证明双方仍处于同一 Session，并仍保留所需的有界恢复状态时，才允许 Session
resume；不能证明时必须创建新 Session。实现不必采用上述编号作为 wire message，也不要求
所有 endpoint 按某个固定顺序启动。

当一个 Edge 的不同 network interface 提供同一公开 peer 时，它们是 transport 选择，
不是不同 Edge。未定义 transport migration/resume 的实现在切换 interface 时建立新
Session，不得用相同 device identity 伪装旧 Session 已连续恢复。

## Resource 交付

| Resource | Session 内行为 | 中断/新 Session 行为 |
|---|---|---|
| State | 按声明的 QoS 收敛到当前 version，或允许 best-effort latest；完整 snapshot 原子替换 | 同 Session 可重发当前 version；新 Session 由 owner 重新发布仍有效 truth |
| Stream | 按 epoch/sequence/deadline/history 处理；过期或允许丢失 sample 不阻塞后续数据 | 丢弃过期数据；无法连续恢复时建立新 stream epoch |
| Action | 按 action identity 去重；transport acceptance 与 Action result 分开 | 无法证明副作用结果时返回 `indeterminate`；新 Session 默认不自动重放 |
| Blob | 按 content identity 完整校验后原子发布 | 同 Session 是否续传由 Profile 决定；新 Session 默认重新建立 transfer，除非有明确持久 manifest |
| MediaStream | 使用 Stream lifecycle，并按 codec dependency/recovery 信息恢复 | 丢弃过期 frame；必要时从新 epoch 或 recovery point 继续 |

LinkSession 不承诺跨崩溃的通用 exactly-once。需要强幂等的 Action 必须由 operation 提供
持久 idempotency/result 设计，而不是由 Link 猜测。

## ACK、结果与不确定性

transport ACK 或 Link delivery acceptance 只表示对端协议层已经接受相应数据，不表示：

- desired State 已经应用到设备；
- Action 已经成功执行；
- frame 已经 scanout；
- Blob 已被业务激活。

这些结果由 truth owner 的 actual State、Action result 或 Composition status 表达。断线后
若无法确定副作用是否已经发生，结果必须为 `indeterminate`；不能伪装成 `failed` 后自动
重试。

## 时间

实时 Resource 使用 producer/source monotonic timestamp 与 source clock epoch。relay 必须
保留 source time，并将 relay/receive time 分开表达。每个需要跨时钟比较的公开 source
clock 都必须提供到协商时间轴的 mapping 与 uncertainty；缺少映射、连续性未知或精度超出
Profile 预算时，上层不能把这些时钟当成同一时间轴。

liveness 交换可以复用时钟观测，但本规范不规定 NTP、PTP、message layout 或 clock
filter。peer 重启、source clock epoch 变化和 stream epoch 变化必须对上层可见。

## 有界资源与调度

所有 packet、queue、history、retry、reassembly、dedup 和 cache 有固定或协商上限，
并暴露 overflow/drop/failure 计数。调度必须保护控制进展和 deadline，同时避免一个 Blob、
Stream 或 application 无限占用资源。

Core 不规定跨所有 Profile 的统一优先级表。每个 Profile 只声明真实需要并能测试的 QoS、
admission、fairness、drop 和 backpressure 行为；资源不足时不得静默破坏 Resource 契约。

## 安全

产品实现必须使用经过审查的标准协议或密码库提供 peer authentication、confidentiality、
integrity 与 replay protection，不自造密码算法。

安全 transport 建立不自动授予所有 Resource 权限。capability/permission、Session 与
application policy 共同决定可见和可操作 Resource；本版不预先建设通用多租户权限系统。

## Wire 约束

wire 必须有界、可版本化，在读取 payload 前检查长度，在分发前验证完整性，并携带满足
Session、Resource、去重、乱序、丢失和时间判断的信息。

当前不规定 header、field offset、endianness、MTU、packet aggregation、retry window 或
byte layout。它们只能在目标 Profile、接收端资源与 failure vector 明确后进入版本化 wire
规范。

## 验收条件

一个 Link 实现合格需要证明：

- 更换 transport implementation 时，Virtual Edge 与 Resource 业务语义不变；
- Session resume 与新 Session 不混淆，peer reboot 不静默继承旧 authority；
- State、Stream、Action、Blob 和 MediaStream 的终态/恢复符合本页表格；
- ACK、actual application、Action result 和 Composition activation 可独立观察；
- 时钟 uncertainty、duplicate、reorder、loss、disconnect 和 resource exhaustion 可见；
- 持续过载与恢复期间内存、queue、retry、history 和 cache 始终有界。
