# Runtime 契约

> 状态：产品级技术契约。定义 Resource Graph、endpoint catalog、Host desired、Edge actual、
> production lifecycle、动态插拔与 data route；不定义具体 sensor 型号、driver ABI、SDK
> facade 或 transport。

## 边界

Core 建立并约束 peer Session；Runtime 决定当前 Edge 有什么、Host 想让 endpoint 做什么、
Edge 实际应用了什么，以及由此产生哪些 Resource。SDK 把 Host Runtime 的结果投影成
Virtual Edge。

```text
Core READY
  -> Edge CatalogSnapshot
  -> Host CatalogAccept
  -> Host desired State(enabled + parameters + granularity)
  -> Edge admission + adapter apply
  -> Edge actual State(applied desired version + effective configuration)
  -> production RUNNING: every produced data item -> Runtime -> Core -> Host
  -> desired disabled: stop production -> actual STOPPED -> no new data
```

不存在独立 subscribe/unsubscribe control plane。若 Host 不需要某 endpoint 的数据，它修改
desired State 使 Edge 停止生产；不能让 Edge 继续生产，再在 Runtime 中静默丢弃或不发送。

`desired` 与 `actual` 不是 Runtime 自造的 control message：它们是 catalog 中声明的普通
Core State，由 `StatePublisher / StateReplica` 传递；sensor data 是普通 Core Stream，连续
媒体是 MediaStream。Runtime 只增加 endpoint/catalog/production route 的关联与门禁，不得
另建平行 desired/actual truth、subscription slot 或 delivery switch。

Core 不声明 sensor、参数或 endpoint。Runtime 不重新实现 peer identity、Session handshake
或四类 Resource 语义。IP、socket、OS path、I2C address、file descriptor 和 native buffer
handle 不进入本契约。

## Catalog 与 endpoint bundle

Edge Runtime 从以下交集构造 catalog：

```text
Profile 允许范围
  ∩ Edge 私有配置
  ∩ adapter 当前探测结果
  ∩ policy / permission / resource budget
```

`CatalogSnapshot` 是完整、不可变、原子接受的 Resource Graph，至少携带：

- Edge Runtime incarnation 内单调且非零的 `catalog_generation`；
- 每个稳定逻辑 `EndpointId`、当前 `endpoint_incarnation` 与 capability/schema；
- endpoint 所属 Resource descriptor、facet、owner、direction、limits 与 QoS/Profile 引用；
- desired、actual 与 data Resource 的明确关联，不靠名称或 Host 硬编码推断；
- actual/availability facet 的 identity，以及较大 capability/mode table 或 calibration 所引用的
  Blob；动态值由 actual State 表达；
- snapshot 的 entry count、容量边界和完整性信息。

Catalog 被接受后，其 Resource route 可以承载对应 owner 产生的 Resource message；这只表示
route identity 有效，不表示 sensor 正在采集。是否生产、以何种参数生产，只由 desired→actual
控制链决定。

Edge 私有配置决定逻辑 endpoint、允许的 mode/parameter envelope、driver owner 与本地
policy；adapter probe 决定当前是否真实存在、可用和能否 apply。它们不是第二套公开协议，
只能通过 catalog、capability 和 actual State 影响 Host 可见结果。Host 不依赖配置文件路径、
driver 名、bus address 或固定 sensor 数量。

Sensor 不是第五种 Resource。一个 endpoint bundle 可以包含：

| facet | Resource | owner / 含义 |
|---|---|---|
| capability | State 或 Blob | Edge；支持的 mode、range、format、rate、clock、limits |
| desired | State | Host；enabled、生产参数、颗粒度与 QoS intent |
| actual | State | Edge；availability、apply 状态、effective 参数、health、production epoch |
| data | Stream / MediaStream | Edge；在 actual 允许时产生的 sample、event、audio 或 frame |
| operation | Action | 确有副作用的 calibrate、reset、self-test 等离散操作 |
| immutable | Blob | calibration、mode table、model、有限 capture 等不可变内容 |

一个 endpoint 可以有零个或多个 data/operation/Blob Resource。

## Identity 与版本

- `EndpointId` 表示 Profile 内稳定的逻辑槽，不是总线地址或硬件序列号；
- `endpoint_incarnation` 在设备替换、adapter 重启或 continuity 无法证明时更新；
- `catalog_generation` 只在 endpoint/resource/schema/mode envelope 改变时更新；瞬时 health
  或 unavailable 优先由 actual State 表达；
- generation 一旦形成可判定的 offer（包括被拒绝的 offer）便不得用不同内容复用；
- desired 与 actual 各自遵循 State owner/version；actual 必须指出它观察或应用的 desired
  owner/version，迟到结果不能覆盖更新的 desired；
- 每次成功启动或不连续的重配置产生非零 `production_epoch`；data 必须引用当前 production
  epoch，Stream 继续携带 producer incarnation、stream epoch、sequence、source clock epoch/time；
- SessionId、catalog generation、endpoint incarnation、State version、production epoch 与
  stream epoch 不互相替代。

## Desired → actual → production

1. Core 未 `READY` 时，Runtime control/data 均不得发送或接受；
2. Edge 发布完整 catalog；Host 在容量内验证 Profile、schema、identity、facet、owner、limits
   和 endpoint bundle 一致性；
3. Host 只对完整 snapshot 返回 `ACCEPT` 或带明确原因的 `REJECT`，部分 entry 不可见；
4. catalog 双方接受后，Host 才发布 endpoint desired State；没有当前 desired 时，Host-driven
   production 默认不启动；
5. Edge 按 capability、permission、budget、availability 与本地安全 policy 做 admission，再
   调用唯一 truth-owner adapter；
6. Edge actual State 正交报告 availability、apply result、production status、observed/applied
   desired owner+version、effective 参数、reason 与 production epoch；因此“新 desired 被拒绝，
   旧配置仍在 RUNNING”可被准确表达。禁止静默换 format、rate、fields 或 range；
7. 唯一 data gate 是已被 Core State 语义接受的 actual 所声明的 effective-enabled、production
   status 与 production epoch；新 desired 未应用或被拒绝时，旧 applied desired/epoch 可以继续
   `RUNNING / DEGRADED / STOPPING`，其全部样本仍须发送；
8. Host Runtime 校验 catalog/endpoint/schema、actual/production epoch、freshness、gap 与 QoS
   后再交给 SDK。

Edge 为新 production epoch 发布的 actual State 成功进入有序 Core TX 前，不得产生该 epoch
的 public sample；Host 也只有在对应 actual State 经 `StateReplica` 接受后才开放接收 gate。
解码一帧 State 但尚未通过 Core State version/owner/conflict 检查，不得改变 gate。

Host desired 的本地提交、Core delivery acceptance、Edge observed、adapter apply、actual
`RUNNING` 与首个有效 sample 是不同进展，不能合并成一个成功布尔值。

### Disable 与重配置 cut

- desired `enabled=false` 只改变意图，不直接关闭 route；Edge 先发布仍引用旧 applied desired
  和 production epoch 的过渡 actual，停止 producer 后再发布 `STOPPED` actual；
- 重配置时，Edge 可以继续按旧 actual 生产，直到明确 cut；已经产生的数据仍必须发送，并带
  旧 production epoch；`STOPPING` 若仍允许产生数据，旧 gate 必须保持有效；
- 新 effective 配置必须使用新 production epoch；Host 不把旧 epoch sample 解释为新配置；
- 若参数不能支持，Edge 发布 `REJECTED` 或 `DEGRADED` 与明确 effective/reason，不部分静默应用。

## 数据交付与有界性

对于 catalog 中公开的 data Resource，“产生”和“向 Host 交付”不是两个独立业务开关。
本地 safety/control 使用的私有采样可以留在 adapter 内，但一旦被声明并物化为 public Resource
数据，就必须进入 Runtime/Core。

当 Core backpressure、deadline 或 transport failure 阻止交付时，Edge 必须按 Profile 的有界
规则 throttle、停止 source、保留有限 history，或产生可观察的 drop/gap/failure；不能无限缓存，
也不能把静默丢弃伪装成“未订阅”。MediaStream 可以使用专业化数据面，但仍共享相同 endpoint、
desired/actual、production epoch、Session 与 failure 语义。

## 动态插拔与更新

- Profile 已知的可插拔逻辑槽可以保留在 catalog 中，由 actual State 表达 `ABSENT`；
- 真正新增/删除 Resource、schema 或 mode envelope 时，Edge 发布新的完整 generation；
- v0 catalog 改变时所有旧 route/control binding 失效，Host 必须接受新 snapshot 并重新发布
  desired；
- unplug 时 Edge 停止 production，发布 actual unavailable/stream end（仍可发送时），拒绝新
  Action，并使 SDK last-known view 带 stale/freshness；
- replug 或 continuity 无法证明时使用新的 endpoint incarnation、production epoch 与 stream
  epoch；不得复活旧 handle、desired binding 或 sample；
- catalog build/reject 不改变当前 active generation；只有完整新 snapshot 被接受后才原子切换。

## Session 与恢复

每个新 Core Session 都重新交换并接受 catalog，Host 随后重新发布 desired State；地址相同不
表示可以复用旧 control/data binding。Session 丢失时，Host-driven production 默认停止；Profile
只有在明确声明 bounded offline retention 时才能继续。State truth 可以重新 offer，Action 不
自动重放，Blob 是否复用由 content identity/Profile 决定。

若 Host 首次连接时接收一个连续 producer 的 Stream，首样本 sequence 可以大于 1，
`missing_before` 必须显式暴露；不能为新 Host 重置 Edge sequence 或静默吞掉 gap。

## 失败

Runtime 至少区分：

- `CORE_NOT_READY / STALE_SESSION`；
- `CATALOG_CAPACITY / MALFORMED_CATALOG / STALE_CATALOG`；
- `UNKNOWN_ENDPOINT / STALE_ENDPOINT_INCARNATION / UNKNOWN_RESOURCE`；
- `SCHEMA_MISMATCH / UNSUPPORTED_PARAMETER / QOS_UNSUPPORTED`；
- `STALE_DESIRED / DESIRED_CONFLICT / ACTUAL_MISMATCH / PRODUCTION_NOT_RUNNING`；
- `PERMISSION_DENIED / ADMISSION_DENIED / UNAVAILABLE / APPLY_FAILED`；
- `PAYLOAD_CAPACITY / BACKPRESSURE / PROTOCOL_ERROR`。

任一失败不得部分激活 catalog、desired、actual 或 production。catalog staging、State、route、
queue、history 和 pending apply 都有协商或 Profile 固定上限，并暴露 capacity/drop/failure。

## 层级责任

| 层 | 责任 |
|---|---|
| Core | peer/Session identity、handshake、limits、bounded delivery/failure；不理解 sensor |
| Edge Runtime | catalog、desired apply、actual、production lifecycle、route 与 telemetry |
| Host Runtime | catalog mirror、desired truth、actual/gap/freshness 处理 |
| Profile | endpoint schema、参数/颗粒度、QoS、capacity、production cut、hotplug、recovery |
| Adapter | 私有映射、动态检测、真实生产/控制、timestamp 与 native handle |
| Host SDK | typed discovery/configure、Virtual Edge、stale handle；不暴露 transport |
| App | 表达 desired 并消费 actual/data；不推断硬件拓扑 |

## 验收条件

正式 Runtime 实现至少证明：

- Core `READY` 前零 Runtime mutation；新 Session 必须重新协商 catalog 与 desired；
- catalog snapshot 原子接受，stale/conflict/capacity 不部分提交；
- Host desired 可以启停 endpoint、选择参数和数据颗粒度，unsupported 参数明确 reject；
- desired delivery acceptance 不冒充 Edge actual；actual production gate 未激活时不产生 data；
- desired disable 后停止 production；已经产生的数据不因独立 delivery 开关被静默丢弃；
- reconfigure 使用新 production epoch，旧数据不能冒充新 effective 配置；
- unplug/update 关闭旧 production，replug 使用新 endpoint incarnation/epoch；
- wrong Session/catalog/endpoint/resource/kind/schema/State version/production epoch 全部拒绝；
- queue、State、route 与 payload 过载仍有界且可观察；
- simulated Edge、真实 Edge 和 Host SDK 使用同一 catalog/desired/actual/data 语义。
