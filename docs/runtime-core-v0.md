# Runtime negotiation core v0

> 状态：`IMPLEMENTED / HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED`。本页冻结 portable `meiso_runtime` 的
> source 行为与 wire v1；它是 [Runtime 契约](runtime.md) 的有界协商子集，不是 SDK、
> platform adapter 或完整产品 Runtime。

## 层级与依赖

```text
meiso_core
  = Resource primitives + Session Core
        ↓
meiso_runtime
        ↓
platform Runtime / Host SDK（尚未实现）
```

Runtime 永久绑定一个已经 `READY` 的 `MeisoSession`，复制其 SessionId、role 与 negotiated
limits。每次操作重新核对同一 Session 仍为 `READY`；close、loss、identity/limit 变化立即使
Runtime handle、catalog、subscription 与 route 失效。Runtime payload 只能经
`meiso_session_submit_runtime` 与 `meiso_session_receive` 进入或离开，不绕过 Core。

Core 不理解 endpoint、sensor 或参数；Runtime 不重新实现 handshake 和四类 Resource 语义。
Edge 调用方负责把 `Profile ∩ 私有配置 ∩ adapter probe ∩ policy/budget` 转成 catalog。

## v0 数据模型

Endpoint：

```text
EndpointId + endpoint_incarnation + capability_schema + opaque capability bytes
```

Route：

```text
EndpointId/incarnation + ResourceDescriptor + delivery
  + optional selection_schema/max_selection_bytes
```

`ANNOUNCED` route 随 accepted catalog 激活；`HOST_SUBSCRIPTION` v0 只支持 Edge-owned Stream，
还需 Host desired 与 Edge actual `ACTIVE`。Resource delivery 的完整 gate 为：

```text
SessionId + catalog_generation + EndpointId/incarnation
  + ResourceId + kind/schema + request_id + route_epoch
```

订阅以严格递增、非零 `request_id` 区分每次 desired。`enabled=false` 是显式关闭；Edge actual
区分 `APPLYING / ACTIVE / REJECTED / UNAVAILABLE / DISABLED`，并携带 reason、effective
parameters。只有 `ACTIVE` 使用非零 `route_epoch`；旧 request 的迟到 actual/data 不能打开
新 route。

## Catalog 状态

Edge 每次发送一个完整 catalog；Host 在第二个固定 bank 中 staging，成功后才能接受：

```text
ACTIVE(g0) + OFFER/WAIT_DECISION(g1)
  -> ACCEPT: atomically ACTIVE(g1), clear every old subscription
  -> REJECT: keep ACTIVE(g0)
```

- generation 在同一 Runtime handle 内非零且单调；被拒绝的 generation 也进入 high-water，
  不得用不同内容复用；
- 本地 Session `TX_BUSY` 时 offer 没有进入 Core TX，generation 不被消耗，可原值重试；
- Host 容量不足时不建立完整 staging bank，但保留 reject-only decision identity，可发送
  `CATALOG_REJECT_CAPACITY`，不会把 Edge 卡在 pending；
- active catalog 在新 offer staging/reject 期间保持不变；accept 原子替换；
- v0 使用单一完整 frame，无 catalog fragmentation；
- `endpoint_count + route_count <= negotiated max_registry_entries`；endpoint、route、capability、
  per-route parameter 和 frame 都受 caller config/Session limit 双重约束。

Host 接受后立即切换本地 generation；Edge 收到同一有序 `ACCEPT` 后切换。切换窗口内到达的
旧 generation data 被明确拒绝；v0 不承诺无损跨-generation cut。

## Source API 与内存

下表省略 `meiso_runtime_` 前缀：

| API | 责任 |
|---|---|
| `alignment_required / memory_required / init` | 在 caller-owned arena 中建立固定容量 handle |
| `offer / accept / reject_catalog` | 完整 snapshot 与 decision |
| `staged_* / endpoint_* / route_* / find_*` | 接受前审查及 active catalog 查询 |
| `request_subscription` | Host desired；strict request id；参数按 selection schema 解释 |
| `report_subscription` | Edge actual/effective；打开或关闭 route epoch |
| `send_resource / receive` | 当前 route 上的 opaque typed payload |
| `snapshot / subscription_snapshot` | Session、catalog high-water/pending/active 与 desired/actual |

没有 heap、thread、clock、socket、filesystem 或 driver 依赖。同一 Runtime 及其 bound Session
由一个 owner event loop 串行调用；实现不提供 locking。Runtime arena、bound Session arena
与所有 out 参数必须互不重叠；opaque Session arena 的完整范围由 caller 保证。Capability、desired、actual 查询 view
借用 Runtime arena，到下一次同 handle non-const 调用前有效。Resource event payload 借用
Session RX arena，到任一相关 handle 下一次 non-const 调用前有效；需跨调用则复制。

## Wire v1

Runtime payload 位于 Session `RUNTIME` frame 内，不重复 SessionId。整数为 big-endian；C struct
不得直接复制到 wire。

```text
offset  size  field
0       4     "MRT0"
4       1     wire_version = 1
5       1     type
6       2     reserved = 0
8       4     total Runtime payload bytes
12      ...   body
```

| type | value | fixed body/prefix | direction / body |
|---|---:|---:|---|
| `CATALOG` | 1 | 16 | Edge→Host；generation, endpoint_count, route_count；48-byte endpoint record + inline capability；96-byte route record |
| `CATALOG_ACCEPT` | 2 | 8 | Host→Edge；generation |
| `CATALOG_REJECT` | 3 | 12 | Host→Edge；generation, reason, reserved |
| `SUBSCRIPTION_DESIRED` | 4 | 64 | Host→Edge；generation, endpoint, resource, request, enabled, parameter length + bytes |
| `SUBSCRIPTION_ACTUAL` | 5 | 72 | Edge→Host；generation, endpoint, resource, request, status/reason, route epoch, effective length + bytes |
| `RESOURCE` | 6 | 92 | descriptor owner→peer；generation, endpoint, resource, kind/schema, request, route epoch, payload length + bytes |

未知 type、错误 magic/version/reserved/length、trailing bytes、stale generation、错误 endpoint/
resource/kind/schema/request/epoch 或容量超限均不进入 Resource primitive。

可明确映射为 capacity/unsupported 的 catalog 保留 decision token，Host 必须回相应
`CATALOG_REJECT`。`MALFORMED / CONFLICT / UNKNOWN_ENDPOINT / STALE_ENDPOINT` 等结构错误
不产生 decision token；调用方必须以 Core `CLOSE(PROTOCOL_ERROR)` 终止该 Session，不能等待
一个不存在的 catalog decision。

## 已验证与边界

`meiso_runtime_test` 覆盖 READY/arena、catalog atomic accept/reject/high-water、不对称容量 reject、
TX-busy retry、desired≠actual、subscription/announced route gate、identity/schema/request/epoch/
payload failure 与 Core close invalidation。

`meiso_runtime_session_test` 使用真实 Session frame 验证 Session→Runtime→Stream primitive 链、
stale catalog、旧 Session message 和新 Session 重新协商。一个 test-only Profile payload 把
producer/epoch/sequence/source time 与 sample bytes 经 Runtime route 传输，Host 解码后进入
`MeisoStream`；这证明 route gate 与 primitive integration，不定义通用 typed dispatcher 或
产品 Stream schema codec。

`meiso_runtime_wire_test` 以独立 literal golden bytes 冻结六类 Runtime frame，覆盖每个截断点、
trailing byte、header/body reserved 字段与 big-endian decode；malformed-wire 拒绝路径保持全部
公开 Runtime 状态不变。这支持本机 `HOST_WIRE_VERIFIED`，不等于 transport、target 或产品
Profile 验证。

本实现尚不包含：

- endpoint facet/direction/QoS/permission 的产品 schema；
- adapter-driven probe、hotplug supervisor、actual State bundle 与 Profile codec；
- catalog fragmentation、duplicate replay/idempotent response cache；
- State/Action/Blob 的 subscription mode、MediaStream data plane；
- SDK/Virtual Edge、direct-IP bootstrap、安全 transport、target service 或 K230D Runtime evidence。

这些缺口不能由 fixed four-kind lab、Dashboard、Session loopback 或板上已有 driver 代替。
