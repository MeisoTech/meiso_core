# Meiso Glass 文档

> 状态：`CURRENT ENTRY`。本页只导航，不创建第二套产品语义。

## 产品规范

以下五份文档共同拥有通用产品语义，各自只负责一个问题域：

1. [系统设计](system.md)：Host/Edge角色、Core/Runtime/SDK/Profile、Virtual Edge与不变量；
2. [Resource](resource.md)：`State / Stream / Action / Blob / MediaStream`、ownership与lifecycle；
3. [Runtime](runtime.md)：catalog、endpoint、desired/actual、production gate、route与Blob visibility；
4. [Link](link.md)：identity、Session、time、delivery、failure与transport-independent boundary；
5. [Composition](composition.md)：layer、plan/generation、激活、renderer/present与Edge final authority。

版本化Profile/wire只能选择规范显式留开的参数，不能覆盖它们。报告、实现指南、
测试、源码注释或历史文档与上述规范冲突时，必须修正规范或把其降级为evidence。

## 当前实现

- [系统认知图](system-runtime-map.md)：当前进程/通信拓扑、线程、内存与buffer、状态存放、
  连接和断开生命周期、输出与外设边界；它是非规范工程地图；
- [SDK指南](sdk-guide.md)：caller-owned adapter、opaque `EdgeHandle`、simulator/trace、conformance与工作单元；
- [Resource Core v0](resource-core-v0.md)、[Session Core v0](session-core-v0.md)与
  [Runtime Core v0](runtime-core-v0.md)：portable C11 实现合同；Runtime Core 页中的
  MRT0 v2/v3 仅是内部 compatibility boundary；
- [Host集成回执](host-integration-receipt-v0.md)：`COMPOSE_/_PROFILE:v1`、MRT0 v4、6/18 graph、
  terminal/fresh-Session和Looking Glass adapter边界；
- [Display Ingress 本机测试画面输入](../integrations/host/display_ingress_fixture_input.md)：
  `DIFI/v1` 一次性 Host fixture record、公开 codec、fd 所有权、fail-closed 输入边界，以及
  单程序的受控 Host/Edge pair 入口；
- [Host/Edge并行模型](development-model.md)：双方如何依赖同一Profile、simulator和conformance；
- [Composition Edge trace v1](composition-edge-trace-v1.md)：bounded canonical trace与replay。
- [Composition Editor](../devtools/web/composition_editor/README.md)：loopback-only local draft、
  native Host bridge与sanitized read-only truth；
- [C SDK结构审计与整理报告](c-sdk-organization-report.md)：源码/CMake/target/test所有权、
  本轮无语义重排和后续债务；它是工程审计，不是产品语义authority。

## Profile 与 target

- [Wi-Fi telemetry Profile](../profiles/wifi/README.md)与
  [direct-IP实装](wifi-runtime-direct-ip-v0.md)；
- [Composition Profile](../profiles/composition/README.md)与
  [direct-IP MediaStream](media-stream-direct-ip.md)；
- [Display Ingress Profile](../profiles/display_ingress/README.md)：Host 到 Edge 实屏链路的
  单 endpoint、完整帧和异步呈现证据合同；四 route Catalog、Desired/Actual、呈现证据
  codec、一次 RUNNING 数据门、有界媒体 transport 和受控 connected UDP adapter 已实现；
  现已接通本机模拟服务，以及服务器测试端到 K230D 的单帧真实 page-flip/反馈；正式应用 Host、
  连续视频、安全 carrier 与独立肉眼核验仍待完成；
- [K230D Linux Edge基线](k230d-linux-edge-development-baseline.md)与
  [target入口](../targets/k230d-linux/README.md)；
- [Edge local compositor](edge-local-compositor-v0.md)：已验CPU offscreen合成与尚未验物理present的分界。

## 当前证据

- [系统状态](system-report.md)：当前实现、CI、K230D、连接/恢复与未验边界的短摘要；
- [K230D Composition E2E](k230d-composition-e2e.md)：当前正式身份下camera、Blob/Scene/C2、
  CPU render、Render/CameraMedia、telemetry与clean shutdown的fresh target gate；
- [文档归档](archive/README.md)：过去的逐commit报告与开发计划，不再表示当前状态。

## 阅读路径

- 改产品语义：先选择上述五份规范中唯一拥有该问题的文件；
- 改实现：再读对应Core/Profile/target合同和命名测试；
- 判断“已验”：只读与exact source/platform/workload绑定的evidence；
- 历史文档仅用于追溯，不为当前API、状态或证据标签提供authority。
