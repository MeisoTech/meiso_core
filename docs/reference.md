# 工程参考索引

日常使用先读[快速开始](host-edge-quickstart.md)。下列文档用于修改对应模块，不是应用开发的前置阅读。

## 通信与 SDK 实现

- [Resource Core](resource-core-v0.md)、[Session Core](session-core-v0.md)、[Runtime Core](runtime-core-v0.md)：C11 实现合同。
- [Host/Edge 开发边界](development-model.md)：双方交付与验证职责；其中阶段记录按原日期理解。
- [Composition Host 集成](host-integration-receipt-v0.md)、[确定性 trace](composition-edge-trace-v1.md)。

## 能力 Profile

- [Display Ingress](../profiles/display_ingress/README.md)：当前视频/2D 应用的跨端契约。
- [Composition](../profiles/composition/README.md)：场景、离屏渲染与相机媒体路线。
- [Wi-Fi telemetry](../profiles/wifi/README.md)：Wi-Fi 状态采样。
- [Direct-IP Wi-Fi](wifi-runtime-direct-ip-v0.md)、[Composition MediaStream](media-stream-direct-ip.md)：对应 Profile 的接入细节。

## 平台、工具与证据

- [K230D target](../targets/k230d-linux/README.md)、[Linux 开发基线](k230d-linux-edge-development-baseline.md)。
- [相机/Composition E2E](k230d-composition-e2e.md)：独立 camera 已验与 combined 未验范围。
- [Edge 离屏 compositor](edge-local-compositor-v0.md)：CPU 离屏路径，不等同于当前硬件视频显示。
- [Composition Editor](../devtools/web/composition_editor/README.md)：本地场景编辑与调试工具。
- [Display fixture 输入](../integrations/host/display_ingress_fixture_input.md)：DIFI/v1 调试输入格式。
- [历史报告](archive/README.md)：旧阶段状态和结构审计。
