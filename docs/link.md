# 链路契约

> 状态：产品级技术契约。当前实现范围见[系统进展与待决策](system-report.md)。

## 分层

```text
Application
  -> Composition / Object / MediaStream
  -> LinkSession
  -> Carrier
```

- `Application` 与 `Composition` 使用 Object 和 MediaStream，不感知 socket、port 或 carrier。
- `LinkSession` 负责身份、生命周期、投递、资源控制和时间估计。
- `Carrier` 提供有界的数据传递，可以是 UDP/IP、未来 raw adapter 或有线链路。

`a/b/c/d` 是投递语义，不是四条连接。多个 Object 可以共享 carrier packet；应用不得自行创建 per-application connection。

## Object

每个 Object 只有一个 truth owner；它是一种同步或消息形式，不要求对应 heap object，也不要求发送端长期保存。

| 别名 | 名称 | 语义 |
|---|---|---|
| `a` | `Observation` | 无 ACK、允许丢失；只用于过期即可丢弃的采样或遥测 |
| `b` | `LatestState` | 有 ACK；新版本取代待发送旧版本，最终状态不能静默丢失 |
| `c` | `OrderedEvent` | 有 ACK、保持顺序；session 内已接受的事件不得静默丢失 |
| `d` | `Bulk` | 有 ACK、允许分块；整体只能完整交付或明确失败、取消 |

稳定设备状态使用 `b`，不能因为更新频繁而误用 `a`。大型不可变 payload 使用 `d` 完整交付并按 content hash 校验，`b` 再引用该资源；不能把 `b` 直接改成 `d` 而丢失 LatestState 语义。

## MediaStream

MediaStream 承载连续、带 deadline 的 media sample 或 frame，payload 可以是 raw 或 encoded。过期值和失去依赖的值应丢弃，不追求每个值最终送达。

stream 配置与状态使用 `b`，初始化资源使用 `d`。每条流有独立 `stream_epoch` 和 sequence；新 epoch、decoder 丢失参考或重连恢复时从所需格式配置与 recovery point 开始。

MediaStream 有自己的 deadline-aware 调度与 backpressure，不进入 `d` 的完整交付队列。

## 有界资源

所有 packet、queue、重组、重试和去重窗口必须有界。Object plane 拥塞时的保留与准入偏好为：

```text
c > b > a > d
```

- `b` 可以用新版本替换尚未发送的旧版本。
- `a` 可以在过期或拥塞时丢弃。
- `d` 可以暂停，但不能阻塞更高优先级 Object。
- `c` 无法继续接收时必须报告 backpressure、失败或 session fault。

该顺序不是完整调度算法，也不包含 MediaStream。实际调度必须同时保护控制进展、媒体 deadline 与最低公平性，参数由测量后的 adapter profile 定义。

## DeliveryHandle

完整 Link SDK 发送 `b/c/d` 时，调用者应能观察终态：已确认、被新版本取代、失败、取消或 `Indeterminate`；当前 C11 slice 尚未实现完整 DeliveryHandle。

ACK 只表示对端协议层接受了数据，不表示设备已经执行。业务结果由其 truth owner 另行发布状态。`Failed` 表示确定未交付；断线后无法判断事件是否已产生效果时必须返回 `Indeterminate`，不能伪装成失败后自动重试。

`a` 和普通媒体帧默认不创建需要等待的 DeliveryHandle。

## LinkSession

LinkSession 统一拥有 carrier，并负责：

- peer 发现、版本协商、session identity、liveness 与关闭。
- 短暂中断恢复和新 session 建立。
- ACK、去重、重试、排序、分片、流控、pacing 与 backpressure。
- clock offset 与 uncertainty 的持续估计。

产品实现必须使用经过审查的标准安全传输或密码库提供认证、加密和重放防护，不自造密码算法。具体库和 wire 尚未选定。

只有 adapter 能证明双方仍处于同一 session 且保留有界恢复状态时才允许恢复；不能证明时建立新 session。恢复身份未确认前不得发送可能重复产生副作用的 `c`。

## 中断与重连

| 形式 | 同一 session 恢复 | 新 session 或 peer 重启 |
|---|---|---|
| `a` | 不补发过期值 | 丢弃 |
| `b` | 重发当前版本或以更新版本取代 | owner 重新发布当前 truth |
| `c` | 以相同 sequence 重发，接收端去重并保持顺序 | 未确认结果为 `Indeterminate`，不得自动重放 |
| `d` | 按已确认 chunk 继续 | 旧 handle 按证据进入 `Failed` 或 `Indeterminate`；新 transfer 默认重启，只有持久化 manifest、hash 与进度才可续传 |
| MediaStream | 丢弃过期值，必要时请求 recovery point | 新 stream epoch，从 recovery point 开始 |

LinkSession 不承诺跨崩溃的通用 exactly-once。需要该语义的业务必须提供持久化 idempotency key、结果记录或事务日志。

新 session 建立后，owner 重新发布仍有效的 `b`。旧 Composition `prepared_token` 与状态立即失效；只有同一 Edge incarnation 与 monotonic epoch 中仍驻留内存的 active generation，才可按先前明确授予的 offline lease 运行到本地 deadline，且不能自动成为新 session 的授权。高功耗设备默认保持安全状态。

## 时间

实时数据携带 sender monotonic timestamp 与 clock epoch。同步精度未知时，上层不得把两端时钟当成同一个时间轴。

Heartbeat 可以复用时钟观测，但这里不规定 NTP 或具体格式。跨端分析必须记录 offset uncertainty。

## Carrier

reference carrier 使用标准 Wi-Fi association 与同链路 IP/UDP：可以采用固定或 link-local 地址与固定内部端点，无需依赖默认网关或互联网路由。DNS、NAT 和多应用端口管理都不是前置条件；UDP 端点也不属于公开应用接口。

raw Wi-Fi 只能作为可选 carrier adapter。只有两端硬件、驱动和固件可控，且对照测量证明它显著改善目标指标时才引入。更换 carrier 不得改变 Object、MediaStream、session 或 Composition 语义。

## Wire 约束

wire 必须有界、可版本化，在读取 payload 前检查长度，在分发前验证完整性，并携带满足 session、去重、乱序和丢失判断的信息。

当前不规定 header、field offset、endianness、MTU、重传窗口或 byte layout。这些内容依据目标 carrier 与接收端资源测量后再写。

## 验收条件

一个 Link 实现合格需要证明：

- 替换进程内、UDP/IP 或未来 raw carrier 时，上层接口与语义不变。
- 持续过载和断线恢复期间内存保持有界。
- `b` 最终收敛或明确失败；`c` 不静默缺失；`d` 完整完成或明确终止。
- `a` 与过期媒体丢失不阻塞后续数据。
- session、重启、投递不确定性和时钟 uncertainty 对上层可见。
