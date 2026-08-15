# Runtime 契约

> 状态：产品级技术契约。定义 Resource Graph、endpoint catalog、Host desired、Edge actual、
> subscription、动态 lifecycle 与数据 route；不定义具体 sensor 型号、driver ABI、SDK facade
> 或 transport。

## 边界

Core 建立并约束 peer Session；Runtime 决定当前 Edge 有什么、Host 要什么、Edge 实际接受了
什么，以及哪些 Resource 可以在该 Session 中流动；SDK 把 Host Runtime 的结果投影成
Virtual Edge。

```text
Core READY
  -> Edge Runtime CatalogSnapshot
  -> Host Runtime CatalogAccept
  -> ANNOUNCED route ACTIVE
  or
  -> Host desired configuration / delivery request
     -> Edge admission + adapter apply
     -> subscription actual ACTIVE / effective configuration
     -> HOST_SUBSCRIPTION route ACTIVE
  -> State / Stream / Action / Blob / MediaStream delivery
```

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
- route 是 catalog 接受后即可用的 `ANNOUNCED`，还是需要 Host request 与 Edge
  actual 的 `HOST_SUBSCRIPTION`；subscription v0 只用于 Edge-owned Stream；
- actual/availability facet 的 identity，以及较大 capability/mode table 或 calibration
  所引用的 Blob；动态值由 actual State 表达；
- snapshot 的 entry count、容量边界和完整性信息。

Edge 私有配置决定逻辑 endpoint、允许的 mode/parameter envelope、driver owner 与本地
policy；adapter probe 决定当前是否真实存在、可用和能否 apply。这两者是 Edge 实现
输入，不是第二套公开协议；它们只能通过 catalog 和 actual State 影响 Host 可见结果。
Host 不依赖配置文件路径、driver 名、bus address 或固定 sensor 数量。

Sensor 不是第五种 Resource。一个 endpoint bundle 可以包含：

| facet | Resource | owner / 含义 |
|---|---|---|
| capability | State 或 Blob | Edge；支持的 mode、range、format、rate、clock、limits |
| desired | State | Host；是否启用、采集参数与 delivery policy |
| actual | State | Edge；availability、effective 参数、health、rejection/failure |
| data | Stream / MediaStream | Edge；sample、event、audio、camera 或其他连续数据 |
| operation | Action | 确有副作用的 calibrate、reset、self-test 等离散操作 |
| immutable | Blob | calibration、mode table、model、有限 capture 等不可变内容 |

一个 endpoint 可以有零个或多个 data/operation/Blob Resource。配对关系由 catalog facet 和
versioned schema 明确，不靠名称或 Host 硬编码推断。

## Identity 与 generation

- `EndpointId` 表示 Profile 内稳定的逻辑槽，不是总线地址或硬件序列号；
- `endpoint_incarnation` 在设备替换、adapter 重启或 continuity 无法证明时更新；
- `catalog_generation` 只在 endpoint/resource/schema/mode envelope 改变时更新；瞬时 health
  或 unavailable 优先由 actual State 表达，不为每次抖动重建 catalog；
- generation 一旦形成可判定的 offer（包括被拒绝的 offer）便不得用不同内容复用；
- desired request/subscription identity 只在当前 Session 与 catalog generation 内有效；
- sample 仍使用 producer incarnation、stream epoch、sequence、source clock epoch/time；
- SessionId、catalog generation、endpoint incarnation、State version 与 stream epoch 不互相
  替代。

## 协商

1. Core 未 `READY` 时，Runtime control/data 均不得发送或接受；
2. Edge 发布完整 catalog；Host 在容量内验证 Profile、schema、identity、facet、owner、limits
   和 endpoint bundle 一致性；
3. Host 只对完整 snapshot 返回 `ACCEPT` 或带明确原因的 `REJECT`。部分 entry 不能可见；
4. catalog 被双方接受后，Host Runtime 才可提交 endpoint desired State 和 delivery request；
5. Edge Runtime 按 capability、permission、budget、当前 availability 和本地安全 policy 做
   admission，再调用唯一 truth-owner adapter；
6. Edge 发布 endpoint actual State：`ABSENT / PRESENT / STARTING / READY / DEGRADED / FAILED /
   DISABLED / REJECTED`、effective 参数与 reason。禁止静默换 format、rate 或 range；
7. `HOST_SUBSCRIPTION` 只有当前 enabled request 已获得 subscription actual `ACTIVE`，
   且 route 仍绑定当前 `(SessionId, catalog_generation, EndpointId,
   endpoint_incarnation, ResourceId, kind/schema, request_id, route_epoch)` 时，
   该 remote data route 才开放；
8. Host Runtime 校验 identity、epoch、freshness、gap 与 delivery policy 后再交给 SDK。

Host desired 的本地提交、Core delivery acceptance、Edge request acceptance、adapter apply、
actual `READY` 与首个有效 sample 是不同进展，不能合并为一个成功布尔值。

## 采集与 remote delivery

采集和向 Host 发送是两个决定。Edge 可以因本地 safety、共享消费者或 bounded history 在无
remote subscription 时继续采集；remote delivery 默认关闭，只有有效 request/actual route
才能发送。Profile 明确是否支持 always-on push、history cursor、batch、deadline、loss、
offline retention 与自动恢复。

Adapter 在最早可靠边界产生 source identity/time，向 Edge Runtime 提交 platform-independent
sample；Runtime 再绑定 Resource route、执行 admission/rate/budget，并交给 Core。Core 不解释
sensor payload。Host Runtime 不能用 receive time 冒充 source time，也不能用最近一次任意值
填补缺失 sample。

## 动态插拔与更新

- Profile 已知的可插拔逻辑槽可以保留在 catalog 中，由 actual State 表达 `ABSENT`；
- 真正新增/删除 Resource、schema 或 mode envelope 时，Edge 发布新的完整 generation；
- v0 catalog 改变时所有旧 remote route 失效，Host 必须接受新 snapshot 并重新 request；
- unplug 时 Edge 立即关闭 data route，发布 actual unavailable/stream end（仍可发送时），拒绝
  新 Action，并使 last-known SDK view 带 stale/freshness；
- replug 或 continuity 无法证明时使用新的 endpoint incarnation 与 stream epoch；不得复活旧
  handle、旧 subscription 或旧 sample；
- catalog build/reject 不改变当前 active generation；只有完整新 snapshot 被接受后才原子切换。

## Session 与恢复

每个新 Core Session 都重新交换并接受 catalog；地址相同不表示可以复用 route。State owner 可
重新发布仍有效 truth，Stream 是否延续 epoch 由 producer continuity 决定，Action 不自动重放，
Blob 是否复用由 content identity/Profile 决定。v0 不 resume subscription。

若 Host 首次订阅时 Edge 的现存 stream sequence 大于 1，`missing_before` 是合法 observation，
必须显式暴露；不能为新 Host 重置 Edge sequence或静默吞掉 gap。需要 start cursor 时由 Profile
明确冻结。

## 失败与有界性

Runtime 至少区分：

- `CORE_NOT_READY / STALE_SESSION`；
- `CATALOG_CAPACITY / MALFORMED_CATALOG / STALE_CATALOG`；
- `UNKNOWN_ENDPOINT / STALE_ENDPOINT_INCARNATION / UNKNOWN_RESOURCE`；
- `SCHEMA_MISMATCH / UNSUPPORTED_PARAMETER / QOS_UNSUPPORTED`；
- `STALE_REQUEST / REQUEST_CONFLICT / ROUTE_NOT_ACTIVE`；
- `PERMISSION_DENIED / ADMISSION_DENIED / UNAVAILABLE / APPLY_FAILED`；
- `PAYLOAD_CAPACITY / PROTOCOL_ERROR`。

任一失败不得部分激活 catalog、request 或 route。catalog staging、request、route、queue、history
和 pending actual 都有协商或 Profile 固定上限，并暴露 capacity/drop/failure。

## 层级责任

| 层 | 责任 |
|---|---|
| Core | peer/Session identity、handshake、limits、bounded delivery/failure；不理解 sensor |
| Edge Runtime | catalog、endpoint lifecycle、admission、desired→actual、route 与 telemetry |
| Host Runtime | catalog mirror、request 聚合、desired 发布、actual/gap/freshness 处理 |
| Profile | 可选 endpoint、schema/modes、QoS、capacity、hotplug、cursor、failure/recovery |
| Adapter | 私有映射、动态检测、真实采样/控制、timestamp 与 native handle |
| Host SDK | typed discovery/configure/subscribe、Virtual Edge、stale handle；不暴露 transport |
| App | 表达需求并消费 actual/data；不推断硬件拓扑 |

## 验收条件

正式 Runtime 实现至少证明：

- Core `READY` 前零 Runtime mutation；新 Session 必须重新协商；
- catalog snapshot 原子接受，重复幂等，stale/conflict/capacity 不部分提交；
- Host 可以选择 endpoint 子集和参数，unsupported 参数得到明确 reject；
- desired delivery acceptance 不冒充 Edge actual，未 `ACTIVE` route 不发送/接收数据；
- unplug/update 关闭旧 route，replug 使用新 endpoint incarnation/stream epoch；
- wrong Session/catalog/endpoint/resource/kind/schema 的 control/data 全部拒绝；
- queue、request、route 与 payload 过载仍有界且可观察；
- simulated Edge、真实 Edge 和 Host SDK 使用同一 catalog/desired/actual 语义。
