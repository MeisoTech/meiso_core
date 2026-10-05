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

当前 `COMPOSE_/_PROFILE:v1` 有两个可执行 backend：remote/loopback
HostRuntime adapter 投影完整已实现子集；
caller-owned deterministic simulator以显式activate/step实现Wi-Fi、可选DeviceTelemetry State/Stream、
render/camera media State/read，以及显式opt-in的Mesh publish→SceneStore/C2→synthetic offscreen
RenderResult闭环。两者返回同一种opaque handle，application-only TU不读取backend tag。Simulator的
Composition子组件、pending Mesh和媒体buffer均在caller arena内，不读wall clock、不建thread/socket、
不分配heap，也不把recorded sample/frame或synthetic render冒充physical truth；Composition未启用时
Blob/Composition操作明确`unsupported`，DeviceTelemetry未启用时SET_DEVICE同样明确`unsupported`。

同一native Composition Host Bridge现在提供显式`--simulator --interactive`入口，Dashboard以
`--native-host ... --native-simulator`启动真实子进程，并通过与remote模式相同的closed NDJSON命令和
typed snapshot消费上述opaque handle。该模式不读取remote config、不连接control/media TCP、不建立
Session/catalog wire。native simulator显式启用caller-owned CPU raster：Dashboard把模式标为
`NATIVE_SIMULATOR`和`SYNTHETIC_OFFLINE_CPU_RASTER`，除Scene/C2、Wi-Fi/Device及media State外，还接收
一条完整CMFB/v1 320×240 RGBA render preview，并验证payload SHA-256与同world RenderResult一致。
camera route仍只有synthetic State、没有camera frame；`media_bound=false`继续表示没有MediaTransport/TCP
binding。remote模式、其argv和`wire_owner:native_service`保持不变。

Recorded backend artifact使用固定版本、closed-kind、bounded的canonical in-memory trace；它只记录
simulator config、Wi-Fi/media typed desired、显式step/time、完整media unit与terminal，不记录或发明Session/Runtime
wire。reader在暴露config前完整验证header、每record integrity、容量、事件顺序和payload，再以单record
原子cursor驱动fresh simulator。文件或transport I/O继续由caller拥有，trace失败不能吞掉后续record，
recorded truth始终保持synthetic evidence标签。v1要求DeviceTelemetry与Composition simulation均关闭，
`cpu_raster_preview_enabled`也必须为false，所有相关config suffix均须canonical zero，因此新增simulator
能力不会被旧trace静默忽略。

`EdgeHandle` 的公开生命周期绑定一段连续的 Session/catalog truth。Host integration 拥有
连接、event loop、Runtime pump 和 backend replacement；应用只能读取 handle 的
`online / resource-graph-live / terminal`、本地 continuity 与 capability/resource view，
不能取得或驱动 fd、packet、Session、Runtime、catalog wire 或 platform handle。Session
terminal、catalog discontinuity 或 backend replacement 会永久使旧 handle stale；新连接必须
产生新 handle，旧 desired、delivery、actual、Stream cursor、Blob transfer 和 media borrow
均不得复活。

每个 application operation 都由当前 capability 显式声明。第一版 facade 可以不实现某种
State watch、Stream publish、Action 或 Blob direction，但对应 capability 必须缺失，调用面不得
保留静默成功的空操作。成功的本地 command receipt 只证明 SDK 已有界复制/排队；delivery
accepted、Edge actual/effective、Stream sample、Blob publication/visibility 与 Composition
activation仍是彼此独立的可观察阶段。

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

当前 C11 Core 已实现有界 Resource、精确 Profile 握手、一次性 Session、受限 wire 与 POSIX 适配器。
当前两端配对以 [Display Ingress planes-v12](../profiles/display_ingress/planes-v12.md) 为显示契约：
Host 发布空间平面模型，Edge 将串流或静态图贴到平面，并按实际翻页报告呈现证据。
开发台架上，K230D（庐山派）经 DSI→HDMI 转接板出 1080p60 模式，合成器在其中画平面（台架预览）；眼镜光学显示模组尚未接入。
相机与 IMU 在 Edge 采集，Host 消费后可把位姿送回。
断线检测使用 Wi-Fi 链路状态、TCP 关闭和部署的存活探测/超时；本版没有应用层心跳。
参考实现两端的 user timeout 都是 25 s，在此之内的卡顿不算断线（planes-v12 §8）。Edge 的"信号差"提示读控制连接的
TCP 状态：有字节超过 1 s 没被确认就标出，Session 照常，确认回来即恢复。
控制连接卡顿期间，Edge 可以继续显示最后一帧；配置 `correction=IMU` 时还用本地陀螺转动
WORLD 平面。Session 真正结束后
按 Profile 规则撤下平面，重连建立新 Session。

Composition 的通用呈现语义仍有效；旧 Mesh/SceneStore、Coordinator、Composition
Editor/HostBridge/EdgeService 应用栈已从当前显示主线退役，默认关闭，仅显式构建选项可纳入。
当前实现不代表安全 transport、Session resume、通用 discovery/plugin、所有 XR 设备适配或长期性能保证。

当前支持范围和版本配对见[快速开始](host-edge-quickstart.md)与[版本配对](releases.md)；
Core 的实现边界见[实现地图](system-runtime-map.md)，旧阶段报告从[系统状态](system-report.md)追溯。
