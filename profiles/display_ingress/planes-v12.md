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
> 同日（草案，meiso_host #73）：系统平面（`app` 为 NO_APP）画在 APP 平面之后，准星先取系统平面（§5、§11）。
> bytes 不变；早于此的 Edge 只按距离画。

## 1. 解决什么

S-025 把显示统一成"空间中的平面"：Host 决定有哪些平面、各自摆在哪、内容从哪来、属于哪个 APP；
Edge 每次刷新给每个平面算透视变换后贴上屏，自己不渲染网格或物体。内容来源三种：Host 串流、
静态图、Edge 本地 LVGL（S-026 的 HUD）。前两种经本 Profile 传输；第三种只在 Edge 上，本页只规定
Host 能告诉它什么（当前 APP 名）。

v12 是一次显式的应用选择（Session profile version 12），与 v9/v10 并列，不隐式升降级。
Loop 1 验收后 v12 是唯一往前走的显示路径：v9/v10 不再加功能，像 Composition 一样原地留着，
没人用了再退役。大屏串流就是一块 HEAD 锚定的 STREAM 平面。

## 2. Resource Graph

一个 Catalog 最多五个 endpoint，顺序固定：

| endpoint | route | owner / kind / facet | delivery | schema | 含义 |
|---|---|---|---|---|---|
| 显示 `DISPLAY_/INGR_END` | 4 条，与 v2 完全相同 | — | — | `DISP_SCH/CAPABIL_:v2`（32 B） | 串流：H.264 独立帧（v2），作为平面的**纹理**，不再直接上屏 |
| 平面 `DISPLAY_/PLAN_END` | `DISPLAY_/PLN_DES_` | Host / State / DESIRED | inline | `DISP_SCH/PLN_MODL:v1` | 平面模型（完整值） |
| | `DISPLAY_/PLN_ACT_` | Edge / State / ACTUAL | inline | `DISP_SCH/PLN_STAT:v1` | 已应用的模型与来源就绪情况 |
| | `DISPLAY_/PLN_PRES` | Edge / Stream / DATA | inline | `DISP_SCH/PLN_PRES:v1` | 模型首次上屏的证据 |
| | `DISPLAY_/PLN_IMG_` | Host / Blob / IMMUTABLE | inline | `DISP_SCH/PLN_JPEG:v1` | 静态图，内容寻址（SHA-256） |
| 相机（可选） | 3 条，与 v9 相同 | — | — | — | 不变 |
| 跟踪（可选）`DISPLAY_/TRCK_END` | 4 条，见 §10 | — | — | — | IMU 上行、头部位姿下行 |
| 输入（可选）`DISPLAY_/INPT_END` | 3 条，见 §11 | — | — | — | 按键上行 |

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
8       view_layout             u8   1 = MONO（一个视图覆盖整块输出）；2 = SIDE_BY_SIDE（左右两半各一眼）
9       tracking                u8   0 = NONE；1 = ROTATION（保留，不用）；2 = POSE。有跟踪 endpoint 时为 POSE，没有时为 NONE
10      max_planes              u8   1..16
11      max_images              u8   1..8，Edge 同时持有的不同静态图最多几张（§7）
12..15  max_image_bytes         u32  1..1048576，一张图（JPEG）的字节上限，也是图 Blob 路由的 payload 上限
16..17  max_image_width         u16  1..4096
18..19  max_image_height        u16  1..4096
20..35  fov_left/right/up/down  4×f32 弧度，每个视图相同；−π/2<left<right<π/2，−π/2<down<up<π/2
36..39  ipd_m                   f32  MONO 为 0；SIDE_BY_SIDE 为 0.04..0.08
40..41  max_updates_per_second  u16  1..60，平面模型更新的上限
42..63  reserved                全零
```

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
19..31  reserved                全零
```

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
5       anchor                  u8   1 = HEAD（跟头）；2 = WORLD（固定在空间）
6       source                  u8   1 = STREAM；2 = IMAGE
7       opacity                 u8   0..255，255 为不透明；0 为隐藏（§5）
8..23   source_x/y/w/h          4×u32  取样矩形（源像素）；w、h ≥ 1
24..55  image                   32 B   IMAGE：图的 SHA-256；STREAM：全零
56..67  position x/y/z          3×f32  米，|分量| ≤ 1000
68..83  orientation x/y/z/w     4×f32  单位四元数，模平方 0.999..1.001
84..87  width_m                 f32    0 < w ≤ 100
88..91  height_m                f32    0 < h ≤ 100
92..95  reserved                全零
```

坐标系右手，米：x 右、y 上、看向 −z，与 H.264 SEI 投影描述一致。HEAD 平面的位姿在头部坐标系里；
WORLD 平面在 Host 的世界坐标系里（§5）。平面中心在 `position`，局部 x 轴沿宽、y 轴沿高，
正面朝局部 +z。取样矩形左上角贴平面左上角 `(−w/2, +h/2)`。

校验（失败则整个模型拒绝，Edge 保留上一版）：
- 保留字节与名字尾部为零，数值在上述范围；
- 有 STREAM 平面时 `video_desired_version` 必须等于当前 enabled 的显示 Desired 版本，
  取样矩形落在该 Desired 的输入宽高内；
- IMAGE 平面的取样矩形在图解码后检查，越界的平面不画，记为等待（§6）；
- `plane_count ≤ max_planes`；模型引用的不同图片数 ≤ `max_images`。

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
TCP 的重传间隔逐次翻倍，卡 S 秒后控制连接可能要到开始卡后约 2S 才重新通，UDP 视频则一恢复就到；
两端的 user timeout 要按这个留余量（参考实现两端都是 25 s，10 s 的卡顿不断），Host 判"长时间无呈现证据"的上限要比它长。

## 9. 带宽与容量（本版常量）

| 项 | 数值 |
|---|---|
| 模型 | ≤ 1888 B/次，≤ `max_updates_per_second` 次/秒 |
| 静态图 | ≤ `max_image_bytes`/张，4096 B 一块 Blob chunk |
| 串流 | 与 v2 相同：独立帧 H.264，大小与帧率由显示 Desired 和 AU 上限决定 |
| Runtime | 5 endpoint、18 route（Session registry 23）、capability 208 B、Desired 后缀 ≤ 1888 B、Blob 单次 ≤ `max_image_bytes`；对端早于 §11 时 Session 协商成 19，Runtime 按 4 endpoint、15 route 配，Edge 不放输入 endpoint；对端早于 §10 时协商成 14，按 3 endpoint、11 route 配，跟踪与输入都不放 |
| IMU（§10） | 200 Hz × 32 B = 6.4 KB/s；每批 8 个时约 25 条/秒 |
| 位姿（§10） | Host 自定，建议跟相机帧（约 30 Hz），≤ 200 Hz；72 B/条 |
| 按键（§11） | 人按键的速度，24 B/条；积压最多 32 条 |

## 10. 跟踪：IMU 上行、头部位姿下行（meiso_host #53）

S-025：眼镜把相机与 IMU 用同一个时钟打时间戳回传，Host 算出"某一时刻的头部位姿"送回，
眼镜把它推到显示时刻再合成。相机沿用可选相机 endpoint；IMU 与位姿是这个可选 endpoint。
两边的时间都在 Edge 时钟上（clock epoch 4，与 §6 证据相同），Core 不加对时：
Host 的位姿由 Edge 时间的相机帧与 IMU 算出，本来就落在 Edge 时间上。

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
13      correction           u8   0 = NONE：只按位姿里的速度外推；1 = IMU：Edge 用与屏同板的 IMU
                                  从位姿时刻推到显示时刻（要求有 IMU 上行）
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

IMU 到相机、到头部坐标系的外参是 Host 的标定，不上线。

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
  `correction=NONE` 时按两个速度匀速外推，`IMU` 时积分本板陀螺。时刻在将来的位姿直接用，不往回推。
- `state=LOST`，或最新位姿比显示时刻旧 500 ms 以上：保持上一帧用过的位姿，WORLD 平面停在原处，
  不跳回单位位姿；新位姿到了接着用。`world_epoch` 变了直接换，不在两个世界系之间插值。
- `pose_enabled=0` 或还没收到位姿：与 0DoF 相同。
- 同一世界里时刻不晚于已有位姿的记录丢弃。IMU 队列满了丢最旧的（Host 看跳号），位姿只留最新的。

## 11. 输入：按键上行（meiso_host #66）

S-025：交互参考 VR，视野中心一个准星，看向面板再按键，选中或抓起。S-026：锁屏、选连哪台 Host 是眼镜本地的界面。
S-030：APP 只经 Host SDK 拿输入。S-033：Claire 键只归系统。控制板只报原始的按下、松开：去抖在 Edge，
长按、双击等手势由 Host 从按下、松开的时刻判断。

分工：
- **Edge 本地，不上线**：
  - 锁屏：拨下后 Edge 立即整屏黑，平面、HUD、准星都不画，也不上送按键；拨回恢复。不需要 Host，没有 Session 时也一样。
  - 选连哪台 Host：本版不做界面，只定下它归 Edge。
  - 准星，和准星下那块平面的高亮。
  - 这些键不上线，本页也不给它们编号。
- **上送 Host**：其余按键，每条带上按下那一刻准星下的平面。焦点、抓起、拖动、摆正由 Host 决定，结果是新的平面模型（§4）。
- **只归系统**：`SYSTEM` 键（Claire，S-033）。Host 不得把它交给任何 APP。其余键交给哪个 APP 是 Host 的策略。
  参考实现：
  - 五向键的上下左右只给焦点 APP。
  - `SELECT`：准星下不是焦点面板时，用来切换焦点，归系统；准星下正好是焦点面板时，交给这个 APP，相当于点一下。
    长按抓起面板，归系统。
  - `BACK`：短按给焦点 APP；长按把面板摆回视线正前方，归系统。

| route | owner / kind / facet | delivery | schema | 含义 |
|---|---|---|---|---|
| `DISPLAY_/INP_DES_` | Host / State / DESIRED | inline | `DISP_SCH/INP_DESI:v1` | 要不要按键 |
| `DISPLAY_/INP_ACT_` | Edge / State / ACTUAL | inline | `DISP_SCH/INP_STAT:v1` | 上送状态、是否锁屏 |
| `DISPLAY_/INP_KEY_` | Edge / Stream / DATA | inline | `DISP_SCH/INP_KEY_:v1` | 按键事件 |

能力（Edge → Host，`DISP_SCH/INP_CAP_:v1`，16 B，大端）：

```text
0..3    keys       u32  眼镜有的键：第 k 位对应键码 k（下表）；至少一个，第 0 位与第 8..31 位为 0
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
| 7 | SYSTEM | Claire 键，只归系统 |

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
