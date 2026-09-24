# Display Ingress Profile

版本选择、资源、wire 与继承规则在本页及明确链接的版本合同中维护；通用语义由
[Resource](../../docs/resource.md)、[Runtime](../../docs/runtime.md)、
[Link](../../docs/link.md) 和 [Composition](../../docs/composition.md) 拥有。

- 当前应用选择：[v12 空间中的平面](planes-v12.md)（往后唯一的显示路径），以及原地保留、不再加功能的
  [v9 组合视频/HUD/相机](#组合应用-profile-v9)、[v10 原生多区域 scene](#application-v10-native-scene-selection)。
  两端必须显式选同一版本。
- v9/v10 是 **Wi-Fi 量级 Profile**（720p 视频、NV12 相机）；能力分档、会话活性、驻留 lease、单路重新激活与 BLE 适配器边界见 [v11 草案](capability-tiers-v11.md)（#9/#18），其带宽账目表集中列出全部常量。
- [v12：空间中的平面](planes-v12.md)（meiso_edge #2，S-025/S-026）：串流作为平面纹理、平面模型、静态图 Blob；Edge 按位姿做透视合成，本地 HUD 在最上层。
- H.264、RAW、HUD、CameraMedia 的基础字段按各版本继承；[v4/v5](overlay-v4-v5.md)另页保存，未撤销被新版本继承的规则。
- 使用与部署见[快速开始](../../docs/host-edge-quickstart.md)和[K230D 产品入口](https://github.com/MeisoTech/meiso_edge/blob/main/targets/k230d-linux/README.md)。
  单帧模拟、DRM 研究和旧验收顺序见[历史记录](../../docs/archive/display-ingress-bringup-through-20260906.md)，不作为当前产品前置门。
- 当前受控 UDP 搬运不提供认证或加密；最终安全 carrier 仍未交付。

## 组合应用 Profile v9

`DISPLAY_/INGRESS_:v9`复用H.264 v2的四条display资源，再按固定顺序选择完整HUD三条、
完整CameraMedia三条。display必选；两个可选endpoint各自全有或全无，最多3 endpoint、
10 routes、148 capability bytes。所有endpoint共享本次设备incarnation，未知/重复/乱序、
部分资源或能力与资源不一致拒绝整个Catalog。不新增Session、传输或逐帧等待规则；
v1–v8保持原契约，双方明确选择v9，无隐式降级。

Display能力schema为`DISP_SCH/CAPABIL_:v3`，48 bytes：前32 bytes沿用H.264能力；
32..35、36..39为大端output_width/output_height（1..4096），40为video_layout
（0=native左上原尺寸；1=左右眼各自原尺寸居中），41..47必须为零。
输入必须装得进输出；左右模式要求输入/输出宽为偶数。无隐式缩放；未覆盖区域保留
平台背景/安全层，桌面后端以黑色作为背景。
布局描述实际输出栅格，不表示光学校准。evidence_class_mask必须恰好选择SIMULATED或
DRM_KMS_PAGE_FLIP之一，视频与HUD共同遵循；更换后端须重新协商Catalog。
HUD原有36-byte能力/计划保持，画布尺寸必须等于display输出；CameraMedia仍用既有64-byte
能力与来源/desired/actual/media身份。缺少可选endpoint时调用方不得继续宣称该能力可用。

HUD证据schema升级为`DISP_SCH/HUD_PRES:v2`，保留56-byte布局，byte0明确为
1=SIMULATED或2=DRM_KMS_PAGE_FLIP，其余字节保持原定义。消费者严格核对所选类别，
软件合成完成不能表述成物理显示；历史HUD_PRES:v1仍只接受2。隐藏或移动也须完成
实际后端更新后报告对应plan，而不能仅因接收/校验通过即回执。

失败向量包括：四种可选组合、输出越界/奇数左右宽、保留位、错误证据类别、HUD画布不符、
Camera资源乱序及owner/delivery/schema变化；桌面检查合成/移动/隐藏像素并标记模拟，
板端另外测两平面视频+原生ARGB+相机的并发资源和实际显示。

## 这条 Profile 解决什么

### 压缩扩展：`DISPLAY_/INGRESS_:v2`

v1 RAW 的 wire 和行为保持不变。v2 选择 `H264_IDR`：一个 access unit 是独立的
Annex B、8-bit 4:2:0 Baseline H.264 图像，包含 SPS、PPS 和恰好一个 IDR slice。
不接受依赖旧帧的 VCL NAL；每帧都是恢复点，因此沿用 direct latest、缺口丢弃和异步证据，
不引入逐帧 ACK/重传或第二套 Session。该首版压缩选择不承诺支持所有 H.264 编码模式。

v2 保留一个 endpoint/四 route 及 Desired、Actual 的 v1 schema。Capability、FRAME 和
PRES_EVT 使用同名 schema 的 v2；Session Profile version 为 2。Capability 仍是 32 bytes，
byte 25 是格式（0=RAW，仅 v1；1=H264_IDR，仅 v2），26..31 为零。格式是 catalog 内的
不可变能力选择；Desired/Actual 的尺寸、帧率属于该格式。v1/v2 schema 与格式混搭拒绝，
调用方必须把 catalog 格式和本次 Session 选择核对一致，不能隐式降级。

`max_frame_bytes` 在 v2 表示编码 AU 上限，不再要求能容纳 RGBA 原图。每帧长度为
1..max_frame_bytes；DIFR packet layout 仍为 v1，header 中的 full-frame bytes 是该 AU 的
实际长度，所有 fragment 必须一致。内存依然只有接收重组和 latest 两槽，按上限分配；
下一个完整 IDR 可替换旧图。压缩输入的结构验证要求 SPS/PPS 在 IDR 前、无第二个图像、
无非 IDR VCL；完整码流合法性和实际解码尺寸仍由 decoder 检查，失败不产生呈现成功。
packet binding 的 FRAME schema 必须与当前格式匹配，不能把压缩字节当 RAW 校验或发布。

v2 呈现证据沿用 64-byte 布局，32..63 为输入编码 AU 的 SHA-256，而不是解码后像素摘要。
帧身份必须由实际解码输出关联到提交的 sequence，再绑定真实 page-flip；无法关联或没有
实际事件时不得发成功证据。旧 `rgba_frame`/`input_rgba_sha256` C 字段别名仅供兼容，
新 caller 使用 `access_unit`/`input_frame_sha256`。接收、解码输入排队、呈现和证据是不同阶段。

失败向量包括：旧 schema 携带新格式、AU 超限、变长 AU 的末片/替换边界、fragment 长度
冲突、缺 SPS/PPS、非 IDR VCL、多个 IDR、decode 尺寸不匹配和错帧证据。RAW 的既有
golden/failure vectors 必须继续通过；本版本不改变原受控 carrier 的安全能力。

以下既有段落描述 v1 RAW，v2 仅以上述差异替换对应约束。

### 可选的逐帧投影描述：H.264 SEI v1

压缩 AU 可以包含一个 `user_data_unregistered` SEI（payload type 5），UUID 为
`d4e0ea40-6084-4f31-bb01-96bed49bf4a1`，后接下表的128-byte描述。它记录**Host实际用于
渲染的源视图**；一视图表示一个源视图，两视图按左眼、右眼排序，不能把一张合成图凭空
解释成两个有独立姿态的视图。坐标为右手系、米，朝向四元数xyzw，视锥角度为弧度。

| 字节（UUID之后） | 内容 |
|---|---|
| 0..3 | version=1、view_count=1或2、target_time_present=0或1、space_class（1=head、2=application、3=world） |
| 4..7 | 全零 |
| 8..23 | source_space_id、source_space_epoch，各u64，非零 |
| 24..39 | 目标时间clock_epoch、monotonic_ns，各u64；未提供时均为零 |
| 40..83 / 84..127 | 每视图11个binary32：朝向xyzw、位置xyz、FOV left/right/up/down；未用视图全零 |

整数和浮点位模式均大端。浮点必须有限，四元数平方模在0.999..1.001，FOV角度严格在
−π/2..π/2内且left<right、down<up。source空间归Host生产者及本次Session所有，epoch在
基准定义变化时递增；它不是物理校准或追踪有效性的证明。目标时间同属Host生产者时钟，
不暗示与Edge时钟可比，也不是新的deadline字段。没有真实源事实时省略SEI，不填默认姿态。

SEI使用标准RBSP转义，位于该AU的IDR之前，新增NAL不超过256bytes。`max_frame_bytes`
仍约束含SEI的完整AU；DIFR、latest/drop、既有AU SHA-256和呈现证据保持原定义，因此描述
与图像一起被接收、丢弃和关联。无新增控制State、逐帧ACK或第二条数据通道。未知UUID由
投影reader跳过；同一AU重复本UUID、错误版本/长度/数值均拒绝解读，输出保持不变。

`projection.h`提供无堆分配的写入、读取和插入函数；插入保留原有AUD/SPS/PPS顺序。
这项可选描述不改变旧Profile协商，不要求旧接收端执行它；没有描述时仍按既有视频显示。
当前Edge读取元数据但不执行姿态微调，呈现证据也不宣称修正已应用。未来启用修正仍须
显式协商对应能力、坐标/时间映射及执行结果，不能仅凭出现SEI开启。

SEI标准结构可对照[FFmpeg的公开语法实现](https://ffmpeg.org/doxygen/trunk/cbs__sei__syntax__template_8c_source.html)。

Host 已经有一张完整应用画面时，把它作为可过期的连续媒体交给 Edge。Edge 只保留
正在接收的一张和最新完成的一张，随后把最新合法画面交给本机显示后端。显示后端在
真实 KMS page-flip（Linux 内核显示提交完成事件）到达后，可异步发送一条小型呈现证据。

它不是逐帧事务。下一帧不等待上一帧的网络确认、Edge 应用确认或呈现证据。旧帧可以
被更新帧替换；每一帧都是独立完整的 RAW（未压缩完整像素）图，因此序号缺口之后
不需要补传旧帧。

本 Profile 明确选择 Composition 规则中的 **direct latest** 模式，而不是 prepared handoff。
Host 只提供一个固定、opaque、覆盖完整产品显示区域的 `VIEW Quad`，其 source 是
`REMOTE_PROJECTION`。Desired 是这个固定 layer 的完整配置 State（完整配置值）；没有 prepared token、
echo 或逐帧激活握手。Edge-local safety layer 仍由 Edge 拥有，并且不得被本 route 遮挡。
首个 DRM page-flip probe 只能证明底层 scanout 能工作，不能单独证明最终 layer policy。

## 本页的五种“版本”分别指什么

| 对象 | 本 Profile 选择 | 含义 |
|---|---|---|
| Display Ingress Profile | `DISPLAY_/INGRESS_:v1` | 只表示本页 Resource Graph 和 schema 的第一版 |
| Meiso Session wire | `MSL0 v1` | 建立 Host/Edge Session 的帧格式；定义见 [Session Core](../../docs/session-core-v0.md) |
| Meiso Runtime wire | `MRT0 v4` | 搬运 catalog、State 和 inline Stream 的帧格式；定义见 [Runtime](../../docs/runtime.md) |
| 完整帧 Resource schema | `DISP_SCH/FRAME___:v1` | 一张完整 RGBA8888 access unit 的结构；定义在本页“完整帧与 packet” |
| 外部媒体 packet layout | `DIFR packet v1` | 把完整帧拆成 fragment 时使用的 224-byte header；定义在下方 wire 表 |

这五个版本号属于五个不同对象，数字相同不表示同一个版本。完整帧 Resource schema 与
DIFR packet layout 也是两个版本轴：前者定义“帧是什么”，后者定义“帧怎样分片携带”。
当前实现硬校验 `FRAME___:v1 + DIFR packet v1` 这一精确组合；升级其中一个不会自动升级
另一个。Profile 精确选择 Session 和 Runtime wire；连接双方不做隐式降级。

## 精确 Resource Graph

Profile 只有一个逻辑 endpoint，英文人话名为 **projection display ingress**，稳定
EndpointId 是 `DISPLAY_/INGR_END`。Catalog 顺序固定为以下一个 endpoint 和四条 route：

| 人话名 | 精确 ResourceId | owner / kind / facet | delivery | SchemaRef | 含义 |
|---|---|---|---|---|---|
| 输入配置 Desired | `DISPLAY_/DES_STAT` | Host / State / DESIRED | inline Runtime | `DISP_SCH/DESIRED_:v1` | 开关、输入宽高和目标最大帧率 |
| 输入状态 Actual | `DISPLAY_/ACT_STAT` | Edge / State / ACTUAL | inline Runtime | `DISP_SCH/ACTUAL__:v1` | 是否可用、是否已应用、有效配置和媒体绑定 nonce |
| 完整画面 MediaStream | `DISPLAY_/DATA_MED` | Host / Stream / DATA | external media | `DISP_SCH/FRAME___:v1` | Host 到 Edge 的完整 RGBA8888 画面 |
| 呈现证据 Stream | `DISPLAY_/PRES_EVT` | Edge / Stream / DATA | inline Runtime | `DISP_SCH/PRES_EVT:v1` | 已发生的真实或模拟呈现事实；不作为下一帧门槛 |

四个 ResourceId、owner、kind、facet 和 delivery 构成不可拆分的一个 endpoint、四 route
Catalog。媒体 route 的 `max_payload_size` 精确等于 capability 的 `max_frame_bytes`，并且不得
小于已应用的 `input_width * input_height * 4`。任何缺项、多项、顺序、schema、delivery
class 或 endpoint incarnation 不匹配都拒绝整个 catalog，不暴露子集。

Capability payload 是 32 bytes，完整 MRT0 Catalog 是 428 bytes。Session 限额不能只按
Catalog 大小设置：当前 MRT0 v4 仍继承 4096-byte Blob chunk carriage 的 wire 下限，因此
`max_runtime_payload_bytes=4180`、`max_frame_bytes=4224`、`max_registry_entries=5`。本 Profile
没有 Blob route；初始化 MRT0 v4 时使用最小非零 `max_blob_transfer_bytes=1` 只为满足当前
Runtime v4 compatibility layout，这些值不表示新增 Blob 产品能力。Runtime 其余容量固定为
1 endpoint、2 条 endpoint identity history、4 routes、32 capability bytes、每 endpoint
12 Desired suffix bytes和 32 Actual suffix bytes。

## 配置与一次性数据门

Capability 是 `DISP_SCH/CAPABIL_:v1` 的 32-byte canonical big-endian payload：

```text
0..3    max_input_width                 u32，1..4096
4..7    max_input_height                u32，1..4096
8..11   max_frame_rate_millihz          u32，1..240000（0.001 Hz）
12..15  max_frame_bytes                 u32，4..67108864
16..19  max_fragment_payload_bytes      u32，1..max_frame_bytes
20..23  evidence_class_mask             u32
24      deadline_policy                 u8
25..31  reserved                        必须全零
```

`max_frame_bytes` 至少能容纳一个 1×1 RGBA8888 像素。`evidence_class_mask` 只允许 bit 0
`SIMULATED` 和 bit 1 `DRM_KMS_PAGE_FLIP`，且至少一位非零。
`deadline_policy` 只允许 Core 已定义的 1 `UNSUPPORTED` 或 2 `REQUIRE_COMPARABLE`。Profile
第一版只接受 packed `RGBA8888` 和 `RAW`：每个像素依次是
R、G、B、A 四个 8-bit byte，A 必须是 255；没有压缩、delta、
色彩协商、镜片校正或物理 connector 选择。

Desired suffix 是 `DISP_SCH/DESIRED_:v1` 的 12-byte canonical big-endian payload，也是该
媒体输入 envelope 原子子域的完整值：

```text
Runtime 公共 prefix 中的 enabled
0..3    input_width                     u32，1..4096
4..7    input_height                    u32，1..4096
8..11   maximum_frame_rate_millihz      u32，1..240000（0.001 Hz）
```

因此 Desired Resource 的最大 payload 是 Runtime 8-byte prefix 加 12-byte suffix，共 20 bytes；
Actual Resource 是 56-byte prefix 加 32-byte suffix，共 88 bytes。Evidence payload 固定 64
bytes。关闭状态的 Desired 仍携带完整有效外壳，方便全新会话重发当前配置。

宽、高和帧率共同决定帧校验、buffer budget 与生产代际，不能拆成三个会产生不完整中间
组合的 State。亮度、光学、布局和其他普通显示调节不属于这个 State；未来应使用独立窄
State，因此它们不能无故关闭媒体门。

Actual 使用 Runtime 公共 prefix 报告 observed/applied Desired、apply result、availability、
effective enabled、production status、reason 和 production epoch。Display Ingress v1 只把
RUNNING 作为 active 状态；其 Profile suffix 是 `DISP_SCH/ACTUAL__:v1` 的 32-byte canonical
big-endian payload，只补充：

- 实际接受的输入宽高与最大帧率；
- 当前媒体通道的随机 nonce。

```text
0..3    effective_input_width            u32
4..7    effective_input_height           u32
8..11   effective_max_frame_rate_millihz u32
12..15  reserved                         必须全零
16..23  channel_nonce.high               u64，nonce 整体非零
24..31  channel_nonce.low                u64
```

第一版明确令媒体帧 header 中的 `media_stream_epoch` 等于 Runtime `production_epoch`；它是
同一生产代际在外部媒体 packet 中的重复 scope 校验，不是新的独立代际。Actual suffix
不重复携带 epoch，也不携带会在同一 epoch 内变化的丢帧计数。后者只属于本地媒体对象
snapshot 和 E2E receipt；若将来需要远端持续观测，应使用独立 telemetry Stream。

STOPPED、FAILED 和 STARTING Actual 的 Profile suffix 必须为空；虽然通用 Runtime 还能表达
DEGRADED/STOPPING，本窄 Profile 第一版拒绝这两个状态。新 Desired 正在处理、被
拒绝或处理失败，但旧 production 仍在运行时，Actual 可以报告 `observed=new / applied=old`，
并继续携带旧 production 对应 byte-exact 不变的 32-byte suffix；这不会关门或改变 epoch。
RUNNING 必须引用一个 `enabled=true` 的完整已应用 Runtime Desired State，不能只拿 12-byte
envelope 猜测开关状态。因而新观察到的 disabled Desired 不会关闭仍引用旧 enabled Desired
的 production；但 disabled Desired 一旦成为 `applied`，伪造的 RUNNING Actual 必须被拒绝。

Edge 作为 Actual owner，必须在 `meiso_runtime_send_state_offer()` 前解码 Runtime prefix，再用
本 Profile 校验 suffix、capability 和已应用 Desired。Host 收到 Actual 后也必须先做同一
Profile 校验，成功才可调用 StateReplica，并把 `RECEIVE_NEW` 交给
`meiso_runtime_commit_state_receive()`；校验失败时不得调用 StateReplica，应以
`MEISO_STATE_RECEIVE_INVALID` 调用 Runtime commit 清理该次 pending，绝不能作为
`RECEIVE_NEW` 提交。Runtime 本身把 suffix 当 opaque bytes，不能替代这一步。

每个全新会话或一次生产周期（production epoch）只支付一次
`Desired -> Actual RUNNING -> Host 本地提交该 Actual` 数据门。Host 收到 Actual 后必须依次
完成 Profile 校验、StateReplica `RECEIVE_NEW`，再让
`meiso_runtime_commit_state_receive(..., MEISO_STATE_RECEIVE_NEW)` 成功；三步全部完成后即可
发送媒体，不等待 Actual 的 State ACCEPT 再回到 Edge。门打开后，普通的后续完整帧不再
等待 Actual。新 Desired 在 admission 期间不撤销旧 RUNNING；只有新 envelope 真正应用时
才建立更高 production epoch 和新 nonce。未来独立的显示调节 State 不应无故重开媒体门。

媒体对象的 Runtime binding 必须直接来自当前 handle 的
`meiso_runtime_v4_external_media_binding()` 成功结果；该查询本身只会返回当前已激活的
external-media production。媒体对象初始化还会硬校验 exact endpoint
`DISPLAY_/INGR_END`、ResourceId `DISPLAY_/DATA_MED`、SchemaRef
`DISP_SCH/FRAME___:v1`、Stream/DATA、Host owner，以及非零 endpoint、production/media
epoch、nonce 和 Host producer incarnation，并要求 media epoch 与 Runtime production epoch
相等。它不接受一个仅仅“也是 Host-owned”的任意 route。

## 完整帧与 packet

每个 access unit 是一张完整、独立的 packed RGBA8888 图：

```text
bytes = input_width * input_height * 4
```

Frame sequence 在每个 `media_stream_epoch` 内从 1 严格递增。Host 可以跳过尚未开始发送的
旧值；Edge 记录 sequence gap，但下一张完整帧本身就是恢复点。没有逐帧业务 ACK，也不因
缺口请求补传旧帧。

外部媒体 packet 使用 `DIFR`，即 Display Ingress Frame（显示输入帧）的 4-byte magic。
它的第一版使用固定 224-byte canonical big-endian header，随后是一个 fragment：

```text
0..3      magic `DIFR`                                      u32
4..5      packet schema version (=1)                        u16
6..7      BEGIN/END/RECOVERY flags                          u16
8..11     header bytes (=224)                               u32
12..15    complete packet bytes (=224+fragment bytes)       u32
16..47    exact SessionId                                   4 * u64
48..55    catalog generation                               u64
56..71    endpoint id                                       2 * u64
72..79    endpoint incarnation                              u64
80..95    media ResourceId                                  2 * u64
96..103   production epoch                                  u64
104..111  media stream epoch                                u64
112..127  channel nonce                                     2 * u64
128       producer role                                     u8
129       DATA facet                                        u8
130       EXTERNAL_MEDIA delivery                           u8
131       deadline present，canonical 0 或 1                u8
132..135  reserved，必须全零                                4 bytes
136..143  producer incarnation                              u64
144..151  frame sequence                                    u64
152..159  source clock epoch                                u64
160..167  source monotonic time                             u64
168..175  sender deadline clock epoch                       u64
176..183  sender deadline monotonic time                    u64
184..191  producer-side whole-frame drop high-water         u64
192..195  full-frame bytes                                  u32
196..199  fragment offset                                   u32
200..203  fragment bytes                                    u32
204..223  reserved，必须全零                                20 bytes
224..     fragment bytes
```

Header 中的 Profile packet schema version 只版本化这一个 packet layout，不是
`DISPLAY_/INGRESS_` ProfileRef、`MSL0` 或 `MRT0` 的版本。

第一版中的每一张 RAW 图都是恢复帧，因此 `RECOVERY` 当且仅当 `BEGIN`，且不存在依赖
旧帧的 delta；`BEGIN` 当且仅当 offset 为零，`END` 当且仅当 fragment 正好到达完整帧末尾。
双方必须从同一个已接受的 Runtime external-media binding 初始化媒体对象。每个 packet
重复携带完整的 routing/production scope，是为了让接错 socket、旧 Session、旧 catalog、
旧 endpoint、旧 production/media epoch 或旧 nonce 在任何像素发布前被拒绝；不是一次新的
OPEN/BIND 往返。Resource descriptor 本身不进入 DIFR；`catalog generation + ResourceId`
把 packet 唯一关联到双方已接受的 descriptor，其 schema、kind、owner 和 max payload 已在
媒体对象初始化时校验。

`DIFR` 本身不带 checksum、MAC 或签名。本对象只接收由 carrier 作为一条完整 packet 交付的
字节；最终 carrier 必须满足 Link 规定的完整性和认证要求。当前切片尚未实现或证明该 carrier，
不能把 E2E 中比较的 SHA-256 当成 wire 安全机制；那个 SHA-256 只用于验收时对齐两端输入。

### 受控普通 UDP 行为路径

这里的普通 UDP 指未加认证或加密的 UDP。这个 UDP 搬运适配器使用调用方已经创建、
连接并设为非阻塞（暂时无数据时立即返回）的 IPv4/IPv6 UDP socket。
一个完整 `DIFR` packet 精确对应一个 UDP datagram；不增加 OPEN/BIND、业务 ACK、重传、排序
或第二个 Core Session。Host 媒体侧只发送，Edge 媒体侧只接收。一次成功的 `send` 只表示
本机内核接受了整个 datagram，不表示 Edge 接收、frame 发布或实屏呈现。

本受控路径把 UDP payload 硬限制为 1200 bytes，因此 `DIFR` 的 fragment 最多为
`1200 - 224 = 976` bytes。超限 datagram 必须用 `MSG_TRUNC`（内核返回的“收到的
datagram 大于接收缓冲”标志）检出并整包丢弃，不能把截断 prefix 送给媒体 receiver。
丢失、重复和乱序只影响当前 partial frame；后续完整帧从新的 `RECOVERY|BEGIN` 恢复，
不等待旧帧补传。

UDP 适配器自己的单元测试只在同一进程内通过 IPv4/IPv6 本机回环地址搬运；下一节另用
两个真实进程证明同机接线。两者都没有证明跨机器真实网络。这个 UDP 适配器只用于受控
行为验证和端到端初次打通。socket 固定对端地址不等于验证
发送者身份；它不提供 Link 要求的身份认证、机密性、完整性或防重放，也不提供拥塞控制、
自动存活检测、网络路径迁移或弱网性能保证。
因此它不是最终产品 carrier，不能据此声称 Link 安全或完整 E2E 已关闭。

每个全新会话对象（Session）和媒体对象必须使用全新的 connected UDP 四元组（本地 IP/端口 +
对端 IP/端口），并创建新的媒体 transport/adapter 内存区；不支持旧四元组复用、断点续用
或跨 Session 残留队列继承。按本 Profile 已冻结的 fail-closed 规则，若旧 scope datagram
仍到达新对象，Session/catalog/endpoint/epoch/nonce 任一不匹配都会终止该媒体对象；
适配器不得把它静默降级成普通丢包。

### 受控双进程模拟 service

仅供诊断；实现说明见[早期模拟 service 记录](../../docs/archive/display-ingress-bringup-through-20260906.md#受控双进程模拟-service)。

### 受控单帧 DRM service

仅供诊断；实现说明见[单帧 DRM 记录](../../docs/archive/display-ingress-bringup-through-20260906.md#受控单帧-drm-service)。

## 固定内存与 latest 规则

Host 发送侧固定拥有：

- 一张已经开始交给 carrier、不可再修改的 in-flight 帧；
- 一张尚未开始发送、可被更新输入整体替换的 latest 帧；
- 一个 header + fragment 的 packet scratch。

Edge 接收侧固定拥有：

- 一张尚未完整、对显示后端不可见的 reassembly 帧；
- 一张已经完整校验、可借给显示后端的 latest 帧；
- 一个 packet scratch。

对齐后的精确 arena 大小是 `2048 + 2 * frame_bytes + 224 + max_fragment_bytes`。
所有 slot 在初始化时由 caller arena 一次性提供，稳态不分配。partial、重叠、越界或长度
不符的帧绝不变成 latest；Host submit 与 Edge fragment admission 都检查每个像素的 A=255。
新的完整帧可以替换尚未消费的 latest，并增加
独立 drop counter。carrier 暂时不可写只保持当前 fragment，不推进任何 cursor。

latest 的优先级在 fragment 边界生效：已经借给 carrier 的 packet 保持 byte-exact，直到
carrier 明确完成；若此时旧 in-flight 还有未发送 fragment 且 latest 已存在，发送侧丢弃旧帧
剩余部分、把 drop high-water 增加一次，并让 latest 从新的 RECOVERY BEGIN 开始。所有计数
加法在 packet 借出前预检，因此一个已经成功借出的 exact PacketRef 不会在完成时才因溢出失败。

三个丢帧视角不能相加成一个“总丢帧数”：packet 的 producer drop high-water 是 Host 报告的
累计上游丢帧加本媒体对象的 latest 替换/余段抢占；Edge sequence gap 只描述序号缺口；
`reassembly_frames_dropped` 只在一个已占用 partial reassembly 被清除或被新 BEGIN 替换时
增加一次。没有 partial 的 orphan continuation 只返回 `DISCONTINUITY`，不增加 frame drop；
stale/older packet 也不得清掉一个 newer partial。

`submit()` 完成私有拷贝后，表示媒体对象不再读取 caller bytes；它不表示网络交付、Edge
接收、物理显示或 Looking Glass source 可以释放。业务 source release 仍由 Looking Glass
自己的 `SourceReleaseAuthority` 决定。

## 过期与呈现证据

帧携带 source time 和可选 sender deadline；present deadline 属于 source clock，因而它的
clock epoch 必须与 source time 相同。deadline policy 为 unsupported 时明确返回
`DEADLINE_UNSUPPORTED`。需要 deadline 时，只有 adapter 已证明它可映射到本地单调时钟且
uncertainty 可接受，才允许比较；证明不足明确返回 `DEADLINE_UNCOMPARABLE`，不得伪装成
未过期。只有 mapped `local_now > local_deadline` 才返回 `DROPPED_EXPIRED`；两者相等时仍然
可接收。这与 Core Stream 的 admission 边界完全一致，不引入本 Profile 自己的第二套规则。

呈现证据是小型、单向、无需业务 ACK 的 Stream sample。`DISP_SCH/PRES_EVT:v1` payload
固定为 64-byte canonical big-endian：

```text
0       evidence_class                   u8
1       connector_kind                   u8
2..3    reserved                         必须全零
4..7    output_width                     u32，1..4096
8..11   output_height                    u32，1..4096
12..15  reserved                         必须全零
16..23  media_stream_epoch               u64，等于对应 production epoch
24..31  displayed_frame_sequence         u64，非零
32..63  完整 Host 输入 RGBA 的 SHA-256    32 bytes
```

`evidence_class` 只允许 1 `SIMULATED` 或 2 `DRM_KMS_PAGE_FLIP`。`connector_kind` 只允许
0 `UNSPECIFIED`、1 `HDMI` 或 2 `DSI`；simulated 必须使用 `UNSPECIFIED`，真实 page-flip
必须使用 HDMI 或 DSI。Core `MeisoStreamSample.source_time` 就是 Edge 本地 page-flip 时间，
不在 payload 重复；该 evidence Stream 自己的 Edge producer epoch/sequence 也不能代替
payload 引用的 Host media epoch/frame sequence。

`DRM_KMS_PAGE_FLIP` 只证明本地 KMS latch 事件与输入身份匹配，不单独证明人眼可见像素、
面板、光学或最终 layer policy。只有上层验收合同明确接受该 evidence class 时，才可关闭
对应的 KMS 呈现里程碑；`SIMULATED` 永远不得关闭 Looking Glass 的 PRESENTED 历史。证据
丢失不阻塞后续帧。最终可见屏 E2E 还需要独立物理观察，并要求至少一条证据与 Host 输入
sequence 和双方对完整输入计算的 SHA-256 精确相符。

## 失败矩阵

| 情况 | 结果 | 旧画面 |
|---|---|---|
| Session/catalog/endpoint/production gate 尚未成立 | 不创建媒体对象，不接受 frame | 保持 |
| Desired 超 capability 或完整帧大小溢出 | Actual REJECTED，说明 unsupported/admission reason | 保持 |
| deadline 不支持、不可比较或已过期 | 返回三个不同状态；不把未知当可用 | 保持 |
| Host 提交 A 不是 255、重复或倒退 sequence | 同步拒绝且发送槽不变 | 保持 |
| in-flight 存在又来新帧 | 写入或整体替换 latest；当前 packet 完成后抢占旧帧余下部分 | 保持到新帧完整 |
| fragment 丢失、重复、重叠或乱序 | 丢弃当前 partial，记录 discontinuity | 保持 |
| UDP 暂时不可读/写或系统调用被信号打断 | 不推进 packet、frame 或计数；交回事件循环稍后重试 | 保持 |
| UDP 短发送或其他读写错误 | 媒体对象进入粘滞终态；socket 仍由调用方持有 | 按上层断线策略处理 |
| 新完整帧到达而旧 latest 未消费 | 原子替换旧 latest并记 drop | 直到消费新 latest 前保持 |
| packet binding 不属于当前对象 | fail closed；旧对象终止，caller 关闭该 carrier | 保持或按上层断线策略处理 |
| control Session、catalog、endpoint 或 epoch 变化 | 旧媒体对象终止；新 scope 用新 arena 重建 | 本 Profile 不替产品选择 hold/fallback |
| exact DRM probe 不能证明选定 connector/mode/CRTC/plane/fourcc 路径可提交 | Actual 不得报 RUNNING；返回硬件 admission blocked | 由既有系统保持 |

所有可重试容量/暂时不可写返回都必须在失败时保持对象、caller 输入和输出字节不变。
会终止对象的协议错误必须显式报告 terminal reason，不能伪装成普通 backpressure。

## 研究与早期实现记录

K230D topology / TEST_ONLY policy 的唯一维护入口是[DRM research](https://github.com/MeisoTech/meiso_edge/blob/main/research/k230d-drm/README.md)，
不属于默认产品后端。原 v1 非目标、研究边界与阶段顺序见[历史记录](../../docs/archive/display-ingress-bringup-through-20260906.md)。

## Display Ingress v6/v7: bounded CJK screen text

The legacy overlay selections are v6 (native video) and v7 (side-by-side video);
current services compose these resources through v9 above.
They replace the [v4/v5 selections](overlay-v4-v5.md); their layer ordering, State
activation, physical evidence and pixel budgets are unchanged. Mixed selections
fail Session negotiation. HUD capability and plan schemas advance to v3;
status/evidence retain their existing schemas.

The screen State references the configured video Desired version, not a received
video frame. It can therefore be presented before any video arrives, including
a session containing only screen content. Video Actual indicates that ingress is
armed; it does not assert a physical video presentation. The independent screen
Actual/evidence still requires its own physical flip. A screen-only session can
close normally with zero video frames. On K230D the decoder queues and buffers
are initialized on the first video submission; opening the output for screen
content alone does not allocate decoded or compressed video surfaces.

Capability adds a big-endian font mask:u32 after the existing 32 bytes. Bit 0
selects the original 5x7 label font; bit 1 selects CJK16. At least one known bit
is required. CJK16 is advertised only when its local glyph asset is available.
The plan remains 112 bytes: byte 58 is font (0 or 1), bytes 59–63 are reserved
zero. Hidden plans require font 0; text length still counts at most 48 bytes.
Evidence hashes all 112 bytes, including the font and exact text bytes.

Font 0 retains its original repertoire and metrics. CJK16 consumes strict UTF-8
and draws one spacing glyph per code point, 16 pixels tall. Basic printable
ASCII U+0020–007E advances 8 pixels; U+3000–30FF, U+3400–4DBF, U+4E00–9FFF and
U+FF01–FFEF advance 16 pixels. It provides short Chinese/Japanese and basic Latin
labels, with no normalization, complex-script shaping, multiline layout or emoji
claim. Integer scaling, colors and borders retain their existing meaning.
Overlong UTF-8, invalid continuations, unsupported code points, unadvertised fonts
and text extending outside the region are rejected before rendering. Invalid
Host file updates retain the previous accepted message while video continues.

K230D uses a local, read-only fixed glyph bank, not a full image in Desired or a
second media channel. Glyph data and its license accompany the installed runtime;
absence of that optional asset leaves only font 0 available. Acceptance includes
mixed text round-trip, malformed UTF-8, font capability mismatch, identical left
and right glyph pixels, unchanged raster bounds, actual update/hide evidence
alongside compressed video, and bounded memory on K230D.

For K230D, generate the optional 2 MiB bank with
`targets/k230d-linux/make_overlay_font.py INPUT.hex.gz OUTPUT.bin` (in meiso_edge); the converter
pins the upstream archive hash and documents its download URL. Configure
`MEISO_K230D_OVERLAY_FONT_FILE` to that generated file when installing the
`K230dVideoRuntime` component. CMake installs the bank with its SIL OFL license
and attribution; the font is derived from [GNU Unifont](https://unifoundry.com/unifont/).
The installed init script selects the bank under the runtime prefix. Foreground
launchers can set `MEISO_OVERLAY_FONT` to an explicit local bank; an invalid
explicit path fails startup. With no selected asset, only font 0 is advertised.
The platform reads 32 bytes per used glyph through one descriptor, with at most
1536 bytes of glyph rows per update, while reusing the existing HUD pixel buffer.

## Historical v4: compressed video with a native screen layer

版本合同及继承规则见[v4 原生图层](overlay-v4-v5.md#historical-v4-compressed-video-with-a-native-screen-layer)。

## Historical v5: side-by-side video with native screen content

版本合同及布局公式见[v5 左右眼布局](overlay-v4-v5.md#historical-v5-side-by-side-video-with-native-screen-content)。

## v8: video ingress with camera uplink

`DISPLAY_/INGRESS_:v8` composes the unchanged v2 H.264 display endpoint (four
routes) and Composition's existing CameraMedia endpoint (three routes), in
that order, within one Session and Catalog. It does not select native HUD or
stereo placement. Camera capability, Desired, Actual, OPEN/BIND and RAW media
units retain their [Composition definitions](../composition/README.md). The
camera endpoint incarnation equals the display incarnation in this selection.
No second Session or application-specific wire is introduced.

Camera Desired has independent Host state version 1/delivery generation 11;
Edge Actual has version 1/delivery generation 12. The first implementation
accepts one enabled configuration per connection. Host first configures video,
then camera; neither camera frames nor video frames wait for per-frame ACKs.
Camera failure invalidates the combined connection; a fresh connection requires
a new catalog, Desired/Actual and media binding. Close revokes both producers.

The initial desktop adapter exposes 320x240 packed RGBA8, at up to 5 Hz. The
Host starts at 2 Hz: 614400 payload bytes/s (4.92 Mbit/s), before framing and
carrier overhead. Camera uses the existing bounded CameraMedia TCP adapter;
downlink video retains UDP. TCP may delay current camera data behind older
bytes on a poor link; this is a functional baseline, not a low-latency weak-link
claim. In-flight/latest slots and application copies remain bounded.

Desktop source time is the Edge's monotonic time after a complete capture
frame is dequeued, not a measured sensor exposure. No optical calibration or
Host/Edge clock synchronization is asserted. Host consumers retain the Edge
clock identity, sequence and epoch, and mark exposure/calibration unknown.
Acceptance covers exact graph/version rejection, independent control states,
wrong binding and stale sequence rejection, partial-frame/EOF cleanup, and
simultaneous real camera uplink plus application video downlink.

The earlier K230D Linux service selected this v8 graph with `--camera-media-fd`
and `--camera-device` (default `/dev/video1`). It reuses the existing V4L2
adapter: 320x240 NV12, four 115200-byte MMAP buffers, up to 5 transmitted Hz.
At the Host's initial 2 Hz this is 230400 payload bytes/s (1.84 Mbit/s).
ISP and decoder allocations are additional; this is not a full board memory
budget. Capture must start before publishing the catalog; capture failure or
carrier loss terminates the combined service and releases its camera buffers.
The adapter's source time/sequence provenance is preserved; zero driver
timestamps remain dequeue times, not exposure measurements.

NV12 describes pixel layout, not colorimetry. A Host converting these bytes
must select the source's documented matrix/range explicitly; the current
deployment may select BT.601 limited range only for a source confirmed to use
it. Current services use the v9 optional graph described above. This does not add
color calibration to the wire. v8 still does not select
native HUD/stereo placement; testing this graph must not be reported as those
combined product modes passing.

### Application v10: native scene selection

Application v10 keeps the v9 optional graph (display, optional native 2D,
optional camera) and all existing ownership, delivery and evidence rules.
For a selected native 2D endpoint, HUD_CAP and HUD_PLAN are schema v4: capability
is the scene capability below, Desired suffix is its variable-length scene
plan (maximum 1440 bytes). HUD status v1 and presentation v2 are unchanged;
their generation/digest identify the entire scene and the observed DRM/software
completion of that exact Desired version. Delivery generation remains 9; it
is independent of the Profile version. Source/optical validity remains Host
policy and must clear/invalidate the scene through the same owned control path.
Scene State persists until replacement or Session cleanup; Host owns content
deduplication and local observation-deadline renewal, which creates no new Edge
presentation evidence or wire generation when the displayed content is unchanged.

Session selection is explicit on both peers. The `*_for_version` helpers
accept only 9 and 10; legacy helpers still mean v9. Catalog validation uses
the selected Session version and rejects cross-version HUD schemas or payload
limits, rather than recognizing a new scene by inspecting unversioned bytes.
Version 10 is opt-in in the process path (`--native-overlay scene`); old `on`
still selects v9 HUD. An application does not gain native scene support solely
because the codec is installed.

The K230D scene adapter reserves two ARGB4444 buffers before publishing this
capability, each bounded to 1,152,000 pixels. Actual rounded DMA bytes and
decoder CAPTURE buffers share the existing 10 MiB pool budget; an incompatible
decoder allocation is rejected before allocating its image buffers. Camera,
input decoder buffers, State storage and OS memory remain additional costs.
It rasterizes directly into the idle DMA buffer with no full-frame staging.
The portable desktop adapter uses the same scene contract and reports software
completion only. On K230D, zero-item updates detach the overlay plane. The
atomic update includes the already-active CRTC, including when the plane was
already empty; completion requires its page-flip event and FB_ID/CRTC_ID readback
of zero. Submission alone is not physical evidence. The reserved scene buffers
remain available for the next nonempty update; removal does not assert DMA
memory or decoder release. Video planes are unchanged.

### Native scene payload (application v10; not selected by v9)

`scene.h` defines a bounded replacement for the single repeated HUD rectangle:
up to 16 independent rectangles with optional existing label/CJK16 text. Each
rectangle uses absolute output coordinates; Host supplies separate left/right
geometry after projection and clipping. Count zero clears the scene. All items
share one immutable generation and active video desired-version reference.
Version 9 and HUD_PLAN v3 remain unchanged. Sending these bytes on that legacy
route is invalid. The application v10 selection above owns their route binding.

Integers are big-endian. Capability is 36 bytes: output width/height, maximum
backing pixels, maximum text bytes per item (0..48), updates per second (1..60)
at offsets 0/4/8/12/16; u16 minimum width/height and power-of-two width/height
alignment at 20/22/24/26; u32 maximum items (1..16) and existing font mask at
28/32. These are actual platform limits; 4096×4096 is the codec dimension
ceiling, not a supported K230D surface size.

Plan length is exactly `32 + 88 × count` (32..1440 bytes). Header contains u64
generation/video desired version at 0/8, u32 canvas width/height at 16/20, u8
count at 24, and seven zero bytes. Each item has u32 x/y/width/height at 0/4/8/12,
straight ARGB8888 border/fill/text colors at 16/20/24, u16 border width at 28,
u8 text scale/font/text byte count at 30/31/32, seven zero bytes and 48 text
bytes at 40 (unused tail zero). Text repertoire and scaling follow HUD v3.
Geometry must fit the canvas and text must fit inside the border; out-of-range
items are rejected, never silently clipped or reprojected on Edge.

Items paint in list order using source-over alpha; transparent interiors do
not erase earlier objects. The renderer clears one backing region per scene.
Its union includes all inter-eye gaps, is padded to the advertised minimum and
alignment, and is shifted inward only as needed to keep padding on canvas.
That entire region consumes the advertised pixel budget, even for thin frames.
Empty scenes need no pixel storage. Platform color precision may be lower than
the logical color (the bounded K230D candidate uses straight ARGB4444).

Codecs and support checks leave outputs untouched on failure. Acceptance covers
different per-eye geometry, overlap transparency, clear, stale video reference,
area including padding/gaps, malformed canonical bytes and bounded writes.
Physical qualification, source binding, invalidation and application receipts
remain separate requirements; codec/raster tests alone do not establish them.


### v9/v10 视频单路停止与陈旧画面清除

Host 隔离视频准入后，可在同一 Desired route/owner/delivery generation 发较新版本的
`enabled=false`，保留最后 enabled Desired 的尺寸/帧率 suffix。停止不是另一张视频帧。
Edge 接受后立即停止视频准入，串行撤下两个视频平面；已提交翻页仍以原身份观察，不能把
`unobserved` 改为“确定未呈现”。native HUD/scene 与 CameraMedia、控制会话继续。
HUD/scene 的 `video_desired_version` 保留最后 **enabled** Desired 版本，几何与输出范围
保持不变直到会话重建；disabled Desired 的新版本不是新的 scene 几何版本。

只有输出适配器确认视频已清除，Edge 才发布较新 Actual：observed/applied 均指向 disabled
Desired，`APPLIED / PRESENT / effective_enabled=false / STOPPED / DISABLED`，production epoch
为 0，profile suffix 为空。K230D 的确认须包含清除提交完成及两视频 plane 的 FB_ID/CRTC_ID
读回为 0；桌面为软件输出清除，不是物理证据。重复 Desired 不重新启动清除或刷新期限。
清除失败/证据缺失不得发布这个成功结果：`FAILED / APPLY_FAILED`，observed 指向停止请求，
applied 保留最后 enabled Desired；该结果不证明原驻留画面已消失，也不释放未知 reservation。
Host 接受停止/失败 Actual 后保留 scene/control，禁止新的本路视频准入；本轮恢复仍须显式
重建会话。单路重新激活由 #18 后续定义。

产品验收目标是在控制链路正常的测试条件下，从 Host 隔离到双眼撤下不超过 1 s。
实现给 Edge 清除观察保留 750 ms（接收 stop 时起算，不因重复请求延长），超时报告失败，
不伪造 STOPPED。该本地观察预算不是协商 lease。收到停止请求之前，本版本没有自动驻留
到期；帧 deadline 仅约束准入。控制链路失联沿已有会话丢失清理路径处理（部署 TCP user
timeout 为 15 s），不能承诺失联后一秒清屏。失联自清的协商 lease 留给 #9/#18。

回归向量：首帧呈现→后续 unobserved→disabled Desired→双眼清除确认→STOPPED；清除期间
scene 在途回执不丢失，停止后 scene 新版本/退出清屏仍成功；重复 stop 不重新激活；旧 enabled
Desired、清除失败/缺失证据都不能制造 STOPPED 或恢复视频；显式会话重建后可重新呈现。
