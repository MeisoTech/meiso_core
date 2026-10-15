# Meiso Core

[![CI](https://github.com/MeisoTech/meiso_core/actions/workflows/ci.yml/badge.svg)](https://github.com/MeisoTech/meiso_core/actions/workflows/ci.yml)

**English** · [中文](#给第一次来的人)

Meiso splits a pair of AR glasses in two. The glasses keep only what has to be on your face (camera, IMU, display)
and send what they see over Wi-Fi to a computer nearby, the **Host** (today an NVIDIA Jetson Orin NX). The Host runs
the apps, head tracking and recognition, and sends back what to show. **This repository is the protocol and SDK
both ends speak.**

- **What is in it**: a C11 core (identity, sessions, State / Stream / Action / Blob delivery); the runtime that
  negotiates capabilities and reconciles desired with actual state; the display contract between Host and glasses,
  [Display Ingress planes-v12](profiles/display_ingress/planes-v12.md), where everything shown is a plane in space
  (streamed video, a still image or a mark) placed by the Host and drawn by the glasses at every refresh; portable
  POSIX adapters; conformance tests.
- **Where it stands**: a development baseline, not a product. It runs between an Orin NX Host and a K230D glasses
  board (128 MiB of RAM) on a lab rig. The link has no authentication or encryption yet: use it only on a network
  you control. The Host system (MeisoOS and its apps, `meiso_host`) and the glasses firmware (`meiso_edge`) are
  separate repositories, not public yet: links to them in the documents will not open.
- **Build**: a C11 compiler and CMake 3.21 or newer (CI builds on Ubuntu 24.04 with GCC, and with Clang under
  ASan/UBSan, and on macOS 15 with Apple Clang):

  ```sh
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build --parallel
  ctest --test-dir build --output-on-failure
  ```

- **License**: Apache-2.0 ([LICENSE](LICENSE)); third-party material keeps its own terms ([NOTICE](NOTICE)).
- **MeisoGlass**: the project's earlier name. This repository was `meiso_glass` until 2026-09-24, when the
  glasses-side code moved out; the installed CMake package is still `MeisoGlass` (`find_package(MeisoGlass)`), and
  older documents say Meiso Glass.
- **GOSIM 2026**: the Meiso demo at GOSIM (16–17 October 2026) runs on this protocol. The glasses at the booth only
  see and show; recognising what is on the table, tracking the head and keeping the pins you place all run on the
  Orin NX box beside them.
- The documents are mostly in Chinese; start at [docs/index.md](docs/index.md).

## 给第一次来的人

Meiso 把一副 AR 眼镜拆成两半：眼镜上只留必须戴在脸上的相机、IMU 和显示，看到的东西经 Wi-Fi 送到旁边的一台电脑，
也就是 **Host**（现在是 NVIDIA Jetson Orin NX）。APP、头部跟踪和识别都在 Host 上算，再把要显示的东西发回眼镜。
**本仓是两端共同遵守的协议和 SDK。**

- **里面有什么**：C11 Core（身份、Session、State / Stream / Action / Blob 的交付）；协商能力、对齐 desired 与
  actual 的 Runtime；Host 和眼镜之间的显示契约 [Display Ingress planes-v12](profiles/display_ingress/planes-v12.md)：
  显示的一切都是空间里的平面（串流视频、静态图或标记），Host 摆放，眼镜每次刷新自己画；可移植的 POSIX 适配器；
  一致性测试。
- **做到哪了**：开发基线，不是产品。在实验台架上跑在 Orin NX Host 和 K230D 眼镜板（128 MiB 内存）之间。
  链路还没有认证和加密，只在你自己控制的网络里用。Host 系统（MeisoOS 和它的 APP，`meiso_host`）和眼镜固件（`meiso_edge`）
  在另外的仓库，暂未公开，文档里指向它们的链接打不开。
- **怎么编**：C11 编译器和 CMake 3.21 或更新，命令同上（CI 在 Ubuntu 24.04 上用 GCC、用 Clang 加 ASan/UBSan，
  在 macOS 15 上用 Apple Clang）。
- **许可证**：Apache-2.0（[LICENSE](LICENSE)）；第三方材料保留各自的条款（[NOTICE](NOTICE)）。
- **和 MeisoGlass 的关系**：MeisoGlass 是项目早先的名字。本仓 2026-09-24 以前叫 `meiso_glass`，那天眼镜端代码拆了出去；
  安装出来的 CMake 包仍叫 `MeisoGlass`（`find_package(MeisoGlass)`），早期文档也写 Meiso Glass。
- **和 GOSIM 的关系**：GOSIM 2026（10 月 16–17 日）上的 Meiso 演示跑的就是这套协议。展台上的眼镜只管看和显示；
  认出桌上的东西、跟踪头部、记住你放下的 pin，都在旁边那台 Orin NX 盒子里算。
- 文档以中文为主，从 [docs/index.md](docs/index.md) 开始。

## 开发者说明


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
| Edge 本地 2D | v9 单区域 HUD；显式 v10 scene 最多 16 个独立矩形及有限文字，受平台像素预算约束。支持更新、清空及与视频叠加。 |
| 应用接入 | 可安装的 C++20 Host 客户端 SDK、本地 IPC 服务、OpenXR 逐眼视频入口；当前一次接入一个应用。 |
| 状态与恢复 | 实际呈现反馈、应用退出清理、连接失效后建立新 Session；当前配对部署已验证 Host/Edge 各自重启恢复。 |
| 相机上行 | 同一会话复用 CameraMedia；K230D 采集 320×240 NV12，可选 ≤0.5 Hz JPEG 关键帧。Host 数据服务与 AI 应用由 Looking Glass / Locate / Echo 负责。 |

这是开发基线。30 Hz 输入不代表已达到 30fps 显示；6DoF、Edge 姿态微调、完整 XR 设备接口、
复杂排版和安全传输仍未交付；出图通过不等于双眼光学校准验收。具体环境和证据边界见快速开始。

## 代码与仓库怎样组合

| 目录 | 职责 |
|---|---|
| `include/meiso/core`、`src/core` | C11 Core：身份、Session、State / Stream / Action / Blob 传输语义。 |
| `src/runtime`、`profiles`、`integrations` | 资源目录、能力协商、desired/actual、媒体契约与 Host facade。 |
| [meiso_edge](https://github.com/MeisoTech/meiso_edge)（暂未公开） | 眼镜端（K230D）：Display Ingress 服务、摄像头、解码、DRM 显示、Linux 服务适配与板级 bring-up，2026-09-24 从本仓拆出（S-027）。 |
| `applications`、`simulation`、`tests` | 使用入口、模拟后端与契约检查。 |

[meiso_host](https://github.com/MeisoTech/meiso_host)（原 Looking Glass，暂未公开）独立拥有 HostRuntime、应用兼容和合成。
它通过 CMake `find_package(MeisoGlass)` 链接本仓安装的库，再交付 Host 服务和客户端 SDK；
Edge 包由 meiso_edge 以本仓固定修订的源码交叉编译。两仓按固定源码组合发布，不需要整仓复制或相同的提交节奏。

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

本仓原始代码以 [Apache License 2.0](LICENSE) 发布。字体、历史 BSP 修改及外部依赖的范围见 [NOTICE](NOTICE)，各自保留上游许可。
