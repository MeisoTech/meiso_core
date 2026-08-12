# Meiso Glass

Meiso Glass 是面向分体式 XR/AR 的开放 Resource 与 Composition 契约。

- `Edge` 拥有实际设备 truth、platform adapter、本地安全行为和已声明的物理输出；
- `Host` 运行应用与重计算，通过 Virtual Edge 操作远端或模拟 Edge；
- 公共 Resource 为 `State / Stream / Action / Blob`；MediaStream 是 Stream 的专业化；
- Session、runtime、sensor、frontend、system image 和 transport implementation 可替换，
  具体有效组合由 capability 与经过验证的 Profile 表达。

项目不绑定某颗 SoC、开发板、操作系统、图形 API 或网络载体，也不规定组件实现顺序。

## 当前代码状态

当前分支包含新的 portable C11 `meiso_resource_core`：共同 identity 与 descriptor 校验、
固定且可 seal 的 descriptor snapshot，以及 caller-owned arena 上的 bounded
`State / Stream / Action / Blob` primitive。它们是 Resource semantic core v0 的
source-level implementation API，不冻结二进制 ABI。

这套实现仍不是 application-facing Resource Runtime 或完整 Meiso Core；它不包含
Virtual Edge、LinkSession/wire、Composition、MediaStream data path、platform adapter 或
target runtime。各 primitive fixture 与 four-kind in-process fixture 的 Host 验证范围见
[系统进展](docs/system-report.md)；这些测试不构成 remote backend、LinkSession 或 target
evidence。

以下重基线前模块继续作为 legacy behavior/workload evidence：

- `meiso_latest_state` 验证固定容量 newest-wins、pending、ACK、重复/冲突与 session 重发；
- `meiso_slice` 验证 frame identity、姿态/时效 gate、lease 与 fail-closed 合成 workload。

这些 legacy 符号、ABI 和 slot layout 不构成新 SDK 兼容面。

Windows 配置入口仍保留；其结果必须按实际 toolchain 和当次 CTest evidence 记录：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

生成的 `build/slice_preview.ppm` 只是 legacy fixture 可视化，不是产品显示模型。

## 文档

- [文档入口与权威关系](docs/index.md)
- [系统设计](docs/system.md)
- [Resource 契约](docs/resource.md)
- [Link 契约](docs/link.md)
- [Composition 契约](docs/composition.md)
- [SDK 设计与开发指南](docs/sdk-guide.md)
- [Resource semantic core v0 实现与验证基线](docs/resource-core-v0.md)
- [系统进展](docs/system-report.md)
