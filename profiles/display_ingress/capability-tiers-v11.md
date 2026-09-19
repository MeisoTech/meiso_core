# Display Ingress v11 草案：能力分档、会话活性、驻留 lease 与 BLE 适配器边界

> 状态：`DRAFT PROFILE CONTRACT`（meiso_glass #9）。依据 shared 决定 S-021、S-022 与 #18/#24 裁定。
> 本页只定义契约；wire 数值、canonical bytes 在 #18 实现并通过桌面对 + K230D 验收后冻结。
> 冻结前 v9/v10 的 bytes、版本号与行为不变。当前入口见 [Display Ingress](README.md)。

## 1. 问题与范围

v9/v10 按 Wi-Fi 量级链路设计：720p H.264 30 Hz 下行、NV12 2 Hz 上行、TCP user timeout 15 s、
没有会话级心跳、没有内容驻留期限、适配器只有 POSIX TCP/UDP。产品常态是约 1 Mbit/s 可用的
BLE 量级链路（S-015/S-016），且"有线、Wi-Fi、BLE 只是传输层，系统按能力选内容形态与速率"（S-022）。
本页定义四件事，Core 的 State/Stream/Action/Blob 语义与 MSL0/MRT0 wire 不变：

1. **能力分档**：双方在 Catalog 协商链路档位与每方向预算，所有内容 Desired 受档位上限约束。
2. **会话活性**：Profile 级慢心跳，用现有 State 路由实现，BLE 上代价约 0.2 kbit/s。
3. **驻留 lease**：视频、scene、相机三类内容各有协商期限；Edge 在失联时自行清除，不依赖 TCP 超时。
4. **单路重新激活**：停止或到期后同会话重新提交视频，不重建 Session（#18 收窄后的第①项）。

BLE 适配器（Lab #4）只在第 7 节划边界，实现落 `adapters/ble/`，Profile 层不变。

## 2. 带宽账目（现状，全部常量集中于此）

| 项目 | 数值 | 来源 |
|---|---|---|
| Profile 上限 | 4096×4096、240 Hz、单帧 64 MiB | `profile.h` 能力字段范围 |
| 下行 720p H.264 全 IDR 30 Hz | 编码目标 4.5 Mbit/s，AU 上限 7.37 Mbit/s | v2/v9 已验路径（Wi-Fi） |
| 上行相机 NV12 320×240 | 2 Hz 1.84 Mbit/s；5 Hz 4.6 Mbit/s | v8 K230D 适配器 |
| 上行相机桌面 RGBA8 320×240 | 2 Hz 4.92 Mbit/s | v8 桌面适配器 |
| 原生 scene（v10） | 单次 ≤1440 B；能力上限 1–60 次/s → ≤691 kbit/s；Locate 静态实测 0.23 kbit/s，bet-1 阶段 1b 变化态 45 kbit/s | scene.h、Lab #21、looking_glass #38 |
| 相机档：JPEG 关键帧 320×240 0.5 Hz | 3.0 kB/s ≈ 24 kbit/s（亮景静止） | Lab #5 |
| 相机档：FPGA DCT8+Rice 位数模型 QVGA | 8.6 kB/s ≈ 69 kbit/s（≈7.6 Hz，亮景）；10 fps 连续流 180–350 kbit/s | Lab #5；vga-snn-research |
| 相机档：CPU H.264 320×240 | 2.9 kB/s ≈ 23 kbit/s（亮景静止） | Lab #5 |
| 会话控制 | MSL0 最小帧 104 B；TCP user timeout 15 s；视频清除观察预算 750 ms；产品停止目标 ≤1 s | session-core-v0、README §停止 |
| BLE 5.x 2M PHY | 可用约 1.0–1.4 Mbit/s，待 Lab #4 实测替换 | direction-notes §1 |

结论：只有 scene 与相机三档在 BLE 预算内；v9/v10 的视频与 NV12 相机是 **Wi-Fi 突发 Profile**。

## 3. 能力分档（S-022）

档位是**每方向可用容量**的协商结果，不是链路类型。链路类型只影响默认档与功耗，不进入内容规则。

| 档 | 每方向预算（提案，待实测冻结） | 典型链路 | 允许的内容形态 |
|---|---|---|---|
| T0 | ≤1 Mbit/s | BLE 2M PHY | scene（模式 3）；模式 2 表单 480×640 每秒 1–3 张；相机关键帧 / FPGA 低清流 |
| T1 | 1–20 Mbit/s | Wi-Fi 常态、USB ECM | 上述 + 模式 2 表单 30 Hz；相机 CPU/硬件 H.264 |
| T2 | >20 Mbit/s | Wi-Fi 突发、有线 | 上述 + 720p 视频（v2/v9 路径） |

规则：

- Edge 在 Catalog 公布 `LINK_CAP`：支持的档位掩码、每档实测或标称的每方向预算（kbit/s）、
  该 Edge 的解码/渲染上限。Host 在 `LINK_DES` 选**一个**档位并给出本会话每方向预算，预算不得超过 Edge 公布值。
- 每条内容 Desired 在被接受前先过档位检查：视频 `input_width×input_height×maximum_frame_rate_millihz`
  折算的目标码率、scene `updates/s × 1440 B`、相机 `bitrate_bps` 或 `fps × 单帧上限`，三者之和不得超过该方向预算。
  超过即 `INVALID_CONFIG`，Edge 不做隐式降级；Host 负责整形与排序。
- 抢占顺序固定：控制 > scene > 相机关键帧 > 相机连续流 > 视频。预算不足时 Host 先降低低优先内容的速率。
- 档位在会话内可换：Host 发新版本 `LINK_DES`；Edge 按新预算重新校验所有已激活 Desired，不再满足的路由按第 5 节到期处理。
- 相机档位用现有 `MeisoCompositionMediaDesired` 表达：`codec`（RAW=1、JPEG=2；v11 预留 H264=3、FPGA_DCT=4）、
  `fps_millihz`、`bitrate_bps`。不加第二套相机 wire。

## 4. 会话活性（Profile 级慢心跳）

不改 MSL0（link.md 要求心跳由版本化 wire 冻结；改 Session wire 会波及所有 Profile）。
v11 用 Runtime State 路由实现：

- Host 每 `keepalive_interval_ms` 发布新版本 `LINK_DES`（内容可与上次相同，版本递增）；
  Edge 用 `LINK_ACT` 回 `observed` 版本。Runtime 帧绑定 SessionId，旧 Session 的回应自然无效。
- Host **最多一个 outstanding**：未收到上一版本的 `LINK_ACT` 前不发下一版本；连续 `keepalive_miss_limit`
  次超时即调用 `meiso_session_transport_lost`，进入现有 sticky 失败与清理路径。
- Edge 侧不因心跳缺失终止 Session；它只驱动第 5 节的 lease 时钟。
- 代价：每方向 8-byte prefix + suffix（≈24 B）每 interval；T0 建议 2000 ms（≈0.2 kbit/s），T1/T2 建议 1000 ms。
- 心跳只证明对端在时窗内回应，不代替 endpoint actual、应用健康或物理呈现证据（link.md）。

## 5. 驻留 lease

| 内容 | 参数（`LINK_DES` suffix） | 续期条件 | 到期动作 | 到期 Actual |
|---|---|---|---|---|
| 视频 | `video_lease_ms` | 该路任一被接受的 Desired，或任一被接受的 `LINK_DES` | 与 disabled Desired 同一清除路径：停准入、串行撤下两平面、确认读回 | `STOPPED`，`observed` = 最后 Desired，`applied` = 最后 enabled Desired，suffix 8 B `terminal_reason=LEASE_EXPIRED` |
| scene | `scene_lease_ms` | 同上 | 渲染空 scene，走现有零项撤平面路径 | HUD Actual applied generation = 0，status 带 `LEASE_EXPIRED` |
| 相机 | `camera_lease_ms` | 同上 | 停止生产，释放 in-flight/latest | CameraMedia Actual `STOPPED`，`terminal_reason=LEASE_EXPIRED` |

- `0` 表示不启用 lease，行为等于 v10（只在收到停止请求或 Session 清理时清除）。
- 约束：`lease_ms ≥ 2 × keepalive_interval_ms`；T0 建议视频/scene 6000 ms、相机 6000 ms；T1/T2 建议 3000 ms。数值待 #18 与 Lab #4 实测后冻结。
- 到期计时从 Edge 接受最后一次续期起算；重复的相同 Desired 不延长（与 #24 一致）。
- 到期不是"确定未呈现"：已提交但未观察的翻页仍按原身份报告 `unobserved`。
- 到期 Actual 在链路恢复后按 State 规则重发；Host 收到即视为该路的**实际去激活证明**，Interlock 据此解除
  reservation 与 quarantine，不需要 Session 重建。链路不恢复则走 Session 清理。
- 与心跳的关系：心跳缺失 → lease 到期 → 清屏；三者时序为 `keepalive_interval < lease ≤ Host 端 transport_lost 判定`，
  保证 Edge 先清屏、Host 后判死，不出现 Host 已重建而 Edge 仍驻留旧画面。

## 6. 单路重新激活（#18 第①项）

- 视频路处于 `STOPPED`（无论停止请求还是 lease 到期）时，Host 可在同一路由/owner/delivery generation
  发布更新版本的 `enabled=true` Desired；Edge 重新准入，开启新的 production epoch，发布 `RUNNING` Actual。
- scene 的 `video_desired_version` 改为引用新的 enabled 版本；Host 须在重新激活后重发 scene 计划，
  Edge 拒绝引用已失效视频版本的 scene（现有规则）。
- HostRuntime 侧：`STOPPED` Actual（含到期）= 实际去激活证明，清除 quarantine；新的 `RUNNING` Actual 到达前不发视频帧。
- 失败：重新激活的清除/分配失败发 `FAILED / APPLY_FAILED`，`applied` 保留上一次 enabled 版本；Host 可再试或重建 Session。
- looking_glass presentation_sink README 中"恢复 = 会话重建"一句随 #18 实现改写。

## 7. BLE 适配器边界（Lab #4，落 `adapters/ble/`）

- 传输：L2CAP CoC（`L2CAP_MODE_LE_FLOWCTL`），credit-based 流控即背压。两条通道：
  `PSM_CTRL` 承载 MSL0/MRT0 完整帧（Session + Runtime State/inline Stream）；`PSM_MEDIA` 承载 DIFR/相机 fragment。
  分片由 L2CAP 做（SDU ≤ 协商 MTU），Profile 不再自分片；T0 下 `max_frame_bytes` 协商为 ≤4096。
- 接口：控制通道实现与 `meiso_posix_tcp.c` 相同的帧读写面；媒体通道实现 `MeisoDisplayIngressMediaTransport`
  的 prepare_outbound/on_readable/on_writable/terminate/snapshot。Core 与 Profile 代码不感知 BLE。
- 调度：适配器内按第 3 节优先级出队；每通道 in-flight 有界；连接间隔与 MTU 由 Rain 在 Lab #4 实测给出，
  作为 T0 预算与心跳间隔的依据。
- 不承诺：认证/加密（与当前 UDP 一致，安全 carrier 另立）、跨传输迁移、多绑定调度（link.md）。

## 8. 实现拆分与验收

| 项 | Owner | 内容 | 完成信号 |
|---|---|---|---|
| meiso_glass #18 | KAngel | v11 选择：`LINK_END_` endpoint（`LINK_CAP/LINK_DES/LINK_ACT`）、心跳、三类 lease、单路重新激活；桌面对 + Zero | 桌面单测覆盖 quarantine → 到期/停止 → 重新激活；Zero 实屏：断控制链路后 ≤ lease 清屏、恢复后不重建会话恢复视频 |
| 档位校验 | KAngel（#18 之后另开 issue） | 第 3 节的 Desired 预算检查与 `INVALID_CONFIG` 向量 | 三类内容超预算各一条拒绝向量；换档后到期向量 |
| Lab #4 | Kache | `adapters/ble/` 两通道 CoC 适配器；BlueZ 虚拟控制器 + netem 模型链路 | Session 在 BLE 上 ≥1 h；scene 端到端 ≤1 s |
| 相机 codec 值 | KAngel（随 Lab #5 收口） | `H264=3`、`FPGA_DCT=4` 进 codec 枚举与掩码 | Lab #5 三档用同一 Desired 表达 |

## 9. 未决（待 Kamico 或实测）

- 三档预算的边界数字与 T0 的 480×640 表单速率（S-022 给的 1–3 张/s 是目标，未实测）。
- 相机默认档（D-3：FPGA 功耗）；FPGA 连续流是否在 T0 默认开启。
- BLE 上心跳与连接间隔的关系：间隔越长越省电，lease 相应变长，清屏延迟上限由 Kamico 定。

## 10. #18 wire 候选与失败向量（尚未冻结）

以下沿用 Profile 的大端定长 suffix；Runtime Desired/Actual prefix 不变。仅显式选择 application v11
时使用。v11 在原 v10 图后可附加一个 link endpoint（两个 State route，无媒体）；不选该
endpoint 时不启用心跳/lease。ID 的 high word 为 `DISPLAY_`，endpoint low word 为
`LINK_END`，两 route 为 `LINK_DES`、`LINK_ACT`；capability schema low word 为 `LINK_CAP`，
三者 schema high word 为 `DISP_SCH`、version=1。

| suffix | 长度 | 字段（依次，均 u32） |
|---|---:|---|
| LINK_CAP | 40 B | tier_mask；T0/T1/T2 各 downlink、uplink kbit/s；decode_max_pixels；render_max_pixels；reserved=0 |
| LINK_DES | 32 B | tier（0/1/2）；downlink、uplink kbit/s；keepalive_interval_ms；keepalive_miss_limit；video/scene/camera_lease_ms |
| LINK_ACT | 32 B | 已接受 LINK_DES 的原 suffix；observed/applied 引用该版本，RUNNING，epoch=该版本（只有控制，无数据 route；遵守 Runtime 新 applied 必须新 epoch 的规则） |

未公布档位的两个预算必须为 0，公布档位的预算必须非零；解码/渲染像素上限非零。
Desired 的 tier 必须公布、两预算不得超过对应公布值。内容的合计预算校验留给 #31。
interval 与 miss_limit 非零；非零 lease 在 `[2×interval, interval×miss_limit]` 内，乘法用 u64。
Host 以首次发送该 outstanding 版本的时刻计算连续 miss；迟到的旧 LINK_ACT 不清除新版本的
outstanding，ACK State accept 不代替 LINK_ACT。没有新 outstanding 时不累计 miss。

v11 视频及相机的非 RUNNING Actual suffix 为 8 B：`terminal_reason:u32, reserved:u32=0`。
terminal_reason=0 表示普通停止/失败，1 表示 LEASE_EXPIRED；到期清除失败仍是 FAILED /
APPLY_FAILED，不得以 reason=1 冒充成功去激活。scene Actual suffix 为 16 B：
`applied_generation:u64, terminal_reason:u32, reserved:u32=0`；到期清除完成后 generation=0。
这三个 Actual route 的 schema version 升 2；旧 v9/v10 的空 suffix、8 B HUD status 及 schema 不变。
到期只改变 effective 状态；保留最后 Desired 身份，不改写为 Host 曾请求 disabled。

失败向量：旧版本拒收 v11 graph；未知枚举/非零保留位/截断/越界预算/过短 lease 不写接收状态；
重复或倒退 Desired 不续期；LINK_DES 不让已到期内容复活；三类时钟独立；时钟倒退不误清屏；
清除 pending/failed 不产生 STOPPED；旧 Session、旧 production epoch 与旧 heartbeat 不能恢复新生产。
