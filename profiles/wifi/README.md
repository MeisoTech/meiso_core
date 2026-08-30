# Wi-Fi telemetry Profile

> 状态：Profile codec 与 host conformance vectors 已实现。本 Profile 把操作系统可见的
> Wi-Fi 链路统计投影成一个 Meiso endpoint；它是网络 telemetry，不是物理 sensor，也不代表
> RF 扫描、定位或连接管理。

## Resource Graph

稳定 Profile 为 `WIFI_TLM/_PROFILE:v1`，逻辑 endpoint 为
`WIFI_TLM/ENDPOINT`。每个 Edge 的 `endpoint_incarnation` 仍由 Runtime 动态提供。

| facet | truth owner | kind | ResourceId | schema | 最大 payload |
|---|---|---|---|---|---:|
| capability | Edge/catalog | catalog bytes | — | `WIFI_SCH/CAPABIL_:v1` | 16 |
| desired | Host | State | `WIFI_TLM/DES_STAT` | `WIFI_SCH/DESIRED_:v1` | Runtime prefix 8 + Profile 8 |
| actual | Edge | State | `WIFI_TLM/ACT_STAT` | `WIFI_SCH/ACTUAL__:v1` | Runtime prefix 56 + Profile 8 |
| data | Edge | Stream | `WIFI_TLM/DATA_STR` | `WIFI_SCH/SAMPLE__:v1` | 88 |

Capability 是 `MeisoRuntimeEndpointDescriptor` 中原子 catalog capability，不再声明一份重复的
capability State。未来大型或独立动态 capability 可以由 catalog 引用 State/Blob；当前 Profile 只有上述
单一 truth。

desired/actual 是真正的 Core State。Runtime 编解码 `enabled`、desired reference、apply result、
availability、production status/reason/epoch 等公共 prefix；本目录仅编解码 capability、desired
参数、actual effective 参数和 Stream sample。不存在 protocol subscription、delivery toggle 或
Profile 私有 gate。

```text
Host desired State(enabled, fields, period)
  -> K230D Edge Runtime
  -> Edge actual State(effective fields/period + Runtime production state)
  -> actual reports producing + nonzero production_epoch
  -> Runtime data route active
  -> Edge produces Stream samples
```

`enabled=false` 表示停止生产。已经越过 adapter 的公开 sample materialization boundary、并通过
本 Profile 校验的 sample，必须提交当前 Runtime data route；不能再因为 Host “未订阅”而不发送。
bounded transport/QoS 丢失必须以 gap/drop/degraded 状态暴露，不能成为隐藏的第二开关。

## 参数与字段

Profile 周期 envelope 为 `100..10000 ms`、基础粒度 `100 ms`。每个 Edge 的 capability 根据
私有配置/probe 声明 envelope 内自己的 min/max/step 和实际支持字段；step 必须是 100 ms 的
非零倍数，且 min/max 落在同一 grid。desired 必须非空且落在该 capability 内；actual effective
可以是已接受的非空选择，也可以
用 `(fields=0, period=0)` 唯一表示当前没有有效生产配置。
`desired.enabled=false` 仍携带完整、合法的参数 snapshot；zero pair 只属于无有效配置的 actual。
Capability 任一字段变化需要新 catalog generation；瞬时字段缺失使用 sample `present_fields` 或
actual availability 表达，不为链路抖动重建 catalog。

| bit | 字段 | wire 值 | 含义 |
|---:|---|---|---|
| 0 | `link_state` | `u8 0/1` | adapter 观察到的 L2 carrier/link flag |
| 1 | `signal_mbm` | `i32` | 信号强度，0.01 dBm（mBm）单位，范围 `-20000..0` |
| 2 | `rx_bytes` | `u64` | 接收累计字节 |
| 3 | `tx_bytes` | `u64` | 发送累计字节 |
| 4 | `rx_packets` | `u64` | 接收累计 packet |
| 5 | `tx_packets` | `u64` | 发送累计 packet |
| 6 | `rx_dropped` | `u64` | 接收累计 drop |
| 7 | `tx_dropped` | `u64` | 发送累计 drop |
| 8 | `rx_errors` | `u64` | 接收累计 error |
| 9 | `tx_errors` | `u64` | 发送累计 error |

`link_state` 不表示 association identity、DHCP/IP、Internet 可达性、route、DNS 或 Meiso
Session health。采样“频率”只由 `sample_period_ms` 表示；Profile 不公开 Wi-Fi channel/frequency。

计数器是当前 adapter 能证明 continuity 的累计 observation；重启、设备替换或计数器 reset
无法证明连续性时必须建立新的 endpoint/producer/stream epoch，不能伪造单调性。

每个 sample 携带 `selected_fields` 与其子集 `present_fields`。未请求字段和已请求但本次不可得
字段都必须编码为零；`present_fields` 区分“真实零值”和“缺失”。sample 的 selection 必须等于
对应 production epoch 的 actual effective selection。Profile codec 检查单帧 canonical form；
Runtime/adapter 检查它与 capability、desired 和 actual 的跨帧关系。

## Privacy boundary

公开 schema 只允许上表十项。以下内容是明确 denylist，不能塞入 reserved bytes、动态字段、
日志旁路或未经版本化的新 mask bit：

- SSID、BSSID、AP/client/本机 MAC；
- IPv4/IPv6 地址、prefix、gateway、route、DNS、hostname；
- interface name/index、driver path、bus address；
- 密码、PSK、认证材料、连接配置；
- scan result、neighbor/peer 列表、association identifier；
- Wi-Fi channel、channel/center frequency；
- packet payload、域名、流量目的地或可用于位置推断的 AP identity。

signal 是 Host 明确选择的匿名链路量；它不授权扫描或暴露 peer identity。

## Fixed wire

全部整数为 big-endian，大小固定，trailing bytes 与 non-zero reserved bytes 均 malformed。

```text
Capability (16; values are per-Edge within the Profile envelope)
  0  u32 supported_fields
  4  u32 min_period_ms       within 100..10000 / 100-ms base grid
  8  u32 max_period_ms       within 100..10000 / same capability grid
 12  u32 period_step_ms      nonzero multiple of 100

Desired parameters / Effective parameters (8)
  0  u32 selected_fields
  4  u32 sample_period_ms

Sample (88)
  0  u32 selected_fields
  4  u32 present_fields
  8  u8  link_state          + 7 reserved zero
 16  i32 signal_mbm          + 4 reserved zero
 24  u64 rx_bytes
 32  u64 tx_bytes
 40  u64 rx_packets
 48  u64 tx_packets
 56  u64 rx_dropped
 64  u64 tx_dropped
 72  u64 rx_errors
 80  u64 tx_errors
```

Codec 不分配内存；decode 输出 storage 由调用者提供。Host vectors 固定四种 payload 的 literal golden
bytes，并覆盖每个 truncation point、trailing bytes、未知 mask、非法 rate/range、reserved 与
canonical-zero 失败。它们不证明 Linux adapter、direct-IP transport 或 K230D board behavior。
