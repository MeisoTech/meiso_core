# Display Ingress v12：空间中的平面（0DoF 起步）

> 状态：`VERSIONED PROFILE CONTRACT`（meiso_edge #2，S-025/S-026）。本页只定义两端契约。
> 2026-09-24 第一版实现在 Zero 与 Orin NX 上验收后冻结；v9/v10 的 bytes、版本号与行为不变。
> 同日加可选的跟踪 endpoint（§10，meiso_host #53）：只加不改，没有它的 v12 Catalog 照旧合法。
> 2026-09-25 写明视频呈现顺序（§6）与观察中断（§2）（meiso_core #36）：现有 Edge 已经这样做，
> 本页写明它并规定 Host 能据此推出什么；bytes 与版本号不变。
> 2026-09-26 加可选的输入 endpoint（§11，meiso_host #66）：按键上行、准星下的平面、本地锁屏。只加不改，
> 没有它的 v12 Catalog 照旧合法。
> 同日（meiso_host #71）：Edge 释放平面模型不再引用的图（§7）；`opacity` 为 0 表示隐藏（§4、§5）。bytes 不变；
> 早于此的 Edge 不释放图，并按原来的校验拒收含 `opacity` 0 的模型（Host 这时改用撤平面，§7）。
> 同日（meiso_host #73）：系统平面（`app` 为 NO_APP）画在 APP 平面之后，准星先取系统平面（§5、§11）。
> bytes 不变；早于此的 Edge 只按距离画。
> 2026-09-29：§14 增加 Edge 自定位、独立视图与 XR 提交证据（meiso_core #43）。保持 Profile v12，
> 旧枚举与布局的行为不变；新增行为必须由显式能力选择。
> 2026-09-30：§15 加可选的音频 endpoint（按住 `SYSTEM` 说话，meiso_host #93）。只加不改，没有它的 v12 Catalog
> 照旧合法；完整上限增至 7 endpoint、21 route、registry 28。
> 2026-10-01（meiso_edge #35）：写明 `correction=IMU` 时 Edge 怎样用陀螺（§10）：位姿晚到或断了也接着转，
> 位置保持；IMU 到头部的旋转在眼镜本地配置。bytes 与版本号不变；`correction=NONE` 的 Edge 行为不变。
> 2026-10-04：§16 加可选的动效（meiso_host #116）：眼镜按目标和时长插值，新锚点 FOLLOW（懒跟随）。
> 2026-10-05（meiso_host #160，S-045）：§11 加键码 8 `MENU`，系统键：展开、收起系统菜单，只归系统。只加不改：
> 早于此的对端按原来的校验拒收第 8 位，所以眼镜只向同一版的 Host 报这个键。
> 能力字节 42、平面记录 92..94 与锚点 3 只在 Edge 声明后使用；不声明的 Edge 照旧拒收非零字段，Host 照旧一下子到位。

## 1. 解决什么

S-025 把显示统一成"空间中的平面"：Host 决定有哪些平面、各自摆在哪、内容从哪来、属于哪个 APP；
Edge 每次刷新给每个平面算透视变换后贴上屏，自己不渲染网格或物体。内容来源三种：Host 串流、
静态图、Edge 本地 LVGL（S-026 的 HUD）。前两种经本 Profile 传输；第三种只在 Edge 上，本页只规定
Host 能告诉它什么（当前 APP 名）。

v12 是一次显式的应用选择（Session profile version 12），与 v9/v10 并列，不隐式升降级。
Loop 1 验收后 v12 是唯一往前走的显示路径：v9/v10 不再加功能，像 Composition 一样原地留着，
没人用了再退役。大屏串流就是一块 HEAD 锚定的 STREAM 平面。

## 2. Resource Graph

基线 Catalog 最多五个 endpoint，顺序固定；§14 的独立视图可增加一个只含能力的 endpoint：

| endpoint | route | owner / kind / facet | delivery | schema | 含义 |
|---|---|---|---|---|---|
| 显示 `DISPLAY_/INGR_END` | 4 条，与 v2 相同，XR 证据见 §14 | — | — | `DISP_SCH/CAPABIL_:v2`（32 B） | 串流：H.264 独立帧（v2），作为平面的**纹理**，不再直接上屏 |
| 平面 `DISPLAY_/PLAN_END` | `DISPLAY_/PLN_DES_` | Host / State / DESIRED | inline | `DISP_SCH/PLN_MODL:v1` | 平面模型（完整值） |
| | `DISPLAY_/PLN_ACT_` | Edge / State / ACTUAL | inline | `DISP_SCH/PLN_STAT:v1` | 已应用的模型与来源就绪情况 |
| | `DISPLAY_/PLN_PRES` | Edge / Stream / DATA | inline | `DISP_SCH/PLN_PRES:v1` | 按会话声明的类别报告模型呈现或提交（§6、§14） |
| | `DISPLAY_/PLN_IMG_` | Host / Blob / IMMUTABLE | inline | `DISP_SCH/PLN_JPEG:v1` | 静态图，内容寻址（SHA-256） |
| 相机（可选） | 3 条，与 v9 相同 | — | — | — | 不变 |
| 跟踪（可选）`DISPLAY_/TRCK_END` | 4 条，见 §10 | — | — | — | IMU 上行、头部位姿下行 |
| 输入（可选）`DISPLAY_/INPT_END` | 3 条，见 §11 | — | — | — | 按键上行 |

§14 的“眼镜自带定位”与上表跟踪互斥，放在相同位置；视图描述放在所有上述 endpoint 之后。
本页“翻页/上屏”及从后续 AU 推断旧 AU 离屏的规则适用于原证据类别；XR 类别严格按 §14.3
使用提交接受里程碑，不把提交解释为物理呈现或确定离屏。

显示与平面两个 endpoint 必选，相机、跟踪、输入可选，出现时按上表的顺序。平面 endpoint 的 capability 是 `DISP_SCH/PLN_CAP_:v1`（64 B）。
所有 endpoint 共用本次设备 incarnation。缺项、多项、乱序、schema 或 delivery 不符，整个 Catalog 拒绝。

显示 endpoint 的 Desired 仍给出串流宽高与帧率；这个宽高就是串流纹理的尺寸。v9 的输出几何与
`video_layout` 不属于 v12：画面怎样上屏只由平面模型决定。视频帧的呈现证据沿用 v2：一帧**第一次
被合成进一个已翻页的缓冲**时，按它的 AU SHA-256 发 `PRES_EVT`。

### 串流拼图

只有一路串流。多块 STREAM 平面各取串流纹理里的一块矩形，等于 Host 把所有会变的内容拼进同一帧。
这张拼图由 Host 系统排，APP 只拿到并只画自己那一块；会变的内容走串流，不变的走静态图（§7）。
MeisoOS 的统一调度从这里接：谁占拼图的哪一块、哪块平面放在哪，都是 Host 系统的决定。

### 观察中断

链路卡住时 Host 收不到视频证据。这不是停视频的理由：Host 可以照常准入、发送新帧（发不出去的由
有界队列丢弃）。卡顿前后没有证据的 AU 仍按 unobserved 报给应用；之后任一 AU 的证据结束它们
"可能还在屏上"的不确定（§6）。卡顿中 Edge 照画最后一帧，平面与 HUD 照常。
Host 要确认视频已撤下时才走下面的停视频。

### 停视频

显示 Desired 可以像 v9/v10 那样改成 `enabled=false`（尺寸后缀不变、版本更高）。Edge 停止收视频，
STREAM 平面不再画、计为等待；等一帧不含它们的画面真实翻页后，才发布
`APPLIED / STOPPED / DISABLED`、production epoch 0、后缀为空的 Actual。本 Session 内不能重新启用，
要视频就建新 Session。

## 3. 平面能力（Edge → Host，64 B，大端）

```text
0..3    output_width            u32  1..4096，整块输出的像素
4..7    output_height           u32  1..4096
8       view_layout             u8   1 = MONO；2 = SIDE_BY_SIDE；3 = RUNTIME_VIEWS（§14）
9       tracking                u8   0 = NONE；1 = ROTATION（保留，不用）；2 = POSE（§10）；3 = EDGE（§14）
10      max_planes              u8   1..16
11      max_images              u8   1..8，Edge 同时持有的不同静态图最多几张（§7）
12..15  max_image_bytes         u32  1..1048576，一张图（JPEG）的字节上限，也是图 Blob 路由的 payload 上限
16..17  max_image_width         u16  1..4096
18..19  max_image_height        u16  1..4096
20..35  fov_left/right/up/down  4×f32 弧度，每个视图相同；−π/2<left<right<π/2，−π/2<down<up<π/2
36..39  ipd_m                   f32  MONO 为 0；SIDE_BY_SIDE 为 0.04..0.08
40..41  max_updates_per_second  u16  1..60，平面模型更新的上限
42      motion                  u8   位 0 TRANSITION，位 1 FOLLOW（§16）；其余位为零
43..63  reserved                全零
```

MONO 为一个视图覆盖整块输出，SIDE_BY_SIDE 为左右两半各一眼；两者的输出尺寸、FOV/IPD
仍按上表解释。RUNTIME_VIEWS 的旧几何字段须为 +0，独立视图定义见 §14.2。
FOV 与 IPD 描述 Edge 实际用来投影的视图，Host 据此把平面摆进视野；它不是光学标定结论。
浮点均为 IEEE-754 binary32 大端，必须有限。

## 4. 平面模型（Host → Edge，Desired 后缀，32 + 40×A + 96×P B）

头部 32 B：

```text
0..7    generation              u64  非零；内容变了就换新值，与 State 版本无关
8..15   video_desired_version   u64  有 STREAM 平面时等于当前 enabled 的显示 Desired 版本，否则为 0
16      plane_count  P          u8   0..16；0 表示清空
17      app_count    A          u8   0..8
18      focused_app             u8   0xFF 表示无；否则 < A。HUD 显示它的名字
19..23  reserved                全零
24..27  world_epoch             u32  EDGE 时非零，其他 tracking 时零（§14）
28..31  reserved                全零
```

旧模式仍要求 19..31 全零。
非 EDGE 会话的 24..27 必须保持零，由能力相关的模型准入检查验证。

APP 表，每项 40 B，顺序即下标：

```text
0..3    app_id                  u32  非零，表内唯一；Host 分配，跨模型版本稳定
4       name_size               u8   1..32
5..7    reserved                全零
8..39   name                    严格 UTF-8，不含控制字符；尾部全零
```

平面表，每项 96 B：

```text
0..3    plane_id                u32  非零，表内唯一；跨版本稳定，表示"同一块面板"
4       app                     u8   APP 表下标；0xFF 表示属于 Host 系统
5       anchor                  u8   1 = HEAD（跟头）；2 = WORLD（固定在空间）；3 = FOLLOW（懒跟随，§16）
6       source                  u8   1 = STREAM；2 = IMAGE
7       opacity                 u8   0..255，255 为不透明；0 为隐藏（§5）
8..23   source_x/y/w/h          4×u32  取样矩形（源像素）；w、h ≥ 1
24..55  image                   32 B   IMAGE：图的 SHA-256；STREAM：全零
56..67  position x/y/z          3×f32  米，|分量| ≤ 1000
68..83  orientation x/y/z/w     4×f32  单位四元数，模平方 0.999..1.001
84..87  width_m                 f32    0 < w ≤ 100
88..91  height_m                f32    0 < h ≤ 100
92      transition_cs           u8     0..250，过渡时长，单位 10 ms；0 为一下子到位（§16）
93      motion_style            u8     低 4 位 curve、高 4 位 enter，各 0..2（§16）
94      follow_deg              u8     FOLLOW 为 1..60，其余锚点为 0（§16）
95      reserved                全零
```

坐标系右手，米：x 右、y 上、看向 −z，与 H.264 SEI 投影描述一致。HEAD 平面的位姿在头部坐标系里；
WORLD 平面在 Host 的世界坐标系里（§5）。平面中心在 `position`，局部 x 轴沿宽、y 轴沿高，
正面朝局部 +z。取样矩形左上角贴平面左上角 `(−w/2, +h/2)`。

校验（失败则整个模型拒绝，Edge 保留上一版）：
- 保留字节与名字尾部为零，数值在上述范围；
- 有 STREAM 平面时 `video_desired_version` 必须等于当前 enabled 的显示 Desired 版本，
  取样矩形落在该 Desired 的输入宽高内；
- IMAGE 平面的取样矩形在图解码后检查，越界的平面不画，记为等待（§6）；
- `plane_count ≤ max_planes`；模型引用的不同图片数 ≤ `max_images`；
- 92..94 非零或锚点为 3，只在能力的 `motion` 声明了对应的位时合法（§16）。

## 5. Edge 怎样画

- 每次刷新，Edge 对每个视图、每个平面，用视图位姿与 FOV 把四个角投到屏上，
  以一个 3×3 透视矩阵把取样矩形贴上去（双线性取样，按 `opacity` 做直 alpha 混合）。
- 背面朝向视图的平面不画；任一角在近裁剪面（0.05 m）之内或之后的平面，本版不画。
- 画的先后（meiso_host #73）：先画 APP 的平面，再画系统的平面（`app` 为 NO_APP，如系统助手的面板、home 面板）；
  每一组里由远到近，按平面中心到视图原点的距离，距离相同按表中顺序。所以 APP 的面板离得再近（比如走到钉在房间里的面板跟前），
  也盖不住系统面板。
- Edge 本地 HUD 画在所有平面之上，平面永远不遮挡它。有输入 endpoint 且 `pointer=HEAD_RAY` 时，
  准星与准星下平面的高亮也是 Edge 本地界面，画在平面之上（§11）。
- `opacity` 为 0 的平面隐藏：不画，既不算 `drawn` 也不算 `waiting`（§6），也不会是准星下的平面（§11）。
  它引用的图照样算被模型引用，Edge 留着（§7），所以再显示只需一个新模型，不用重送图。
- 本地锁屏中（§11）什么都不画，整屏黑。
- `tracking=NONE`（0DoF）：头部位姿恒为单位，WORLD 平面看起来与 HEAD 相同。
- `tracking=POSE`：WORLD 平面按显示时刻的头部位姿变换（§10）；HEAD 平面和 HUD 不受影响。
  `ROTATION` 保留不用（S-025 不做 3DoF）。
- `tracking=EDGE`：位姿由 Edge 本地计算，按 §14 的 world_epoch 和有效性决定 WORLD 是否绘制。
- STREAM 平面画当前最新解出的一帧，第一帧到之前不画。IMAGE 平面在图到齐并解码后才画。

## 6. 状态与证据

线上编号（Host 与 Edge 都按这些值收发）：
- 显示 endpoint 沿用 v2：Desired 的 delivery generation 为 7，Actual 为 8；视频呈现证据流的 stream epoch 为 3。
- 平面模型 Desired 的 delivery generation 为 13，平面 Actual 为 14。平面 Actual 每发一版，production epoch 加一；
  证据用最近一版 Actual 的 production epoch 发送，stream epoch 为 1，序号逐条递增。
- 证据样本的 source time 是 Edge 单调时钟上的翻页时刻（clock epoch 4），不宣称与 Host 时钟可比。

Actual 后缀 16 B：

```text
0..7    generation              u64  已应用模型的 generation；尚无则 0
8       drawn                   u8   现在能画的平面数
9       waiting                 u8   在等来源的平面数（图未到或解码失败、串流未出第一帧、取样越界）
10..15  reserved                全零
```

模型通过校验即为 APPLIED；等待来源不算失败。`drawn`/`waiting` 变化时 Edge 发布新的 Actual。

证据 64 B：

```text
0       evidence_class          u8   1 = SIMULATED；2 = DRM_KMS_PAGE_FLIP，与 Catalog 一致
1       drawn                   u8
2       waiting                 u8
3..7    reserved                全零
8..15   generation              u64
16..23  desired_version         u64
24..55  model_sha256            32 B  模型规范字节的 SHA-256
56..63  reserved                全零
```

一个 generation 第一次被合成进已翻页的缓冲时发一次；此后 `drawn`/`waiting` 变化再发一次。
证据只在真实翻页（或桌面的软件完成）之后发，不以收到、校验通过代替。
选择 `XR_FRAME_SUBMITTED` 的会话改用 §14 的提交里程碑，不能把它解释成翻页。

视频呈现的顺序（meiso_core #36）：
- 同一媒体绑定（media stream epoch）内，Edge 只让串流往前走：`frame_sequence` 不大于已发布 AU 的帧不再发布；
  每次翻页前都用最新解出的一帧重新合成，不把装着旧 AU 的缓冲再翻上屏。
- 视频证据按翻页顺序发送。积压时可以丢掉最旧的证据，但不重排。
- 所以 Host 收到 AU N 的证据，就知道同一绑定内 `frame_sequence` 小于 N 的每个 AU 从那次翻页起不在屏上。
  这不说明它们从没显示过：没有证据的仍报 unobserved，只是不再"可能还在屏上"。

## 7. 静态图

- 图是基线 JPEG（SOF0，8 位，1 或 3 分量），字节数 ≤ `max_image_bytes`，宽高 ≤ 能力上限。
  ContentId 是字节的 SHA-256。用 Runtime 的 Host→Edge Blob 搬运（BEGIN/READY/CHUNK/COMMIT/RESULT）。
- 模型与图的先后不限：模型先到时 IMAGE 平面记为等待，图到后开始画并更新 Actual 与证据。
- 先后怎么算（meiso_host #71）：模型（平面 Desired 的状态 Offer）和图（Blob）走同一个 Session 的同一条有序控制连接。
  Edge 按完整收到的顺序处理，一张图以它的 COMMIT 为准；Host 按发出的顺序记。下面说某张图"在模型之后"，
  指这张图的 COMMIT 排在这个模型的 Offer 后面。
- Edge 持有的图：
  - 持有：最近应用的平面模型引用的图，加上在它之后收到的图。
  - 应用：可用的模型一到就应用，不等它的图；缺的图记为等待（§6），到了再画。
  - 释放：每应用一个平面模型，Edge 释放它不引用的图，图 Blob 和解码结果一起放掉，腾出位置给后来的图。
    §6 里观察到但不能用（REJECTED）的模型不释放，Edge 还在画旧模型。
  - 上限：同时持有的不同图超过 `max_images` 时，Edge 以 CAPACITY 拒绝新图。
- Host 的账按 Edge 报的状态记，不按自己发出了什么：
  - Edge 持有的图不会多于：它的 Actual 最近报为已应用（`applied_desired`）的模型 A 引用、已经送到的图，
    加上 A 之后送出的所有图。只有看到 Actual 报 A 已应用，Host 才把 A 不引用、在 A 之前送的图从账上去掉。
    REJECTED 的模型不会被报为已应用，账不减：之后账上的空位偏少，放不下的图就等，靠退回"模型先走"恢复——
    下一个可用的模型先发、被应用、释放旧图，Host 看到它的 Actual 后再送图。
  - 送一张图之前：账上的图加上它不超过 `max_images`；超过就等，等 Actual 报一个新模型已应用、账变小。
  - 发一个模型之前：它要的、Edge 缺的图，和账上的图加起来不超过 `max_images` 时，可以先送图、再发模型，
    换图时面板不空。超过时模型先发：它不等图就能应用，应用时释放旧图（相当于一个先撤掉旧图的中间模型）；
    Host 看到它的 Actual 后再送新图，这期间那些平面空着。所以整套换图（比如 4 张全换）也不会卡死。
  - 同一张图再送是合法的（Edge 还持有时答 DUPLICATE）。换图不需要新 Session，比如系统面板每次更新一张（meiso_host #71）。
- 图属于 Session：新 Session 需重新送图。
- 旧端：早于本条的 Edge 不释放图。这时一个 Session 累计送到 `max_images` 张不同的图以后，
  再送新图会被 CAPACITY 拒绝，引用它的平面一直等待，已有的图照常画。
- 旧端：早于本条的 Edge 拒收含 `opacity` 0 的模型（按原来的校验整个拒掉，不回 Actual），继续画上一个模型；
  这时 Host 改用撤平面。一个 Session 里第一个含隐藏平面的模型照常发；有 Actual 观察到含隐藏平面的模型，
  这个 Session 就一直用 `opacity` 0。发出后 2 s 内没有，Host 认定对端不收，这个 Session 余下的模型都去掉隐藏平面
  （再显示时重送图），并把当前模型重发一次。

## 8. 断线

Session 结束（任何原因）时 Edge 立刻撤下这个 Session 的全部平面，只留本地 HUD，HUD 显示已断开；
连接恢复、新 Session 送来模型后面板回来。Host 在新 Session 里重发模型与图，不假定 Edge 记得上一次。
断线由 Edge 本地发现：Wi-Fi 链路断开立即发现；Host 进程退出由 TCP 关闭发现；
对端静默由部署的 TCP 存活探测与 user timeout 兜底。本版不加应用层心跳。
卡顿不是断线：在部署的 user timeout 之内 Session 一直在，视频按 §2 的观察中断处理，不重建。
媒体（UDP）出错也不是断线（meiso_core #45）：对端不可达、网络不可达、接口下线、ICMP 拒收、缓冲满、
本机防火墙丢包，发和收都只丢这一包、按类计数，Session 的死活只由控制连接判断；
其余错误（包过大、描述符无效等配置或程序错误）照旧终止媒体，诊断带 errno。
TCP 的重传间隔逐次翻倍，卡 S 秒后控制连接可能要到开始卡后约 2S 才重新通，UDP 视频则一恢复就到；
两端的 user timeout 要按这个留余量（参考实现两端都是 25 s，10 s 的卡顿不断），Host 判"长时间无呈现证据"的上限要比它长。

## 9. 带宽与容量（本版常量）

| 项 | 数值 |
|---|---|
| 模型 | ≤ 1888 B/次，≤ `max_updates_per_second` 次/秒 |
| 静态图 | ≤ `max_image_bytes`/张，4096 B 一块 Blob chunk |
| 串流 | 与 v2 相同：独立帧 H.264，大小与帧率由显示 Desired 和 AU 上限决定 |
| Runtime | 基线 5 endpoint、18 route（Session registry 23）、capability 208 B；§14 扩展至 6/18、registry 24、352 B；§15 音频至 7/21、registry 28、368 B。Desired 后缀 ≤ 1888 B、Blob 单次 ≤ `max_image_bytes`；对端早于 §11 时协商 19，Runtime 按 4/15 配，不放输入；早于 §10 时协商 14，按 3/11 配，跟踪与输入都不放 |
| IMU（§10） | 200 Hz × 32 B = 6.4 KB/s；每批 8 个时约 25 条/秒 |
| 位姿（§10） | Host 自定，建议跟相机帧（约 30 Hz），≤ 200 Hz；72 B/条 |
| 按键（§11） | 人按键的速度，24 B/条；积压最多 32 条 |
| 音频（§15） | 只在按住 `SYSTEM` 时：32 KB/s 加每条 24 B 的头，≤ 1944 B/条；一段最多 `max_utterance_ms`，整段缓存 |

## 10. 跟踪：IMU 上行、头部位姿下行（meiso_host #53）

S-025：眼镜把相机与 IMU 用同一个时钟打时间戳回传，Host 算出"某一时刻的头部位姿"送回，
眼镜把它推到显示时刻再合成。相机沿用可选相机 endpoint；IMU 与位姿是这个可选 endpoint。
两边的时间都在 Edge 时钟上（clock epoch 4，与 §6 证据相同），Core 不加对时：
Host 的位姿由 Edge 时间的相机帧与 IMU 算出，本来就落在 Edge 时间上。

**相机帧的时刻**（meiso_edge #37）：相机帧的 source time 是这幅图与同板 IMU 对齐的有效拍摄时刻，用 Edge 时钟。
- 它由标定得到，约为曝光的中点；卷帘快门取整幅中间那一行。
- 曝光到驱动打戳之间的偏差由 Edge 在源头扣掉，按这副眼镜自己的标定。
- 没有标定的 Edge 送驱动时间戳或出队时刻；这个误差算在眼镜上，不由消费方补。
- Host 的 VIO、透视、App 都按 source time 原样使用，不再各自加延迟（OKVIS2 `image_delay` 为 0）。
- 标定跟着眼镜走，以后经能力上报给 Host（meiso_core #47 评论）；在那之前，标定只存在眼镜本地。

| route | owner / kind / facet | delivery | schema | 含义 |
|---|---|---|---|---|
| `DISPLAY_/TRK_DES_` | Host / State / DESIRED | inline | `DISP_SCH/TRK_DESI:v1` | 开不开 IMU 上行、会不会送位姿 |
| `DISPLAY_/TRK_ACT_` | Edge / State / ACTUAL | inline | `DISP_SCH/TRK_STAT:v1` | IMU 与位姿各自的状态 |
| `DISPLAY_/TRK_IMU_` | Edge / Stream / DATA | inline | `DISP_SCH/TRK_IMU_:v1` | IMU 样本，成批 |
| `DISPLAY_/TRK_POSE` | Host / Stream / DATA | inline | `DISP_SCH/TRK_POSE:v1` | 头部位姿 |

能力（Edge → Host，`DISP_SCH/TRK_CAP_:v1`，32 B，大端）：

```text
0..3    imu_rate_millihz     u32  0 = 没有 IMU 上行；否则 50000..2000000
4..7    accel_range_mps2     f32  满量程，0 < v ≤ 2000（imu_rate 为 0 时为 +0）
8..11   gyro_range_rps       f32  满量程，0 < v ≤ 200（同上）
12      max_imu_batch        u8   1..32，一条 IMU 记录最多几个样本
13      correction           u8   0 = NONE：只按位姿里的速度外推；1 = IMU：Edge 积分与屏同板的陀螺
                                  转动头部，位姿晚到或断了也接着转（要求有 IMU 上行；见下面"Edge 怎样用"）
14..15  max_prediction_ms    u16  1..200，Edge 最多往前推多久
16..31  reserved             全零
```

Desired 后缀 16 B：`0 imu_enabled u8 0/1`（capability 没有 IMU 时只能为 0）、`1 pose_enabled u8 0/1`、
其余全零。Actual 后缀 16 B：`0 imu_state u8`（0 OFF、1 RUNNING、2 FAILED）、`1 pose_state u8`
（0 OFF：与 0DoF 相同；1 APPLYING：WORLD 平面按收到的位姿画）、其余全零。

线上编号：Desired 的 delivery generation 为 15，Actual 为 16。两者任一开着时 Actual 为 RUNNING。
IMU 与位姿两条流都用 Actual 的 production epoch（production gate 按 Runtime 规则由 Edge 的 Actual 建立，
Host 拥有的位姿流也一样），stream epoch 都为 1，序号逐条递增。Runtime 要求同一 epoch 内 Actual 字节不变，
所以 Actual 只在有效配置变（Desired 变、IMU 坏了或恢复）时换，每换一次 production epoch 加一；
某一刻位姿用上没有（下面的保持）不进 Actual。Actual 换了之后，Host 在接受它之前发出的旧 epoch 位姿
到 Edge 时被 Runtime 拒收（`PRODUCTION_EPOCH_MISMATCH` 或 `ROUTE_NOT_ACTIVE`），Edge 丢掉，不算 Session 错误；
Host 接受新 Actual 后用新 epoch 接着发。

IMU 记录（Edge → Host，16 + 32×n B）：

```text
0..7    first_sequence       u64  本 Session 从 1 起连续编号；跳号即丢了样本
8       count n              u8   1..max_imu_batch
9..15   reserved             全零
每个样本 32 B：
0..7    time_ns              u64  Edge 时钟上的采样时刻，记录内严格递增
8..19   accel x/y/z          3×f32  m/s²，IMU 自己的轴，含重力，|v| ≤ 1.1 × 满量程
20..31  gyro x/y/z           3×f32  rad/s，IMU 自己的轴，|v| ≤ 1.1 × 满量程
```

IMU 到相机、到头部坐标系的外参是 Host 的标定，不上线。`correction=IMU` 时 Edge 要用 IMU 到头部的旋转：
它和 Host 用同一份标定，在眼镜本地配置，同样不上线（meiso_edge #35）。Edge 拿 Host 位姿里的转动核对陀螺积出的
转动，长期对不上（例如配错了标定）就不用陀螺，与 `NONE` 相同。

位姿记录（Host → Edge，72 B）：

```text
0..7    time_ns              u64  非零，Edge 时钟：这个位姿描述的时刻
8..11   world_epoch          u32  非零；Host 的世界坐标系重置（定位重新初始化）时换新值
12      state                u8   1 = TRACKING；2 = LOST
13..15  reserved             全零
16..27  position x/y/z       3×f32  头部原点在世界里的位置，米，|分量| ≤ 1000
28..43  orientation x/y/z/w  4×f32  头部 → 世界的旋转，单位四元数（容差同平面模型）
44..55  velocity x/y/z       3×f32  世界系线速度，m/s，|分量| ≤ 20
56..67  angular x/y/z        3×f32  头部系角速度，rad/s，|分量| ≤ 35
68..71  reserved             全零
```

头部坐标系与世界坐标系就是 §4 的（右手，米，x 右、y 上、看向 −z；头部原点是投影的视点）。

坏记录只丢这一条：一条 IMU 或位姿记录校验不过（长度不对、非有限值、越界、四元数不单位、保留位非零、
时刻为 0 或不递增、IMU 序号倒退），收方丢掉它并计数，不算 Session 错误，后面的好记录照常用。
位姿是 VIO 的浮点输出，偶尔一条坏数不该让画面断掉。Runtime 这一层（route、producer、epoch、流序号）
以及能力、Desired、Actual 出错，仍按原来的规则处理。

Edge 怎样用：
- 每次合成取最新的一条位姿，从 `time_ns` 推到这一帧预计翻页的时刻，推多久不超过 `max_prediction_ms`：
  `correction=NONE` 时按两个速度匀速外推。`IMU` 时转动按本板陀螺从 `time_ns` 积分：陀螺量到了的部分照用，
  只有越过最新样本的那一段按最后的角速度外推、不超过 `max_prediction_ms`；位置照旧按线速度外推。
  时刻在将来的位姿直接用，不往回推。
- `state=LOST`，或最新位姿比显示时刻旧 500 ms 以上：保持上一帧用过的位姿，WORLD 平面停在原处，
  不跳回单位位姿；新位姿到了接着用。`world_epoch` 变了直接换，不在两个世界系之间插值。
  `correction=IMU` 且陀螺可用时只保持位置：转动接着按陀螺积分，WORLD 平面留在房间里，
  直到新的 TRACKING 位姿到、`world_epoch` 变了或 Session 结束。陀螺不可用（最新样本比显示时刻旧
  100 ms + `max_prediction_ms` 以上，或核对不过）时与 `NONE` 相同。
- `correction=IMU` 且陀螺可用时，到达的位姿早于 Edge 还留着的最早陀螺样本（断线恢复时补到的积压），
  就积不到现在，Edge 不用它，接着按陀螺转，等能接上的位姿。
- `pose_enabled=0` 或还没收到位姿：与 0DoF 相同。
- 同一世界里时刻不晚于已有位姿的记录丢弃。IMU 队列满了丢最旧的（Host 看跳号），位姿只留最新的。

## 11. 输入：按键上行（meiso_host #66）

S-025：交互参考 VR，视野中心一个准星，看向面板再按键，选中或抓起。S-026：锁屏、选连哪台 Host 是眼镜本地的界面。
S-030：APP 只经 Host SDK 拿输入。S-033：Holo 键（原名 Claire，S-039）只归系统。控制板只报原始的按下、松开：去抖在 Edge，
长按、双击等手势由 Host 从按下、松开的时刻判断。

分工：
- **Edge 本地，不上线**：
  - 锁屏：拨下后 Edge 立即整屏黑，平面、HUD、准星都不画，也不上送按键；拨回恢复。不需要 Host，没有 Session 时也一样。
  - 选连哪台 Host：本版不做界面，只定下它归 Edge。
  - 准星，和准星下那块平面的高亮。
  - 这些键不上线，本页也不给它们编号。
- **上送 Host**：其余按键，每条带上按下那一刻准星下的平面。焦点、抓起、拖动、摆正由 Host 决定，结果是新的平面模型（§4）。
- **只归系统**：`SYSTEM` 键（Holo 键，S-033、S-039）和 `MENU` 键（系统菜单，S-045）。Host 不得把它们交给任何 APP。
  其余键交给哪个 APP 是 Host 的策略。
  参考实现：
  - 五向键的上下左右只给焦点 APP。
  - `SELECT`：准星下不是焦点面板时，用来切换焦点，归系统；准星下正好是焦点面板时，交给这个 APP，相当于点一下。
    长按抓起面板，归系统。
  - `BACK`：短按给焦点 APP；长按把面板摆回视线正前方，归系统。
  - `MENU`：短按展开或收起系统菜单；长按召回全部窗口（S-045）。

| route | owner / kind / facet | delivery | schema | 含义 |
|---|---|---|---|---|
| `DISPLAY_/INP_DES_` | Host / State / DESIRED | inline | `DISP_SCH/INP_DESI:v1` | 要不要按键 |
| `DISPLAY_/INP_ACT_` | Edge / State / ACTUAL | inline | `DISP_SCH/INP_STAT:v1` | 上送状态、是否锁屏 |
| `DISPLAY_/INP_KEY_` | Edge / Stream / DATA | inline | `DISP_SCH/INP_KEY_:v1` | 按键事件 |

能力（Edge → Host，`DISP_SCH/INP_CAP_:v1`，16 B，大端）：

```text
0..3    keys       u32  眼镜有的键：第 k 位对应键码 k（下表）；至少一个，第 0 位与第 9..31 位为 0
4       pointer    u8   0 = NONE；1 = HEAD_RAY：Edge 画准星，事件里带准星下的平面
5..15   reserved   全零
```

| 键码 | 名字 | 控制板上 |
|---|---|---|
| 1 | UP | 五向键上 |
| 2 | DOWN | 五向键下 |
| 3 | LEFT | 五向键左 |
| 4 | RIGHT | 五向键右 |
| 5 | SELECT | 五向键按下（"确认"） |
| 6 | BACK | 返回 |
| 7 | SYSTEM | Holo 键（原名 Claire，S-039），只归系统 |
| 8 | MENU | 系统键（S-045：S-035 的自定义键改作它），只归系统；2026-10-05 起 |

Desired 后缀 16 B：`0 keys_enabled u8 0/1`，其余全零。有输入 endpoint 时 Host 必须发这个 Desired（可以为 0），
Edge 才能报 Actual。Actual 后缀 16 B：`0 keys_state u8`（0 OFF；1 RUNNING：上送按键）、`1 locked u8 0/1`、
其余全零。锁屏与 Desired 无关，Desired 为 0 时也照报。

线上编号：Desired 的 delivery generation 为 17，Actual 为 18。按键流用 Actual 的 production epoch，
stream epoch 为 1，序号逐条递增。Actual 只在 Desired 变或锁屏切换时换，每换一次 production epoch 加一，
之后的事件用新 epoch 发。按键流与 Actual 同属 Edge、走同一条有序连接，换之前发出的事件先于新 Actual 到，
照常被收下。锁屏时还没发出的事件丢掉：屏已经黑了，之后再执行只会让人意外。

按键事件（Edge → Host，24 B）：

```text
0..7    time_ns            u64  Edge 时钟（clock epoch 4）：这次变化的时刻，取去抖前的第一次跳变
8       key                u8   键码，必须在 capability 里
9       action             u8   1 = PRESS；2 = RELEASE
10..11  reserved           全零
12..15  target_plane       u32  这一刻准星下的平面 plane_id；0 = 没有（或 pointer=NONE）
16..23  target_generation  u64  target 所在模型的 generation；target 为 0 时为 0
```

- **交替**：每个键的事件 PRESS、RELEASE 交替。RUNNING 之前就按住的键，松开时不报，下次按下才报。
- **准星下的平面**：
  - 取最近一次合成所用的头部位姿，从头部原点沿 −z（正前方）打一条射线，与这一帧画了的平面的矩形求交，
    取画在最上面的那块（§5）：射线碰到系统平面时取系统平面里最近的，否则取 APP 平面里最近的。
  - HEAD 与 WORLD 平面都算，HUD 不算。
  - Host 按这个 plane_id 行动，不用自己的位姿重算，因为用户看见的是 Edge 画的那一帧。
- **顺序与积压**：
  - 事件按发生顺序发，不合并、不重排；`time_ns` 不减（去抖推迟决定的变化，时刻取不早于前一条的）。
  - 积压（如卡顿中）超过 32 条时丢最旧的。
  - Host 收到某键的 PRESS 而它认为这个键已按住时，按"先松开再按下"处理；收到没按住的键的 RELEASE，忽略。
  - Actual 报 `locked=1` 时，Host 把它认为按住的键都当作已松开（锁屏前按住、锁屏后松开的键不会再报）。
  - 按下时刻太旧的事件（如卡顿后才到）还执行不执行，由 Host 决定。
- **坏记录**只丢这一条，同 §10。
- **锁屏中没有呈现证据**：锁屏时 Edge 不画平面，视频与模型都没有呈现证据。
  - Edge 在第一帧黑屏翻页之后才报 `locked=1`，并排在此前已有的呈现证据之后（解锁同理，第一帧亮屏翻页之后报）。
    所以 Host 读到 `locked=1` 时，锁屏前显示过的帧的证据都已到；还没有证据的帧不会再显示，屏幕状态是已知的（黑）。
  - Host 看到 `locked=1` 应暂停送视频帧；已送出、没有证据的帧按"没显示、屏幕已知"处理，不隔离视频。
  - `locked=1` 期间，Host "多久没有呈现证据就隔离视频"的计时不走。
  - 解锁后 Edge 恢复画面，Host 接着送帧，不重建会话。位姿（§10）停不停由 Host 决定。
- **锁屏只管屏幕，不管记录**：相机、IMU 上行和系统默认记录照常（S-014）。要停记录用 Host 的记录暂停
  （参考实现 `meiso-history pause`）。
- **旧端**：
  - 对端早于本节时，Session 注册表协商成 19 或更小，Edge 不放输入 endpoint。
  - 这时准星不画，本地锁屏照样能用。
  - Host 不知道锁屏，锁得久了会按 §2 与部署的上限隔离视频、重建会话。

## 12. 不在本版

- 深度网格。S-025 说深度网格场景就是大量小平面，但现有稀疏网格有 30×34 到 120×135 个顶点，
  一格一条 96 B 的平面记录走不通。以后作为"带深度网格的平面"另加；16 块的上限只管普通面板，
  不为深度网格放大。
- 准星在面板上的落点（面板内的指点、点击位置）。本版只报准星下是哪块面板。
- 选连哪台 Host 的界面：归 Edge 本地（§11），本版不做。
- IMU 上行改走 UDP 以扛卡顿（meiso_core #36 验收时留下）。
- 放音（助手的回答念出来）：另开一节，等喇叭接上、电流量过再写。音频上行见 §15。

## 13. 验收

- 呈现顺序（§6）：媒体接收在较新的帧发布后，迟到的旧帧不再发布（`SEQUENCE_REPLAY`）。
- 编解码：golden bytes 往返；保留位、越界数值、非有限浮点、非单位四元数、重复 id、
  坏 UTF-8、`video_desired_version` 不符、超出能力，全部拒绝且不改输出。
- 桌面：模拟后端画出的像素与几何期望一致（换位姿后四角跟着变）。
- 板上（meiso_edge #2）：三样同屏、换位姿、断线与恢复、实际翻页证据。
- 跟踪（§10）：能力、Desired、Actual、IMU、位姿的 golden bytes 往返；保留位、越界、非有限值、时间不递增、
  批量超限、`world_epoch`/时刻为 0、没有 IMU 却要 IMU、跟踪与 `tracking` 字段不一致、optional endpoint 乱序，
  全部拒绝。经过两端 Runtime：一条坏位姿照常送到、收方丢掉，Session 还在，下一条好位姿照常用。板上（meiso_host #53）：Host 脚本位姿下 WORLD 平面跟着动、HEAD 平面与 HUD 不动。
- 输入（§11）：
  - 能力、Desired、Actual、按键事件的 golden bytes 往返。
  - 这些都拒收：保留位非零、没有键或第 0 位/第 8 位以上的键、事件的键不在能力里、action 不是 1/2、target 为 0 而 generation 非 0、时刻为 0。
  - 注册表协商：对端 19 时 Catalog 不能有输入 endpoint，23 时可以；输入 endpoint 排在跟踪之后，乱序拒绝。
  - 经过两端 Runtime：按键事件按序到 Host；锁屏、解锁各换一次 production epoch，之后的事件用新 epoch 照常到；
    一条坏事件照常送到、Host 丢掉，Session 还在。
  - 准星求交：给定位姿与模型，求出的平面与期望一致（最近的一块、背面与没画的不算）。
  - 系统平面在上（meiso_host #73）：APP 平面比系统平面近、两者重叠时，重叠处是系统平面的像素，准星取系统平面。
  - 板上（meiso_host #66）：锁屏 5 分钟再解锁，量面板回来要多久；全程会话不重建。
- 静态图（§7）：
  - Blob 存储能释放一张已发布的图，释放后查不到，位置给下一张，同一张图能再发布。
  - 两端：换图次数超过 `max_images`，每次都不被拒；不再引用的图被释放；再用一张已释放的图，Host 重新送，
    平面照常画出。
  - Host 记账（对着一个按本节行事的 Edge，它的 Actual 和 Blob 结果可以晚到）：满容量整套换图（4 张全换）不被拒、不卡死；
    REJECTED 以后 Host 不减账，新图等到有模型应用后再送，不被拒。
  - 隐藏（`opacity` 0）的平面不画、不是准星下的平面，它的图不释放；再显示不重送图。
  - 对着早于本条的 Edge，Host 改用撤平面：含隐藏平面的模型被拒以后，去掉隐藏平面的模型被应用，显示、收起照常。
- 板上（meiso_host #71）：系统面板反复更新几十次，每次都上屏，Session 不重建；收起再打开不重送图。

## 14. Edge 自定位、独立视图与 XR 提交

本节对应 meiso_vr #1 的 Kache 09-29 裁定。在 v12 原地扩展；不建立 VR 专用 Profile。
§3 的 `tracking=3 EDGE`、`view_layout=3 RUNTIME_VIEWS` 及显示 capability 的
`evidence_class_mask=4`（bit 2：`XR_FRAME_SUBMITTED=3`）均为显式选择。旧的 NONE/POSE、
MONO/SIDE_BY_SIDE、SIMULATED/DRM_KMS_PAGE_FLIP 的解释、字节和行为保持。
旧端对这些新增值、位或模型保留区明确拒绝，不得降级成 NONE/MONO/DRM 后继续。

### 14.1 眼镜自带定位

endpoint 为 `DISPLAY_/LPOSEEND`，capability schema `DISP_SCH/LPS_CAP_:v1`，16 B：
0..1 是大端 u16 `max_samples_per_second`，1..200；2..15 保留为零。
它与 §10 `TRCK_END` 互斥；`tracking=EDGE` 当且仅当存在它，POSE 当且仅当存在 §10。
定位来源不绑定 VR、OpenXR 或某种传感器；本轮 K230D 不实现此 endpoint。

| route | owner / kind / facet | schema | payload |
|---|---|---|---|
| `DISPLAY_/LPS_DES_` | Host / State / DESIRED | `DISP_SCH/LPS_DESI:v1` | Runtime prefix + 16 B |
| `DISPLAY_/LPS_ACT_` | Edge / State / ACTUAL | `DISP_SCH/LPS_STAT:v1` | Runtime prefix + 16 B |
| `DISPLAY_/LPS_POSE` | Edge / Stream / DATA | `DISP_SCH/LPS_POSE:v1` | 72 B |

全部 inline。Desired 后缀 byte 0 `enabled` 为 0/1，其余零；关闭只停止上行，
不关闭 Edge 为显示而运行的本地定位。Actual 后缀 byte 0 `state` 为 0 OFF、1 RUNNING、
2 FAILED，其余零；OFF 对应 production STOPPED、FAILED 对应 production FAILED，
两者 gate 关闭，RUNNING 建立非零 production epoch。Desired/Actual delivery generation
分别为 19/20；位姿 stream epoch 为 1、序号递增，使用当前 Actual 的 production epoch。
实际配置或服务状态改变才换 Actual/production epoch；短暂失跟踪通过位姿 LOST 表示。

位姿复用 §10 的 72 B 数值布局、坐标系与校验，使用独立的 Edge-owned schema/route。
`time_ns` 是 Edge 单调时钟上所描述的**当前定位时刻**，不得晚于发出时刻；禁止上传
预测显示时刻的位姿。Edge 本地渲染可以另取预测显示时刻的定位，不绕经 Host。
只发送最新待发样本，不等业务 ACK、不补发历史，频率不得超过能力上限。
位置、方向均有效且被跟踪时才报 TRACKING；否则报 LOST，数值仍须有限合法。
速度不可靠时填零。坏数值、时刻倒退或过期只丢当前记录并计数，不终止 Session；
Runtime 的 route/producer/production epoch 失配仍按既有 gate 处理。

`world_epoch` 为非零 u32；新 Session 从新的共享世界开始。Edge 管理本地定位空间到共享世界
的转换。定位参考空间改变时，能保持旧共享世界就更新转换；不能保持则换 epoch、报 LOST。
模型头 24..27 是大端 u32 `world_epoch`，19..23、28..31 仍为零；EDGE 模型必须非零，
非 EDGE 必须零。WORLD 只在模型 epoch 与有效定位 epoch 相同且定位新鲜时绘制/求交；
不匹配或 LOST 时 WORLD 不画、计 waiting，HEAD 保持。隐藏平面仍不计 drawn/waiting。
Host 收到新 epoch 后重建布局；禁止跨世界插值。长按 BACK 仍由 Host 改模型，不命令运行时 recenter。

位姿、§11 按键、提交和预计显示时刻都属于同一 Edge 时钟（clock epoch 4）。Core 不加对时，
不认为 Windows 与 WSL 时钟同源。Host 保存原始 Edge 时刻及一次接收/首次发布时刻，供其
`WindowedClockOffset.observe(Edge time, Host publication time)` 使用；转发不重新打接收时刻。
offset 是 Edge−Host 的传输窗口估计，known 不代表误差已知，不作为单程延迟证明。
新 Session 清空时钟估计；world_epoch 改变不表示 Edge 时钟重置。

Host 按已校验 Catalog 的 tracking 选择唯一来源：POSE 用 HeadFusion，EDGE 用 Edge 位姿，
NONE 不启用定位；registry 容量 gate 只作前提，不选择来源。首页、抓起、摆正和按键解释共用该来源。
以 Edge 按键时刻查同一 epoch 的历史，候选最多 128 条且最多保留 500 ms；有两侧有效样本
时插值，否则只用不晚于按键的有效样本。样本年龄超过 500 ms、LOST、Actual OFF/FAILED
或 epoch 失配时不执行 WORLD 交互，不等未来包补位。HEAD 输入仍可使用 §11 的 target。
按键 target/generation 来自 Edge 最近一次有效提交所用模型；Host 拒绝已失效模型/世界的目标。

### 14.2 独立视图

`view_layout=RUNTIME_VIEWS` 不存在整块 MONO/SBS 输出栅格。旧 64 B PLN_CAP 的
output_width、output_height、四个 FOV 与 ipd_m 在此分支必须全为 +0，表示不用这些字段；
其他布局仍严格按 §3 原范围解释。不能把超出 4096 的推荐尺寸截小或伪报 MONO。

此布局当且仅当有 `DISPLAY_/VIEWSEND` endpoint，schema 为 `DISP_SCH/VWS_CAP_:v1`。
它只有 immutable capability、没有 route，不需要 Desired/Actual。固定 144 B：

```text
0       view_count             u8，1..2
1..15   reserved               全零
16..79  view[0]                64 B
80..143 view[1]                64 B；view_count=1 时全部为零
每个 view：
0..3    recommended_width      u32，非零，渲染纹理预算信息
4..7    recommended_height     u32，非零，同上
8..23   fov_left/right/up/down 4×f32，范围同 §3
24..35  position x/y/z         3×f32，相对头部视点的米制位置，|分量|≤1
36..51  orientation x/y/z/w    4×f32，视图→头部的单位四元数，容差同 §4
52..63  reserved               全零
```

字段不绑定 OpenXR；未来双目 Edge 可选择它。推荐尺寸不等于光学标定、物理面板分辨率、
串流尺寸或必须分配的显存；消费者计算预算使用检查过的宽整数且受本机资源上限约束。
Host 由各视图 FOV 和相对头部的位姿摆平面；Edge 使用本地运行时的实际视图。
本轮重大视图配置变化重建 Session/Catalog，不另加动态视图状态机。
max_planes 必须扣掉本地 HUD/准星所需层，只报剩余业务层容量；不把未来合层作为预算前提。

### 14.3 提交证据

显示 capability 的 bit 2 只能在 v12/H.264 选择，且与 bit 0/1 互斥。
对应的证据类别为 3 `XR_FRAME_SUBMITTED`，物理呈现始终为 false。它证明运行时接受了
这一版模型和所用 AU；强度与桌面软件完成同级。K230D 的 DRM 证据含义不变。

模型证据仍为 §6 的 64 B；类别 3 使用 56..63 保存大端 u64 `predicted_display_time_ns`，
必须非零。类别 1/2 的该区域仍全零。提交接受时刻在外层 Core Stream `source_time`，
clock epoch 为 4；预计显示时刻也属于此时钟，可能早于实际返回时刻，不强制二者的先后。

视频证据在类别 3 时为 **72 B**：沿用 `DISP_SCH/PRES_EVT:v2` 的前 64 B，类别为 3，
connector_kind 为 UNSPECIFIED，output_width/height 为零（无单一输出栅格），
64..71 追加非零 `predicted_display_time_ns`。原 `displayed_frame_sequence` 槽位在该类别
仅表示被提交的 AU sequence；API 使用同槽别名 `submitted_frame_sequence`，禁止写成已显示。media epoch/AU SHA-256 保持原位，
外层 source_time 是提交接受时刻。类别 1/2 仍严格 64 B，不能带追加字段；
类别 3 严格 72 B，对应视频证据 route 的 payload 上限改为 72，其他会话仍为 64。
模型证据 decode 只校验数值/布局；接收者还须用 `meiso_display_planes_evidence_supported`
按会话视频能力的 evidence mask 检查类别，视频证据使用已有的 `meiso_display_ingress_evidence_supported`。

模型和 AU 证据只在包含它们的帧被运行时成功接受后发，按提交顺序发送。
`shouldRender=false`、会话不可见、调用失败、锁屏或对应内容未提交时不发内容成功。
首次接受新模型/新 AU 发一次，drawn/waiting 变化时模型再发。空模型撤内容也要等实际接受
清除提交。运行时 swapchain 的 acquire/wait/release 与销毁由 Edge 正确维护。

Host 按会话声明的类别选择完成里程碑：模型进度、有界发送、§7 的资源记账/释放及输入/首页
操作的 executed 可以以提交为准；不写入物理 PRESENTED 历史，不在报告中称为“上屏”。
收到新 AU 的提交证据不证明旧 AU 已经离开屏幕；只能把预计显示时刻后的替换标为估计。
§2 停视频与 §11 黑屏/解锁 Actual 在 XR 会话等待相应清除/恢复提交被接受，不声称屏幕物理
状态已知；Host 可以暂停/恢复准入，未观察的 AU 仍不能升级为已显示或肯定未显示。

本轮延迟主数为 Host 发出模型→Edge 提交接受的 p50/p95，另报预计显示时刻减提交时刻。
跨钟映射不足时只报往返上界，并加佩戴实物观察确认内容出现；不要求光子延迟。

### 14.4 容量和失败向量

完整上限为 6 endpoint、18 route、registry 24、capability 352 B。定位与旧跟踪互斥，
所以自定位的最大全量组合为 6 endpoint/17 route/336 B；无相机的 VR 组合更小。
新可选项仅在协商 registry≥24 时使用；不足时需要它们的 Edge 明确拒绝本次接入，
不能假报旧能力。旧端协商 23/19/14 时 Runtime 仍按原来的容量级别配置。§15 再加音频后的上限见 §15.4。
Desired、模型、JPEG、Blob chunk、Session/Runtime 帧上限不变。

必须验证：旧解码器拒绝每个新增枚举/bit、非零模型 epoch 与 72 B 视频证据；
旧模式 golden bytes 不变；混用两个定位 endpoint、错 owner/schema/顺序、view_count/保留位/
FOV/四元数越界、旧布局带视图 endpoint、EDGE 模型 epoch 为零、非 EDGE 模型 epoch 非零、
类别与证据长度/预测时刻不符，全都拒绝且不改输出。经过真实 Runtime 的错误位姿只丢样本，
下一条好样本继续，production gate 失配拒绝；Host/Edge 实现还须验证时钟、旧世界停止绘制、
LOST/FAILED/过期时 WORLD 停交互、HEAD 保持，以及提交不能变成物理呈现。

可执行入口：`tests/test_display_ingress_edge_extensions.c`；真实旧 SDK 拒绝检查及固定修订
复现见 [legacy-v12-consumer](../../tests/legacy-v12-consumer/README.md)。codec/Catalog/Runtime
测试不替代 Host 时钟/交互、Edge 绘制与头显提交实测。

## 15. 音频：按住说话（meiso_host #93）

纲领第 4 点：按住 `SYSTEM` 键问系统助手 Holo。S-032/S-033：助手是系统的，APP 拿不到系统权限。
S-035：不做常开麦克风，也不做唤醒词。本节只管眼镜到 Host 的上行，放音另开一节（§12）。

### 15.1 谁能打开麦克风

- **只有眼镜上的物理按键能打开麦克风。**
  - Edge 在 `SYSTEM` 键（§11 键码 7）按下时开始录，松开时停，不等 Host。
  - Host、APP、远端都没有办法让 Edge 开始录。
- Host 的 Desired 只是许可，不是开关：
  - `allowed=0`（或还没有 Desired）时 Edge 什么都不录，按键照常上送；
  - `allowed=1` 时 Edge 也只在按住 `SYSTEM` 时录。
- Edge 只在同时满足下面几条时开始录：
  - 音频 Actual 为 READY；
  - 输入 Actual 为 RUNNING，且没有锁屏；
  - 这次 `SYSTEM` PRESS 已按 §11 上送。
- 锁屏时不录。§11 锁屏时不上送按键，这里也一样。
- 录音时 Edge 在 HUD 上画麦克风标记。这由 Edge 本地画，同准星，不需要 Host。
- **只归系统**：
  - 音频和由它得到的文字只交给系统助手，Host 不得交给任何 APP，与 `SYSTEM` 键同一规则（§11）。
  - 以后 APP 若要麦克风，另开权限，不在本版。
- **不进默认记录**：
  - 原始音频不进系统默认记录（S-014），只在 Host 内存里用于识别，用完即丢，不写盘。
  - 识别出的文字和助手的回答进助手的对话记录，跟着记录暂停与删除走（参考实现 `meiso-history pause`/`delete`）。
  - 开发时可以显式打开保存原始音频，默认关。

### 15.2 线路与编码

| route | owner / kind / facet | delivery | schema | 含义 |
|---|---|---|---|---|
| `DISPLAY_/AUD_DES_` | Host / State / DESIRED | inline | `DISP_SCH/AUD_DESI:v1` | 允不允许按住说话 |
| `DISPLAY_/AUD_ACT_` | Edge / State / ACTUAL | inline | `DISP_SCH/AUD_STAT:v1` | 麦克风状态 |
| `DISPLAY_/AUD_PCM_` | Edge / Stream / DATA | inline | `DISP_SCH/AUD_PCM_:v1` | 音频记录 |

Endpoint 为 `DISPLAY_/AUDI_END`，排在所有可选 endpoint 最后（视图之后）；它的三条 route 排在输入的之后。
音频只随带 `SYSTEM` 键的输入 endpoint 出现，没有输入或输入没有 `SYSTEM` 键的 Catalog 不能有音频。

能力（Edge → Host，`DISP_SCH/AUD_CAP_:v1`，16 B，大端）：

```text
0..3    sample_rate_hz     u32  本版只有 16000
4       channels           u8   1
5       format             u8   1 = S16LE
6..7    max_utterance_ms   u16  1000..60000：一段最长多久
8..15   reserved           全零
```

Desired 后缀 16 B：`0 allowed u8 0/1`，其余全零。

Actual 后缀 16 B：`0 state u8`，其余全零。
- 0 OFF：不许录，或还没有 Desired；
- 1 READY：按住 `SYSTEM` 时录；
- 2 FAILED：麦克风打不开。

Actual 只在 Desired 变、麦克风坏了或恢复时换，每换一次 production epoch 加一。正在不在录不写进 Actual。

线上编号：
- Desired 的 delivery generation 为 21，Actual 为 22。
- 音频流用 Actual 的 production epoch，stream epoch 为 1，序号逐条递增。

音频记录（Edge → Host，24 + 2×n B；头部大端，样本小端）：

```text
0..7    time_ns     u64  Edge 时钟（clock epoch 4）：这条第一个样本的采样时刻
8..11   utterance   u32  本 Session 从 1 起，每次按住加一
12..15  index       u32  这段里的第几条，从 0 起连续
16      flags       u8   bit0 FIRST：index 为 0 的那条，且只有它；
                         bit1 LAST：这段的最后一条；
                         bit2 TRUNCATED：到了 max_utterance_ms 还按着；
                         bit3 ABORTED：这段作废（锁屏、Desired 改为禁止、麦克风出错）。
                         TRUNCATED 与 ABORTED 只随 LAST，且不同时出现；其余位为 0
17      reserved    全零
18..19  count n     u16  0..960（最多 60 ms）；0 只随 LAST
20..23  reserved    全零
然后 n 个 S16LE 样本，单声道
```

记录长度必须正好是 24 + 2×n。坏记录只丢这一条，Session 不断，同 §10、§11。

### 15.3 一段话

- **分段**：
  - 每次 `SYSTEM` PRESS 开一段。这段音频属于刚上送的那条 PRESS：两者同属 Edge、走同一条有序连接，PRESS 先到。
  - 松开后，Edge 发完剩下的样本，最后一条带 LAST（可以没有样本）。
  - 到了 `max_utterance_ms` 还按着：这段结束，最后一条带 LAST|TRUNCATED，松开之前不再开新的一段。
  - 锁屏、Desired 改为禁止、麦克风出错：这段结束，最后一条带 LAST|ABORTED，Host 丢掉整段。
- **短按与按住**：
  - Edge 每次按下都录，不做判断。
  - Host 用 §11 那一对 PRESS、RELEASE 的 `time_ns` 算按了多久：
    - 短于 300 ms（`MEISO_DISPLAY_AUDIO_SHORT_PRESS_MS`）算短按：这段不送识别，直接丢掉，`SYSTEM` 按一次点按处理（参考实现：开关助手面板）；
    - 300 ms 及以上算按住说话。
- **一段之内不丢**：
  - 语音转文字宁可晚，不能缺，这一点与视频相反。
  - Edge 缓存整段（30 s 为 960 KB）。卡顿时不丢样本，恢复后接着发；积压时一条可以放到 960 个样本，好追上。
  - 只有 Session 断了这段才作废：Edge 丢掉没发完的，新 Session 不补发；Host 丢掉没收到 LAST 的那段。
  - Host 看到同一段里 `index` 不连续（有坏记录被丢）时，丢掉整段并计数。
- **不挤住按键和 IMU**：
  - 按住说话时，按键和 IMU 照常准时到。
  - Edge 先发按键、IMU 与证据；音频只在它们没有待发时发，而且不在连接里积压。
  - 卡顿恢复后，先到的是按键与 IMU，音频随后追上。

带宽：16 kHz × 2 B = 32 KB/s（256 kbit/s），加每条 24 B 的头；20 ms 一条时约 50 条/秒。

### 15.4 容量与旧端

- 完整上限增至 7 endpoint、21 route、registry 28、capability 368 B（§14.4 的 352 B 加 16 B）。
  音频记录最大 1944 B，在 Session 与 Runtime 的帧上限之内。
- 协商 registry ≥ 28 时 Runtime 按 7/21/368 配。24/23/19/14 保持原来的容量级别（§9、§14.4）。
- 对端早于本节时，Session 注册表协商成 24 或更小，Edge 不放音频 endpoint：
  - 新 Edge 对旧 Host、旧 Edge 对新 Host 都能建立 Session；
  - 按键照常，`SYSTEM` 照常上送（短按照旧），只是没有语音。
- 不在本版的：放音（§12）、APP 用麦克风、压缩（Opus 等）、常开与唤醒词（S-035）。

### 15.5 验收

- 能力、Desired、Actual、记录的 golden bytes 往返。
- 这些都拒收，且不改输出：
  - 保留位非零、采样率不是 16000、声道不是 1、格式不是 S16LE、`max_utterance_ms` 越界；
  - state 越界；
  - 时刻为 0、utterance 为 0；
  - FIRST 与 `index` 为 0 不一致、未知 flag、TRUNCATED 或 ABORTED 不随 LAST、两者同时出现；
  - 空记录不带 LAST、n 超过 960、长度与 n 不符。
- Catalog：
  - 完整组合（相机、跟踪、输入、视图、音频）为 7/21/368；
  - 音频排在视图之后，乱序拒绝；
  - 没有输入或输入没有 `SYSTEM` 键时不能有音频；
  - 关掉音频后 bytes、endpoint、route 与本节之前完全相同。
- 注册表协商：对端 28 时可以有音频；24 时不能，Runtime 按 6/18/352 配；23/19/14 不变。
- 经过两端 Runtime：
  - 一段三条（含 960 个样本的最大一条、没有样本的 LAST）按序完整到达；
  - 一条坏记录照常送到、Host 丢掉，Session 还在；
  - 改为禁止换一次 production epoch，旧 epoch 的记录 Edge 不能再发。
- 可执行入口：`tests/test_display_ingress_audio.c`。
- 板上（meiso_host #93）：
  - 按住 `SYSTEM` 说话，助手面板出现文字。量按下到第一条音频到 Host、松开到文字出现的 p50/p95；
  - 按住时 Wi-Fi 卡顿 1、3、10 s，一段之内不丢（`index` 连续），文字照样出现；
  - 没按键、锁屏时 Host 收不到音频，Host 盘上没有原始音频；
  - 短按照旧开关面板，不送识别。

## 16. 动效：眼镜按目标插值（meiso_host #116）

面板出现、收起、挪动，Host 只发目标和时长，过渡由眼镜按自己的时钟和位姿逐帧算。Host 不用每帧发一个模型，
动画也不受 `max_updates_per_second`、Wi-Fi 来回和 Host 那边旧位姿的影响。懒跟随（FOLLOW）的面板平时停在房间里，
头转开超过一个角度才挪回面前，跟着头的那一段用的是眼镜手上最新的位姿。

### 16.1 能力与旧端

能力字节 42 `motion`（§3）：
- 位 0 `TRANSITION`：按 92、93 字节插值（16.3）。
- 位 1 `FOLLOW`：认锚点 3（16.4）。只有 `TRANSITION` 也为 1 时才可以声明。

不声明的 Edge 这里是 0，照 §4 拒收 92..94 非零或锚点 3 的模型。Host 看到 0 就把这些字段写 0、不用锚点 3，和本节之前一样：
一下子到位；要淡入淡出，就逐个模型改 `opacity`（受 `max_updates_per_second` 限制）。

### 16.2 字段

| 字节 | 名字 | 取值 |
|---|---|---|
| 92 | `transition_cs` | 0..250，单位 10 ms：从眼镜上正在显示的样子到这条记录的样子要多久。0 为一下子到位 |
| 93 | `motion_style` | 低 4 位 `curve`，高 4 位 `enter`，各 0..2 |
| 94 | `follow_deg` | 锚点 3 时 1..60（度），其余锚点为 0 |
| 95 | reserved | 全零 |

`curve`（进度 t 从 0 到 1，p 为走过的比例）：
- 0：线性，p = t；
- 1：先快后慢，p = 1 − (1 − t)³；
- 2：两头慢，p = 3t² − 2t³。

`enter`，面板出现时的样子。"出现"指这块面板不在上一版已应用的模型里，或在里面但 `opacity` 为 0、这次不为 0：
- 0：从 `opacity` 0 直接到目标，不过渡；
- 1：从 `opacity` 0 淡入；
- 2：淡入，同时宽高从 92% 放大到 100%，中心不动。

`enter` 1、2 用这条记录的 `transition_cs` 和 `curve`；`transition_cs` 为 0 时与 0 相同。

### 16.3 过渡

时刻用 Edge 的单调时钟，从 Edge 应用这个模型（§6 APPLIED）的那一刻开始算。每块面板逐帧求它此刻的位置、朝向、宽高和 `opacity`：
- 上一版已应用的模型里有同一个 `plane_id`、锚点也没变，又不是出现（16.2）：
  - 起点取它此刻显示中的值（可能还在上一次过渡的半路），终点取新记录。
  - 用 `transition_cs` 和 `curve` 走完：位置、宽高、`opacity` 线性，朝向按较短的弧做球面插值。
- **目标没变就不重新开始**：Host 会因为别的原因重发整张平面表，比如别的面板变了、一个 APP 的标注每秒更新几次、
  另一块面板在逐步改 `opacity`。所以这块面板的锚点、位置、朝向、宽高、`opacity`、92..94 若与上一版逐字节相同，
  它正在走的过渡照旧按原来的起始时刻走完，不从此刻重新计时。
- 锚点变了：一下子到位。在两种坐标系之间插值没有意义。
- 图或取样矩形变了：位置这些照样插值，图本身一下子换掉。交叉淡化要同时留着两张图，本版不做。
- 出现：按 `enter`（16.2）。
- 收起：写 `opacity` 0 加时长，面板淡出。图还在模型里，§7 的账不变。
- 撤平面（下一版模型里没有它）：一下子消失。要淡出，Host 先写 `opacity` 0 加时长，过了时长再撤。
- 目标 `opacity` 为 0 的平面，从模型应用起就算隐藏：不算 `drawn`/`waiting`（§6），也不当准星下的平面（§11）。
  它在淡出的这段时间里照样画。目标不为 0 的平面，淡入时就算 `drawn`，也可以是准星下的平面。
- 准星求交（§11）和画的先后（§5）都用这一帧画出来的位置和大小。
- 证据不变（§6）：新 generation 第一次翻页上屏就发，说明开始显示，不说明动画播完。Host 的 `executed` 回执含义也不变。

### 16.4 FOLLOW（锚点 3）

- 位置和朝向按头部坐标系写，意思是"跟上来以后待在头的哪儿"。
- 平时它停在房间里不动：停的位置记在 Edge 画 WORLD 平面用的那个世界系里。
- 第一次出现时，按这一帧的头位直接放到跟上后的位置，再按 `enter` 出现。
- **成组**：`app` 相同（同一个 APP，或同为 NO_APP）且 `follow_deg` 相同的 FOLLOW 平面自动成一组。
  - 组的基准是组里 `plane_id` 最小的那块。Host 每版平面表的顺序可能变，最小的 `plane_id` 不变。
  - APP 想要两组，用两个不同的角度。
- **偏离**有两个方向，都从头出发：一个指向基准此刻的中心，一个指向基准跟上后该在的中心（这一帧的头位乘它在头部坐标系里的位置）。
  偏离就是这两个方向的夹角，偏航和俯仰都算。
  - 跟上后的位置在正前方时，偏离就是视线和基准方向的夹角。
  - 不在正前方的面板（比如视线下方的一行字）停好以后偏离为 0，不会反复触发。
- **用哪个头位**：用这一帧渲染所用的同一个位姿，也就是 §10 预测到上屏时刻的那个（`correction=IMU` 时含陀螺）。
  判断和画面用同一个位姿，不会出现该挪了却没挪，或者挪的时候抖一下。
  Host 的位姿晚到或断了，按 §10 接着用陀螺转，FOLLOW 照样挪回面前。
- **挪回**：偏离超过 `follow_deg` 时，整组同时开始挪，每块用自己的 `transition_cs` 和 `curve`。
  - 挪的这段时间里头可能还在转，所以每帧的终点按这一帧的头位重新算，进度按 `curve(经过时间 / 时长)` 走。到时刻就正好在面前。
  - 挪到以后停在最后算出的位置，偏离归 0；要再超过 `follow_deg` 才动。
  - 挪的途中再超过角度不重新开始。`transition_cs` 为 0 时直接跳到位。
- **模型变了**：
  - 记录逐字节不变（16.3）：照旧，停着的停着，挪着的挪完。
  - 跟上后的位置或别的目标变了：从此刻显示中的样子按新记录过渡，终点逐帧按头位算，过渡完停住。
- **世界变了**：
  - Host 摆正（§11，长按 BACK）改写的是模型里 WORLD 平面的位置，世界系不变。停着的 FOLLOW 面板不在这些位置里，留在原处，
    偏离超过角度时照常跟回来，不用特殊处理。
  - 位姿在同一个世界里跳了（比如 Host 重新对齐 VIO）：停着的 FOLLOW 面板和 WORLD 平面一起跟着跳，之后照常判断偏离。
  - 世界系换了（§10 位姿的 `world_epoch` 变了，或 §14 的 `world_epoch` 变了）：旧世界里的停靠位置没有意义。
    FOLLOW 面板按这一帧的头位直接放到跟上后的位置，不插值。这与 §10 不跨世界插值一致。
- `tracking=NONE`，或还没有位姿（§10 `pose_enabled=0`、还没收到）：FOLLOW 与 HEAD 相同。

### 16.5 校验

失败则整个模型拒绝（§4 照旧）：
- 能力：`motion` 只用位 0、1；`FOLLOW` 为 1 时 `TRANSITION` 必须为 1。
- 平面：`transition_cs` ≤ 250；`curve`、`enter` 各 ≤ 2；95 为零；锚点 3 时 `follow_deg` 为 1..60，其余锚点为 0。
- 对着能力：`TRANSITION` 为 0 时 92..94 必须全零；锚点 3 只在 `FOLLOW` 为 1 时合法。

### 16.6 不在本版

- 关键帧序列、弹簧曲线、取样矩形动画、两张图之间的交叉淡化。
- 收起时缩小：`enter` 只管出现。
- 文字逐字出现：Host 换图来做。
- 世界里的标注（meiso_host #107）：Host 本地的 SDK 能力，不在本契约。

### 16.7 验收

- 编解码：能力和平面记录的新字段 golden bytes 往返；`motion` 的其余位、只有 `FOLLOW` 没有 `TRANSITION`、
  `transition_cs` 251、`curve`/`enter` 为 3、95 非零、`follow_deg` 与锚点不符，全部拒绝且不改输出。
  对着 `motion` 为 0 的能力，带 92..94 或锚点 3 的模型不被支持；早于本节的解码器拒收它们。
- 插值（Edge，桌面）：
  - 一个模型改位置加时长：逐帧采样，曲线对得上；动画中途再发一个，从当时的位置接着走。
  - 中途来一个这块面板逐字节相同的模型：过渡照原来的时刻走完，不重新计时。
  - `enter` 2：出现时宽高 92% 起，到时长正好 100%。
  - 收起（`opacity` 0 加时长）：淡出途中照画，从应用起不算 `drawn`、不是准星下的平面。
- FOLLOW（Edge，桌面）：
  - 转头 10°（`follow_deg` 7.5）：挪回面前，用时等于时长；同组两块一起到。
  - 挪的途中头继续转：到时刻正好在这一刻的面前。
  - 跟上后在视线下方 10° 的面板，停好后不再触发。
  - 平面表顺序换了：基准不变，不额外触发。
  - 断链转头（meiso_edge #35 的情形）：Host 位姿断了还在转头，FOLLOW 照样用眼镜自己的位姿加陀螺挪回面前。
  - 世界系换了：直接放到面前，不插值。
- 旧端：不声明 `motion` 的 Edge 收到非零字段整个拒收；Host 对它写零，照旧逐步改 `opacity`。
- 板上：
  - 量过渡中那几帧的渲染耗时。`opacity` 小于 255 要混合，放大要缩放，都比不透明直拷贵，眼镜两个 APP 加 Holo 时余量很紧（meiso_edge #44）。
  - 台架录屏 60 fps：出现、收起、懒跟随、断链转头，以及两个 APP 加 Holo 面板时的淡入淡出。请 Kamico 看观感。
