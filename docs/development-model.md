# Host/Edge 并行开发模型

> 状态：当前实施与协作指南。产品语义仍只由
> [系统](system.md)、[Resource](resource.md)、[Link](link.md) 和
> [Composition](composition.md) 规范定义。

## 目标

Edge 开发和 Host 开发应能并行，不以某块板、某条网络或某个未完成的驱动为
前置条件。Host 开发者面对 Virtual Edge 和 Resource/Composition 契约；Edge 开发者
负责将真实设备、本地 policy 与同一契约对接。

```text
Host application                  Edge implementation
       |                                  |
       +---- Virtual Edge contract -------+
       |                                  |
 macOS simulator / replay          Linux runtime / adapters
       |                                  |
       +------ shared conformance --------+
```

## 双方稳定边界

Host 开发者可以依赖：

- Host/Edge identity、Session、capability 和 Resource descriptor 的显式边界；
- `State / Stream / Action / Blob` 及 MediaStream/Composition 的公开语义；
- local submit、delivery acceptance、Edge actual 和 physical result 相互独立；
- 有界 queue、deadline、freshness、disconnect 和明确 failure；
- simulated、recorded 与 physical Edge 使用同一业务 API。

Host 业务代码不得依赖 K230D 的 core 分工、Linux/NuttX、driver handle、串口节点、
硬编码地址或某个具体 transport。这些只存在于 Edge adapter、连接配置和 Profile。

## 使 Host 可独立开发的交付物

每一个对外能力在请求 Host 开发者对接前，Edge 方必须同时提供：

1. 指向公开规范的 capability/Resource 说明，不复制第二套语义；
2. 带版本且有界的 schema 或 wire 契约；
3. 可在 macOS 运行的 deterministic simulated Edge；
4. success、loss、duplicate、reorder、disconnect、reboot 和 exhaustion 的固定 fixture；
5. Host 与 Edge 都可运行的 conformance 结果格式；
6. 真实 Edge 的 capability 和 evidence 边界。

在 wire/handshake v0 被接受前，Host 可针对 Virtual Edge 与 simulator 开发，但不应自行
冻结 packet layout 或把临时 socket API 升级为产品契约。

## 责任分工

| Edge 方 | Host 方 | 共同验收 |
|---|---|---|
| 公开契约提案、Edge Runtime、simulator、schema/reference codec、platform adapter、板级 evidence | Host SDK/Virtual Edge facade、应用逻辑、Host 资源管理与 UI/frontend | 共享 fixture 的语义分类一致；capability、时间、性能与 physical evidence 分开记录 |

任何一方要改变公开行为，都先修改最小权威规范和 failure vector，再修改实现。

## 垂直切片迭代

开发不按“先写完 Host，再写完 Edge”推进，而是一次打通一条可运行路径：

1. **Session milestone**：macOS simulator 和 Linux Edge 分别实现同一最小 Session
   契约，交换 identity、capability 和 descriptor snapshot；
2. **Resource slice**：在隔离的 conformance/test namespace 中端到端验证四类
   Resource、断线与资源耗尽；这些 synthetic fixture 不声明任何 physical capability；
3. **Sensor slice**：先用 recorded/synthetic Stream，再替换一个真实 sensor adapter；
4. **Rendering slice**：先用 synthetic input 得到 off-screen render result，再接入
   Composition activation 和真实 display；
5. **Media slice**：在 lifecycle 成立后引入 MediaStream 数据面，不用大 payload 反向
   定义 Resource/Session；
6. **Profile gate**：对已声明的组合做 perf、fault、recovery、memory 和 thermal 验收。

序号表示当前开发主线，不是 Core 强制的组件顺序。每一 slice 必须同时给出可在
macOS 重现的 Host evidence 与它真正要声明的 Edge evidence。一个 milestone 可以拆成
多个独立提交和验收的小工作单元。

## 当前第一切片

当前只选择 `Session milestone`，并拆成三个原子工作单元：

1. **本地契约**：冻结开发 Profile 的 identity/version、最小 handshake/wire、认证与安全
   transport、资源上限、失败码和 golden trace；macOS Host 与 deterministic simulator
   通过同一组向量。认证与安全选择尚未被接受时，只能称为 pre-Link lab transport，不能
   报告 LinkSession conformance；
2. **Linux / Wi-Fi**：Edge 上启动最小 Linux Runtime，只发布隔离 test namespace 中的
   synthetic capability；同一 Host backend 经 Wi-Fi/IP 完成显式连接和 Session snapshot。
   首版人工给出地址，不同时建设 discovery、媒体或真实 sensor；
3. **连续性与故障**：分别验证正常断开、network loss、重连和 Edge reboot；每个结果都有
   明确上限、Session identity 和可重现 fixture。

USB 当前只作串口恢复与调试；USB network 在电气角色和 runtime capability 实证后再用
同一 Link 契约加入。三个工作单元完成前，不声称 Link、sensor、Edge-side Rendering
Runtime 或产品互操作 Profile 已实现。
