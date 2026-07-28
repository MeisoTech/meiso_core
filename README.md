# Meiso Glass

Meiso Glass 是一套面向分体式 XR/AR 眼镜的开放系统契约。

- `Edge` 是穿戴设备端，拥有传感器、姿态、最终合成与显示安全。
- `Host` 是外部计算设备，消费 Edge 数据并完成业务与重计算。

项目不绑定某颗 SoC、操作系统、图形 API 或无线载体。公开 SDK 只固化已经有可执行验证的角色、数据语义和安全边界。

## 当前代码

仓库目前提供两个固定容量、无网络依赖的 C11 模块：

- `meiso_slice`：Edge 产生 camera 与带时间戳的 IMU 数据，Host 生成框，Edge 按当前姿态修正并决定显示。
- `meiso_latest_state`：一槽 `LatestState(b)` 的 newest-wins、pending、ACK、幂等和新 session 重发。

开发板上的 camera 背景仅用于观察，不是产品显示模型。

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

生成的 `build/slice_preview.ppm` 只是开发可视化。生产 LinkSession、公共 wire、K230D adapter 和通用 Composition API 尚未实现。

## 文档

- [系统设计](docs/system.md)
- [链路契约](docs/link.md)
- [Composition 契约](docs/composition.md)
- [SDK 设计与开发指南](docs/sdk-guide.md)
- [系统进展与待决策](docs/system-report.md)
- [文档入口](docs/index.md)
