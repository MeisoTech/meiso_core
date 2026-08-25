# Meiso Glass 文档

## 产品规范

以下五份文档共同拥有通用产品语义；各自只拥有一个问题域：

1. [系统设计](system.md)：角色、Virtual Edge、Core/Runtime/SDK/Profile 与产品不变量；
2. [Resource 契约](resource.md)：State、Stream、Action、Blob、ownership、lifecycle 与 QoS；
3. [Runtime 契约](runtime.md)：catalog、endpoint、State-driven desired/actual、production 与 data route；
4. [Link 契约](link.md)：identity、Session、time、delivery、failure 与 transport-independent
   boundary；
5. [Composition 契约](composition.md)：layer、generation、激活和 Edge final authority。

被接受且版本化的 Profile/wire 契约可以填充上述规范明确委托的开放参数，但不能覆盖
通用语义。除此之外不存在第二份“总体唯一规范”；overview、实现指南、报告、源码注释或
Git 历史与上述规范冲突时，必须显式修正规范或把其降级为 evidence，不能让两套定义并存。

## 实现与证据

- [SDK 设计与开发指南](sdk-guide.md)：实现边界、adapter pattern、conformance 与工作单元；
- Composition Profile v3 Host facade：公开 `include/meiso_virtual_edge.h` 与
  `include/meiso_composition_edge.h` 只给应用 opaque handle、typed command/snapshot 和
  media borrow；`include/meiso_composition_edge_simulator.h`提供caller-owned显式step simulator，
  包含deterministic Wi-Fi、可选DeviceTelemetry、两路media与opt-in离线Mesh→Scene/C2→RenderResult子集；
  [Composition Edge deterministic trace v1](composition-edge-trace-v1.md)固定caller-owned canonical
  trace bytes、closed event order、integrity与逐record replay；
  `devtools/composition_virtual_edge_host.*`承载当前HostApp integration与两backend dispatch，仍由
  integration独占socket/event loop；
- [Resource semantic core v0](resource-core-v0.md)：当前 portable primitives、内存模型与
  conformance 矩阵；这是实现基线，不是产品规范；
- [Session Core v0](session-core-v0.md)：当前 portable handshake、wire、limits、lifecycle
  与 Runtime-message gate；Runtime catalog/route codec 不属于 Session Core；
- [Runtime negotiation core v0](runtime-core-v0.md)：当前 portable State/Stream-driven catalog、
  bounded endpoint identity history、exact State commit、production gate，以及严格 MRT0 v2/v3；
  v3 增加 Host→Edge Blob carriage 与通用 Core BlobStore publish bridge。它是 Host
  implementation evidence，不是产品规范或 target evidence。R2a 另以独立 Runtime--SDK
  scheduler 提供 per-peer outbound fairness；该对象不进入 Core、wire 或 direct Runtime API；
- [Wi-Fi telemetry Profile v0](../profiles/wifi-telemetry-v0/README.md)：首个版本化 endpoint
  Profile；固定 capability、Host desired、Edge actual 与匿名 Stream sample schema；
- [Direct-IP Wi-Fi Runtime v0 实装规则书](wifi-runtime-direct-ip-v0.md)：该 Profile 在 macOS
  Host Dev Tool 与 K230D Linux Edge target 上的 locator/config、catalog/State/Stream 顺序、
  lifecycle、操作步骤与严格 evidence 边界；不是产品规范；
- [Direct-IP MediaStream v0 实装规则书](media-stream-direct-ip-v0.md)：Composition scene Profile v3
  选定的DeviceTelemetry、RenderPreviewMedia、CameraMedia endpoint、独立numeric direct-IP media
  transport、Session-bound lifecycle与验收门；MRT0 v4与bounded MediaTransport已有portable实现，
  K230D camera source另有单适配器三帧物理采集证据；
- [K230D Composition debug 联调证据 v0](k230d-composition-debug-e2e-v0.md)：准确source/板实例上的
  fresh camera、combined render+camera、local Web bridge/editor与零资源债清理四个独立门；
- [系统进展](system-report.md)：当前代码成熟度、已脱敏实验事实、K230D lab evidence 与
  未验证能力。
- [Host/Edge 并行开发模型](development-model.md)：双方如何依据同一契约、simulator
  和 conformance 独立开发与逐片对接。
- [Host 集成回执 v0](host-integration-receipt-v0.md)：当前 direct-IP Session→Runtime→Resource
  接入顺序、Host handle 生命周期、协议入口与证据边界；不是 SDK API。
- [Edge local compositor v0 开发方案](edge-local-compositor-v0.md)：Runtime--SDK 层的
  Composition coordinator、renderer/present seam、选择性迁移与验收；Core 不变。C2 portable
  coordinator、R1 Runtime v3 Blob carriage 与 R2a portable fair scheduler/combined builder 已有 Host
  evidence；R2b0已有真实AF_INET loopback happy-path semantic/wire/transport evidence，R2b1完成
  same-Session scene lifecycle，R2b2完成same-Session atomic catalog replacement/rebind/visibility重建；
  R2b3又完成explicit same-process RST/new-Session recovery slice，R2b4完成真实TCP max-Blob与资源边界；
  R2b5完成真实TCP wrong-Profile fail-closed、双向分别制造的`WOULD_BLOCK/TX_BUSY`零推进与direction-local
  5/6-slot fairness；K1a已有Host synthetic reference renderer和offline runner evidence，但K230D
  target run/present、自动resume、fresh Edge与其余negative matrix仍未验证。
- [K230D Linux Edge 开发基线](k230d-linux-edge-development-baseline.md)：当前候选实现中
  大小核、Linux、NuttX、sensor、network 与 rendering 的责任分工；它不是互操作 Profile。

## 当前实验入口

- [Composition scene lab v0](../profiles/composition-scene-lab-v0/README.md)：exact composite Session
  Profile 实验合同；C0 已冻结 IDs、capabilities、Presentation/Scene/Mesh/RenderResult codec 与 Host
  wire vectors；C1 已用真实 Blob/State/Stream primitive 验证 bounded world store、atomic activation
  与 deterministic result；C2 已验证 portable coordinator 的 desired shape、generation/digest/token、
  Session scope、supersede/disable transaction；R1 已验证 MRT0 v3 Host→Edge Blob wire/FSM、Core
  publication bridge 与 discontinuity；R2a 已验证 exact 3/9/920 Profile v2 builder、per-peer fair TX
  scheduler 与 State/Blob RX obligation；R2b0 已用真实AF_INET loopback贯通双方scheduler、Blob/
  State/Stream与C2 active happy path，R2b1在同一Session中继续验证world reconfigure、旧/新epoch gate、
  Blob零重传、drained disable与Wi-Fi独立；R2b2又在Session不变时完成catalog g1→g2 atomic accept、
  scheduler全route rebind、gate/visibility/world重建及新epoch恢复；R2b3再以真实RST使旧对象fail closed，
  并在新AF_INET Session中显式重建desired/visibility/world/gate；R2b4又在该连接上验证exact
  262144-byte `MSH0`的64×4096无逐块ACK传输、262145-byte本地拒绝、反向Wi-Fi/RenderResult并行与
  resource ledger；R2b5又验证wrong-Profile握手fail-closed、两方向各自真实backpressure恢复及Host/Edge
  successful-slot上界5/6。两方向socket saturation不是同时制造，且没有wall-clock或性能结论。K1a
  已验证Host上的bounded 320×240 synthetic renderer与`baseline_cube_once` offline receipt；它不执行
  Runtime/Session/State/Stream、offline lease、K230D driver或physical output。official cross-build已为
  `TARGET_BUILD_VERIFIED`，但ELF未执行，target runtime仍为`TARGET_NOT_RUN`。
  后续Profile v3已冻结DeviceTelemetry、RenderPreviewMedia、CameraMedia与direct-IP MediaStream
  contract，并已有codec、Runtime v4、bounded media transport及K230D camera source的三帧target
  receipt；当前debug target又有一次Host editor/local bridge与combined render+camera board run。
  该受限run不提升历史offline artifact的标签；自动resume、通用fresh Edge、physical presentation与
  SCAP 16-slot aggregate仍未验证；
- [Four-kind lab v0](../profiles/four-kind-lab-v0/README.md)：固定的 State / Stream / Action /
  Blob 纵向 integration CI Profile；不是产品 Runtime/Profile；
- [K230D Linux target](../targets/k230d-linux/README.md)：正式 Wi-Fi Runtime Edge、固定four-kind fixture、
  build-only Composition offscreen candidate，以及独立V4L2 camera capture receipt；这些evidence不互换，
  Composition offscreen candidate仍未板上运行；
- [Wi-Fi Runtime 面板](../devtools/web/wifi_runtime_dashboard/README.md)：启动正式 macOS Host 并
  脱敏投影 catalog、desired/actual、production gate 与 Stream；不实现 wire/control 或 SDK；
- [Four-kind integration 面板](../devtools/web/four_kind_dashboard/README.md)：固定 lab fixture 的
  本地可视化 Dev Tool；不属于正式 Runtime 或 SDK。

规范说明必须成立的行为；指南说明如何实现和验证；报告只陈述特定版本、平台、条件和
时间窗口下的 evidence。组件、frontend、runtime、system image 和开发顺序由 capability、
Profile 与当前工作问题决定，不进入 Core。
