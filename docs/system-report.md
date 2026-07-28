# 系统进展与待决策

> 日期：2026-07-28
> 状态：公开、已脱敏的阶段报告。本页是当前进展和待回复事项的唯一入口，不包含私有原始日志、地址、凭据、设备标识或板级临时路径。

## 本轮结论

系统主线已经从“验证板能否运行”推进到可测量的 Host—Edge 闭环：

```text
i.MX8MM Edge 代理：camera + 有时间身份的代理 IMU
  -> H.264 MediaStream
  -> Orin Nano Linux Host：解码 + YuNet BBox
  -> deadline-bound BBox
  -> Edge：姿态修正 + 开发板预览合成 + 显示
```

同一实机路径还验证了 Host-owned 完整配置、Edge-owned 实际状态、ACK 丢失重试、重启重发、lease 安全态、有界历史和 Dashboard。结论只证明 i.MX8MM 代理与 Orin Host 的 contract/adapter 形状，不证明 K230D 产品性能。

## 三层证据

| 层次 | 已有证据 | 不能声称 |
|---|---|---|
| 公共仓库 | C11 进程内 camera/IMU/BBox/姿态修正闭环；一槽 `LatestState(b)` portable core；Windows/GCC 测试与 Cortex-M4 compile gate | 当前 fixture 仍以 `CameraFrame(d)` 和 `OverlayState(b)` 表达旧切片，不能代表已测连续媒体语义 |
| 双板实验 | i.MX8MM Edge 代理与 Orin Host 的媒体、控制、历史、BBox、修正和显示 | 公共 wire、生产安全传输、真实 IMU/麦克风 |
| 目标产品 | 角色、所有权和验收边界 | K230D driver、资源、codec、render、时延或功耗 |

## 双板结果

统一的 640×480 workload 得到以下阶段结论：

| 能力 | 结果 | 边界 |
|---|---|---|
| camera 与媒体 | Hantro H.264 + RTP + Orin 硬解码，最终验收窗口 Host decode 27.950 fps、Edge scanout 27.364 fps | 私有实验 wire 不是公共协议；K230D codec 未测 |
| 网络故障隔离 | 注入媒体发送失败时，Edge capture、encode 与本地显示继续约 27 fps | 只证明当前有界 adapter 结构 |
| Host BBox | YuNet 消费精确 media identity；当前约占 1.09 个 Orin CPU core，GPU/DLA 未使用 | 只声明人脸框，不声明身份识别或 World Model |
| 时间与姿态 | 固定容量 identity/pose ring、插值、过期与 uncertainty gate、非零框修正均通过 | IMU 是确定性代理源；真实 IMU、fusion 与标定未完成 |
| 设备配置 | 六字段完整状态、丢失 ACK、同版本重试、幂等、Host 重启重发、camera/IMU/preview 独立切换通过 | 字段和私有 packet 不是公共 ABI |
| 实际状态 | ACK 与设备应用分离；camera 只有出现首个真实 frame 后才报告 applied | K230D platform adapter 仍需重复该门槛 |
| 麦克风 | 请求被协议接受，支持字段继续应用；Edge 明确报告 unavailable/false/0 与 `MIC_UNAVAILABLE`，不生成假音频 | 当前硬件没有真实采集证据 |
| 闭环 trace | 验收窗口发送 116 个 Host decision，收到 67 个 Edge result，0 reject，并保留可关联的当前 session trace | result 是允许丢失的诊断观测，不阻塞显示 |
| Host 历史 | SQLite/WAL 元数据与 digest 校验的 H.264 segment 独立写入、查询和恢复 | 不是无限存储；保留策略已成为下一决策 |
| 长时间运行 | metadata 与 payload 分别达到额度后精确进入 `BOUNDED_STOP`，Link、检测、overlay 与显示继续 | 恢复积累需要新的存储/保留策略 |
| Dashboard | 显示连接、期望/实际状态、运行与存储健康；查询有界历史且无浏览器错误 | 不是直接设备控制面，也不提供任意文件访问 |

## Object 与 MediaStream 状态

| 语义 | 当前证据 | 结论 |
|---|---|---|
| `Observation(a)` | 100 Hz 代理 IMU、deadline BBox、best-effort OverlayResult 已进入实机闭环 | 已做双板集成验证 |
| `LatestState(b)` | 完整配置、ACK/重试/幂等、Edge actual state、重启重发和 lease safe state | 已做双板集成验证；本轮开始提取公共 portable core |
| `OrderedEvent(c)` | fixture 验证重复、缺口与顺序 | 当前 Demo 没有真实 one-shot workflow，不虚构驻留业务路线 |
| `Bulk(d)` | fixture 验证分块、完整性、digest、有限重试和失败 | 只用于 immutable/on-demand payload；连续 camera 不使用 |
| `MediaStream` | 连续 H.264、恢复点、有界 backpressure、硬解码和本地 scanout | 已做双板集成验证 |

四种 Object 语义都有可执行证据，但只有 `a/b` 是当前驻留闭环的业务对象；`c/d` 没有必要为了形式完整而塞入实时路径。

公共 C11 slice 早于双板媒体实验，仍用完整 `CameraFrame(d)` 和可靠 `OverlayState(b)` 驱动合成。它目前只是一条确定性 fixture：不能把它描述为 27 fps MediaStream，也不能让实时 BBox 等待可靠状态交付。是否迁移这个公开 API 见 D3。

## 被否决的路线

- 每帧 raw camera 作为可靠 `Bulk(d)`：约 0.25 fps 的旧路径和约 59 Mbit/s 的 raw 试验都不满足实时目标，并曾严重干扰 100 Hz 时序。
- 软件 JPEG：当前代理约 12 fps，未达到 24 fps。
- per-frame ACK/重传：会把过期媒体变成实时阻塞源。
- 只凭驱动 `STREAMON` 成功就报告 applied：实机曾出现调用成功但没有新 frame 的假阳性。
- 为当前 YuNet 先建设 GPU/DLA 框架：现有 CPU 余量充足，缺少需求和收益证据。

## 对公共 SDK 的影响

本轮公共实现已增加一个无网络、无 heap、调用方提供固定 storage 的 `LatestState(b)` core，覆盖 newest-wins、pending、ACK、duplicate/conflict、version wrap 和新 session 重发。重试间隔由 Link adapter 调度，core 只保证未确认 truth 一直可取。它不公开私有 UDP 布局，也不把 ACK 当成设备应用结果。

后续顺序：

1. 用 Edge-owned actual state fixture 连接 `DesiredEdgeConfig` 与真实应用结果。
2. 定义最小 LinkSession identity/time 接口和 MediaStream adapter 边界。
3. 保留 SQLite/文件为 Host reference integration，不写进 portable ABI。
4. K230D 到板后依次获得 target build、camera/display、真实 IMU/标定、codec 和 Wi-Fi 证据。

## 尚未验证

- K230D 的准确板卡、runtime、toolchain、外设、内存和硬件加速路径。
- 真实 IMU timestamp、fusion、camera-to-IMU 外参与 display present timestamp。
- 可用麦克风的采集、时间身份与音频 MediaStream。
- 生产 LinkSession 的标准安全传输、公共 wire、重连和资源参数。
- 生产眼镜的光学畸变、双目和安全 compositor。

## 需要你的回复

### D1：历史数据保留策略（会阻塞继续积累）

当前 metadata 与视频 payload 已各自到达有界额度，既有证据没有被自动删除。

建议：先保留既有证据，确定目标介质和保留目标后，再实现“导出后删除”或“滚动保留”；不默认删除。

需要你决定：希望保留多长时间、写到哪种介质，以及是否授权删除已导出的旧数据。

你的回复：

>

### D2：首个公共 SDK 稳定面

建议：先稳定 `C11 portable core + adapter interfaces`。Host 的 Python/C++、SQLite 和 Dashboard 先作为 reference integration，避免在目标板和业务还未稳定时同时承诺多语言 ABI。

需要你决定：是否接受这个顺序；如果必须优先支持某种 Host 语言，请写明语言和首个调用场景。

你的回复：

>

### D3：公开 BBox 语义迁移

双板结果表明，人脸框引用精确源 frame 且有短 deadline；过期结果应丢弃，不能让重试的旧框覆盖新画面。因此实机路径把 BBox 作为 `Observation(a)`。稳定 UI、HUD 或完整 Presentation plan 仍然是 `LatestState(b)`。

建议：下一独立工作单元把旧公开 fixture 的 `OverlayState(b)` 迁为 deadline-bound `BBoxObservation(a)`；同时保留真正持久显示意图的 `b` 语义，不把两者合并。

需要你决定：是否接受这项公开 API 迁移；如果你希望检测框在断线后继续保持，请写明最大保持时间和安全行为。

你的回复：

>

### D4：K230D 到板后的首轮范围

建议顺序：target build 与 monotonic clock → camera/display → 真实 IMU 与标定 → codec → Wi-Fi integration。这样最早能重复现有闭环，又不会把网络问题和驱动问题混在一起。

需要你补充：准确板卡、runtime、camera、display、IMU 和 Wi-Fi 器件；如优先级不同，请写明。

你的回复：

>
