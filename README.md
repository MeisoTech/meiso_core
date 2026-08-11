# Meiso Glass

Meiso Glass 是面向分体式 XR/AR 的开放 Resource 与 Composition 契约。

- `Edge` 拥有实际设备 truth、platform adapter、本地安全行为和已声明的物理输出；
- `Host` 运行应用与重计算，通过 Virtual Edge 操作远端或模拟 Edge；
- 公共 Resource 为 `State / Stream / Action / Blob`；MediaStream 是 Stream 的专业化；
- Session、runtime、sensor、frontend、system image 和 transport implementation 可替换，
  具体有效组合由 capability 与经过验证的 Profile 表达。

项目不绑定某颗 SoC、开发板、操作系统、图形 API 或网络载体，也不规定组件实现顺序。

## 当前代码状态

当前 C11 代码来自 Resource 重基线前的实验，只保留为可执行行为 evidence：

- `meiso_latest_state` 验证固定容量 newest-wins、pending、ACK、重复/冲突与 session 重发；
- `meiso_slice` 验证 frame identity、姿态/时效 gate、lease 与 fail-closed 合成 workload。

这些符号和 API 不是新公共 SDK，不享有兼容承诺。它们通过测试也不代表 Resource Runtime、
LinkSession、Virtual Edge 或平台 adapter 已实现。

现有 Windows baseline 可以继续复现实验：

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
- [系统进展](docs/system-report.md)
