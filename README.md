# Meiso Glass

Meiso Glass 是面向分体式 XR/AR 的开放 Resource 与 Composition 契约。

- `Edge` 拥有实际设备 truth、platform adapter、本地安全行为和已声明的物理输出；
- `Host` 运行应用与重计算，通过 SDK 的 Virtual Edge 操作远端或模拟 Edge；
- 公共 Resource 为 `State / Stream / Action / Blob`；MediaStream 是 Stream 的专业化；
- Session、runtime、sensor、frontend、system image 和 transport implementation 可替换，
  具体有效组合由 capability 与经过验证的 Profile 表达。

项目不绑定某颗 SoC、开发板、操作系统、图形 API 或网络载体，也不规定组件实现顺序。

公开边界为三层：Core 拥有 identity、connection、handshake、Session 与 Resource delivery
primitive；Runtime 拥有 Resource Graph、capability、endpoint 和控制逻辑；Host SDK 拥有
Virtual Edge 与应用 API。IP、port 和 TCP adapter 属于 bootstrap/transport，不是 peer identity。

## 当前代码状态

当前分支包含新的 portable C11 `meiso_resource_core`：共同 identity 与 descriptor 校验、
固定且可 seal 的 descriptor snapshot，以及 caller-owned arena 上的 bounded
`State / Stream / Action / Blob` primitive。它们是 Resource semantic core v0 的
source-level implementation API，不冻结二进制 ABI。

当前还包含 caller-owned arena 上的 `meiso_session_core`：exact protocol/Profile、
`OPEN/ACCEPT/CONFIRM/READY`、bounded wire 与 Runtime-message gate。fresh Host build
的 CTest `11/11` 通过，支持 `HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED`；该结果
不声明 TCP、认证、安全 transport 或 target evidence。

这套实现仍不是 application-facing Resource Runtime 或完整产品系统；它不包含
Virtual Edge、Resource Registry/endpoint control、Composition Runtime、MediaStream data path、
TCP adapter 或 platform adapter。
K230D conformance target 已完成 host-side cross-build、image inspection，并在
`CanMV-K230D-Zero V1.1` 上得到七套 suite 全部通过的 `BOARD_VERIFIED` evidence。这个状态只
覆盖 Resource semantic core v0 的单进程 target runner，不构成 remote backend、LinkSession、
Composition、MediaStream、完整冷启动或产品 Profile evidence。各层验证范围见
[系统进展](docs/system-report.md)。

Windows 配置入口仍保留；其结果必须按实际 toolchain 和当次 CTest evidence 记录：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

## 文档

- [文档入口与权威关系](docs/index.md)
- [系统设计](docs/system.md)
- [Resource 契约](docs/resource.md)
- [Link 契约](docs/link.md)
- [Session Core v0 实现契约](docs/session-core-v0.md)
- [Composition 契约](docs/composition.md)
- [SDK 设计与开发指南](docs/sdk-guide.md)
- [Resource semantic core v0 实现与验证基线](docs/resource-core-v0.md)
- [系统进展](docs/system-report.md)
- [Host/Edge 并行开发模型](docs/development-model.md)
- [K230D Linux Edge 开发基线](docs/k230d-linux-edge-development-baseline.md)
