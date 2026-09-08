# Meiso Glass

[![CI](https://github.com/anantheparty/meiso_glass/actions/workflows/ci.yml/badge.svg)](https://github.com/anantheparty/meiso_glass/actions/workflows/ci.yml)

Meiso Glass 是面向分体式 XR/AR 的开放 Resource 与 Composition 契约，以及一组
portable C11 参考实现和可重复实验。

## 模型

- `Edge` 拥有设备 truth、platform adapter、本地安全行为，并对已声明的物理输出拥有最终 authority；
- `Host` 运行应用与重计算，通过 portable Edge facade 操作远端或模拟 Edge；
- 公共 Resource 是 `State / Stream / Action / Blob`，MediaStream 是 Stream 的专业化；
- Core 负责 identity、Session 与 Resource delivery，Runtime 负责 capability、endpoint 和控制，
  Host SDK 负责 opaque `MeisoEdgeHandle` 与应用 API。

项目不绑定特定 SoC、开发板、操作系统、图形 API 或网络载体。IP、端口和 TCP adapter
属于 bootstrap/transport，不是 peer identity。

## 当前状态

| 组件 | 已有实现 | 证据边界 |
| --- | --- | --- |
| Resource Core | caller-owned arena 上的 bounded `State / Stream / Action / Blob` | portable C11 source API，不冻结二进制 ABI |
| Session 与 Runtime | exact Profile handshake、bounded wire、catalog、State/Stream production gate、Blob carriage 与 MRT0 v4 external route | Host semantic/wire 与 POSIX loopback；不包含安全 transport 或通用 discovery |
| Wi-Fi telemetry | macOS Host、K230D Linux Edge、direct-IP State/Stream 闭环与本地 dashboard | 已有受限 board evidence，不等于通用产品 Runtime |
| Composition Profile | `COMPOSE_/_PROFILE:v1`、MRT0 v4、6/18 graph、SceneStore、coordinator、scheduler 与双路 media | Host/loopback semantic、wire 与 transport evidence；fresh board gate 待本轮收口 |
| CPU raster / Edge compositor | bounded 320×240 reference CPU off-screen renderer 与 integrated K230D backend | 旧身份有 board render/CMFB 证据；正式身份待 fresh 复验；不证明 driver scanout 或物理显示 |
| K230D camera capture | bounded Linux V4L2 four-buffer NV12 adapter 与一次三帧 target receipt | `BOARD_VERIFIED` 仅指单适配器物理采集；不证明 CameraMedia transport、combined service、性能或 E2E |
| K230D Composition E2E chain | 同一Session内Blob、Scene/C2、CPU render、Render/Camera Media、Wi-Fi/DeviceTelemetry与local Web bridge | [当前身份的实机复验门](docs/k230d-composition-e2e.md)尚待fresh run；旧身份证据只保留在archive，不证明当前工作树 |
| Composition Host SDK facade | opaque `EdgeHandle`、exact 6/18 discovery、typed desired/actual/Stream/Blob/Composition snapshot、media borrow/release、caller-owned deterministic simulator、canonical trace v1，以及同一native Composition Host Bridge的Composition Editor离线入口 | HostRuntime/AF_INET与simulator两种backend；remote Composition Editor可在typed transport/Session/catalog terminal后启动fresh Host child并回放最新local editor intent，但不resume旧Session/handle，也不等于通用SDK/backend或Edge重启恢复；`--simulator`可从本地编辑器真实子进程驱动Mesh→Scene/C2→source-pinned CPU RenderResult和完整320×240 RGBA CMFB preview、Wi-Fi、DeviceTelemetry及两路media State，但不建立Session/TCP、camera frame、target或physical evidence，也不承诺跨target像素一致；trace v1仍只重放Wi-Fi/media |

逐版本提交、CI、资源账本和 target receipt 统一记录在
[系统进展](docs/system-report.md)，不在首页重复维护。

## 构建与测试

Linux/macOS：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

纯 library/SDK 构建不创建应用和测试 target：

```sh
cmake -S . -B build-sdk \
  -DBUILD_TESTING=OFF \
  -DMEISO_BUILD_APPLICATIONS=OFF \
  -DCMAKE_INSTALL_PREFIX=/tmp/meiso
cmake --build build-sdk --parallel
cmake --install build-sdk --component Core
cmake --install build-sdk --component Runtime
```

仓外 consumer 可以只选 Core，或在同一 prefix 中安装 Core+Runtime 后链接
`Meiso::Runtime`：

```cmake
find_package(MeisoGlass 0.1 CONFIG REQUIRED COMPONENTS Runtime)
target_link_libraries(my_runtime PRIVATE Meiso::Runtime)
```

公开头按职责使用 `<meiso/core/...>`、`<meiso/runtime/...>`、
`<meiso/profiles/...>` 等 namespace；本轮是 pre-public 硬迁移，不提供旧根头、旧 target alias
或 forwarding shim。完整组件图与安装例见 [SDK 指南](docs/sdk-guide.md)。

> 注：当前仓库尚无 `LICENSE`/`COPYING`/`NOTICE`。上述 package 已通过技术安装与
> relocation 验证，但不因此自动具备对外分发授权。

Windows（MSVC x64 v143 + Ninja）：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

## 从这里开始

产品契约：

- [文档入口与权威关系](docs/index.md)
- [系统设计](docs/system.md)
- [Resource](docs/resource.md)、[Runtime](docs/runtime.md)、[Link](docs/link.md) 与
  [Composition](docs/composition.md) 契约

实现与接入：

- [Jetson → K230D 视频、消息与应用 SDK 快速开始](docs/host-edge-quickstart.md)
- [SDK 设计与开发指南](docs/sdk-guide.md)
- [Session Core v0](docs/session-core-v0.md) 与
  [Runtime negotiation core v0](docs/runtime-core-v0.md)
- [Host 集成回执 v0](docs/host-integration-receipt-v0.md)
- [Composition Edge deterministic trace v1](docs/composition-edge-trace-v1.md)

实验与 target：

- [Wi-Fi telemetry Profile](profiles/wifi/README.md) 与
  [direct-IP 实装规则](docs/wifi-runtime-direct-ip-v0.md)
- [Composition Profile](profiles/composition/README.md) 与
  [Edge local compositor 方案](docs/edge-local-compositor-v0.md)
- [Composition Editor](devtools/web/composition_editor/README.md)
- [K230D Linux Edge](targets/k230d-linux/README.md)
- [K230D Composition E2E 验收状态](docs/k230d-composition-e2e.md)

## 尚未完成

当前仓库不是 application-facing 完整产品：已有正式
`COMPOSE_/_PROFILE:v1` 的首个 portable Edge
facade、包含离线Composition编辑闭环的deterministic simulator与同一native Composition Host Bridge/Dashboard入口、固定版本canonical trace，但通用backend plugin、discovery/reconnect、
State watch、Action、认证与安全transport尚未完成。
旧身份的 Composition Edge 已有一次准确 K230D 上的 combined off-screen
render/camera MediaStream 与 local Web bridge 证据；当前正式身份需 fresh 复验。
Dashboard 仅实现 host-local fresh-child 自动重连，板端
network/Edge恢复、Session resume、通用fresh Edge、性能、长稳和physical presentation evidence仍未完成。
