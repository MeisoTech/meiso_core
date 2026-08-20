# Meiso Glass

[![CI](https://github.com/anantheparty/meiso_glass/actions/workflows/ci.yml/badge.svg)](https://github.com/anantheparty/meiso_glass/actions/workflows/ci.yml)

Meiso Glass 是面向分体式 XR/AR 的开放 Resource 与 Composition 契约，以及一组
portable C11 参考实现和可重复实验。

## 模型

- `Edge` 拥有设备 truth、platform adapter、本地安全行为，并对已声明的物理输出拥有最终 authority；
- `Host` 运行应用与重计算，通过 SDK 的 Virtual Edge 操作远端或模拟 Edge；
- 公共 Resource 是 `State / Stream / Action / Blob`，MediaStream 是 Stream 的专业化；
- Core 负责 identity、Session 与 Resource delivery，Runtime 负责 capability、endpoint 和控制，
  Host SDK 负责 Virtual Edge 与应用 API。

项目不绑定特定 SoC、开发板、操作系统、图形 API 或网络载体。IP、端口和 TCP adapter
属于 bootstrap/transport，不是 peer identity。

## 当前状态

| 组件 | 已有实现 | 证据边界 |
| --- | --- | --- |
| Resource Core | caller-owned arena 上的 bounded `State / Stream / Action / Blob` | portable C11 source API，不冻结二进制 ABI |
| Session 与 Runtime | exact Profile handshake、bounded wire、catalog、State/Stream production gate、MRT0 v2/v3 Blob carriage | Host semantic/wire 与 POSIX loopback；不包含安全 transport 或通用 discovery |
| Wi-Fi telemetry | macOS Host、K230D Linux Edge、direct-IP State/Stream 闭环与本地 dashboard | 已有受限 board evidence，不等于通用产品 Runtime |
| Composition scene lab | codec、SceneStore、coordinator、fair scheduler，以及真实 TCP lifecycle、explicit same-process reconnect、分方向 backpressure 与 max-Blob 实验 | Host/loopback semantic、wire 与 transport evidence |
| K1a reference renderer | bounded 320×240 synthetic offscreen renderer 与 offline runner | `HOST_OFFSCREEN_REFERENCE_VERIFIED`、`TARGET_BUILD_VERIFIED`、`TARGET_NOT_RUN`；不证明几何正确、driver、scanout 或物理显示 |

逐版本提交、CI、资源账本和 target receipt 统一记录在
[系统进展](docs/system-report.md)，不在首页重复维护。

## 构建与测试

Linux/macOS：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

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

- [SDK 设计与开发指南](docs/sdk-guide.md)
- [Session Core v0](docs/session-core-v0.md) 与
  [Runtime negotiation core v0](docs/runtime-core-v0.md)
- [Host 集成回执 v0](docs/host-integration-receipt-v0.md)

实验与 target：

- [Wi-Fi telemetry Profile](profiles/wifi-telemetry-v0/README.md) 与
  [direct-IP 实装规则](docs/wifi-runtime-direct-ip-v0.md)
- [Composition scene lab](profiles/composition-scene-lab-v0/README.md) 与
  [Edge local compositor 方案](docs/edge-local-compositor-v0.md)
- [K230D Linux Edge](targets/k230d-linux/README.md)

## 尚未完成

当前仓库不是 application-facing 完整产品：Virtual Edge/SDK、通用 probe/hotplug、
MediaStream data path、认证与安全 transport 尚未完成。Composition 的 K230D candidate
只完成交叉构建；自动 reconnect/resume、fresh Edge、板端运行、真实 renderer backend、性能和
physical presentation evidence 均未完成。
