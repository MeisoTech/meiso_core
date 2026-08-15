# K230D Linux Edge 开发基线

> 状态：`SELECTED_IMPLEMENTATION_BASELINE / PROFILE_NOT_DEFINED /
> BOARD_DUAL_OS_BOOT_OBSERVED / NETWORK_ACCESS_PENDING`。
> 本页定义当前 K230D 候选实现的责任分工与验收边界，不改变 Host/Edge、Resource、Link
> 或 Composition 语义，也不是 Host 可直接编码的互操作 Profile。

## 一个 Edge，两个执行域

K230D 整块设备是一个 `Edge`，不把两颗 core 建模为 Host 与 Edge，也不把 NuttX
建模为第三个 peer。

```text
selected low-bandwidth sensors         camera / media / Linux-owned sensors
              |                                      |
small core / NuttX sensor plane        Linux platform adapters
              |                                      |
       bounded internal bridge                       |
              +------------------+-------------------+
                                 |
                    big core / Linux Edge Runtime
                    +-- Resource + capability + LinkSession
                    +-- Wi-Fi / future USB network adapters
                    +-- Composition + Edge-side Rendering Runtime
```

## 责任

| 域 | 当前责任 | 明确不负责 |
|---|---|---|
| 小核 NuttX | 对确有必要的 sensor 做采集、最早可靠时间戳、小型有界缓冲和健康状态 | 不建立 Host LinkSession，不拥有 Wi-Fi/USB 产品协议，不运行第二套 Resource Runtime 或 Composition |
| 大核 Linux | Edge identity/incarnation、capability/Resource Runtime、LinkSession、Wi-Fi 与未来经验证的 USB network、高带宽 media、Composition、Edge-side rendering 与大部分 platform adapter | 不把内部 core/OS 结构泄漏给 Host API |
| macOS Host | Virtual Edge、Host SDK、simulator/replay、应用与协议测试 | 不直接操作 NuttX IPC、Linux driver handle 或物理 sensor bus |

因此选中小核采集时，“`sensor -> 小核 -> Wi-Fi -> Host`”在实现上是：

```text
sensor -> NuttX acquisition -> internal bridge -> Linux Resource Runtime
       -> Linux-owned LinkSession -> Wi-Fi (or future USB network) -> Host
```

Wi-Fi 当前由 Linux 管理。把 Wi-Fi stack 也下放到小核会引入第二套网络、安全和
Session lifecycle，当前没有需求支持这个成本。

## Sensor 边界

- 不是每个 sensor 都必须放小核。Linux 已有成熟驱动或需要大带宽 buffer 的 camera/media
  endpoint 可直接由 Linux adapter 拥有；
- 低延迟、低功耗、持续采样或需要在 Linux 繁忙/重启时仍保持的 sensor，才是 NuttX
  候选；
- 一个物理 endpoint 只有一个 driver owner。Linux 和 NuttX 不能同时控制同一 bus/device；
- 小核输出包含 schema version、producer incarnation、stream epoch/sequence、source clock
  epoch、source monotonic time 和 health。跨核需要比较时间时，Linux 还要给出逐 source clock
  的 mapping 与 uncertainty；Linux 只能在这些证据有效时发布对应 Resource；
- 小核或 sensor 重启时，对应 endpoint 更新 producer incarnation、stream epoch 和 actual
  status；只有 source clock continuity 无法证明时才更新 source clock epoch。如果 Linux
  Edge identity 与公开 Session 连续性仍可证明，不必因内部重启自动新建 LinkSession。

## 内部桥的最小要求

核间 IPC/shared memory/RPMsg 是 platform-private bridge，不是公开 Link wire。首版只需：

- 明确 schema/version、两端 incarnation、source clock epoch 与 stream epoch；
- 有界 queue 与可观察 overflow/drop；
- 不用 Linux receive time 伪装 sensor source time；
- 断开、小核重启、旧 sample 和新 epoch 的可执行测试；
- Linux 未获得新鲜且完整的 sample 时，capability/status 显式 degraded/unavailable。

首版不建设通用分布式 runtime、双向 RPC framework 或另一套 Resource wire。

## 重启与连续性

| 事件 | 对外结果 |
|---|---|
| 整板/A-B boot | device identity 可保持；Edge boot/runtime incarnation、LinkSession、source clock epoch 和所有 stream epoch 全新 |
| Linux Edge Runtime 重启 | 新 Edge runtime incarnation 与新 LinkSession；即使 NuttX 存活，外部 Stream 也以新 epoch 开始 |
| NuttX/sensor plane 重启或 heartbeat 超时 | Linux LinkSession 可保持；受影响 endpoint 先变为 degraded/unavailable，重建后使用新 producer incarnation 和 stream epoch |
| 内部 bridge continuity 无法证明 | 丢弃旧 queue/cache，更新 bridge generation 和受影响 stream epoch；不补造 sample |
| Wi-Fi/IP 变化 | 地址不是 identity；只有对等方与有界恢复状态能证明连续性时才 resume，否则新 Session |

network 断开时，小核只能保留已经配置并对外声明的有界 history；overflow/drop 必须向
Linux 和 Host 可观察，不得无限缓存。

## Network 与 Host 测试

Linux Edge Runtime 拥有公开 LinkSession。Wi-Fi 和将来经实证的 USB network 只是
可替换 carrier：

- 同一 handshake、Session identity、schema、Resource 和 failure 必须在两者上成立；
- Host 应用不根据 carrier 分叉业务逻辑；
- 首版一次只激活一个 carrier，不承诺 multi-binding、无缝迁移或 Host switching；
- macOS 先用 simulated/local backend 跑同一 conformance，当前只经 Wi-Fi 连接物理
  Edge。USB network 在它的电气角色、driver 和稳定性单独验证后再加入，不是首个
  Session slice 的硬门。

“串口 + Wi-Fi SSH”是 bring-up 和部署通道，不是 Meiso 业务 Link：串口用于恢复和
本地 shell，SSH 用于受控开发部署，Meiso Link 仍通过独立、可版本化、可验证的
network protocol 实现。

## Rendering 边界

Edge-side Rendering Runtime 运行在大核 Linux，可使用已验证的 Linux graphics/media
adapter。它只产生 renderer-local candidate 和 failure；Composition 仍拥有 plan preparation、
activation、hold-last/safety policy 与最终 presentation authority。`REMOTE_PROJECTION` 中的
场景渲染仍可由 Host 完成，Edge 负责 decode/correction/composition/scanout；`EDGE_NATIVE`
才把所声明的本地生成工作放在 Edge。实际分工由未来互操作 Profile 选择。小核不运行
renderer 或第二套 Composition。

## 当前证据边界

当前已选择一份包含 Linux/NuttX、A/B rootfs、串口恢复、Wi-Fi 配置和公钥 SSH 的
K230D 私人开发镜像。它已完成介质写入与精确 source-span 读回；本次上电后的同时、有限
RX-only 串口捕获分别取得已缓冲的 Linux `Welcome to Buildroot`，以及 NuttX 12.4.0
NuttShell prompt。这些 marker 支持两个执行域曾启动至各自 userland/shell，但没有捕获完整
boot transition 或证明持续 liveness。

这次观察没有固定完整 Linux build identity，也没有证明 Linux login、Wi-Fi association、SSH、
USB network、sensor bridge、Rendering Runtime 或任何产品 Link。当前 USB 证据仍只是双串口，
不能解释为 USB network。

该镜像中的 NuttX 仍是预编译启动 payload；当前项目没有冻结的 NuttX sensor
source/config、可用的 Linux↔NuttX bridge 或 lifecycle supervisor。内核出现 generic IPC
configuration 不等于板上 IPC 已可用。第一个 bridge 工作单先用 synthetic counter 证明
容量、时间、重启和丢失，再选一个真实低带宽 sensor。

因此本页只冻结开发分工。它必须在获得稳定 Profile ID/version、Resource/capability catalog、
QoS/limits、handshake/wire、authentication/security、failure codes、golden vectors、完整 trace、
simulator artifact 和相应板端 evidence 后，才能晋升为互操作 Profile。每个能力仍按实际
capability 和对应 physical/integration evidence 逐项晋升。
