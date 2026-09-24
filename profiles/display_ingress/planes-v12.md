# Display Ingress v12：空间中的平面（0DoF 起步）

> 状态：`DRAFT PROFILE CONTRACT`（meiso_edge #2，S-025/S-026）。本页只定义两端契约；
> 第一版实现随 meiso_edge #2 在 Zero 与 Orin NX 上验收后冻结。冻结前 v9/v10 的 bytes、版本号与行为不变。

## 1. 解决什么

S-025 把显示统一成"空间中的平面"：Host 决定有哪些平面、各自摆在哪、内容从哪来、属于哪个 APP；
Edge 每次刷新给每个平面算透视变换后贴上屏，自己不渲染网格或物体。内容来源三种：Host 串流、
静态图、Edge 本地 LVGL（S-026 的 HUD）。前两种经本 Profile 传输；第三种只在 Edge 上，本页只规定
Host 能告诉它什么（当前 APP 名）。

v12 是一次显式的应用选择（Session profile version 12），与 v9/v10 并列，不隐式升降级。

## 2. Resource Graph

一个 Catalog 最多三个 endpoint，顺序固定：

| endpoint | route | owner / kind / facet | delivery | schema | 含义 |
|---|---|---|---|---|---|
| 显示 `DISPLAY_/INGR_END` | 4 条，与 v2 完全相同 | — | — | `DISP_SCH/CAPABIL_:v2`（32 B） | 串流：H.264 独立帧（v2），作为平面的**纹理**，不再直接上屏 |
| 平面 `DISPLAY_/PLAN_END` | `DISPLAY_/PLN_DES_` | Host / State / DESIRED | inline | `DISP_SCH/PLN_MODL:v1` | 平面模型（完整值） |
| | `DISPLAY_/PLN_ACT_` | Edge / State / ACTUAL | inline | `DISP_SCH/PLN_STAT:v1` | 已应用的模型与来源就绪情况 |
| | `DISPLAY_/PLN_PRES` | Edge / Stream / DATA | inline | `DISP_SCH/PLN_PRES:v1` | 模型首次上屏的证据 |
| | `DISPLAY_/PLN_IMG_` | Host / Blob / IMMUTABLE | inline | `DISP_SCH/PLN_JPEG:v1` | 静态图，内容寻址（SHA-256） |
| 相机（可选） | 3 条，与 v9 相同 | — | — | — | 不变 |

显示与平面两个 endpoint 必选，相机可选。平面 endpoint 的 capability 是 `DISP_SCH/PLN_CAP_:v1`（64 B）。
所有 endpoint 共用本次设备 incarnation。缺项、多项、乱序、schema 或 delivery 不符，整个 Catalog 拒绝。

显示 endpoint 的 Desired 仍给出串流宽高与帧率；这个宽高就是串流纹理的尺寸。v9 的输出几何与
`video_layout` 不属于 v12：画面怎样上屏只由平面模型决定。视频帧的呈现证据沿用 v2：一帧**第一次
被合成进一个已翻页的缓冲**时，按它的 AU SHA-256 发 `PRES_EVT`。

## 3. 平面能力（Edge → Host，64 B，大端）

```text
0..3    output_width            u32  1..4096，整块输出的像素
4..7    output_height           u32  1..4096
8       view_layout             u8   1 = MONO（一个视图覆盖整块输出）；2 = SIDE_BY_SIDE（左右两半各一眼）
9       tracking                u8   0 = NONE；1 = ROTATION；2 = POSE（见 §5）
10      max_planes              u8   1..16
11      max_images              u8   1..8，每个 Session 最多几张不同的静态图
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
7       opacity                 u8   1..255，255 为不透明
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
- 由远到近画：按平面中心到视图原点的距离，距离相同按表中顺序。
- Edge 本地 HUD 画在所有平面之上，平面永远不遮挡它。
- `tracking=NONE`（0DoF）：头部位姿恒为单位，WORLD 平面看起来与 HEAD 相同。
  `ROTATION`、`POSE` 留给 IMU 与 Host 定位接入后（S-025），届时 WORLD 平面按显示时刻的头部位姿变换；
  HEAD 平面不受影响。本版 Edge 只报 NONE。
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

## 7. 静态图

- 图是基线 JPEG（SOF0，8 位，1 或 3 分量），字节数 ≤ `max_image_bytes`，宽高 ≤ 能力上限。
  ContentId 是字节的 SHA-256。用 Runtime 的 Host→Edge Blob 搬运（BEGIN/READY/CHUNK/COMMIT/RESULT）。
- 模型与图的先后不限：模型先到时 IMAGE 平面记为等待，图到后开始画并更新 Actual 与证据。
- 同一 Session 内同一张图只需送一次；每个 Session 最多 `max_images` 张不同的图，超过时 Edge 以
  CAPACITY 拒绝。图属于 Session：新 Session 需重新送图。

## 8. 断线

Session 结束（任何原因）时 Edge 立刻撤下这个 Session 的全部平面，只留本地 HUD，HUD 显示已断开；
连接恢复、新 Session 送来模型后面板回来。Host 在新 Session 里重发模型与图，不假定 Edge 记得上一次。
断线由 Edge 本地发现：Wi-Fi 链路断开立即发现；Host 进程退出由 TCP 关闭发现；
对端静默由部署的 TCP 存活探测与 user timeout 兜底。本版不加应用层心跳。

## 9. 带宽与容量（本版常量）

| 项 | 数值 |
|---|---|
| 模型 | ≤ 1888 B/次，≤ `max_updates_per_second` 次/秒 |
| 静态图 | ≤ `max_image_bytes`/张，4096 B 一块 Blob chunk |
| 串流 | 与 v2 相同：独立帧 H.264，大小与帧率由显示 Desired 和 AU 上限决定 |
| Runtime | 3 endpoint、11 route、capability 160 B、Desired 后缀 ≤ 1888 B、Blob 单次 ≤ `max_image_bytes` |

## 10. 验收

- 编解码：golden bytes 往返；保留位、越界数值、非有限浮点、非单位四元数、重复 id、
  坏 UTF-8、`video_desired_version` 不符、超出能力，全部拒绝且不改输出。
- 桌面：模拟后端画出的像素与几何期望一致（换位姿后四角跟着变）。
- 板上（meiso_edge #2）：三样同屏、换位姿、断线与恢复、实际翻页证据。
