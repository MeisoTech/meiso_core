# Display Ingress 单帧实机结果 · 2026-09-06

Linux x86_64 服务器运行 Host 测试角色，K230D Linux 6.6.36 运行 Edge DRM 角色。
服务器是替代测试端，尚未使用 Jetson 或正式 Looking Glass HostRuntime。
Edge 路由通过 wlan0；TCP/UDP 均为本次运行新建的实际跨机连接，SSH 只启动程序和收集日志。

## 已通过

- 五项相关测试：Profile、Host fixture input、media transport、POSIX UDP、原有 simulated service。
- GCC 14.1.1/Xuantie V3.0.2 的 Linux/riscv64 严格告警交叉构建，以及服务器本机构建。
- 公开 DIFI 输入经真实 MSL0/MRT0 会话、Catalog、Desired/Actual 和 DIFR 媒体传输到 Edge。
- Host 输入为 64×32 opaque RGBA，共 8,192 bytes。Edge 将其 nearest-neighbor 铺到实际
  1920×1080 DSI 输出，RGB565 dumb buffer 为 4,147,200 bytes、pitch 3840。
- TEST_ONLY、真实非阻塞提交、匹配 cookie/CRTC 的 page-flip 事件及平面属性回读均成功。
- Host 与 Edge 收据中的 Session、production/media epoch、frame sequence、输入 SHA-256、
  输出尺寸和内核 page-flip 时刻一致；使用 `DRM_KMS_PAGE_FLIP`，没有模拟呈现替代。
- 完成后恢复原平面属性；原拓扑与内核日志未变化。两个成功运行使用全新 Session/socket。
- 负向实机运行把显示路径设为非 DRM 字符设备：Edge 报告 display/admission failure，Host
  因会话未完成而失败。未输出呈现证据，退出后无 DRM owner。

最终 Edge ELF SHA-256：
`353c955f3a526dc66e9e6b87d8a9a80975f3a40d37ddf5e44ef8a4f0cef07546`。
完整输入 SHA-256：`6263a3293a08b71bfb86819e711f4b4d56c5270e83a8fb19867affa21fdd3ee6`。
最终 ELF 静态 BSS 245,024 bytes；这不等于进程峰值或整机内存预算。

## 结论范围

证明受控网络下的一张输入帧到 K230D KMS latch 并回传 Host 的接线。
肉眼、面板/光学/splitter、连续视频帧率/时延、压缩解码、2D 叠加、姿态修正及完整
Looking Glass/Jetson E2E 尚未由本实验验证。未改变原 Profile wire 或 Core 传输语义。

可复现构建/角色参数见 [target 入口](../README.md#display-ingress-单帧实机路径)。
私有启动器、原始日志、实际地址和工具链位置保留在 lab 的 KAngel 记录，不进入本仓。
