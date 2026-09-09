# Meiso Glass

[![CI](https://github.com/MeisoTech/meiso_glass/actions/workflows/ci.yml/badge.svg)](https://github.com/MeisoTech/meiso_glass/actions/workflows/ci.yml)

Meiso Glass 是分体式 AR 设备的通信与设备 SDK。眼镜端 **Edge** 负责采集、解码和显示；
外部 **Host** 运行应用、渲染和重计算，通过同一套接口交付内容、读取设备能力与实际状态。

当前可运行组合是 **Jetson Host → K230D 128 MiB Edge**：Host 提供左右眼压缩视频，
Edge 硬件解码并叠加本地绘制的框和中文短消息。纯消息可以独立运行。

- **运行设备或写应用**：[Host/Edge 快速开始](docs/host-edge-quickstart.md)
- **选择配套版本**：[版本配对与发布](docs/releases.md)
- **理解代码边界**：[SDK 指南](docs/sdk-guide.md) · [实现地图](docs/system-runtime-map.md)
- **修改协议或查细节**：[文档导航](docs/index.md)

## 当前能做什么

| 能力 | 当前范围 |
|---|---|
| Host 视频 → Edge 显示 | 1280×720 H.264 左右眼画面；1920×1080 物理输出，每眼原生像素居中。输入和输出尺寸分别配置。 |
| Edge 本地 2D | 一个矩形/短文字区域，可双眼重复；支持更新、隐藏、纯消息和视频叠加。 |
| 应用接入 | 可安装的 C++20 Host 客户端 SDK、本地 IPC 服务、OpenXR 逐眼视频入口；当前一次接入一个应用。 |
| 状态与恢复 | 实际呈现反馈、应用退出清理、连接失效后建立新 Session；当前配对部署已验证 Host/Edge 各自重启恢复。 |
| 相机与其他数据 | 已有独立 Composition 相机/遥测实现；相机适配器为 320×240 NV12。持续感知、AI 推理和录像应用尚未整合。 |

这是开发基线。30 Hz 输入不代表已达到 30fps 显示；6DoF、Edge 姿态微调、完整 XR 设备接口、
复杂排版和安全传输仍未交付；出图通过不等于双眼光学校准验收。具体环境和证据边界见快速开始。

## 代码与仓库怎样组合

| 目录 | 职责 |
|---|---|
| `include/meiso/core`、`src/core` | C11 Core：身份、Session、State / Stream / Action / Blob 传输语义。 |
| `src/runtime`、`profiles`、`integrations` | 资源目录、能力协商、desired/actual、媒体契约与 Host facade。 |
| `targets/k230d-linux` | K230D 的摄像头、解码、DRM 显示和 Linux 服务适配。 |
| `applications`、`simulation`、`tests` | 使用入口、模拟后端与契约检查。 |

[Looking Glass](https://github.com/MeisoTech/looking_glass) 独立拥有 HostRuntime、应用兼容和合成。
它通过 CMake `find_package(MeisoGlass)` 链接本仓安装的库，再交付 Host 服务和客户端 SDK；
本仓另外交叉编译 Edge 包。两仓按固定源码组合发布，不需要整仓复制或相同的提交节奏。

协议核心可以跨平台复用；运行包仍需要相应 OS、编解码和设备驱动。板级固件及私有实验资料不属于
本 SDK 的源码包。

## 构建可移植库

需要 C11 编译器和 CMake 3.21 或更高版本。此命令构建库与测试，不会部署设备：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

只构建 SDK 可添加 `-DBUILD_TESTING=OFF -DMEISO_BUILD_APPLICATIONS=OFF`。
按组件安装、仓外链接和 Windows 构建见 [SDK 指南](docs/sdk-guide.md)；运行 Jetson/K230D
使用 [配对快速开始](docs/host-edge-quickstart.md)，不是上面这条本机构建命令。

## 参与开发

遵循 [贡献约定](AGENTS.md)，从一条具体应用闭环开始；协议变更同步其所属规范，
平台变化补相应实机结果。提交时保留别人的工作中内容，原始日志、凭据和固件镜像不进产品仓。

仓库尚未提供源代码许可证；技术预发布不自动授予开源使用或再分发权利。
