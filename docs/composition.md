# Composition 契约

> 状态：产品级技术契约。定义双方语义；具体数据结构和 wire 编码在实现前补充。

## 分层边界

Composition 位于 Runtime--SDK/application integration 层，不进入 Core。Core 只交付通用
State、Stream、Action、Blob 与 Session-bound Runtime payload；Runtime 先验证 accepted catalog、
Resource owner/schema/version/epoch，Composition 才消费 typed Resource。compositor 不解析
Session wire、socket、PeerId 或 transport frame，renderer/decoder/scanout adapter 也不能反向
修改 Core truth。

Host compositor 可以拥有 logical scene、representation planning 与应用 lifecycle；Edge
Composition 仍独占 plan preparation、降级/安全 policy、显示边界激活与 physical presentation
authority。renderer 只返回完整 candidate 或 failure，不产生 `prepared_token`、不激活 plan、
不把完成渲染冒充为 scanout。

## 共同契约，不同职责

Host 与 Edge 理解同一套 Composition contract，但只实现各自声明的能力。

| layer 来源 | Host 职责 | Edge 职责 |
|---|---|---|
| `REMOTE_PROJECTION` | 渲染、编码并提供生成投影所依据的 pose 与 projection | 解码、执行协商出的 correction profile，并完成光学校正与 scanout |
| `EDGE_NATIVE` | 提供应用意图、状态与资源 | 本地生成 layer，并进入相同的最终 compositor |

来源是逐 layer 能力。远端 Projection、本地应用 layer 与 Edge 安全 HUD 可以同时存在。

`REMOTE_PROJECTION` 与 `EDGE_NATIVE` 都是 capability-selected source；Core 不要求设备实现
其中任意一种，也不规定二者的优先级。声明 presentation capability 的 Profile 必须明确
支持的 source 组合；没有 presentation capability 的实现可以两者都不提供。Edge 安全层
不受 Host 遮挡，其优先级由本地 policy 决定。

## Layer 模型

`PresentationPlan` 是有序 layer 集合。每个 layer 声明 source、几何形式、空间绑定、依赖资源和允许的降级范围。

- `Projection` 表达有明确视锥和 render pose 的场景内容。
- `Quad` 表达平面 HUD 或面板；其他曲面类型由 capability 扩展。
- `VIEW` 内容相对当前视图；`WORLD` 内容必须绑定明确的 tracking space、anchor 与 epoch，不存在无来源的绝对“世界坐标”。Edge 根据声明选择修正方式。

framebuffer 是 layer payload，不是 Composition 模型：结合视锥与 render pose 时可表达 `Projection`，结合平面元数据时可表达 `Quad`。通用 `Z distance` 不能表达透视、遮挡和深度相关视差；这些能力需要 depth、多个明确 layer，或 Edge-native scene。即使 `VIEW` layer 不做世界锁定，它仍经过设备光学校正和最终 scanout。

## Composition Resources

| Resource | 真值方 | 形式 | 含义 |
|---|---|---|---|
| `CompositionCapabilities` | Edge | State | 支持的 layer、source、修正方式、格式与资源边界 |
| `PresentationPlan` | Host | State | 完整期望内容、plan generation 与可接受范围 |
| `PresentationStatus` | Edge | State | 实际选择、准备或激活状态及失败原因 |
| application native state | Host | State | Edge-native application layer 的当前 truth |
| immutable asset | Host | Blob | 按 content identity 校验的模型、字体或其他资源 |
| projection stream config | Host | State | codec、尺寸、projection 与 stream epoch |
| projection stream status | Edge | State | decoder 状态、已接受 epoch 与 recovery point 需求 |

连续 projection frame 使用 [MediaStream](resource.md#mediastream)，不能伪装成必须完整送达
的 Blob。Edge 本地安全层由 Edge 拥有，不依赖 Host plan 才能存在。

## 代际与激活

- Edge 的 capability generation 变化后，Host 必须针对新能力重新声明 plan。
- State version 负责 snapshot 新旧关系，plan generation 标识不可变候选；两者不能混用。
- `PresentationPlan` 是完整候选状态。required、preferred 与允许降级的范围必须显式，Edge 不得静默改变语义。
- Edge 准备新 generation 时继续显示旧 generation。capability 只声明静态边界，当前资源是否允许准备或双代持由 `PresentationStatus` 明确接受或拒绝。
- `prepared_token` 绑定 session、capability generation、plan generation、候选摘要和 Edge 选择的精确配置。同一 plan generation 不能更改候选，只能更新激活字段。
- Edge 通过 `PresentationStatus` 返回 token；Host 在新的 plan version 中回显后，Edge 只在显示边界原子激活完整 snapshot。
- Link ACK 只表示 plan 已送达；Edge-owned `PresentationStatus` 只能证明 generation 已准备或由 Edge 激活，不能证明像素已经实际显示。

offline lease 是 duration；Edge 首次接受对应 plan version 时转换为本地 monotonic deadline，
同版本重传不能续期。短暂 transport/Session 中断时，Remote Projection 只可在有界时间内
修正最后可用帧，随后进入本地安全层；Edge Native 只在 lease 内继续运行。

新 session 会使旧 status 副本与 `prepared_token` 失效，Host 必须重新声明 plan。若 Edge incarnation 与 monotonic epoch 未变、active generation 仍驻留内存且 lease 未到期，Edge 可以暂时保留内容，并在新 session 重新发布实际状态；Edge 重启后一律不能从缓存复活。缓存资源可以按 content hash 复用，但不是当前 truth 或授权；Remote Projection 从新的 stream epoch 与 recovery point 恢复。
