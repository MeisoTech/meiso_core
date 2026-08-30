# Composition Profile

> 状态：`IMPLEMENTED / HOSTED VERIFIED / FRESH K230D E2E PENDING`。本页只定义
> 当前正式 Profile 的 exact identity、固定 Resource Graph、容量和验收边界。
> 通用语义由 [Resource](../../docs/resource.md)、[Runtime](../../docs/runtime.md)、
> [Link](../../docs/link.md) 和 [Composition](../../docs/composition.md) 拥有。正式化前的
> Profile 分版与逐 commit 证据已[归档](../../docs/archive/composition-profile-pre-formal-history.md)。

## Exact selection

| 项目 | 当前值 |
|---|---|
| ProfileRef | `COMPOSE_/_PROFILE:v1` |
| Session wire | `MSL0 v1` |
| Runtime wire | `MRT0 v4` |
| catalog | 6 endpoints / 18 routes / 2032 bytes |
| Session limits | frame 8236 B / Runtime payload 8192 B / registry 24 |
| Runtime limits | endpoint history 12 / capability bytes 276 |

Profile 和 wire 必须 exact match；不支持 fallback、子集 catalog 或隐式升级。
MRT0 v2/v3 仅作为内部协议 fixture 的兼容边界，不是当前 Composition
Profile 的连接路径。

## Resource Graph

Catalog 是一个原子 snapshot。全部 identifier 都是两个 8-byte packed word；
`\\0` 表示 identifier 中的实际 NUL byte，不是文档缩写。

| endpoint | routes | delivery |
|---|---|---|
| `WIFI_TLM/ENDPOINT` | `DES_STAT` Host State; `ACT_STAT` Edge State; `DATA_STR` Edge Stream | inline / inline / inline |
| `COMPOSE_/PRESENT_` | `PLAN_DES` Host State; `PLAN_ACT` Edge State | inline / inline |
| `COMPOSE_/SCENE___` | `SCN_DES\0` Host State; `SCN_ACT\0` Edge State; `MESH_BLB` Host Blob; `RES_STR\0` Edge Stream | all inline |
| `DEVICE_T/ENDPOINT` | `DES_STAT` Host State; `ACT_STAT` Edge State; `DATA_STR` Edge Stream | inline / inline / inline |
| `RENDER_M/EDIA_END` | `DES_STAT` Host State; `ACT_STAT` Edge State; `DATA_MED` Edge MediaStream | inline / inline / external |
| `CAMERA_M/EDIA_END` | `DES_STAT` Host State; `ACT_STAT` Edge State; `DATA_MED` Edge MediaStream | inline / inline / external |

Wi-Fi 使用 `WIFI_TLM/_PROFILE:v1`，DeviceTelemetry 使用
`DEVICE_T/_PROFILE:v1`。它们的独立 ProfileRef 定义可复用的 schema；
Composition Session 只协商上表的单一 `COMPOSE_/_PROFILE:v1`。

Schema namespace 是 `WIFI_SCH`、`COMP_SCH`、`DEVICE_S` 和 `MEDIA_SC`；
schema version 均为 1。Catalog builder 必须检查 endpoint、ResourceId 和 schema
collision，失败不得部分修改输出。

## Scene 与 presentation

Host 通过 immutable Mesh Blob、完整 `SceneDesired` snapshot 和
`PresentationPlan` 表达一个 Edge-native projection layer。当前 capability 固定：

- 320×240 RGBA8888 + depth32 off-screen target，最高 10 Hz；
- 最多 32 objects、8 unique meshes、单 Blob 256 KiB；
- 16 published Blob slots、1 staging transfer、4096-byte Blob chunk；
- 2 MiB per-world 与 4 MiB active+prepared mesh budget；
- 2 target sets、2 jobs、4 RenderResult slots，64×48 RGB565 thumbnail。

Mesh、scene、plan、status 和 RenderResult 都是 canonical big-endian codec；不以
`sizeof(C struct)` 决定 wire。数值、reserved byte、大小、ContentId 和
owner/version 任一不合法都整体拒绝。

SceneStore 只在 Blob 完整校验并对当前 owner scope 可见后准备新 world。
Coordinator 拥有 plan prepare、token echo 和 frame-boundary 激活；renderer 只能返回
完整 render 结果或失败，不生成 token、不激活 plan、不声称物理 present。

`RenderResult` 是 6304-byte inline Stream sample，包含 applied scene owner/version、
plan/world generation、完整 target SHA-256 和 thumbnail。它不是 framebuffer 交付、
fence、vblank 或 panel 证据。

Schema/codec/catalog 位于本目录；SceneStore、Coordinator 与 renderer 实现位于
[`composition/`](../../composition/)。

## Telemetry 与 media

Wi-Fi 和 DeviceTelemetry 各有独立 desired/actual/production epoch/Stream sequence。
其中 Wi-Fi 的对外语义名为 `wifi-telemetry`；DeviceTelemetry 只暴露
Profile 声明且 adapter 真实可读的 temperature/load/memory 字段，不暴露
locator、MAC、SSID、credential 或驱动路径。

RenderMedia 和 CameraMedia 是两个独立 endpoint：

- RenderMedia 返回完整 320×240 RGBA8888 off-screen access unit，必须绑定
  applied scene、active plan 和 world；
- CameraMedia 返回 physical camera 的真实 access unit；没有 camera 时必须
  `ABSENT/UNAVAILABLE/FAILED`，不得以 synthetic/render frame 代替；
- 当前 codec 为 RAW，单 fragment 上限 4096 bytes；render 单帧上限
  307200 bytes，camera 单帧上限 460800 bytes；
- `active_route_mask` 必须显式为 render、camera 或 both；0 和未知 bit
  是 `INVALID_CONFIG`；
- 每路只有 in-flight/latest 与 reassembly/latest 的有界 slot，两路按
  fragment opportunity 持久 round-robin；慢消费者只能触发整帧替换和可观测 drop/gap。

完整媒体合同见 [Direct-IP MediaStream](../../docs/media-stream-direct-ip.md)。

## Connection lifecycle

Control 和 media 均使用来自本地配置的 numeric direct-IP plain TCP locator；
locator 不是 identity。Control Session 先到 READY，完整 catalog 接受且 media
desired/actual RUNNING 后，media 才能按 exact SessionId、catalog generation、route、
endpoint incarnation、production/media epoch 和 nonce 执行 OPEN/BIND。

EOF、RST、protocol failure、catalog replacement、endpoint replacement 或 production epoch
变化使旧 binding 永久失效。新 TCP 必须创建新 Session、arena、Runtime、
handle 和 media transport；不 resume 旧 transfer、desired、token、sequence 或 view。
当前没有 authentication、encryption、discovery 或协商心跳。

## Implementation and evidence

正式 public entry 是 `<meiso/profiles/composition/profile.h>` 和
`<meiso/profiles/composition/media_transport.h>`；CMake target 是
`Meiso::CompositionProfile`。Host 使用 `CompositionHostRuntime/CompositionRemoteEdge`，
K230D 使用 `K230dCompositionEdgeRuntime/K230dCompositionEdgeService`，本地工具位于
[`devtools/web/composition_editor/`](../../devtools/web/composition_editor/)。

portable codec、bounded lifecycle、simulator、POSIX/AF_INET 与 Host/Edge 组合路径已有
测试证据。当前正式身份的 fresh K230D cross + board E2E 仍待本轮收口；
旧身份的实机记录只作历史。具体门见
[K230D Composition E2E](../../docs/k230d-composition-e2e.md)。

即使 fresh board gate 通过，也只证明指定 K230D/GC2093、源码、工具链和
受控网络中的 CPU off-screen/render-camera-media 链路；不证明 physical display、
fence/vblank、codec、security、performance、power 或 soak。
