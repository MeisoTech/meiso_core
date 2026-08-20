# 系统设计

> 状态：产品设计文档。定义角色、Core/Runtime/SDK/Profile 边界与产品不变量；
> Resource、Runtime、Link 和 Composition 的技术语义分别由对应规范定义。

## 目标

Meiso Glass 面向分体式 XR/AR：把受重量、功耗和散热约束的设备端与外部计算环境分开，
通过开放的 Resource 与 Composition 契约形成可替换实现和可验证的应用入口。

系统不绑定某颗 SoC、开发板、sensor、操作系统、runtime、图形 API、frontend、系统镜像
或 transport。更换实现时，公开业务语义不随之重写。

## 两个公开角色

| 角色 | 职责 |
|---|---|
| `Edge` | 拥有实际设备 truth、platform adapter、本地时间/安全行为，以及存在相应 capability 时的最终物理输出 |
| `Host` | 运行应用和重计算，按 ownership 发布或消费声明的 Resource，并提出 presentation intent |

Host 与 Edge 是协议角色，不是 CPU core、进程、板卡或操作系统。Host Runtime、daemon、
CLI、Dashboard、simulator 和调试工具只是角色的实现或包装，不构成第三个公开角色。

一个物理 Edge 可以由多个 core、OS、进程或固件域共同实现，但它对 Host 仍暴露
一个受控的 Edge identity、capability 和 Resource Graph。内部 IPC 不自动构成新的
Host/Edge 关系或公开 LinkSession。内部 runtime 重启或 bridge continuity 无法证明时，
必须按受影响 Resource 的 lifecycle 与 identity 规则显式暴露 discontinuity，不得用旧
缓存伪装连续。

## 系统边界

```text
Host applications / services / frontends
    ↓
Host SDK: Virtual Edge / EdgeHandle
    ↓
Host Runtime: Resource Graph / policy / control
    ↓
Host Core: identity / Session / Resource delivery
    ↕ Profile-selected transport implementation
Edge Core: identity / Session / Resource delivery
    ↑
Edge Runtime: Resource Graph / policy / control
    ↑
capability / platform adapters / actual endpoints

```

这是一组责任边界，不要求每层都是独立进程，也不要求所有 payload 经过同一复制或内存
路径。

## Virtual Edge

Virtual Edge 是 Host SDK 对远端或模拟 Edge Resource Graph 的本地、版本化、能力受限投影。
应用通过 `EdgeHandle` 一类 facade 发现 capability、读取 State、订阅 Stream、调用 Action
和引用 Blob。

Virtual Edge 不是透明远程内存：

- local submit、Link 接受、peer online 和 actual application 是不同状态；
- last-known State 必须带 freshness/session 信息，不能伪装成当前在线 truth；
- Stream/Action 依赖 live Session，离线时必须按声明失败或结束；
- simulator、in-process loopback、recorded replay 和 physical Edge 可以实现同一 facade，
  但各自证据范围不同。

## 系统不变量

- 每个同步事实只有一个 truth owner；replica 不与 owner 竞争写入。
- Host desired 与 Edge actual 分开；transport ACK 不能证明物理应用或 Action 成功。
- capability 描述 runtime 真实支持的 endpoint、format、limits 和 behavior；缺少能力时
  显式 unavailable/rejected，不生成假数据。
- 所有 queue、history、reassembly、retry、cache 与 offline behavior 有界且可观察。
- Link、Host application 或模型失败不能阻塞 Edge 已声明的本地实时与安全路径。
- network receive 不能直接修改正在使用的物理状态；新 snapshot/generation 完整准备后
  才能在对应边界激活。
- session、boot/clock epoch、resource identity 和 stream epoch 不互相替代。

## Core、Runtime、SDK 与 Profile

`Meiso Core` 拥有跨端协议内核：

- Host/Edge 角色、peer/boot/Session identity 与精确 protocol/Profile；
- connection、handshake、Session lifecycle、liveness、failure 与 bounded limits；
- 版本化 wire 与 `State / Stream / Action / Blob` 的 transport-independent delivery
  primitive 和语义。

Runtime 拥有 Resource Graph：Edge 从私有配置、adapter 实测、policy 和 Profile 交集生成
catalog；Host 接受 catalog 后以 Core State 发布 desired，Edge 做 admission、adapter apply，
再以 Core State 发布 actual/effective status。sensor data 使用 Core Stream；Runtime 不另建
subscription 或 desired/actual wire。只有当前 Session/catalog/endpoint incarnation 与 actual
production state 下已激活的 route 才能传输 Resource data。Core 不声明 sensor，不为 camera、
IMU、microphone、某块开发板或某个 frontend 建立特例。Core `READY` 只允许
Runtime 交换；它不表示 catalog 已接受或任何 endpoint 已 `READY`。完整契约见
[Runtime 契约](runtime.md)。

Host SDK 拥有 Virtual Edge、typed async API 与 application view；它不拥有 socket、
handshake 或物理设备控制。Composition 是 SDK/Runtime 消费的产品契约，不是 Session
Core 的连接职责。

Profile 选择一组已验证的 Host/Edge implementation、capability、Resource/QoS、transport
arrangement 和 Composition behavior。系统只保证声明并验证过的组合，不承诺任意组合的
笛卡尔积。

规范不规定 native、OpenXR、headless、camera、display、codec、network 或其他组件的
实现先后。先后属于工作计划；runtime 与 Profile 只声明最终可用组合和行为。

## Composition 边界

内容在哪里生成，与内容如何锚定和激活是不同维度。每个 layer 可以使用
`REMOTE_PROJECTION` source，也可以使用 `EDGE_NATIVE` source；可用 source 由 capability
声明，多个来源可以共存。

当 Edge 提供 presentation capability 时，所有 layer 都进入 Edge 拥有的最终激活、
显示时钟和安全 policy。GPU、fixed-function、display plane 或软件 compositor 是实现细节。
完整契约见 [Composition 契约](composition.md)。

## 通信边界

应用只使用 SDK 的 Virtual Edge、Resource 与 Composition，不拥有 socket、port、
packet 或设备网卡。Core LinkSession 负责 peer/session identity、time 与
delivery/failure；Resource 可见性和 endpoint lifecycle 属于 Runtime。

transport 的类型、拓扑、数量和切换能力由 Profile/实现决定。本版 Core 不承诺 binding
cardinality、运行中 transport migration 或通用 transport-topology scheduler。完整契约见
[Resource 契约](resource.md)、[Runtime 契约](runtime.md)和[Link 契约](link.md)。

## 非目标

Core 当前不定义：

- 通用 distributed transaction、Action DAG 或 exactly-once workflow engine；
- 任意 QoS 组合、通用 carrier scheduler 或自制密码系统；
- OS daemon layout、IPC、buffer API、driver ABI、system image 或 provisioning 策略；
- ML/VIO/SLAM、OpenXR、Wayland、Android 或应用框架内部架构；
- 固定 sensor 集合、设备 Profile 或开发顺序。

## 当前实现范围

当前公开 C11 实现已经包含 `meiso_resource_core`：共同值类型与 descriptor validation、
固定 immutable descriptor snapshot，以及四种 bounded in-process Resource primitive。

这是 Resource semantic core，不是本页所定义的完整产品系统。当前 `meiso_session_core`
已实现 exact protocol/Profile、四步 handshake、
bounded wire 和 Runtime-message gate；命名的状态机/wire/integration tests 支持
`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED`。POSIX platform layer 另有 caller-owned
connected-stream adapter，并在 macOS `AF_INET` loopback TCP 上通过 bounded partial-I/O、
failure 与双向 payload 测试。当前 portable `meiso_runtime` 已实现 READY Session 绑定、原子
catalog、bounded endpoint-incarnation history、Core State desired/actual exact commit gate 与
Core Stream data route；source v0 / wire v2 的三套 Host test 支持
`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED`。所选 `wifi-telemetry-v0` slice 另有 K230D Linux
probe/adapter、numeric direct-IP target 与一次受控 board run；这些是 Profile/platform evidence，
不是portable Runtime的通用能力。通用hotplug、Action/MediaStream、Edge→Host/并行/resumable Blob
route、SDK/Virtual Edge、discovery/reconnect与security adapter仍未完成。R2a另有Composition Profile v2
专用的caller-owned fair scheduler；它是Runtime--SDK对象，不是Core/wire或通用产品QoS。
`composition-scene-lab-v0` 已分层完成 C0 codec、C1 SceneStore、C2 coordinator 与 R1 portable
Blob carriage。Profile v1 保留 MRT0 v2，明确拒绝 Blob/IMMUTABLE route；Profile v2 显式选择
MRT0 v3 并允许 Host→Edge Blob；R2a进一步完成exact 3/9/920 combined builder与portable scheduler。
R2b0又在真实`127.0.0.1` AF_INET `SOCK_STREAM`上用双方nonblocking POSIX adapter、17-byte I/O与双方
scheduler贯通Blob→Core publish→显式visibility、Scene/Plan→C2 ACTIVE及两条exact Stream route，形成
loopback happy-path semantic/wire/transport evidence。R2b0自身不覆盖wrong Profile、catalog replacement、
reconnect、backpressure、fair-contention stress、Host editor或K230D renderer。R2b1又在同一真实TCP
Session内保持Plan与cube publication不变，完成TRS、camera、共享mesh新增对象、删除对象、actual
epoch cut、drained disable及独立Wi-Fi continuation；R2b1自身不覆盖catalog replacement、RST/reconnect、
max-Blob与target。R2b2又在同一Session内完成catalog g1 staged→atomic g2 accept、全route scheduler
rebind、Runtime gate reset/high-water preservation、capability-generation-2 coordinator scope/visibility/
world重建及Blob cache duplicate revalidation。R2b3又以真实RST使旧Session/Runtime/Scheduler/route/
epoch fail closed；SceneStore旧world保持到不同SessionId/新arena连接上的新coordinator开始control scope，
此时才清world/visibility并使旧coordinator/token/world失效。随后显式重发State、重建visibility/world/
gate；同process Core cache duplicate可复用。该结果不是自动resume或fresh Edge，
max-Blob、wrong Profile、backpressure与target仍未覆盖。
Resource semantic core v0 的准确 conformance image 已在 K230D
上人工执行并通过，但这不证明 Session、TCP 或安全。实现状态和平台证据见
[系统进展](system-report.md)；实现边界见 [Session Core v0](session-core-v0.md)与
[Runtime negotiation core v0](runtime-core-v0.md)。
