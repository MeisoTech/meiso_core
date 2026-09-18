# Meiso Glass 实现地图

> `ENGINEERING MAP / NON-NORMATIVE`，2026-09-18，基于 `e9f555e` 的代码布局。
> 产品语义由[五份通用规范](index.md#产品规范)及所选[Display Ingress Profile](../profiles/display_ingress/README.md)拥有。
> 本页描述职责和依赖，不用历史板测替代当前版本的验收。

## 产品与代码怎么分开

Host 运行应用、交付内容并理解 Edge 实际状态；Edge 采集数据并完成有限本地渲染、解码和显示。
K230D 是 **128 MiB 整机内存**目标，应用、Linux/ISP、CMA、网络与编解码/显示缓冲共同占用预算。

| 职责 | 代码归属 |
|---|---|
| 身份、Session、State / Stream / Action / Blob 传输语义 | Meiso [`include/meiso/core/`](../include/meiso/core/)、[`src/core/`](../src/core/) |
| catalog、能力/路由、desired/actual 与媒体契约 | Meiso [`src/runtime/`](../src/runtime/)、[`profiles/`](../profiles/)、[`integrations/`](../integrations/)；这里的 Runtime 是 SDK 协议运行库 |
| 应用 API/IPC、合成、编码、感知与记录服务 | 独立 [Looking Glass Host assembly](https://github.com/MeisoTech/looking_glass/blob/main/sdc/integrations/meiso/presentation_sink/README.md)；链接安装的 Meiso C SDK |
| Edge 产品进程、控制会话与媒体接线 | [`applications/display_ingress/`](../applications/display_ingress/)；不依赖 `devtools/` 源码 |
| K230D V4L2 采集、MVX 解码、DRM 显示与启动 | [`targets/k230d-linux/`](../targets/k230d-linux/README.md)，交付 `K230dVideoRuntime` 组件 |
| 桌面 Edge 与协议/输入诊断 | [桌面开发入口](desktop-development.md)；模拟完成和物理呈现分别标注 |
| 内核、设备树、rootfs 与厂商补丁 | 独立 [k230d-bsp](https://github.com/MeisoTech/k230d-bsp)，不在 SDK 中保存补丁副本 |

Meiso 与 Looking Glass 保持独立仓库，使用安装包和精确源码锁配对，不靠源码复制或相同版本号隐式匹配。
构建与版本选择见[SDK 指南](sdk-guide.md)、[配对与发布](releases.md)。Locate/Echo 使用 Looking Glass 安装的 Host SDK；
应用拥有识别与体验，平台拥有原始采集数据的传输和共享记录入口。

## 当前数据路径

| 模式 | 路径及实现边界 |
|---|---|
| Host 渲染视频 | 应用 → Host 编码 → Display Ingress 压缩媒体 → Edge MVX/DRM；左右眼复用同一解码帧，无额外全帧重排。输入与物理输出尺寸分开，按 Profile 原生像素放置，不自动缩放或光学校准。 |
| Edge 原生屏幕信息 | Host 提供计划 → 同一 Session 的 Desired → Edge 本地绘制 → Actual/呈现 Stream。v9 是单区域 HUD；显式 v10 scene 支持最多 16 个独立矩形及有限文字，平台可声明更小能力。scene 可不播放视频。 |
| Edge 采集上行 | V4L2/桌面相机 → 既有 CameraMedia → Host 服务/应用。K230D 适配器采集 320×240 NV12，可选 JPEG 关键帧 ≤0.5 Hz、≤30 KiB；发送频率不等于传感器/ISP 已省电。 |
| 世界空间简单 3D / 6DoF | Host 提供对象、Edge 渲染是产品方向；当前未交付实机 IMU 跟踪与姿态微调闭环。研究基准不等于这条产品能力已完成。 |

State 送达、配置 Actual、媒体接收和物理翻页是不同事实。桌面完成不能写成实屏；
超时未观察到呈现不能写成确定未呈现。正常退出、异常隔离和会话重建按所选合同处理，
不能由入口文档承诺任意故障后已清屏或应用自动重放。

## 平台和使用入口

- [K230D 构建、安装和启动](../targets/k230d-linux/README.md)：当前产品源码、CI 交叉构建、解码/scene 缓冲与相机边界。
- [Host/Edge 快速开始](host-edge-quickstart.md)：含初次技术预览的视频/HUD 命令与注明版本的证据；当前 Host 选项跟随其安装包入口。
- [桌面开发](desktop-development.md)：跨平台接线与模拟证据；Host 的 scene 路径可不编入 Jetson 视频编码后端，平台支持范围由 Looking Glass 声明。
- [版本配对](releases.md)：源码 SHA、运行包和 BSP 分别记录；技术预览不代表完整产品上线。

安全 carrier、完整 XR 设备兼容、光学校准、长时运行和功耗仍需各自验证；协议身份声明不等于密码学认证。
单次 720p 或 scene 结果不能推出全部分辨率与相机/视频并发都适合 128 MiB 设备。

## Composition 与历史

[Composition 通用规范](composition.md)仍定义呈现责任和激活语义；Display Ingress 明确选择其 direct-latest 路径。
旧 Mesh/SceneStore、Coordinator、Composition Editor/HostBridge/EdgeService 应用栈则默认关闭，
仅 `MEISO_BUILD_LEGACY_COMPOSITION=ON` 选入。当前 CameraMedia 依赖继续保留，详见[组件边界](sdk-guide.md#3-cmake-package-与组件)。

原折叠的线程、内存账本与架构问题已移到[截至 2026-09-02 的 Composition 工程地图](archive/composition-runtime-map-through-20260902.md)；
其中“当前”“未完成”只描述原提交，不能解释为今天的 Display Ingress 产品状态。
