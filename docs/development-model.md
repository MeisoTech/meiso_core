# Host/Edge 并行开发模型

> 状态：当前实施与协作指南。产品语义仍只由
> [系统](system.md)、[Resource](resource.md)、[Runtime](runtime.md)、[Link](link.md) 和
> [Composition](composition.md) 规范定义。

## 目标

Edge 开发和 Host 开发应能并行，不以某块板、某条网络或某个未完成的驱动为
前置条件。Host 开发者面对 Virtual Edge 和 Resource/Composition 契约；Edge 开发者
负责将真实设备、本地 policy 与同一契约对接。

```text
Host application
       ↓
Host SDK / Virtual Edge
       ↓
Host Runtime                         Edge Runtime / adapters
       ↓                                  ↑
Host Core ──── Session/wire contract ──── Edge Core
       ↑                                  ↑
macOS Host harness                    K230D Linux target
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
Virtual Edge 是 Host SDK 契约，不是跨端 wire；跨端稳定边界是 Core Session/wire
与 Runtime Resource 契约。

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

1. **Session milestone**：macOS Host harness 和 Linux Edge 实现同一最小 Core Session
   契约，只交换 identity、protocol/Profile、limits 和 Session state；
2. **Runtime slice**：Core `READY` 后由 Edge 发布完整 catalog，Host 接受后提交
   desired/subscription，Edge 发布 actual 并只对 `ACTIVE` route 开放 Resource data；
   在隔离的 conformance/test namespace 中验证 catalog update、参数 reject、断线和资源耗尽；
   synthetic fixture 不声明任何 physical capability；
3. **Sensor slice**：先用 recorded/synthetic endpoint bundle，再替换一个真实 sensor
   adapter；Host 仍只见 catalog/desired/actual/data，不见板级配置或 driver；
4. **Rendering slice**：先用 synthetic input 得到 off-screen render result，再接入
   Composition activation 和真实 display；
5. **Media slice**：在 lifecycle 成立后引入 MediaStream 数据面，不用大 payload 反向
   定义 Resource/Session；
6. **Profile gate**：对已声明的组合做 perf、fault、recovery、memory 和 thermal 验收。

序号表示当前开发主线，不是 Core 强制的组件顺序。每一 slice 必须同时给出可在
macOS 重现的 Host evidence 与它真正要声明的 Edge evidence。一个 milestone 可以拆成
多个独立提交和验收的小工作单元。

## 当前 Runtime 切片

Session Core 已有 Host semantic/wire evidence；POSIX adapter 只有指定 macOS loopback
transport evidence。当前主线是 [Runtime negotiation core v0](runtime-core-v0.md)：

1. 冻结 catalog、desired/actual、subscription 与 route gate 的有界 portable 合同；
2. 用同一真实 Session frame 验证 catalog 原子切换、参数请求、actual 与 stale route；
3. 在 K230D Linux Edge 侧把私有配置与 adapter probe 映射成 catalog，不把板级字段泄漏给
   Host；
4. 接入一个真实 endpoint adapter，并让 macOS Host fixture 只依赖相同 catalog/schema；
5. 再验证插拔、catalog update、network loss、新 Session 重协商和资源耗尽。

旧 four-kind lab 是固定顺序 evidence，不是正式 Runtime。TCP、认证、K230D、真实 endpoint
和 SDK 只能按各自实测声明；Host semantic/wire 测试不证明 network、security 或 target。
