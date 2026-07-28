# SDK 设计与开发指南

> 状态：当前实现指南。稳定语义以[系统设计](system.md)、[链路契约](link.md)和[Composition 契约](composition.md)为准。

## SDK 只解决什么

SDK 连接两个公开角色：

```text
Host 应用与重计算
  <-> Object / MediaStream / Composition
  <-> Link adapter
  <-> Edge portable core
  <-> camera / IMU / codec / display platform adapter
```

当前只建设有实验证据的最小闭环。SDK 不负责制作或刷写系统镜像，也不把 Linux 进程称为 RTOS 实现。Host、Edge 是协议角色，不是 A53、M4、操作系统或进程名称。

## 当前模块

| 模块 | 责任 | 当前状态 |
|---|---|---|
| `meiso_slice` | frame identity、姿态修正、overlay 安全门和进程内 fixture | 已实现并可在 Windows 复现 |
| `meiso_latest_state` | 一槽 `LatestState(b)` 的替换、pending、ACK、幂等与 session 重发 | 本轮先定义契约，随后实现 |
| Link adapter | 把 Object 与 MediaStream 映射到 session 和载体 | 仅有私有实验候选，公共 wire 未定 |
| Edge platform adapter | 接入 camera、IMU、codec、display 和 monotonic clock | i.MX8MM 有代理实验；K230D 未实现 |
| Host integration | detector、历史存储和 Dashboard | 有 Orin 实验，不属于 portable core |

portable core 使用 C11、调用方提供的固定容量内存和显式状态码，不依赖 heap、socket、线程、Linux 或 RTOS。平台层负责并发、驱动、硬件 buffer 和实际时间源。

## 数据怎么选

| 需求 | 使用形式 |
|---|---|
| 过期即可丢弃的 IMU、遥测、deadline-bound BBox 或结果观测 | `Observation(a)` |
| 开关、采集参数、设备实际状态、稳定显示意图 | `LatestState(b)` |
| 拍照、标记时间线等只执行一次且有顺序的动作 | `OrderedEvent(c)` |
| 有限、不可变、必须完整校验的资源或按需快照 | `Bulk(d)` |
| 连续 camera、音频或投影帧 | `MediaStream` |

不要为了覆盖四个别名虚构业务。当前驻留闭环使用 `a`、`b` 与 MediaStream；`c`、`d` 只在出现真实 one-shot 或 immutable payload 时进入业务路径。

## 当前对象与所有权

- Host 拥有完整 `DesiredEdgeConfig(b)`；Dashboard 只能发布新的完整状态，不能直接写 Edge 设备。
- Edge ACK 只表示协议接收。Edge 另行发布 `EdgeState(b)`，包含期望版本、实际应用版本、能力、实际状态和有界错误。
- Edge 拥有 `ImuObservation(a)` 与连续 camera MediaStream。所有样本携带 Edge monotonic timestamp、clock epoch 和 stream identity。
- Host 的 BBox 必须引用精确源 frame。过期 BBox 是 `Observation(a)`；需要持续保持的 UI 或 Presentation plan 是 `LatestState(b)`，二者不能混用。
- Edge 在实际显示评估后可以发送一次无 ACK 的 `OverlayResult(a)`。它只用于诊断和历史关联，不能成为 scanout 前置条件。

## `LatestState(b)` portable core 验收

本轮核心只实现状态投递语义，不实现 socket 或 wire：

1. publisher 和 replica 都使用调用方提供的固定容量 byte storage，不分配内存。
2. version 从 1 开始并严格递增。更新版本替换尚未确认的旧版本；同版本同内容幂等，同版本不同内容是冲突；回绕必须显式失败。
3. 首次发送立即可取；未收到精确 ACK 时始终保持 pending，Link adapter 按自己的有界 interval 重取并发送。精确 ACK 清除 pending，旧 ACK 或未来 ACK 不得错误完成当前版本。portable core 不拥有时钟或调度器。
4. 新 session 使 replica 的旧副本失效，并让 publisher 重新发布当前 truth。session identity、持久化和安全传输仍由上层 Link adapter 负责。
5. replica 只有完整复制后才接受新版本。重复的同版本同内容需要再次 ACK，但不得重复触发业务副作用。
6. ACK 不代表设备已应用。对象 adapter 必须通过 Edge-owned actual state 表达成功、部分支持或失败。

## 实时与平台边界

- capture、Edge-local scanout、codec/carrier、IMU 和 Link control 是独立的有界消费者；网络或模型失败不能卡住本地显示。
- 连续媒体使用 latest-wins 的有界 broker，生产者不等待 per-frame ACK。丢帧、覆盖、发送失败和恢复点必须计数。
- `FrameKey` 至少区分 clock epoch、stream instance 和 frame sequence。Host 本地帧号不能替代 Edge identity。
- Edge 保留固定上限的 IMU 时间环。缺少正确 epoch、姿态、标定或时效时，新的 Host overlay 必须明确隐藏。
- “已应用”必须由实际进展证明，例如首个真实 camera frame、实际采样或实际 scanout；驱动调用返回成功本身不够。
- Host 历史是非阻塞 consumer。SQLite、文件布局和 Dashboard 不进入 portable core ABI；存储必须报告 `ACTIVE`、`BOUNDED_STOP` 或 `FAILED`。

## 验证 profile

| profile | 用途 | 必须保留的真实语义 |
|---|---|---|
| `Host-only` | Edge 硬件不在场时开发 Host | Object 代际、session gate、frame identity、时间和失败行为 |
| `Edge-only` | Host 不在场时开发驱动、姿态与显示 | 同一 Host intent/overlay、lease、安全降级和显示结果 |
| `Integration` | 双方和载体一起运行 | 上述语义加真实吞吐、时延、资源、断线与恢复证据 |

i.MX8MM 是 K230D 到板前的 Edge 验证代理。它能验证 contract 和 adapter 形状，不能替代 K230D 的目标构建、driver、内存、codec、render 或性能证据。

## 工作顺序

每个行为变更只回答一个问题，并按以下顺序完成：

1. 记录观测基线和真实问题。
2. 先修改最小权威规范与可执行验收。
3. 实现最小变化；不顺带建设无当前需求的平台服务。
4. 运行 build、正向与失败测试，检查实际输出。
5. 把结果写入[系统进展与待决策](system-report.md)，再提交该工作单元。

下一顺序是：完成公共 `LatestState(b)` core；再建立最小 LinkSession identity/time 接口与 MediaStream adapter 边界；拿到 K230D 后才实现目标 platform adapter。没有真实业务前不扩建 `c/d` 运行时。
