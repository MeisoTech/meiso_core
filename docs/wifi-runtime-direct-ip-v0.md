# Direct-IP Wi-Fi Runtime v0 实装规则书

> 状态：所选垂直切片的实现与联调规则；不是新的产品规范。通用语义由
> [系统](system.md)、[Resource](resource.md)、[Runtime](runtime.md)和
> [Link](link.md)拥有；字段与 canonical wire 由
> [Wi-Fi telemetry Profile v0](../profiles/wifi-telemetry-v0/README.md)拥有。

## 1. 选择与非目标

本切片固定以下组合：

| 项目 | v0 选择 |
|---|---|
| Link bootstrap | 配置给出的 numeric IPv4 + TCP port；Host `connect`，Edge `listen/accept` |
| Core | exact peer/Profile handshake、一次性 Session、bounded Resource delivery |
| Runtime | 一个 endpoint；Edge catalog、Host desired State、Edge actual State、Edge data Stream |
| Profile | `WIFI_TLM/_PROFILE:v1` / `wifi-telemetry-v0` |
| Host 实现 | macOS foreground Dev Tool；有限 sample 数后主动 disable |
| Edge 实现 | K230D 大核 Linux foreground target；一个连接、一个有界 pending sample |

Direct IP 只是当前 bootstrap/transport 选择，不进入 Core identity，也不承诺 discovery、DNS、
自动重连、运行中迁移、daemon、autostart 或 SDK API。Wi-Fi endpoint 只报告匿名链路 telemetry；
它不管理 Wi-Fi association、凭据、DHCP、route 或 Internet 连通性。SSH/串口只用于部署、启动、
日志和维护，不是 Meiso Link，也不承载 Runtime 数据。

## 2. 代码归属

| 路径 | 归属 | Host/Edge 开发者可依赖的边界 |
|---|---|---|
| `src/meiso_session.c`、`src/meiso_resource.c`、`src/meiso_state.c`、`src/meiso_stream.c` | portable Core | Session 与 State/Stream primitive；不理解 Wi-Fi |
| `src/meiso_runtime.c` | portable Runtime core | catalog、State commit、production gate 与 Stream route；不读取 IP/sysfs |
| `profiles/wifi-telemetry-v0/` | versioned Profile | capability、desired/effective 参数与 sample schema |
| `adapters/posix/meiso_posix_tcp.*` | transport adapter | 已连接 nonblocking stream 与 Session frame 的有界搬运；不 connect/listen |
| `targets/k230d-linux/wifi_linux_adapter.*` | Edge private adapter | 私有 interface locator 到匿名 Linux telemetry 的映射 |
| `targets/k230d-linux/wifi_runtime_edge.c` | Edge target | listen、catalog、desired apply、actual、production 与 shutdown policy |
| `devtools/macos/wifi_runtime_host.c` | Host Dev Tool | 可执行联调客户端；不是 Host SDK、Virtual Edge 或应用 API |

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
| Host | `connect_ipv4`、`connect_port`、local/expected PeerId、`selected_fields`、`sample_period_ms`、`sample_count` | locator、握手约束、desired 与有限测试窗口 |
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
  -> Host desired Core State(enabled + selected_fields + sample_period_ms)
  -> Edge admission / adapter apply
  -> Edge actual Core State(applied desired + effective config + production status/epoch)
  -> actual enters Edge ordered Core TX; Host exact StateReplica commit
  -> corresponding Edge/Host production gates open
  -> each materialized Wi-Fi sample -> Runtime DATA route -> Core Stream -> Host
  -> Host desired Core State(enabled=false)
  -> Edge stops source and drains already-materialized old-epoch data
  -> Edge actual Core State(STOPPED) closes production gate
  -> normal Session close
```

这里没有 subscription、unsubscribe、delivery toggle 或 Profile 私有 gate：

- Host 不需要数据时，以 desired `enabled=false` 停止 Edge 生产；不能让 source 继续生产后在
  Runtime 静默丢弃。
- desired delivery acceptance、Edge observed、adapter apply、actual `RUNNING` 和首个 sample 是
  不同进展；Edge 只有在 actual 进入 ordered Core TX 后、Host 只有在同一 actual 被 exact
  `StateReplica` commit 后才能打开各自 gate。
- desired disable 本身不关 gate。producer 停止前已经物化的 sample 仍使用旧 production epoch
  发送；`STOPPED` actual 被接受后才禁止后续 data。
- adapter 返回 `OK` 且 sample 通过 Profile canonical 校验，即越过 public materialization
  boundary；当前 Edge 必须把它送入 Runtime/Core。若 outbound 暂时受阻，只保留一个有界 pending
  sample 并停止继续采样，不引入第二个“是否发送”决定。
- sample `selected_fields` 必须等于当前 actual effective selection；`present_fields` 只表达本次
  可得子集。字段定义、零值与 privacy denylist 不在本页重复，以 Profile 为准。

## 6. 当前 target 生命周期与失败

| 条件 | 当前行为 |
|---|---|
| desired 参数超出 accepted capability | actual `REJECTED`；不伪造 RUNNING |
| desired disable | 停止 source，排空已物化数据，发布 actual `STOPPED` |
| 单次读取缺少某个已选字段 | sample 中该 present bit 清零；累计计数 high-water 仍保留 |
| production 开始后 source 不可用或累计计数回退 | continuity 不可证明；停止新采样和新 inbound control，排空已有 outbound，以 `SHUTDOWN` 关闭 Session |
| peer/Profile/route/epoch/State identity 不匹配或 malformed | 由 Core/Runtime 拒绝；不部分改变 catalog、State 或 gate |
| 新进程或新 Session | 使用新 identity；重新 handshake、catalog accept 和 desired，不继承旧 gate |

当前 Edge 是 one-shot foreground target，不在同一 Session 内热修复 continuity、重连或重建
catalog。进程重启产生新 identity，Host 必须重新协商。这个选择是当前实现边界，不是通用
Runtime 对 hotplug/recovery 的限制。

## 7. 构建与联调

Host：

```sh
cmake -S . -B build
cmake --build build --target meiso_wifi_runtime_host
cp devtools/macos/wifi_runtime_host.example.conf /path/to/ignored/host.local.conf
./build/meiso_wifi_runtime_host --config /path/to/ignored/host.local.conf
```

Edge：

```sh
MEISO_K230_LINUX_CC=/path/to/riscv64-unknown-linux-gnu-gcc \
  targets/k230d-linux/build_wifi_runtime_edge.sh /tmp/meiso_wifi_runtime_edge-k230d
cp targets/k230d-linux/wifi_runtime_edge.example.conf /path/to/ignored/edge.local.conf
./meiso_wifi_runtime_edge-k230d --config /path/to/ignored/edge.local.conf
```

先启动 Edge，再启动 Host。经 SSH/scp/串口放置 binary/config 只是维护步骤，不计入 Link
通过。一次正常验收要求 Host 依次观察 Core READY、catalog accepted、actual RUNNING、请求数量的
合法 sample、actual STOPPED 与 normal close；Edge 同时正常退出。任一侧单独返回成功、socket
connect、SSH 可达或 cross-build 成功都不能替代这条 target transcript。

## 8. 证据边界

| 证据 | 当前可声明 | 不能声明 |
|---|---|---|
| `meiso_wifi_runtime_tcp_test`，本机真实 `AF_INET` loopback | Session + Runtime v2 + Profile + Core State/Stream 的 synthetic direct-IP 路径 | remote network、K230D、真实 Linux Wi-Fi source |
| macOS `meiso_wifi_runtime_host` warnings-as-errors build | Dev Tool 与当前公共接口可编译链接 | 与 Edge 互操作、SDK/Virtual Edge、板端执行 |
| K230D Linux Edge exact-current-source cross-build | `2ac960c`、Xuantie GCC 14.1.1、91,128-byte RISC-V ELF、SHA-256 `24b02ff30ca9f1caa0542a997bdea93785d23766fdd22e06a9aee8a83a2b76f6` | board run、source truth、network interoperability |
| K230D direct-IP Runtime run，2026-08-16 | K230D Buildroot Linux 6.6.36 与 macOS 完成两轮 READY、catalog、enable/RUNNING、连续 sample、disable/STOPPED、normal close；双方均 rc=0；限这两个 one-shot 参数组合的 `BOARD_VERIFIED` | 长稳、性能、network-loss、reconnect、hotplug、continuity-loss recovery、安全 transport |

本次板测的脱敏 transcript 为：catalog generation 1，1 endpoint/3 routes；Host desired v1 选择
6 个字段、500 ms、enabled；Edge actual v1 `RUNNING`；Stream sequence 1..5、五次 present mask
均匹配选择、无 gap；Host desired v2 disabled；Edge actual v2 `STOPPED`；双方 normal close。
第二轮使用新进程/Session，desired v1 只选 link+signal、1000 ms；actual 精确确认该 effective
参数，Stream sequence 1..3 只携带这两个字段且无 gap，其余 counter 字段全部 absent；随后同样
disable、`STOPPED`、normal close。它直接证明 producer 的字段形状与 cadence 由 desired/actual
State 协商，而不是 source 固定产出后再按“订阅”丢弃。
adapter 直接观测到 wireless sysfs、carrier、signal 与累计 counters；公开 evidence 不保存实际
locator、SSID、MAC、PeerId、credential、原始 counter 值或原始日志。

早期 `four-kind-lab-v0` 的 Wi-Fi/板端结果不经过当前 `meiso_runtime` catalog/desired/actual，
不能提升本切片的 evidence 等级。后续 fault/soak 板测仍必须记录 exact source/binary identity、
双方配置的脱敏形状与完整终态，并明确真实 probe/Stream 数据及未覆盖项。
