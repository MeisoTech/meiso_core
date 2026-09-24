# Direct-IP Wi-Fi Runtime v0 实装规则书

> 状态：所选垂直切片的实现与联调规则；不是新的产品规范。通用语义由
> [系统](system.md)、[Resource](resource.md)、[Runtime](runtime.md)和
> [Link](link.md)拥有；字段与 canonical wire 由
> [Wi-Fi telemetry Profile](../profiles/wifi/README.md)拥有。

## 1. 选择与非目标

本切片固定以下组合：

| 项目 | v0 选择 |
|---|---|
| Link bootstrap | 配置给出的 numeric IPv4 + TCP port；Host `connect`，Edge `listen/accept` |
| Core | exact peer/Profile handshake、单一不恢复的 Session、bounded Resource delivery；Session 内可传递多个 Runtime desired |
| Runtime | 一个 endpoint；Edge catalog、Host desired State、Edge actual State、Edge data Stream |
| Profile | `WIFI_TLM/_PROFILE:v1` / `wifi-telemetry` |
| Host 实现 | macOS foreground Dev Tool；保留有界 one-shot，`--interactive` 在同一 Session 接受 `apply(enabled=true/false)` 与 `close` |
| Edge 实现 | K230D 大核 Linux foreground target；单连接、多 desired、一个有界 pending sample |

Direct IP 只是当前 bootstrap/transport 选择，不进入 Core identity，也不承诺 discovery、DNS、
自动重连、运行中迁移、daemon、autostart 或 SDK API。Wi-Fi endpoint 只报告匿名链路 telemetry；
它不管理 Wi-Fi association、凭据、DHCP、route 或 Internet 连通性。SSH/串口只用于部署、启动、
日志和维护，不是 Meiso Link，也不承载 Runtime 数据。

## 2. 代码归属

| 路径 | 归属 | Host/Edge 开发者可依赖的边界 |
|---|---|---|
| `src/core/meiso_session.c`、`src/core/meiso_resource.c`、`src/core/meiso_state.c`、`src/core/meiso_stream.c` | portable Core | Session 与 State/Stream primitive；不理解 Wi-Fi |
| `src/runtime/meiso_runtime.c` | portable Runtime core | catalog、State commit、production gate 与 Stream route；不读取 IP/sysfs |
| `profiles/wifi/` | versioned Profile | capability、desired/effective 参数与 sample schema |
| `adapters/posix/meiso_posix_tcp.*` | transport adapter | 已连接 nonblocking stream 与 Session frame 的有界搬运；不 connect/listen |
| `targets/k230d-linux/adapters/wifi_linux_adapter.*`（meiso_edge） | Edge private adapter | 私有 interface locator 到匿名 Linux telemetry 的映射 |
| `targets/k230d-linux/wifi_runtime_edge.c`（meiso_edge） | Edge target | listen、catalog、desired apply、actual、production 与 shutdown policy |
| `devtools/macos/wifi_runtime_host.c` | Host Dev Tool | 可执行联调客户端；不是 Host SDK、Virtual Edge 或应用 API |
| `devtools/web/wifi_runtime_dashboard/` | 可视化 Dev Tool | 固定 API 把用户意图交给 native Host，并投影脱敏 NDJSON；不解析 wire、不拥有 Runtime truth |

Host 产品代码只应依赖未来 SDK/Virtual Edge 与公开 Profile，不应依赖上述 Dev Tool 的 CLI、
NDJSON、地址字段、poll loop 或 K230D adapter。

## 3. Locator、identity 与配置

`connect_ipv4/listen_ipv4 + port` 只定位一个 socket peer；PeerId、Session incarnation、
SessionId、EndpointId、endpoint incarnation 与 production/stream epoch 才用于各自协议作用域。
相同 IP 不代表相同 Edge，IP 改变也不自动产生新 identity。Host 与 Edge 都从本地私有配置读取
自己的 PeerId 和期望对端 PeerId；每次进程运行从操作系统熵源生成新的 Session nonce/incarnation，
Edge 同时建立新的 endpoint/producer identity。

| 配置所有者 | 必需字段 | 用途；不得进入公开 telemetry payload 的内容 |
|---|---|---|
| Host | 基础：`connect_ipv4`、`connect_port`、local/expected PeerId；one-shot 另需 `selected_fields`、`sample_period_ms`、`sample_count` | locator、握手约束；interactive desired 由当前 command 给出，配置中的三个 one-shot 字段可全部省略 |
| Edge | `listen_ipv4`、`listen_port`、local/expected PeerId、`interface`、`allowed_fields`、period min/max/step | locator、握手约束、私有 source 选择与 capability envelope |

Tracked example 只含 TEST-NET/通配地址与 synthetic identity。部署值放在忽略的 local config；
不得提交 locator、设备 identity、interface、凭据或原始日志。当前 plain TCP 上的 exact PeerId/
Profile 检查只是协议匹配，不提供对端认证、机密性或完整性；只在受控开发网络使用。

## 4. Catalog 构造与接受

Edge 启动时按下式生成一个完整 catalog，而不是把配置文件或 probe 结果直接暴露给 Host：

```text
Wi-Fi Profile envelope
  ∩ Edge allowed_fields/rate 私有配置
  ∩ Linux adapter probe 的真实可用字段
  ∩ target 固定 policy / capacity
  -> capability + DESIRED(State) + ACTUAL(State) + DATA(Stream)
```

当前 adapter 先要求所配 interface 存在 Linux `wireless` sysfs 目录，再只探测 carrier/link、
`/proc/net/wireless` signal 和十个 allowlist 字段中的计数文件。interface name、SSID、BSSID、
MAC/IP、channel、driver path 和 association 信息不进入 catalog、State 或 Stream。Host 必须按
Profile、schema、owner、facet、ResourceId、capacity 和 endpoint incarnation 验证完整 snapshot，
再原子 `ACCEPT`；不能部分接受或从本地硬编码推断 sensor 数量。

## 5. 唯一控制与数据序列

```text
TCP connected
  -> Core OPEN / ACCEPT / CONFIRM / READY
  -> Edge CatalogSnapshot
  -> Host CatalogAccept
  -> start: Host desired Core State(enabled=true + parameters)
  -> Edge actual Core State(STARTING/PENDING)
  -> STARTING accepted -> actual RUNNING/APPLIED(new epoch) -> Core Stream samples
  -> active reconfigure: new desired -> actual RUNNING/PENDING(old effective/epoch)
  -> pending actual accepted -> production cut -> actual RUNNING/APPLIED(new epoch)
  -> stop: Host desired Core State(enabled=false)
  -> Edge cuts source, drains already-materialized old-epoch data
  -> actual STOPPING/PENDING(old effective/epoch) -> STOPPED/APPLIED
  -> Session remains READY
  -> close: Core normal Session close
```

这里没有 subscription、unsubscribe、delivery toggle 或 Profile 私有 gate：

- Host 不需要数据时，以 desired `enabled=false` 停止 Edge 生产；不能让 source 继续生产后在
  Runtime 静默丢弃。
- desired delivery acceptance、Edge observed、adapter apply、actual `RUNNING` 和首个 sample 是
  不同进展；Edge 只有在 actual 进入 ordered Core TX 后、Host 只有在同一 actual 被 exact
  `StateReplica` commit 后才能打开各自 gate。
- 运行中重配置先公布 `RUNNING/PENDING` actual，其 applied desired、effective 参数和
  production epoch 仍指向旧生产。该 actual 被接受前旧 epoch 继续产出；接受后才切断，
  并以严格新 epoch 公布 `RUNNING/APPLIED`。
- desired disable 本身不关 gate。producer 停止前已物化的 sample 仍使用旧 epoch
  发送；排空后的 `STOPPING/PENDING` 保留旧 applied/effective/epoch，后续
  `STOPPED/APPLIED` 才关闭 gate。此时 Core Session 仍为 READY，只有 `close` 才正常关闭。
- adapter 返回 `OK` 且 sample 通过 Profile canonical 校验，即越过 public materialization
  boundary；当前 Edge 必须把它送入 Runtime/Core。若 outbound 暂时受阻，只保留一个有界 pending
  sample 并停止继续采样，不引入第二个“是否发送”决定。
- sample `selected_fields` 必须等于当前 actual effective selection；`present_fields` 只表达本次
  可得子集。字段定义、零值与 privacy denylist 不在本页重复，以 Profile 为准。

## 6. 当前 target 生命周期与失败

| 条件 | 当前行为 |
|---|---|
| desired 参数超出 accepted capability | actual `REJECTED`；不伪造 RUNNING |
| inactive 上 enable | actual `STARTING/PENDING` 被接受后发布 `RUNNING/APPLIED` 与新 epoch |
| active 上修改参数 | 旧 epoch 继续到 `RUNNING/PENDING` 被接受；再切换到 `RUNNING/APPLIED` 新 epoch |
| desired disable | 停止 source、排空已物化数据，发布 `STOPPING/PENDING` 后再发布 `STOPPED/APPLIED`；Session 保持 READY |
| `close` | 若必要先 disable 到 `STOPPED`，再发起 Core normal close；Edge 结束该 foreground connection attempt |
| 单次读取缺少某个已选字段 | sample 中该 present bit 清零；累计计数 high-water 仍保留 |
| production 开始后 source 不可用或累计计数回退 | continuity 不可证明；停止新采样和新 inbound control，排空已有 outbound，以 `SHUTDOWN` 关闭 Session |
| peer/Profile/route/epoch/State identity 不匹配或 malformed | 由 Core/Runtime 拒绝；不部分改变 catalog、State 或 gate |
| 新进程或新 Session | 使用新 identity；重新 handshake、catalog accept 和 desired，不继承旧 gate |

当前 Edge 是单连接 foreground target：一个 Core Session 内接受多个 desired，stop 不退出，
normal close 才结束该 attempt。它不在同一 Session 内热修复 continuity、重连或重建 catalog。
进程重启产生新 identity，Host 必须重新协商；这是 target 边界，不是通用 Runtime 限制。

## 7. 构建与联调

Host：

```sh
cmake -S . -B build
cmake --build build --target meiso_wifi_runtime_host
cp devtools/macos/wifi_runtime_host.example.conf /path/to/ignored/host.local.conf
./build/meiso_wifi_runtime_host --config /path/to/ignored/host.local.conf
```

上述不带 flag 的命令保留有界 one-shot。持续联调使用：

```sh
./build/meiso_wifi_runtime_host \
  --config /path/to/ignored/host.local.conf \
  --interactive
```

interactive stdin 只接受精确 compact NDJSON；`stop` 是 `enabled=false` 的 desired，
不是订阅命令：

```json
{"command":"apply","enabled":true,"selected_fields":63,"sample_period_ms":500}
{"command":"apply","enabled":false,"selected_fields":63,"sample_period_ms":500}
{"command":"close"}
```

Edge（在 meiso_edge 检出中执行）：

```sh
MEISO_K230_LINUX_CC=/path/to/riscv64-unknown-linux-gnu-gcc \
  targets/k230d-linux/build_wifi_runtime_edge.sh /tmp/meiso_wifi_runtime_edge-k230d
cp targets/k230d-linux/wifi_runtime_edge.example.conf /path/to/ignored/edge.local.conf
./meiso_wifi_runtime_edge-k230d --config /path/to/ignored/edge.local.conf
```

先启动 Edge，再启动 Host。经 SSH/scp/串口放置 binary/config 只是维护步骤，不计入 Link
通过。continuous 验收要求 Host 在同一 Session 依次观察 Core READY、catalog accepted、
`STARTING -> RUNNING`、数据、active reconfigure 的旧/新 epoch 分界、`STOPPING -> STOPPED`；
stop 后 Session 仍 READY，最后 normal close，Edge 才正常退出。任一侧单独返回成功、socket
connect、SSH 可达或 cross-build 成功都不能替代这条 target transcript。

需要页面检查时，保持 Edge foreground attempt 运行，再启动 loopback-only Dashboard：

```sh
python3 devtools/web/wifi_runtime_dashboard/server.py \
  --host-binary /absolute/path/to/meiso_wifi_runtime_host \
  --config /absolute/path/to/ignored/host.local.conf
```

服务只绑定 loopback，默认端口为 `49999`（`--port N` 可改），并固定启动
`<host> --config <config> --interactive`。浏览器 API 为：

```text
POST /api/v1/connect
POST /api/v1/apply   {"selected_fields":N,"sample_period_ms":N}
POST /api/v1/stop
POST /api/v1/close
GET  /api/v1/events?after=<SSE event id>
```

Web 只把这四个固定用户意图交给 native Host，并投影 Host 已校验的脱敏事件；
desired 仍由 Host 通过 Core State 发布。Web 不解析 Meiso wire，不是 Runtime/SDK/Virtual
Edge，不实现 subscription 或第二套协议 truth。完整边界见其
[README](../devtools/web/wifi_runtime_dashboard/README.md)。

## 8. 证据边界

| 证据 | 当前可声明 | 不能声明 |
|---|---|---|
| `meiso_wifi_runtime_tcp_test`，本机真实 `AF_INET` loopback | Session + Runtime v2 + Profile + Core State/Stream；RST 后旧 Session/gate/epoch 失效，新连接必须重新 catalog、desired、actual 后才恢复 Stream | remote network、K230D、真实 Linux Wi-Fi source |
| macOS `meiso_wifi_runtime_host` warnings-as-errors build | Dev Tool 与当前公共接口可编译链接 | 与 Edge 互操作、SDK/Virtual Edge、板端执行 |
| K230D Linux Edge cross-build，2026-08-16 | `2ac960c`、Xuantie GCC 14.1.1、91,128-byte RISC-V ELF、SHA-256 `24b02ff30ca9f1caa0542a997bdea93785d23766fdd22e06a9aee8a83a2b76f6` | board run、source truth、network interoperability |
| K230D Linux Edge cross-build，2026-08-17 | Xuantie RISC-V ELF 95,360 bytes；SHA-256 `fe8e20272917d04b4b90a565c8c9c4041a9062d717810c811fd6c7c517ce4dbc` | board run、source truth、非该 digest artifact 的属性 |
| K230D direct-IP Runtime run，2026-08-16 | K230D Buildroot Linux 6.6.36 与 macOS 完成两轮 READY、catalog、enable/RUNNING、连续 sample、disable/STOPPED、normal close；双方均 rc=0；限这两个 one-shot 参数组合的 `BOARD_VERIFIED` | 长稳、性能、network-loss、reconnect、hotplug、continuity-loss recovery、安全 transport |
| K230D continuous Runtime run，2026-08-17 | 同一 Core Session 内 `0x03f/500 ms -> 0x003/1000 ms`，两段合计 5 个真实 sample；`STARTING/RUNNING`、reconfigure `PENDING` 旧 epoch/新 `RUNNING`、`STOPPING/STOPPED`、Host/Edge normal close 均 rc=0 | 性能、长稳、自动重连、SDK、其他 Profile 或参数组合 |
| loopback Dashboard + native Host + K230D，2026-08-17 | 同一 Session 内 READY/catalog、`0x03f/500 ms` 持续 sample、`0x003/1000 ms` 新 epoch/sequence 重启、reload 后 Session/sequence 继续且 control 恢复而不重放历史 sample、`STOPPING/STOPPED` 后仍 READY、同 Session 再 start 新 epoch | Dashboard 独立证明协议正确性、SDK、订阅、安全/性能，或本次未请求的 normal close/rc |
| loopback Dashboard + 正式 Host + K230D，2026-08-16 | 页面完整投影第三次 3-sample 实机 transcript；requested=received、Host/Edge rc=0、normal close，locator/identity/network secrets 未进入浏览器 | Dashboard 自身提供协议正确性、远端控制、产品 SDK 或额外 target 能力 |
| K230D connection-loss + fresh process recovery，2026-08-16 | `RUNNING`/10 samples 后强制终止 Host，Host 与 Edge 均非零退出；不重启板、不改 Wi-Fi，fresh Edge/Host 重新完成 Session、catalog、desired、actual、5 samples 与 normal close，双方 rc=0 | 自动 reconnect/resume、旧 desired/gate 继承、OS/network outage、failure reason 跨进程持久化或 soak |
| K230D bounded cadence run，2026-08-16 | link+signal、100 ms desired 下连续接收 sequence 1..64；无 gap/missing，actual 参数精确匹配，`STOPPED`/normal close，双方 rc=0 | latency/jitter 分布、吞吐上限、deadline 保证、长稳或拥塞行为 |

新的 continuous 板测脱敏 transcript 为：2026-08-17，Host 与 K230D 在同一 Core Session 内先以
`selected_fields=0x03f` / `500 ms` 启动，观察 `STARTING/PENDING -> RUNNING/APPLIED`；
运行中改为 `0x003` / `1000 ms`，先观察保留旧 effective/epoch 的 `RUNNING/PENDING`，
再观察新 epoch 的 `RUNNING/APPLIED`。两段合计传递 5 个真实 Core Stream sample；disable
依次经过 `STOPPING/PENDING -> STOPPED/APPLIED`，最后 Host/Edge normal close，双方 rc=0。
对应 Edge 是 95,360-byte Xuantie RISC-V ELF，SHA-256 为
`fe8e20272917d04b4b90a565c8c9c4041a9062d717810c811fd6c7c517ce4dbc`。该记录不保存 locator、
SSID、PeerId、原始 counter 或原始日志，也不外推性能、安全、自动重连或 SDK。

同日的当前 Dashboard 实机验证以固定 control API 启动同一 native Host：页面先观察
Core READY 与 catalog，再在同一 K230D Session 内观察 `0x03f/500 ms` 持续 sample，
运行中改为 `0x003/1000 ms` 后新 epoch 的 Stream sequence 重新起算。页面 reload 不重启
native Host；同一 Session 与 sequence 继续，control 状态恢复，历史 sample 不向新页面重放。
disable 显示 `STOPPING -> STOPPED` 后 Session 仍 READY，并在同 Session 内再次 start 到新 epoch。
这只证明 Web 固定 control 与脱敏 projection 对 native Host 持续链路的集成，不扩展协议、
target 或 SDK 范围。该页面 attempt 尚未请求 normal close，因而不记录它的 Host/Edge rc；
前段独立 native run 的 normal close/rc 证据与之分开。

2026-08-16 的两轮 one-shot 板测脱敏 transcript 为：catalog generation 1，1 endpoint/3 routes；Host desired v1 选择
6 个字段、500 ms、enabled；Edge actual v1 `RUNNING`；Stream sequence 1..5、五次 present mask
均匹配选择、无 gap；Host desired v2 disabled；Edge actual v2 `STOPPED`；双方 normal close。
第二轮使用新进程/Session，desired v1 只选 link+signal、1000 ms；actual 精确确认该 effective
参数，Stream sequence 1..3 只携带这两个字段且无 gap，其余 counter 字段全部 absent；随后同样
disable、`STOPPED`、normal close。它直接证明 producer 的字段形状与 cadence 由 desired/actual
State 协商，而不是 source 固定产出后再按“订阅”丢弃。
adapter 直接观测到 wireless sysfs、carrier、signal 与累计 counters；公开 evidence 不保存实际
locator、SSID、MAC、PeerId、credential、原始 counter 值或原始日志。

随后 fault run 在 actual `RUNNING` 且已交换 10 个 sample 时强制终止 Host。Host 返回非零；Edge
观察连接失败、停止 one-shot attempt 并返回非零，没有发布 `STOPPED` 或 normal close。板与 Wi-Fi
均未重启，fresh Edge 进程与 fresh Host 随即建立新 Session，并从 catalog/desired/actual 重新
协商；5 个新 sample 后 `STOPPED`、normal close，双方 rc=0。该结果只证明显式 process restart
recovery；当前 target 仍没有自动 reconnect/resume。

另一个 bounded run 只选择 link+signal，以 Profile 最小 100 ms desired 周期接收 sequence 1..64；
每个 Host sample 均 `gap=false/missing_before=0`，actual 精确确认 100 ms，随后 `STOPPED`、normal
close，双方 rc=0。它只是约 6.4-second functional cadence window，不是性能或实时基准。

早期 `four-kind-lab-v0` 的 Wi-Fi/板端结果不经过当前 `meiso_runtime` catalog/desired/actual，
不能提升本切片的 evidence 等级。后续 fault/soak 板测仍必须记录 exact source/binary identity、
双方配置的脱敏形状与完整终态，并明确真实 probe/Stream 数据及未覆盖项。
