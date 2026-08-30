# Direct-IP MediaStream v0 开发历史

> 状态：`HISTORICAL / NON-AUTHORITATIVE`。本页保留旧 Profile 身份和
> 当时验收分片；当前合同只读[Direct-IP MediaStream](../media-stream-direct-ip.md)。

> 状态：`IMPLEMENTED / PORTABLE + AF_INET + BOUNDED K230D EVIDENCE`。本文为
> Composition scene Profile v3 选择
> control/media transport、endpoint bundle、lifecycle 与验收边界；它不是新的 Core Resource，
> 也不提升任何 Host、K230D 或 physical-output evidence。通用语义由[系统](../system.md)、
> [Resource](../resource.md)、[Runtime](../runtime.md)、[Link](../link.md)和
> [Composition](../composition.md)拥有。

## 1. 问题与非目标

本切片要让一个本机 Host editor 与统一 debug Web 在同一个 Edge scope 下完成：

- 通过 Mesh Blob 与完整 Scene State 创建、移动、缩放、删除对象并修改 render camera；
- 独立启停 Wi-Fi、设备 telemetry、render preview 和 physical camera producer；
- 以 numeric direct-IP 连接 K230D，持续接收小 telemetry 与两路有界视频；
- 让媒体拥塞、慢浏览器或单个 producer failure 不阻塞 Core control、actual State 或另一 endpoint。

它不定义 discovery、自动 reconnect/resume、认证、加密、通用媒体协商、任意 codec、物理
scanout 或产品 SDK。`RenderPreviewMedia` 是 off-screen debug observation，不是 panel capture；
`CameraMedia` 是 physical camera endpoint，不是 `SceneDesired.camera` 中的 render viewpoint。

## 2. Profile 与兼容边界

Composition scene Profile v1/v2 的 ProfileRef、MRT0 v2/v3 wire、3 endpoint/9 route catalog、
codec、limits 与 evidence 全部保持 byte-exact。实现不得向旧 Profile 添加 endpoint、route、
delivery class 或 fallback。

新组合使用 `K230_SCN/_LAB_V00:v3`，控制面选择 `MRT0 v4`。v4 只为 catalog route 增加明确的
`INLINE_RUNTIME / EXTERNAL_MEDIA` delivery class 与 external media unit limit；State、普通 Stream、
Blob、Session 与 Core frame 的现有语义不改变。Profile v3 不得降级到 v2/v3 Runtime wire，任一端
不支持 exact Profile/wire 时在 handshake fail closed。

Profile v3 的完整 catalog 固定为 6 endpoints / 18 routes，selected Session
`max_registry_entries=24`。exact identifier、schema version、capability bytes、catalog bytes、Session
limits 与媒体 frame offsets 必须在 G0 codec vector 中冻结，不能由 C struct layout 或 target
运行时临时决定。

| endpoint | routes | 复用或新增 |
|---|---:|---|
| `WIFI_TLM/ENDPOINT` | desired / actual / data = 3 | 原 Wi-Fi Profile，`INLINE_RUNTIME` |
| `PRESENT_` | desired / actual = 2 | 原 Composition Profile |
| `SCENE___` | desired / actual / Mesh Blob / RenderResult = 4 | 原 Composition Profile |
| `DEVICE_T/ENDPOINT` | desired / actual / data = 3 | DeviceTelemetry v1，`INLINE_RUNTIME` |
| `RENDER_MEDIA` | desired / actual / data = 3 | 新 RenderPreviewMedia v1，data 为 `EXTERNAL_MEDIA` |
| `CAMERA_MEDIA` | desired / actual / data = 3 | 新 CameraMedia v1，data 为 `EXTERNAL_MEDIA` |

旧 identifiers 必须保持不变；三个新 endpoint 使用独立 namespace，并与 Wi-Fi/Composition 的
endpoint、ResourceId、SchemaId 做完整 collision check。catalog 是一个原子 snapshot；Host 不能
只接受媒体或 telemetry 子集。

DeviceTelemetry 使用 `DEVICE_T/_PROFILE:v1`。Wi-Fi、Device 与两路 media 的 schema
版本均为 1；schema low word 使用语义 identity：capability=`CAPABIL_`、desired=`DESIRED_`、
actual=`ACTUAL__`、telemetry sample=`SAMPLE__`、media access unit=`UNIT____`。对应 schema
namespace 分别为 `WIFI_SCH`、`DEVICE_S` 与 `MEDIA_SC`，不从 low word 编码历史版本号。

## 3. 新 endpoint 语义

### DeviceTelemetry v1

capability 是 Edge 配置、真实 adapter probe、policy 与资源预算的交集，声明支持的 field mask、
period range/step、单位和 presence 规则。首版字段只可从以下类别选择真实可读项：on-die
temperature及其 uncertainty、CPU/load、memory。serial、MAC、IP、SSID、filesystem path、driver
locator 与 credential 不得进入 capability、State 或 Stream。

Host desired 是 `enabled + selected_fields + sample_period_ms`；Edge actual 使用 Runtime canonical
prefix 报告 observed/applied desired、availability、effective selection/period、production status 与
epoch。data 是小型 Edge-owned Core Stream，使用 selected/present masks；缺失值用 absence 表示，
不得伪造为零。Composition 的 Blob/world/target/job/result queue 账本继续由 `SceneStatus` 拥有，
DeviceTelemetry 不复制第二份 truth。

旧 four-kind lab 的 thermal probe、vendor conversion、bounded preflight 和 uncertainty 规则可以作为
adapter 输入，但它的私有 lab wire、combined State 或 socket transcript不进入本 Profile。

### RenderPreviewMedia v1

capability 声明真实 backend 可产出的 format/codec、extent、cadence、maximum access-unit bytes、
queue/recovery limits。Host desired 只控制 preview producer 的 enabled、mode/format/codec、extent、
cadence、bitrate/GOP/recovery intent；render pose 与 projection 仍由 `SceneDesired.camera` 唯一拥有。

Edge actual 报告 effective media parameters、availability、apply result、production status/epoch、
media stream epoch、channel nonce、queue/drop/recovery status。每个 frame 还必须绑定 applied
SceneDesired owner/version、active plan generation 与 world generation。Preview 完成只证明对应
off-screen frame；它不设置 `DISPLAY_COMMIT_ACCEPTED / VBLANK_OBSERVED / PHYSICAL_VERIFIED`。

现有 6304-byte `RenderResult` 仍是 <=10 Hz 的 metadata + 64x48 RGB565 Core Stream，可与 preview
并存；不得扩大它、把媒体 fragment 当作 RenderResult sample，或用它替代 MediaStream gap。

### CameraMedia v1

capability 只声明 adapter 真实探测到且 policy 允许的 physical source/mode，包括 format/codec、
extent、cadence、maximum access-unit bytes、clock/recovery 与 buffer limits。desired/actual 的控制形状
与 RenderPreviewMedia 相同，但 identity、owner、production epoch、stream epoch、queue 和 failure
完全独立。

camera 不存在、模式不可用或 capture continuity 丢失时，actual 必须明确 `ABSENT`、`UNAVAILABLE`、
`REJECTED`或`FAILED`；不能注册假 producer 或输出 synthetic frame 冒充 physical evidence。camera
frame 不携带或替代 render-camera truth。

## 4. Control plane 与 media plane

Core control 使用一个由 ignored local config 指定的 numeric IPv4/TCP 连接，继续承载 Session、
catalog、State、普通 Stream、Blob 和 Runtime failure。IP/port 是 bootstrap locator，不是 peer、
endpoint 或 media identity。

媒体使用第二个独立、nonblocking、numeric direct-IP TCP 连接，由 Profile v3 adapter 管理；首版在
一条 Edge-listen/Host-connect media socket 上 multiplex `RENDER_MEDIA` 与 `CAMERA_MEDIA`。媒体 socket
不建立第二个 Core Session，也不传输 control desired/actual。其 locator 同样只来自两端 ignored
config，不进入 catalog、State、浏览器命令或 telemetry。

建立顺序固定为：

```text
Core READY -> Profile v3 catalog ACCEPTED
  -> desired -> actual RUNNING accepted by Host Core State
  -> MEDIA_OPEN(exact SessionId + catalog generation)
  -> MEDIA_BIND(exact route + endpoint incarnation + production epoch
                + media stream epoch + Edge-generated channel nonce)
  -> complete access units
```

`channel_nonce` 是当前 Session/endpoint/production 的相关性值，不是认证 token。wrong Session、
catalog、route、endpoint incarnation、production epoch、stream epoch 或 nonce 必须在任何 payload
publication 前 fail closed。catalog replacement、endpoint re-incarnation、new production epoch 或
Session terminal 立即使旧 bind 失效；新 Session 不继承 desired、actual、bind、frame sequence 或
浏览器 view。

媒体 TCP failure终止该multiplex connection上的全部media bind，Edge通过仍存活的control Session分别
发布两个endpoint的真实failure actual；
control failure则关闭media socket并使全部 bind 失效。首版没有自动 reconnect/resume。

## 5. Frame、buffer 与 backpressure

MediaStream sample identity 是完整 access unit，不是 TCP write、fragment 或 decoder buffer。canonical
header 至少携带 exact route binding、production/stream epoch、nonzero frame sequence、source clock
epoch/time、format/codec、key/delta/recovery dependency、unit bytes、fragment offset/bytes、end flag 与
producer cumulative drop count。整数、reserved bytes、fragment coverage和最大值必须由 Profile codec
严格校验。

接收方只有在同一 frame 的所有 bytes exact、无 gap/overlap、长度与 dependency 合法后才原子发布；
partial frame、旧 epoch、重复不同内容或 malformed fragment永不可见。frame sequence 缺口与 producer
drop counter 必须投影为 MediaStream gap，不能被 TCP ordered delivery隐藏。

每条 media route 固定：一个正在序列化的完整 frame、一个待发送 latest frame；新 frame 到达而
latest slot已占用时原子替换旧 latest、增加 whole-frame drop counter，不追加无界队列。接收方每条
route最多一个 reassembly frame和一个已发布 latest slot。两个route按fragment opportunity做持久 RR；
一条route busy/invalid不得饿死另一条。

media socket 的 `WOULD_BLOCK` 只保持当前 fragment 与上述有界 slots，不推进frame sequence或伪装
发送成功。它不得阻塞/占用 Core control socket、R2a control/Blob/small-Stream scheduler 或 endpoint
adapter event loop；必要时 producer按Profile throttle/whole-frame drop。desired disable先停止新frame、
处理已物化的有界frame，再发布 `STOPPED`；Host接受STOPPED后不得接受该epoch的新frame。

## 6. Host editor 与统一 Web

一个 native Host process 是 Session、Runtime、State version、Blob ContentId/visibility、scene snapshot
和 media receiver 的唯一 owner。browser只通过loopback fixed HTTP command提交用户意图，通过SSE观察
sanitized control/State/telemetry，通过binary WebSocket观察本地投影的complete media frame：

```text
browser <-> loopback Web adapter <-> native Host <-> Meiso control/media TCP <-> Edge
```

WebSocket payload不是Meiso wire；browser不解析catalog/frame、不选择production epoch、不持有PeerId、
channel nonce或locator。刷新页面只读取native Host的sticky snapshot，不重启Host、不重发Blob/desired、
不产生第二个truth owner。每个浏览器client也只有一个latest frame slot；慢client不能反压native media
receiver或Edge。页面必须区分desired delivery、Edge actual、small RenderResult thumbnail、render
preview、physical camera与gap，不把任何一种标成physical display capture。

## 7. Security 与 privacy

首版control/media均为plain TCP，只允许受控开发网络。exact PeerId/Profile、SessionId、channel nonce、
source digest 与TCP ordered delivery都不提供对端认证、机密性、抗篡改或重放保护。UI绑定loopback不能
提升Edge链路安全。认证/加密以后由显式security transport Profile提供，不能静默把本切片晋升为安全
连接。

公开receipt不得包含locator、完整identity/nonce、credential、原始camera frame、原始设备日志或唯一
硬件标识。Web sanitizer只允许Profile声明的非敏感字段；未知key在进入有界history前丢弃。

## 8. 验收门

1. **G0 Profile/wire：** v1/v2 golden与产品Core source/API/wire零diff；冻结v3/v4 exact identifiers、
   schemas、limits、catalog和media frame literals；覆盖每个截断点、reserved、capacity、wrong Profile、
   bind identity、fragment gap/overlap/replay。
2. **G1 Host/editor：** 相同mesh/scene fixture通过in-memory、real AF_INET loopback和recorded backend；
   create/move/scale/camera/delete/disable的State/Blob/actual identity exact，Web reload不重放或换owner。
3. **G2 media loopback：** actual前零frame；enable/reconfigure/disable使用严格新epoch和whole-frame cut；
   两路media与Wi-Fi/DeviceTelemetry/RenderResult同时进展；partial I/O、真实`WOULD_BLOCK`、慢client、
   whole-frame drop/gap、RST/new Session均有bounded/exact证据。
4. **G3 target：** 保存exact source/binary/toolchain hash与K230 arena/heap/buffer/queue/socket资源账本；
   同一Profile v3 Session完成mesh->scene ACTIVE->render preview与真实camera frame->独立reconfigure/
   stop，同时Wi-Fi/device telemetry继续。不存在camera时只接受truthful unavailable transcript。

G0–G2已有portable/real-AF_INET证据；旧身份下的G3记录已归档。当前正式身份必须按
[K230D Composition E2E](../k230d-composition-e2e.md)重新完成fresh K230D/GC2093 run，旧receipt不能
替代。该证据即使完成也不外推为scanout/physical presentation、未测codec、性能、长稳、安全、
自动resume或其他sensor。
