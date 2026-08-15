# Meiso Glass 文档

## 产品规范

以下四份文档共同拥有通用产品语义；各自只拥有一个问题域：

1. [系统设计](system.md)：角色、Virtual Edge、Core/Runtime/SDK/Profile 与产品不变量；
2. [Resource 契约](resource.md)：State、Stream、Action、Blob、ownership、lifecycle 与 QoS；
3. [Link 契约](link.md)：identity、Session、time、delivery、failure 与 transport-independent
   boundary；
4. [Composition 契约](composition.md)：layer、generation、激活和 Edge final authority。

被接受且版本化的 Profile/wire 契约可以填充上述规范明确委托的开放参数，但不能覆盖
通用语义。除此之外不存在第二份“总体唯一规范”；overview、实现指南、报告、源码注释或
Git 历史与上述规范冲突时，必须显式修正规范或把其降级为 evidence，不能让两套定义并存。

## 实现与证据

- [SDK 设计与开发指南](sdk-guide.md)：实现边界、adapter pattern、conformance 与工作单元；
- [Resource semantic core v0](resource-core-v0.md)：当前 portable primitives、内存模型与
  conformance 矩阵；这是实现基线，不是第五份产品规范；
- [Session Core v0](session-core-v0.md)：当前 portable handshake、wire、limits、lifecycle
  与 Runtime-message gate；不包含 TCP、安全 transport 或 Resource Registry；
- [系统进展](system-report.md)：当前代码成熟度、已脱敏实验事实、K230D lab evidence 与
  未验证能力。
- [Host/Edge 并行开发模型](development-model.md)：双方如何依据同一契约、simulator
  和 conformance 独立开发与逐片对接。
- [K230D Linux Edge 开发基线](k230d-linux-edge-development-baseline.md)：当前候选实现中
  大小核、Linux、NuttX、sensor、network 与 rendering 的责任分工；它不是互操作 Profile。

## 当前实验入口

- [Four-kind lab v0](../profiles/four-kind-lab-v0/README.md)：固定的 State / Stream / Action /
  Blob 实验契约；不是产品 Profile；
- [K230D Linux lab endpoint](../targets/k230d-linux/README.md)：板端/Host 同源手动 fixture；
- [本地实验面板](../devtools/web/four_kind_dashboard/README.md)：macOS 上的脱敏可视化 Dev Tool，
  不实现 wire，也不属于 SDK。

规范说明必须成立的行为；指南说明如何实现和验证；报告只陈述特定版本、平台、条件和
时间窗口下的 evidence。组件、frontend、runtime、system image 和开发顺序由 capability、
Profile 与当前工作问题决定，不进入 Core。
