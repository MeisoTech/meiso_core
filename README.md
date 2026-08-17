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
`OPEN/ACCEPT/CONFIRM/READY`、bounded wire 与 Runtime-message gate。命名的 Session
状态机、wire 与 four-kind integration tests 支持
`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED`。

当前 portable `meiso_runtime` 已实现 State/Stream-driven negotiation source v0 与 wire v2：
它永久绑定 READY Session，原子接受 endpoint catalog，并以有界且不驱逐的 identity history
保留 production epoch continuity、防止 retired pair 复活；desired/actual 作为 Core State、data
作为 Core Stream 传递。State receive candidate 必须逐字节匹配同一次 `StateReplica` commit，
只有被接受的 actual 才能开放 production epoch gate。三套 Host test 支持当前
`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED`，不构成 transport 或 target evidence。

POSIX build 另含 platform-layer connected-stream adapter。它借用 already-connected、
nonblocking `SOCK_STREAM` fd，以 caller-owned bounded RX/TX buffer 对接完整 Session frame；
真实 macOS `AF_INET` loopback TCP unit test 与 Dev Tool 支持该条件下的
`TRANSPORT_VERIFIED`。这不证明 remote/direct-IP、Wi-Fi、K230D 或安全 transport。

首个正式 Runtime 垂直切片已选择 `wifi-telemetry-v0`：本机真实 `AF_INET` 测试贯通 Core
READY、catalog、Host desired State、Edge actual State、production gate 与 Core Stream；macOS
Host Dev Tool 和 K230D Linux Edge target 使用 numeric direct-IP 私有配置；同一路径已在
K230D Linux 与 macOS 间验证单一 Core Session 内的启动、运行中重配置、停止与正常关闭。
native Host 的 `--interactive` 模式在同一 READY Session 中以
`apply(enabled=true/false)` 和 `close` 实现启动、停止与关闭；
desired/actual 都是 Core State，数据是 Core Stream，不存在 subscription control plane。
构建、运行与严格证据边界见
[Direct-IP Wi-Fi Runtime v0 实装规则书](docs/wifi-runtime-direct-ip-v0.md)。
同一正式 Host 可由 loopback-only
[Wi-Fi Runtime dashboard](devtools/web/wifi_runtime_dashboard/README.md) 启动并可视化；Web 只把
`connect/apply/stop/close` 固定意图交给 native Host，并投影 Host 已校验的脱敏事件。
它不解析 wire，不实现 Runtime/SDK/Virtual Edge，也不是 subscription 或第二套
协议 truth。

这套实现仍不是 application-facing 完整产品 Runtime 或产品系统；它不包含 endpoint
facet/direction/QoS/permission schema、真实 probe/hotplug adapter、typed dispatcher、Virtual
Edge/SDK、Composition Runtime、MediaStream data path、通用 discovery/reconnect/service、认证
或安全 transport。所选 Wi-Fi target 的 config/probe 与 connect/listen 是 platform slice，不进入
portable Core。
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
- [Runtime 契约](docs/runtime.md)
- [Link 契约](docs/link.md)
- [Session Core v0 实现契约](docs/session-core-v0.md)
- [Runtime negotiation core v0 实现契约](docs/runtime-core-v0.md)
- [Wi-Fi telemetry Profile v0](profiles/wifi-telemetry-v0/README.md)
- [Direct-IP Wi-Fi Runtime v0 实装规则书](docs/wifi-runtime-direct-ip-v0.md)
- [Wi-Fi Runtime dashboard](devtools/web/wifi_runtime_dashboard/README.md)
- [Composition 契约](docs/composition.md)
- [SDK 设计与开发指南](docs/sdk-guide.md)
- [Resource semantic core v0 实现与验证基线](docs/resource-core-v0.md)
- [系统进展](docs/system-report.md)
- [Host/Edge 并行开发模型](docs/development-model.md)
- [K230D Linux Edge 开发基线](docs/k230d-linux-edge-development-baseline.md)
