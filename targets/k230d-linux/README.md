# K230D Linux targets

## V4L2 camera capture source（THREE_FRAME_CAPTURE_RECEIPT）

当前bounded adapter固定progressive NV12 `320x240`、stride `320`、sizeimage `115200`，要求至少4个
single-planar MMAP buffer，并保持一个dequeued borrow直至exact QBUF release。准确K230D上的一次受控
probe在既有GC2093 camera server存活时取得3帧，每帧`bytes_used=115200`。driver raw field为ANY，
但adapter只在`G_FMT`已确认progressive后按format-inherited接受；raw driver sequence均为0且标为
absent，effective sequence因此由adapter synthetic为`1/2/3`，producer drop均为0。

三帧SHA-256依次为：

- `c67befc577d44d7518e927b49dd54c65c3b5fdd8c6409ac915c96b0e475776cc`
- `ee89441206693ee78516165413658dae3a55522cfc9e5fa10583756ea6a7f9ad`
- `e7ee27ee8fefa008622e14c4444556e36d037f67b8bc1beca7e3c7ef9711f104`

probe `rc=0`、未超时、stderr为空且server仍存活；最终phase为`STOPPED`，mapped/queued均为0。cross
ELF SHA-256为`eda3eb1d8eeb94832e8867197a7c5e1a54241e29c0d87a6b1901723d8b90d4df`，结构化stdout
receipt SHA-256为`86de6baf0b2797713217113035e5bd28cc963504b38c8f17e4867cb0a18eef33`。

public commits `c301f73`与`1cdb584`分别完成native metadata/probe与portable receipt校验；Actions run
`32707593568`四个job全部success。independent media-route mask提交`e2dbee4`另有Actions run
`32707744553`四个job全部success，但该Host CI不扩大本节board evidence。

状态只为single-adapter physical capture `BOARD_VERIFIED`。它没有经过CameraMedia transport、combined
Host/Edge service或浏览器，也不证明desired/actual闭环、encoder、性能、长稳、自动恢复或E2E；更不
改变下节Composition offscreen candidate的`TARGET_NOT_RUN`。

## Composition offscreen candidate（TARGET_BUILD_VERIFIED / TARGET_NOT_RUN）

public commit `c3b4770` 的portable synthetic reference renderer已在Host得到
`HOST_OFFSCREEN_REFERENCE_VERIFIED`；Actions run `32326481615`四个job全部success。public commit
`b9ec6a3` 另提供offline runner与build-only K230D Linux candidate：

```sh
MEISO_K230_LINUX_CC=/path/to/riscv64-unknown-linux-gnu-gcc \
  targets/k230d-linux/build_composition_offscreen_runner.sh \
  /tmp/meiso_composition_offscreen_runner-k230d
```

脚本只编译，不部署或运行；compiler path不进入仓库。official K230 SDK默认Linux toolchain为
`Xuantie-900-gcc-linux-5.10.4-glibc-x86_64-V2.6.0`；本次使用的449,883,716-byte archive SHA-256为
`17decb02130b8d79a1eb8c2126e17749fbea603dae7d95f20cd4ab411f3f1a41`，compiler报告
GCC 10.2.0 / Xuantie Toolchain V2.6.0 B-20220715。在linux/amd64 Ubuntu 22.04 container中调用上述
tracked脚本，两次fresh build产物byte-exact：83,152 bytes，SHA-256
`05accb66e621c0bef981e78312fae851a8f0cb8b233e1a9c89e4d648340972f8`。只读ELF receipt为ELF64
little-endian RISC-V、RVC、double-float LP64D，interpreter为
`/lib/ld-linux-riscv64v_xthead-lp64d.so.1`。因此build可标`TARGET_BUILD_VERIFIED`。

该ELF没有被执行，也没有board identity或target NDJSON receipt；runtime状态仍严格为
`TARGET_NOT_RUN`。target-macro source只定义neutral `OFFSCREEN_REFERENCE_VECTOR_PASSED`/
`k230d_linux_candidate` build contract，不能输出或推导`TARGET_OFFSCREEN_REFERENCE_VERIFIED`，更不能
声明K1 complete。

native runner只执行`baseline_cube_once`，`full_lifecycle=false`：一个canonical triangle-list cube Blob、
一个object/world、一个Plan和一次320×240 synthetic render。它不连接下节Wi-Fi target，不使用
Session/Runtime/scheduler/State/Stream/TCP，也不执行offline lease；renderer不是geometry-correct rasterizer，
结果没有CMA/driver/fence/scanout/physical或performance含义。external RenderResult Stream仍未接入。

当前macOS arm64 receipt的五个显式caller allocation均alignment 8：

| arena / domain | bytes |
|---|---:|
| BlobStore / `cpu_heap_core_blob` | 4,720,304 |
| SceneStore / `cpu_heap_profile` | 31,656 |
| Coordinator / `cpu_heap_profile` | 1,520 |
| reference renderer / `cpu_heap_reference_targets` | 1,268,872 |
| runner scratch / `cpu_heap_runner` | 14,720 |
| **subtotal** | **6,037,072** |

renderer项含两个target的1,228,800 bytes与最小metadata 40,072 bytes，均为CPU heap，不是GPU/CMA。
subtotal不含`Runner` stack、libc/stdio、process/OS memory或任何未实例化的transport/Stream对象。
checked allocator只证明runner wrapper在enable前5次成功allocation、enable后0次attempt并最终5次free，
不证明全进程no-heap。Actions run `32327841849`四个job全部success；这是Host CI，不是target run。

## Wi-Fi Runtime Edge v0

当前正式 Runtime target 是
[`wifi-telemetry-v0`](../../profiles/wifi-telemetry-v0/README.md) 的单连接 Linux Edge。
它以 numeric direct-IP 接受一个 Host Core Session，从私有 config 与 Linux Wi-Fi probe 的
交集生成 catalog，并在该 Session 内处理多个 desired；`stop` 后仍 READY，normal
`close` 才结束 foreground attempt。数据序列为
`desired Core State -> actual Core State -> production gate -> Core Stream`。不存在
subscription；一旦 sample 越过 adapter materialization boundary，就必须进入 Runtime/Core。
完整 locator/identity、lifecycle、Host 联调和 evidence 规则见
[Direct-IP Wi-Fi Runtime v0 实装规则书](../../docs/wifi-runtime-direct-ip-v0.md)。

用 Canaan/Xuantie glibc compiler 交叉构建；compiler path 只在调用时提供：

```sh
MEISO_K230_LINUX_CC=/path/to/riscv64-unknown-linux-gnu-gcc \
  targets/k230d-linux/build_wifi_runtime_edge.sh /tmp/meiso_wifi_runtime_edge-k230d
```

把 [`wifi_runtime_edge.example.conf`](wifi_runtime_edge.example.conf) 复制到 ignored local path，
替换 numeric listen locator、synthetic PeerId 和私有 interface；不得提交部署值或凭据。然后在
K230D Linux 上启动一个 foreground attempt：

```sh
./meiso_wifi_runtime_edge-k230d --config /path/to/ignored/edge.local.conf
```

macOS 对端是 Dev Tool，不是 SDK：

```sh
cmake -S . -B build
cmake --build build --target meiso_wifi_runtime_host
./build/meiso_wifi_runtime_host --config /path/to/ignored/host.local.conf
```

上述命令是有界 one-shot。单 Session 持续联调使用 native Host interactive 模式：

```sh
./build/meiso_wifi_runtime_host \
  --config /path/to/ignored/host.local.conf \
  --interactive
```

stdin 命令为精确 compact NDJSON：

```json
{"command":"apply","enabled":true,"selected_fields":63,"sample_period_ms":500}
{"command":"apply","enabled":false,"selected_fields":63,"sample_period_ms":500}
{"command":"close"}
```

enable 从 inactive 经 `STARTING/PENDING -> RUNNING/APPLIED`；active reconfigure 先公布
`RUNNING/PENDING` 的旧 effective/epoch，待接受后切换到新 epoch 的
`RUNNING/APPLIED`；disable 先切断 source 并排空已物化 sample，再经
`STOPPING/PENDING -> STOPPED/APPLIED`。`STOPPED` 不关闭 Session。

需要本地页面复核时，可用 loopback-only Dashboard 启动同一 native Host：

```sh
python3 devtools/web/wifi_runtime_dashboard/server.py \
  --host-binary /absolute/path/to/meiso_wifi_runtime_host \
  --config /absolute/path/to/ignored/host.local.conf
```

页面只提供固定 `connect/apply/stop/close` 意图和脱敏事件投影；它不解析 wire，
不是 Runtime/SDK/Virtual Edge，也不是 subscription control plane。浏览器 API 固定为
`POST /api/v1/connect|apply|stop|close` 与 `GET /api/v1/events`。

Edge 只读取配置 interface 的 carrier/operstate、`/proc/net/wireless` signal 与八个累计计数；
interface/SSID/BSSID/MAC/IP/channel/driver path 不进入公开 payload。production 后 source
unavailable 或计数器回退表示 continuity 丢失：Edge 停止新采样与新 control，排空已有 outbound，
再以 `SHUTDOWN` 关闭 Session；恢复须由新进程、新 identity 和完整重协商完成。

当前已有本机真实 `AF_INET` synthetic Runtime test、macOS Host build、K230D cross-build
与真实 direct-IP K230D board run。2026-08-17 Edge 产物为 95,360-byte Xuantie RISC-V ELF，
SHA-256 为 `fe8e20272917d04b4b90a565c8c9c4041a9062d717810c811fd6c7c517ce4dbc`。同一
Core Session 内实测 `0x03f/500 ms -> 0x003/1000 ms`，两段合计 5 个真实 sample，
完整观察 `STARTING/RUNNING`、reconfigure `PENDING` 旧 epoch/新 `RUNNING`、
`STOPPING/STOPPED` 与 Host/Edge normal close，双方 rc=0。其受限 `BOARD_VERIFIED` 记录见
[规则书的证据边界](../../docs/wifi-runtime-direct-ip-v0.md#8-证据边界)。plain TCP 的 exact
PeerId/Profile 匹配仍不是认证、机密性或完整性；SSH/串口只属于部署与维护。

同日 Dashboard 以同一 native Host/K230D Session 验证 READY/catalog、`0x03f/500 ms`
持续 sample、运行中 `0x003/1000 ms` 新 epoch/sequence 重启；reload 后同一
Session/sequence 继续、control 恢复且不重放历史 sample。`STOPPING -> STOPPED` 后
Session 仍 READY，并在同 Session 内再 start 到新 epoch。该页面 attempt 尚未请求
normal close，不记录 Host/Edge rc；它只证明 Web projection/control integration，不扩展
native protocol 证据范围。

2026-08-16 的实机 fault run 在 production 中终止 Host；当时的 one-shot Edge 观察连接失败
并非零退出。
不重启板或 Wi-Fi，fresh Edge/Host 随即从 Session 与 catalog 重新协商，并完成 5-sample normal
run。该结果证明显式 process restart recovery，不是自动 reconnect/resume。

一个独立 bounded cadence run 以 link+signal、100 ms desired 收到 sequence 1..64，Host 无
gap/missing，双方正常关闭。该约 6.4-second 窗口不是性能、实时或长稳证明。

## Four-kind integration CI endpoint

This target builds the bounded native Edge/Host fixture selected by
[`four-kind-lab-v0`](../../profiles/four-kind-lab-v0/README.md). It is a manual lab executable, not
a daemon, boot service, board image, product Runtime, discovery mechanism, or security transport.
It remains a retained vertical integration fixture and evidence for the fixed transcript; it is not
the K230D implementation entry for
the formal [Runtime contract](../../docs/runtime.md).

## Build

Build the macOS Host with the repository CMake target:

```sh
cmake -S . -B build
cmake --build build --target meiso_four_kind_lab
```

Build the K230D Linux Edge with the official Canaan/Xuantie glibc compiler. The compiler path is
provided at invocation time and is never recorded in the repository:

```sh
MEISO_K230_LINUX_CC=/path/to/riscv64-unknown-linux-gnu-gcc \
  targets/k230d-linux/build_four_kind_lab.sh /tmp/meiso_four_kind_lab-k230d
```

The script deliberately does not build a root filesystem, flash storage, install files, or create
autostart state. Deployment over a maintenance surface is separate from the Meiso Link evidence.

## Run

Start one bounded Edge attempt on the board:

```sh
./meiso_four_kind_lab-k230d --edge --port 27640 --sessions 1
```

Copy [`direct-ip-lab.example`](../../config/direct-ip-lab.example) to an ignored local location. Put
only the numeric IPv4 locator in the fixed environment variable; the executable validates the
tracked Profile label and port and never emits the address:

```sh
MEISO_LAB_EDGE_ADDRESS='<numeric-ipv4>' \
  ./build/meiso_four_kind_lab --host --config /path/to/local-config --test all
```

`--test state|stream|action|blob|all` selects requested NDJSON verdicts. Every selection still runs
the full Registry, State, Stream, Action and Blob transcript because the four distinctions are the
evidence boundary. Output is a fixed, dashboard-safe NDJSON projection; it excludes locator,
interface name, SSID, BSSID, MAC, credentials, nonces, complete identifiers, sysfs paths, raw thermal
values and digests.

## Platform observations

- Wi-Fi input is limited to carrier/operstate, cumulative RX/TX counters and `/proc/net/wireless`
  signal. It is network telemetry, not a physical sensor.
- The only current physical sensor input is the K230 on-die thermal source. A separate child gets at
  most 1000 ms before `listen` to validate the thermal-zone type, register/code guards and vendor
  conversion; the Session path uses only the cached result. Timeout, absence or rejection stays
  absent. Only corrected milli-Celsius and fixed untrimmed `3000 mC` uncertainty cross the wire.
- Action resets only the executable's counter baseline/window. It does not change the interface,
  driver, route, thermal hardware, Session, kernel counters, or persistent state.
- Plain TCP and fixed public lab PeerIds provide exact Profile/role matching, not authentication,
  confidentiality or integrity. Use only on a controlled lab network.

## 2026-08-16 validation

The 96104-byte RISC-V Linux ELF
(`sha256:41a7a7b3930d2b738c42fb127d2628b7b3b623a74952ca061b41703f247f738b`)
was deployed temporarily to `/tmp` on K230D. One macOS-to-Edge Wi-Fi run completed the Session and
reported State `3/3`, Stream `3/3`, Action `4/4` and Blob `3/3` PASS. The sanitized sample included
Wi-Fi carrier/counters/signal and corrected on-die junction temperature (`59.002 °C ±3 °C`).

This is a semantics/data-exchange result, not a throughput, latency, soak, reconnect, security,
product Runtime, camera, IMU or NuttX-bridge result.
