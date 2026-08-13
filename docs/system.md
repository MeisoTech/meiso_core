# 系统设计

> 状态：产品设计文档。定义角色、Virtual Edge、Core/Profile 边界与产品不变量；
> Resource、Link 和 Composition 的技术语义分别由对应规范定义。

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

## 系统边界

```text
Host applications / services / frontends
    ↓
Virtual Edge / EdgeHandle
    ↓
Resource Runtime
    ↓
LinkSession
    ↓
Profile-selected transport implementation

Edge Resource Runtime
    ↓
policy / capability / platform adapters
    ↓
actual sensor / media / presentation / device endpoints
```

这是一组责任边界，不要求每层都是独立进程，也不要求所有 payload 经过同一复制或内存
路径。

## Virtual Edge

Virtual Edge 是 Host 侧对远端或模拟 Edge Resource Graph 的本地、版本化、能力受限投影。
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

## Core、Capability 与 Profile

`Meiso Core` 只稳定：

- Host/Edge 角色与 truth ownership；
- Virtual Edge 的显式异步边界；
- `State / Stream / Action / Blob` 语义；
- identity、Session、time、failure 与 bounded-resource 规则；
- Composition 的 layer、generation、激活和 Edge final authority。

runtime capability 声明当前实际存在的 sensor、media、display、codec、input、compute 或
其他 endpoint。Core 不为 camera、IMU、microphone、某块开发板或某个 frontend 建立特例。

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

应用只使用 Virtual Edge、Resource 与 Composition，不拥有 socket、port、packet 或设备
网卡。LinkSession 负责 peer/session identity、time、delivery/failure 与 Resource 可见性；
其公开语义不依赖具体 transport。

transport 的类型、拓扑、数量和切换能力由 Profile/实现决定。本版 Core 不承诺 binding
cardinality、运行中 transport migration 或通用 transport-topology scheduler。完整契约见
[Resource 契约](resource.md)和[Link 契约](link.md)。

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

这是 Resource semantic core，不是 application-facing Resource Runtime，也不是本页所定义的
完整 Meiso Core。Virtual Edge、Link、Composition、MediaStream、capability discovery、
platform adapter 与产品级 target runtime 尚未实现；Resource semantic core v0 的准确
conformance image 已在 K230D 上人工执行并通过，但这不实现上述缺失层。
`meiso_latest_state` 和 `meiso_slice` 仍只作为 legacy evidence，不享有兼容承诺。实现状态和
平台证据见[系统进展](system-report.md)。
