# Display Ingress Profile

> 状态：媒体传输已实现；Linux 显示选择与资源状态逻辑已通过主机测试；Catalog、State、
> 呈现证据、真实设备接入和物理端到端验收仍待完成。DRM/KMS 指 Linux 内核的显示管理
> 和模式设置接口。
> 本页固定 Host 到 Edge 实屏链路的最小产品合同。当前只实现并测试了 DATA route 的
> Profile-local 媒体对象；完整 Catalog、State codec、呈现证据、carrier 和实机路径尚未完成。
> 通用规则仍由 [Resource](../../docs/resource.md)、[Runtime](../../docs/runtime.md)、
> [Link](../../docs/link.md) 和 [Composition](../../docs/composition.md) 拥有。

## 这条 Profile 解决什么

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
EndpointId 是 `DISPLAY_/INGR_END`。它有四条 route：

| 人话名 | 精确 ResourceId | owner / kind / facet | delivery | SchemaRef | 含义 |
|---|---|---|---|---|---|
| 输入配置 Desired | `DISPLAY_/DES_STAT` | Host / State / DESIRED | inline Runtime | 下一切片冻结 | 开关、输入宽高和目标最大帧率 |
| 输入状态 Actual | `DISPLAY_/ACT_STAT` | Edge / State / ACTUAL | inline Runtime | 下一切片冻结 | 是否可用、是否已应用、有效配置和媒体代际 |
| 完整画面 MediaStream | `DISPLAY_/DATA_MED` | Host / Stream / DATA | external media | `DISP_SCH/FRAME___:v1` | Host 到 Edge 的完整 RGBA8888 画面 |
| 呈现证据 Stream | `DISPLAY_/PRES_EVT` | Edge / Stream / DATA | inline Runtime | 下一切片冻结 | 已发生的真实或模拟呈现事实；不作为下一帧门槛 |

四个 ResourceId、owner、kind、facet 和 delivery 已选择；本提交只冻结并实现 DATA route 的
`DISP_SCH/FRAME___:v1`。其 `max_payload_size` 必须非零且不小于已应用的
`input_width * input_height * 4`。Desired、Actual、evidence 的精确 schema 和完整 Catalog
builder 仍待下一切片，因此现在不能声称四条 route 的 Catalog 已可构造或已通过 conformance。
未来 builder 必须把一个 endpoint、四条 route 作为完整原子快照；任何缺项、多项、schema、
delivery class 或 endpoint incarnation 不匹配都拒绝整个 catalog，不暴露子集。

## 配置与一次性数据门

Capability 声明 Edge 当前能接收的最大输入宽高、最大帧率、最大完整帧字节数和最大
fragment 字节数。Profile 第一版只接受 packed `RGBA8888` 和 `RAW`：每个像素依次是
R、G、B、A 四个 8-bit byte，A 必须是 255；没有压缩、delta、
色彩协商、镜片校正或物理 connector 选择。

Desired 是该原子子域的完整值：

- Runtime prefix 中的 `enabled`；
- `input_width` 与 `input_height`；
- `maximum_frame_rate_millihz`。

Actual 使用 Runtime 公共 prefix 报告 observed/applied Desired、apply result、availability、
effective enabled、production status、reason 和 production epoch。Profile suffix 只补充：

- 实际接受的输入宽高与最大帧率；
- 非零 `media_stream_epoch`；
- 当前媒体通道的随机 nonce；
- 接收侧完整帧丢弃累计值。

每个 fresh Session、production epoch 或 media epoch 只支付一次
`Desired -> Actual RUNNING -> Host accepts Actual` 数据门。门打开后，普通的后续完整帧
不再等待 Actual。改变宽高或其他会改变帧 envelope 的配置必须建立新的 production/media
epoch；未来独立的显示调节 State 不应无故重开媒体门。

媒体对象的 Runtime binding 必须直接来自当前 handle 的
`meiso_runtime_v4_external_media_binding()` 成功结果；该查询本身只会返回当前已激活的
external-media production。媒体对象初始化还会硬校验 exact endpoint
`DISPLAY_/INGR_END`、ResourceId `DISPLAY_/DATA_MED`、SchemaRef
`DISP_SCH/FRAME___:v1`、Stream/DATA、Host owner，以及非零 endpoint、production/media
epoch、nonce 和 Host producer incarnation。它不接受一个仅仅“也是 Host-owned”的任意 route。

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

呈现证据是小型、单向、无需业务 ACK 的 Stream sample。payload 至少包含：

- 被显示帧的 media epoch、sequence，以及需要验收时由 Edge 对完整输入计算的 SHA-256；
- Edge 本地 page-flip 时间；
- 实际输出宽高与 connector 类型；
- evidence class：真实 DRM/KMS page-flip 或明确的 simulated。

只有真实 page-flip class 可被上层解释为物理呈现。simulated 只用于测试，不得关闭
Looking Glass 的 PRESENTED 历史。证据丢失不阻塞后续帧；最终 E2E 只要求至少一条证据
与 Host 输入的 sequence 精确相符；验收构建还要比较双方对该完整输入计算的 SHA-256。

## 失败矩阵

| 情况 | 结果 | 旧画面 |
|---|---|---|
| Session/catalog/endpoint/production gate 尚未成立 | 不创建媒体对象，不接受 frame | 保持 |
| Desired 超 capability 或完整帧大小溢出 | Actual REJECTED，说明 unsupported/admission reason | 保持 |
| deadline 不支持、不可比较或已过期 | 返回三个不同状态；不把未知当可用 | 保持 |
| Host 提交 A 不是 255、重复或倒退 sequence | 同步拒绝且发送槽不变 | 保持 |
| in-flight 存在又来新帧 | 写入或整体替换 latest；当前 packet 完成后抢占旧帧余下部分 | 保持到新帧完整 |
| fragment 丢失、重复、重叠或乱序 | 丢弃当前 partial，记录 discontinuity | 保持 |
| 新完整帧到达而旧 latest 未消费 | 原子替换旧 latest并记 drop | 直到消费新 latest 前保持 |
| packet binding 不属于当前对象 | fail closed；旧对象终止，caller 关闭该 carrier | 保持或按上层断线策略处理 |
| control Session、catalog、endpoint 或 epoch 变化 | 旧媒体对象终止；新 scope 用新 arena 重建 | 本 Profile 不替产品选择 hold/fallback |
| exact DRM probe 不能证明选定 connector/mode/CRTC/plane/fourcc 路径可提交 | Actual 不得报 RUNNING；返回硬件 admission blocked | 由既有系统保持 |

所有可重试容量/暂时不可写返回都必须在失败时保持对象、caller 输入和输出字节不变。
会终止对象的协议错误必须显式报告 terminal reason，不能伪装成普通 backpressure。

## K230D 本机 DRM probe 的当前实现边界

本节补充术语：libdrm 是用户态调用库；connector 是物理输出口，CRTC 是当前扫描输出的
显示时序，plane 是可叠加的图层，zpos 是图层上下顺序。下文保留这些内核字段名，便于
与驱动枚举结果逐项核对。

当前仓库已有一层**与 libdrm 无关的设备选择和资源状态逻辑**。主机测试会注入一组
假设备操作（测试代码模拟打开、提交和事件，不会打开真实 `/dev/dri`），但还没有把这些
操作接到 K230D 的 libdrm，也没有板端可执行程序或板端结果。因此它不能生成真实 DRM
证据。收据固定写 `INJECTED_OPS_UNVERIFIED`（表示“由测试注入，未经物理设备验证”）和
`physical_evidence=false`。即使模拟事件让状态进入 `KMS_LATCHED`，也只证明状态逻辑按
合同推进，不证明 K230D、HDMI/DSI 链路或屏幕发生过任何事。

该策略内核固定以下安全选择，未来 native translation 只能如实提供事实，不能放宽：

- 在最多 8 个 connector、8 个 CRTC、16 个 plane 的完整快照中，只接受唯一一条有效
  显示路径：connector 已连接、已有 encoder/CRTC 绑定，并且该 CRTC 明确处于 `ACTIVE`
  且带 current mode。另一个已连接但未指向 active CRTC 的 connector 不算有效路径；
  所有对象按 ID 严格排序；
- `possible_crtcs` 的 bit 明确定义为排序后的 canonical CRTC 数组下标。libdrm 原始数组若
  顺序不同，native translation 必须先重映射，不能把原始 mask 直接交给策略层；
- 只选择兼容该 CRTC、支持 DRM `RGB565/RG16`、当前 `FB_ID=0 && CRTC_ID=0` 的
  immutable `OVERLAY` plane；primary、cursor 和 unknown type 一律不用；
- 目标 CRTC 上每个 active plane 和候选 overlay 的 type、zpos 都必须是 immutable truth，
  且候选当前 zpos 严格高于所有 active plane。驱动没有 zpos 或不能证明时直接
  `ADMISSION_BLOCKED`，不按数组位置、plane ID 或 K230 示例布局猜测；
- 不写 connector 或 CRTC property，不建 mode blob，不改变 current mode，不调用
  `vo_init`，也没有 `ALLOW_MODESET` 的表达入口；唯一写入对象是上述 unused overlay；
- 输入仍是完整 opaque RGBA8888 或 canonical RGB565 big-endian。策略层同步转换成 DRM
  RGB565 little-endian，以 CPU 最近邻等比缩放并在 current mode 内居中补黑；每边最大
  4096 像素，输入与 dumb buffer 各有公开 64 MiB 硬上限；
- plane-only `TEST_ONLY` 接受后才允许 real `NONBLOCK | PAGE_FLIP_EVENT`。real commit
  明确区分 accepted、definitely-not-applied 和 outcome-unknown；只有 exact cookie + CRTC
  的 monotonic event 才成为 `KMS_LATCHED`。无关 event 被消费但不改变 pending；
- event deadline 由未来 runner 有界管理。持续 would-block/interrupt 必须显式声明
  present 或 restore outcome unknown；unknown 时 fd、master、FB、mapping 和 dumb handle
  全部保留，不能猜测“应该没显示”后释放；
- cleanup 先用原值恢复候选 plane 的十个可写字段：`FB_ID`、`CRTC_ID`、
  `SRC_X/Y/W/H`、`CRTC_X/Y/W/H`。先做 `TEST_ONLY`，再提交带 event 的真实恢复；
  `type` 和 `zpos` 只读校验，绝不改写。匹配恢复事件后还要重读这十个字段和只读的
  immutable type/zpos，逐项相同才按
  `rmFB -> unmap -> destroy dumb -> drop master -> close` 释放。

收据绑定规范化 topology SHA-256、driver name/version、connector type/id、CRTC/current
mode、plane/zpos、FB/handle/pitch/size、输入格式/尺寸/SHA-256、缩放 viewport、test/commit/
event 和资源清理账本；不包含 `/dev/dri/*` 路径、设备序列号或私有日志。Topology hash
输入的前 8 byte 固定为 `KDT0 00 00 00 01`：`KDT0` 在这里首次定义为
“K230D DRM topology”的哈希域标签，末尾的 `1` 只表示后续字段排列的第一种布局；当前
没有第二种布局，它也不是网络 wire 版本。其后字段按公开 topology 数组的 ID 排序结果，
整数用 big-endian，定长字符串空余 byte 补零。这个 hash 只防止验收时混淆输入，不提供
认证、签名或传输完整性保护。

由于当前开发环境没有可用的 K230 Xuantie compiler、xf86drm header 或 libdrm artifact，
native translation、runner、cross/link 和 board page-flip 都明确为 `PENDING`。在这些门关闭
以前，Display Actual 不得因本策略内核存在而报告 RUNNING。

## 明确不在第一版里

- carrier 选择及弱网无队头阻塞证明；TCP 只可作受控 bring-up；
- compressed video、delta frame、重传、逐帧 ACK、远端补帧；
- LG batch、source/import/release milestone、authority、lease 或 prepared token；
- 断线后的 hold-last、fallback、黑屏或安全策略；
- DRM connector/mode 的远程选择、lens correction 或 stereo optics；
- carrier 内的认证、加密、anti-replay 实现，自动 reconnect 或跨 Session resume；但最终
  carrier 仍必须满足 Link 的完整性与认证前提。

双眼画面可以由应用先排成一张左右并列 atlas，再作为普通完整 RGBA frame 发送。只要总
宽高落在已接受 capability 内，媒体 wire、buffer 和 DRM 提交路径都无需改变。眼间布局、
畸变和光学参数属于应用/显示调节配置，不是本帧传输合同的隐藏含义。

## 实现和验收顺序

1. **已完成：** DATA route 的 Profile-local `DIFR` codec、两槽媒体状态机和故障矩阵；
2. 冻结其余三个 schema，实现完整 Catalog、Desired/Actual 和 evidence codec；
3. **部分完成：** K230D 本机 probe 的 portable policy/state machine 与 fake failure matrix
   已在 Host 验证；native libdrm translation、runner、cross/link 和 exact board page-flip
   仍待独立切片；
4. integrity-protecting carrier adapter 与 Edge receiver；
5. LG Host latest slot 到 Profile sender 的映射；
6. 真实 Host 输入 SHA-256 与 KMS page-flip evidence 对齐；
7. 可选左右眼 atlas。

第 6 步以前不得声称“Host 应用已经显示到 Edge 实屏”。
