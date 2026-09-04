# Composition 契约

> 状态：产品级技术契约。定义双方语义；具体数据结构、容量和wire编码由
> 版本化Profile/实现合同冻结，不反向改写本契约。

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

呈现计划是有序 layer 集合。每个 layer 声明 source、几何形式、空间绑定、依赖资源和允许的降级范围。支持动态、多 layer 或 prepared handoff 的 Profile 使用显式
`PresentationPlan` 与 plan generation；固定单 layer 的 direct-latest Profile 可以由 Profile
identity 加完整配置 State 定义同一语义，不需要再造一个 plan generation。

- `Projection` 表达有明确视锥和 render pose 的场景内容。
- `Quad` 表达平面 HUD 或面板；其他曲面类型由 capability 扩展。
- `VIEW` 内容相对当前视图；`WORLD` 内容必须绑定明确的 tracking space、anchor 与 epoch，不存在无来源的绝对“世界坐标”。Edge 根据声明选择修正方式。

framebuffer 是 layer payload，不是 Composition 模型：结合视锥与 render pose 时可表达 `Projection`，结合平面元数据时可表达 `Quad`。通用 `Z distance` 不能表达透视、遮挡和深度相关视差；这些能力需要 depth、多个明确 layer，或 Edge-native scene。即使 `VIEW` layer 不做世界锁定，它仍经过设备光学校正和最终 scanout。

声明 presentation capability 的 Profile 必须把每条 MediaStream 关联到明确的 layer
source、几何形式和空间绑定。一个只支持单张固定全屏 `VIEW Quad` 的窄 Profile，可以把
这组不变的 layer 语义写进 Profile identity，并让它的配置 Desired 成为该固定 layer 的
完整当前计划；这不允许实现把任意 framebuffer 当作没有 Composition 含义的像素入口。

## Composition Resources

| Resource | 真值方 | 形式 | 含义 |
|---|---|---|---|
| `CompositionCapabilities` | Edge | State | 支持的 layer、source、修正方式、格式与资源边界 |
| `PresentationPlan` | Host | State | 动态/多 layer Profile 的完整期望内容、plan generation 与可接受范围 |
| `PresentationStatus` | Edge | State | 实际选择、准备或激活状态及失败原因 |
| application native state | Host | State | Edge-native application layer 的当前 truth |
| immutable asset | Host | Blob | 按 content identity 校验的模型、字体或其他资源 |
| projection stream config | Host | State | codec、尺寸、projection 与 stream epoch |
| projection stream status | Edge | State | decoder 状态、已接受 epoch 与 recovery point 需求 |

连续 projection frame 使用 [MediaStream](resource.md#mediastream)，不能伪装成必须完整送达
的 Blob。Edge 本地安全层由 Edge 拥有，不依赖 Host plan 才能存在。

### Host preview 与 physical camera

Profile可以另行声明Edge-owned `RenderPreviewMedia`，把已完成的 off-screen render
作为有界 MediaStream 回传 Host 用于本地观测。它必须绑定applied scene State、active plan/world与独立production/
stream epoch；它不是PresentationStatus、RenderResult小thumbnail、display scanout或physical capture，
也不能设置未观察到的fence/commit/vblank/physical evidence。

physical camera是独立Runtime endpoint，其desired/actual/MediaStream与render preview分别协商和启停。
`SceneDesired.camera`只表示Edge-native renderer的viewpoint/projection，不声明physical camera、capture
format或sensor availability。缺少camera capability时必须truthfully unavailable，不能用render frame或
synthetic source替代。

当前 `COMPOSE_/_PROFILE:v1` 为本地 editor workload 选择上述两种
Edge→Host media endpoint，并保持 Wi-Fi、DeviceTelemetry、Presentation、Scene 与 media
route 在同一原子 catalog。Control 仍使用 State/Blob/小 Stream；持续 frame
通过独立但 Session-bound 的数据面，详见
[Direct-IP MediaStream](media-stream-direct-ip.md)。

## 代际与激活

- Edge 的 capability generation 变化后，Host 必须针对新能力重新声明 Profile 所选的完整
  plan 或固定 layer 配置。
- State version 负责 snapshot 新旧关系；选择显式 `PresentationPlan` 的 Profile 另用 plan
  generation 标识不可变候选，两者不能混用。固定 direct-latest layer 只使用其配置 State
  owner/version，不制造无对应对象的 plan generation。
- 动态 `PresentationPlan` 或固定 layer 配置都必须是自己的完整候选状态。required、
  preferred 与允许降级的范围必须显式，Edge 不得静默改变语义。
- Edge 验证新候选或新配置时继续显示旧的已应用内容。capability 只声明静态边界，当前
  资源是否允许准备、双代持或 latest 替换由 `PresentationStatus` 明确接受或拒绝。

Profile 必须从下列两种激活模式中作出精确选择；不能让实现自行猜测：

- **direct latest**：适合单一、可整体替换且允许跳过中间版本的 layer。Host 发布完整的
  plan/configuration State；Edge 验证后保留旧显示，直到本地安全显示边界直接应用最新
  合法值。它没有 `prepared_token` 或 Host echo。fresh Session 或新的 media production
  epoch 仍遵守 Runtime 的 RUNNING Actual 数据门；门打开后的普通帧不再等待逐帧或普通
  配置 Actual。未应用的旧配置 State 可以被更新配置取代；门内 MediaStream frame 使用
  自己的 epoch/sequence/deadline 和有界 latest 规则，在本地显示边界选择最新完整帧。配置
  State 与 frame 的版本/序号不能互相替代。
- **prepared handoff**：适合必须跨多个资源保持原子一致、且 Profile 明确需要 Host 再确认
  Edge 精确选择的候选。`prepared_token` 绑定 session、capability generation、plan
  generation、候选摘要和 Edge 选择的配置；Host 在新的 plan version 中回显后，Edge 才在
  显示边界激活。选择本模式的 Profile 必须定义 token 失效、重放、替换和断线行为。

当前 `COMPOSE_/_PROFILE:v1` 选择 prepared handoff；其他 Profile 不因复用 Composition
语义而自动继承 token/echo。两种模式下，Link ACK 都只表示 plan 已送达；Edge-owned
`PresentationStatus` 只能证明已准备或已激活，不能证明像素已经实际显示。物理呈现仍需
独立的 fence/page-flip/vblank 类证据。

若 Profile 明确允许断线后继续 Remote Projection 或 Edge Native，它必须选择有界 offline
lease。lease 是 duration；Edge 首次接受对应 plan version 时转换为本地 monotonic deadline，
同版本重传不能续期。短暂 transport/Session 中断时，只可在该本地 deadline 前继续，随后
进入 Profile 声明的本地安全层。没有选择 offline continuation 的 Profile 在 Session 终止时
立即使旧 remote production binding 失效，不能凭缓存自行恢复授权。

新 session 会使旧 status 副本以及 prepared handoff 模式中的 `prepared_token` 失效，Host
必须重新声明 plan 或固定 layer 配置。对选择 offline lease 的 Profile，若 Edge incarnation
与 monotonic epoch 未变、active plan generation 或 fixed-layer config owner/version 仍驻留
内存且 lease 未到期，Edge 可以暂时保留内容，并在新 session 重新发布实际状态；Edge
重启后一律不能从缓存复活。缓存资源可以按 content hash 复用，但不是当前 truth 或授权；
Remote Projection 从新的 stream epoch 与 recovery point 恢复。
