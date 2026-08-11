# Meiso Glass 文档

## 产品规范

以下四份文档共同构成唯一产品规范；各自只拥有一个问题域：

1. [系统设计](system.md)：角色、Virtual Edge、Core/Profile 与产品不变量；
2. [Resource 契约](resource.md)：State、Stream、Action、Blob、ownership、lifecycle 与 QoS；
3. [Link 契约](link.md)：identity、Session、time、delivery、failure 与 transport-independent
   boundary；
4. [Composition 契约](composition.md)：layer、generation、激活和 Edge final authority。

不存在第二份“总体唯一规范”。overview、实现指南、报告、源码注释或 Git 历史与上述规范
冲突时，必须显式修正规范或把其降级为 evidence，不能让两套定义并存。

## 实现与证据

- [SDK 设计与开发指南](sdk-guide.md)：实现边界、adapter pattern、conformance 与工作单元；
- [系统进展](system-report.md)：当前代码成熟度、已脱敏实验事实、K230D lab evidence 与
  未验证能力。

规范说明必须成立的行为；指南说明如何实现和验证；报告只陈述特定版本、平台、条件和
时间窗口下的 evidence。组件、frontend、runtime、system image 和开发顺序由 capability、
Profile 与当前工作问题决定，不进入 Core。
