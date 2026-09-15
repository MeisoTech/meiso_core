# Direct-IP MediaStream

> 状态：`IMPLEMENTED / PORTABLE + AF_INET VERIFIED / FRESH K230D E2E PENDING`。
> 本页定义正式 Composition Profile 的 direct-IP media arrangement；
> 不新增 Core Resource、不提升 target 证据。旧 Profile 身份下的分版记录已
> [归档](archive/media-stream-direct-ip-v0.md)。

## Selection

当前 Profile 是 `COMPOSE_/_PROFILE:v1`，control 使用 `MSL0 v1` + `MRT0 v4`。
Catalog 必须 exact 6 endpoints / 18 routes。Wi-Fi、Presentation、Scene/Blob/RenderResult
和 DeviceTelemetry 使用 `INLINE_RUNTIME`；RenderMedia 与 CameraMedia 的 data route 使用
`EXTERNAL_MEDIA`，两路 desired/actual 仍使用 inline State。

MRT0 v2/v3 仅属于内部 compatibility fixture，正式 Profile 不降级到旧 wire，
也不允许 Host 只接受 media 或 telemetry 子集。Exact graph 见
[Composition Profile](../profiles/composition/README.md)。

## Endpoint semantics

RenderMedia 和 CameraMedia 各包含：

| facet | owner | 含义 |
|---|---|---|
| desired State | Host | enabled、format/codec、extent、cadence、bitrate/GOP/recovery intent |
| actual State | Edge | availability、apply result、effective mode、production/media epoch、nonce、drop/recovery |
| data MediaStream | Edge | 完整 access unit；只在当前 actual gate 内可见 |

RenderMedia 固定为 off-screen observation，每帧绑定 applied SceneDesired owner/version、
active plan generation 和 world generation。它不是 panel capture，不代表 fence、
commit、vblank 或 physical presentation。

CameraMedia 只表示 adapter 真实探测并生产的 physical camera。
`SceneDesired.camera` 是 renderer viewpoint，与 physical camera 的 availability、mode 和
frame 无关。Camera 缺失或失败时 actual 必须 truthful unavailable/failed，不得使用
render 或 synthetic frame 代替。

## Control and media planes

Control 和 media 各使用一条 nonblocking numeric direct-IP plain TCP 连接。Locator
只来自两端本地配置，不进入 PeerId、SessionId、catalog、State、browser
命令或 telemetry。Media socket 不建立第二个 Core Session，不传 desired/actual。

建立顺序是：

```text
control TCP connected
  -> Core Session READY
  -> exact Composition catalog accepted
  -> selected media desired delivered
  -> selected media actual RUNNING accepted by Host
  -> MEDIA_OPEN(SessionId + catalog generation)
  -> MEDIA_BIND(route + endpoint incarnation + production/media epoch + nonce)
  -> complete access units
```

`active_route_mask` 必须在 init 时明确选择 render、camera 或 both；0 和未知 bit
是 `INVALID_CONFIG`。OPEN 不协商 mask；两端必须从同一份 accepted control
actual truth 得到相同选择。不同时 fail closed，不保证进展。

Wrong Session、catalog、route、endpoint incarnation、production/media epoch 或 nonce
在任何 frame publication 前 fail closed。Catalog replacement、endpoint replacement、new
production epoch 或 control terminal 立即使旧 bind 失效。

Media TCP failure 终止该 connection 上的全部 bind，Edge 通过仍存活的 control
Session 发布真实 failure actual。Control failure 关闭 media。当前没有自动
reconnect/resume；新 TCP 需要新 Session、arena、Runtime、handle 和 media transport。

## Frame and bounded buffering

MediaStream sample identity 是完整 access unit，不是 TCP write、fragment 或 decoder buffer。
Canonical header 包含 exact binding、frame sequence、source time、format/codec、dependency、
unit/fragment extent 和 producer cumulative drop count。仅当全部 bytes 连续、无重叠、
大小与 dependency 合法时，Host 才原子发布。

当前上限：

- RAW codec，或显式协商的 CameraMedia JPEG 独立帧；4096-byte maximum fragment；
- RenderMedia: 320×240 RGBA8888，maximum access unit 307200 bytes；
- CameraMedia: maximum access unit 460800 bytes；
- 每路 Edge 只有 in-flight + latest，Host 只有 reassembly + latest；
- 两路在每次成功 fragment completion 后持久 round-robin。

CameraMedia 可声明 `codec=JPEG(2)` / codec mask bit 1。仅搭配
`pixel_format=RGBA8888`（表示解码后的像素布局），不适用于 RenderMedia；RAW 保持原固定
像素字节数规则。JPEG 为单个完整 baseline sequential 8-bit、三分量 JPEG，尺寸须与声明一致，
每帧 KEY、dependency=0、GOP=1；AU 非空且不超过 capability 的 `max_access_unit_bytes`。
分片/重组层只校验声明、长度和绑定，消费端必须在发布像素前验证 JPEG 内容及尺寸；损坏图像
丢弃并诊断，不伪造成功帧。旧端未声明 JPEG 时不能发送 JPEG。
现有 `bitrate_bps=0` 表示无独立编码码率承诺；关键帧通过协商的帧率与 AU 上限限制最坏载荷率，
`max_bitrate_bps` 不替代链路实测。桌面关键帧部署选择 320×240、500 millihz、30720-byte AU；
此部署值不扩大 K230D 能力声明。所有权、实际状态、latest 有界队列与失效规则保持原契约。

Latest 已占用时新帧只能整帧替换并增加 drop counter；不追加无界队列。
`WOULD_BLOCK` 保持当前 fragment 与全部 cursor/counter 不变，不阻塞 control
scheduler 或另一 endpoint。Partial、gap、overlap、replay、wrong scope 或 malformed
frame 不可见，并必须投影为可观测 drop/gap/failure。

Disable 先停止新生产，处理已物化的有界帧，再发布 STOPPED。Host 接受
STOPPED 后不得接受该 epoch 的新帧。

## Host and Composition Editor

Native Host process 是 Session、Runtime、State version、Blob visibility、scene snapshot 和
media receiver 的唯一 owner。浏览器只通过 loopback Web adapter 提交用户意图，
并消费 sanitized control truth 与完整 media frame；它不解析 Meiso wire、不选 epoch、
不持有 identity/nonce/locator。

当前工具是 [`devtools/web/composition_editor/`](../devtools/web/composition_editor/)。
页面刷新只读 native Host 的 sticky snapshot；慢 client 只替换自己的 latest slot，
不反压 native media receiver 或 Edge。UI 必须区分 desired delivery、actual、
RenderResult thumbnail、RenderMedia、physical CameraMedia 和 gap。

## Security and evidence

Plain TCP 只允许受控开发网络。Peer/Profile exact match、SessionId、nonce、
digest 和 TCP ordered delivery 不提供认证、机密性、完整性或 anti-replay。
Public receipt 不得包含 locator、完整 identity/nonce、credential、原始 camera frame、
设备日志或唯一硬件标识。

Portable codec/lifecycle、POSIX/AF_INET 与 Host/editor path 已有测试证据。当前
formal identity 对应的 fresh K230D cross + board run 仍待
[K230D Composition E2E](k230d-composition-e2e.md) 收口。通过后也不外推为
physical display、codec、security、performance、soak 或自动恢复。
