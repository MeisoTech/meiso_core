# Composition Edge deterministic trace v1

## 范围

本格式记录固定 Composition Profile v3 simulator 的确定性输入，用于 Host 应用、fixture 与
conformance 重放。它是 canonical in-memory bytes，不是 Link wire、文件系统 API、远程 transport、
通用 backend 注册格式或 physical evidence。调用方自行持有存储并决定是否把完整 bytes 写入文件或
transport；codec 不打开路径、不持 fd、不分配 heap、不读取 wall clock。

每份 trace 明确带 `SYNTHETIC` flag。重放得到的 sample、Actual 或 frame 只能标记为 recorded/synthetic，
不能据此声明 sensor freshness、driver、network、target 或 physical output。

## 全局上限与整数表示

- 所有多字节整数使用 big-endian；所有 reserved byte 必须为零。
- trace 最多 `16,777,216` bytes、`4,096` records；不允许 trailing bytes。
- trace header 固定 32 bytes；record header 固定 24 bytes。
- 最大 record payload 为 `112 + 460,800 = 460,912` bytes。
- CRC 使用 CRC32C/Castagnoli 的 reflected polynomial `0x82f63b78`，初值与 final XOR 均为
  `0xffffffff`。零长度 payload 的 CRC32C 为零。

## Trace header

| offset | bytes | meaning |
|---:|---:|---|
| 0 | 4 | ASCII `MECT` |
| 4 | 2 | version，exact `1` |
| 6 | 2 | header bytes，exact `32` |
| 8 | 4 | flags，exact `0x00000001` (`SYNTHETIC`) |
| 12 | 4 | record count，`3..4096` |
| 16 | 4 | total bytes，必须等于输入长度 |
| 20 | 4 | bytes `32..total-1` 的 body CRC32C |
| 24 | 4 | header CRC32C；计算时本字段视为零 |
| 28 | 4 | reserved zero |

writer 在 finalize 前产生的 buffer 不是有效 trace。finalize 只在已经记录唯一 final TERMINATE 后写入
header；失败不修改 writer 或 output buffer。

## Record header

| offset | bytes | meaning |
|---:|---:|---|
| 0 | 1 | closed record kind |
| 1 | 1 | flags，exact zero |
| 2 | 2 | record header bytes，exact `24` |
| 4 | 4 | payload bytes |
| 8 | 4 | sequence，从 1 连续递增 |
| 12 | 4 | payload CRC32C；零长度 payload 为零 |
| 16 | 8 | reserved zero |

record 紧密排列、没有 padding。reader init 必须先验证完整 header、body CRC、每个 record header、payload
CRC、sequence、上限、kind、payload shape 与全局事件顺序；任何失败都不写 reader arena 或 output。

## Record kinds

### 1 — CONFIG（384 bytes）

CONFIG 必须是 sequence 1 且只出现一次：

| offset | bytes | meaning |
|---:|---:|---|
| 0 | 8 | catalog generation，nonzero |
| 8 | 8 | continuity serial，nonzero |
| 16 | 1 | desired owner role |
| 17 | 7 | reserved zero |
| 24 | 8 | desired owner incarnation |
| 32 | 8 | initial Wi-Fi production epoch，nonzero |
| 40 | 8 | initial render media epoch，nonzero |
| 48 | 8 | initial camera media epoch，nonzero |
| 56 | 48 | six endpoint incarnations in Profile order |
| 104 | 16 | canonical Wi-Fi capability wire |
| 120 | 48 | canonical DeviceTelemetry capability wire |
| 168 | 64 | canonical render media capability wire |
| 232 | 64 | canonical camera media capability wire |
| 296 | 88 | canonical Wi-Fi sample wire |

能力与 sample suffix 使用各自已冻结的 Profile codec；selected sample fields 必须是 Wi-Fi capability 的
subset。reader 从该 record 候选式解码 `MeisoCompositionEdgeSimulatorConfig`，调用方再用它创建 fresh
simulator arena。v1没有DeviceTelemetry epoch/sample record，因此要求config的
`device_telemetry_enabled=false`、device epoch/sample为canonical zero；enabled config必须由writer拒绝，
不能静默丢字段。

### 2 — ACTIVATE（0 bytes）

exact 一次。SET records 可以在它之前记录，以覆盖 READY 前 local queue；STEP、FRAME 与 TERMINATE
只能在 ACTIVATE 之后出现。

### 3 — SET_WIFI（16 bytes）

offset 0 是 enabled (`0/1`)，1..7 reserved zero，8..15 是 canonical Wi-Fi desired parameters。
disabled 时 suffix 必须全零；enabled 时 capability 必须接受参数。重复 SET 采用 simulator 的 latest
pending replacement 语义。

### 4/5 — SET_RENDER_MEDIA / SET_CAMERA_MEDIA（40 bytes）

offset 0 是 enabled (`0/1`)，1..7 reserved zero，8..39 是 canonical `MDES/32`。disabled 时 MDES
必须全零；enabled 时相应 capability 必须接受参数。每次成功 enabled apply 消耗该 route 的下一个
nonzero epoch；`UINT64_MAX` 只能消费一次。

### 6 — STEP（16 bytes）

offset 0..7 是 clock epoch，8..15 是 monotonic ns，二者必须形成合法 `MeisoTimePoint`。每次 STEP
最多按 Wi-Fi、render、camera 优先级应用一个 pending desired；没有 pending 时是 IDLE。STEP 不编码
输出 transition，replayer 返回 simulator 实际 transition，避免把重复 truth 写成第二套状态机。

### 7 — FRAME（112 + access-unit bytes）

| offset | bytes | meaning |
|---:|---:|---|
| 0 | 1 | source (`RENDER_PREVIEW` or `CAMERA`) |
| 1 | 1 | reserved zero |
| 2 | 2 | frame flags |
| 4 | 2 | pixel format |
| 6 | 2 | codec |
| 8 | 2 | width |
| 10 | 2 | height |
| 12 | 4 | reserved zero |
| 16 | 8 | media stream epoch |
| 24 | 8 | frame sequence，nonzero/strictly increasing |
| 32 | 8 | source clock epoch |
| 40 | 8 | source monotonic ns |
| 48 | 1 | applied Scene owner role |
| 49 | 7 | reserved zero |
| 56 | 8 | applied Scene owner incarnation |
| 64 | 8 | applied Scene State version |
| 72 | 8 | active Plan generation |
| 80 | 8 | world generation |
| 88 | 8 | dependency sequence，v1 exact zero |
| 96 | 8 | producer dropped frames，nondecreasing |
| 104 | 4 | access-unit bytes |
| 108 | 4 | reserved zero |
| 112 | variable | exact complete access unit |

source、epoch、format、codec、dimensions 与 size 必须匹配最近已应用的 enabled media desired。只允许
`KEY` 或 `KEY|RECOVERY`；BEGIN/END 和 unknown flag 均拒绝。render 必须携带 nonzero Host Scene/Plan/
World identity，camera 对应五个 Scene identity 字段必须 canonical zero。

### 8 — TERMINATE（8 bytes）

offset 0 是 nonzero closed `MeisoEdgeTerminalReason`，1..7 reserved zero。TERMINATE 必须 exact 一次且
是最后 record；因此 v1 trace 是完整 bounded run，而不是可静默追加的日志。

## Writer、reader 与 replay 原子性

- writer arena、完整 output capacity、typed input/output 必须 full-range disjoint。每次 append 先验证
  语义、容量、counter 与 alias，再一次提交 record 与 writer cursor；失败保持二者 byte-exact。
- reader arena 借用 immutable trace bytes；两者及 simulator arena 必须 full-range disjoint。reader init
  完整预检 trace，随后 config query 与每次 replay 都重新检查当前 record integrity。
- replay handle 必须由该 reader CONFIG 初始化、处于 fresh simulator lifetime，并且在 reader 完成前只由
  replay API 写入。应用可以在 records 之间读取 snapshot，或 borrow/exact release 已发布 media；不得旁路
  提交 desired、activate、step、publish frame 或 terminate。
- replay 每次只处理一个 record。成功后才推进 cursor；`COMPLETE`、integrity failure、busy borrow 或
  backend rejection 都不推进 reader、不改 result。底层 simulator operation 自身保持 candidate-first，
  因而当前 record 可在解除 borrow 等调用方条件后重试。

## v1 明确不做

- 不记录 Session、Runtime、route key、nonce、fd、socket packet、driver handle 或物理 locator；
- 不记录DeviceTelemetry desired/sample；该可选simulator子集需要未来显式新trace版本；
- 不提供自动 reconnect/resume、wall-clock scheduler、后台 recorder 或目录管理；
- 不支持 arbitrary Profile、generic operation payload、dynamic backend/plugin 或跨版本隐式兼容；
- 不用 trace 成功替代 real AF_INET、K230D、camera、display、security、performance 或 soak evidence。
