# Runtime negotiation core v0 实现契约

> 状态：`IMPLEMENTED / HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED`。本页冻结
> portable `meiso_runtime` source v0 与 Runtime wire v2 的当前行为；产品语义仍由
> [Runtime 契约](runtime.md)拥有。本页不声明 transport、K230D target 或产品 Profile evidence。

## 边界

```text
meiso_resource_core(State / Stream types and primitives)
                    +
meiso_session_core(READY Session + opaque Runtime gate)
                    ↓
             meiso_runtime
                    ↓
       Profile / adapter / Host SDK（外部）
```

一个 Runtime handle 永久绑定初始化时已经 `READY` 的 `MeisoSession`、SessionId、role 与
negotiated limits。每次调用重新核对这些值；Session close/loss 或绑定变化使 handle 失效。
所有 wire payload 只经 `meiso_session_submit_runtime` 和 `meiso_session_receive` 进出。

Runtime v0 不复制 Core Resource 状态机：

- desired/actual payload 由 Profile 填充，但跨端对象是 `MeisoStateOffer`；调用方仍以
  `MeisoStatePublisher / MeisoStateReplica` 负责 owner、version、duplicate、stale、conflict
  与 delivery acceptance；
- data 跨端对象是 `MeisoStreamSample`；调用方仍以 `MeisoStream` 负责 producer/stream epoch、
  sequence、gap 与 queue；
- Runtime 只验证当前 Session/catalog/endpoint/Resource/facet，关联 desired reference、actual
  production state 与 data production epoch；没有 subscription、独立 desired/actual message
  或第二套 Resource truth。

## Catalog v0

Endpoint record 为：

```text
EndpointId + endpoint_incarnation + capability_schema + inline capability bytes
```

Route record 为：

```text
EndpointId/incarnation + MeisoResourceDescriptor + facet
```

当前 source v0 只接受下列 route；枚举中的其他产品 facet 不代表已有 carriage：

| facet | Core kind / owner | 当前作用 |
|---|---|---|
| `DESIRED` | State / Host | Host 发布 enabled 与 Profile 参数 |
| `ACTUAL` | State / Edge | Edge 发布 effective state 与 production gate |
| `DATA` | Stream / Edge | Edge 发送当前 production epoch 的 sample |

Capability 直接内联在 endpoint，`CAPABILITY` route 尚不实现。`OPERATION`、`IMMUTABLE` 以及
非 Stream 的 `DATA` 均返回 `UNSUPPORTED_KIND`。每个 endpoint 最多一个 desired 和一个
actual；存在任意 data route 时两者必须恰好各一个。EndpointId 与 ResourceId 分别唯一，route
必须引用同 incarnation 的已声明 endpoint。

Catalog 是一个完整、单帧 snapshot，使用双 bank staging：

```text
ACTIVE(g0) + STAGED(g1)
  -> ACCEPT queued: atomically ACTIVE(g1), clear desired/actual/data gate
  -> REJECT queued: retain ACTIVE(g0)
```

Generation 非零且在一个 handle 内严格递增；成功进入 Session TX 的 offer（包括随后被拒绝者）
推进 high-water，`TX_BUSY` 不消耗 generation。Host 对 local capacity 或可判定的 unsupported/
conflict 等错误保留 reject decision identity，但不暴露部分 staging catalog；malformed wire 不
形成可接受 snapshot。Runtime 另有不驱逐的 bounded identity ledger；key 是
`(EndpointId, opaque incarnation)`，incarnation 只有 non-zero/equality 语义，没有数值顺序：

- catalog 暂时缺少某 EndpointId 时，其 pair 仍为 current；同一 current pair 在以后 generation
  再出现时继承 `production_epoch_high_water`；
- 从未见过的任意非零 incarnation 都可成为新 current pair，并永久退役同 EndpointId 的旧
  current pair；已退役 pair 再出现返回 `STALE_ENDPOINT`；
- 每个首次接受的 pair（包括后来退役者）永久占一个 `max_endpoint_history` slot；空间不足返回
  `CAPACITY`。新 pair 的 epoch high-water 从零开始。

切换 catalog 始终清空 desired、actual 和 active gate。`MeisoRuntimeConfig` 分别限制
`max_endpoints`、全 handle retained pair 数 `max_endpoint_history`、route、总 capability bytes、
每 endpoint desired/actual Profile suffix；其中 history 至少覆盖 `max_endpoints`。Session limits
另限制完整 frame，且 `max_endpoints + max_routes <= max_registry_entries`。

## State payload 与 production gate

Runtime 提供 canonical payload codec，整数使用 big-endian，Profile suffix 对 Runtime 不透明。

| payload | 固定前缀 | 字段 |
|---|---:|---|
| desired State | 8 bytes | `enabled:u8`，7 bytes zero reserved，随后为 Profile desired bytes |
| actual State | 56 bytes | observed/applied desired presence、owner role/incarnation/version；`apply_result`、`availability`、`production_status`、`effective_enabled`；`production_epoch:u64`、`reason:u16`、6 bytes zero reserved；随后为 Profile effective bytes |

Actual codec 强制 canonical 组合：absent reference 的 owner/version 为零；`APPLY_NONE` 不带
reference；pending/rejected/failed apply 至少带 observed desired；`APPLIED` 的 observed 与
applied reference 相同。`RUNNING / DEGRADED / STOPPING` 必须 availability present、effective
enabled、带 applied desired 且 production epoch 非零；`STARTING / STOPPED / FAILED` 不打开
data gate，absent/unavailable 也不得保持 active production。

Core State 与 gate 的集成顺序是实现合同的一部分：

```text
publisher offer
  -> meiso_runtime_send_state_offer
  -> Session RUNTIME frame
  -> meiso_runtime_receive returns MeisoStateOffer
  -> meiso_state_replica_receive
  -> meiso_runtime_commit_state_receive(same event, receive_status)
```

接收 `STATE_OFFER` 只暂存一个 bounded candidate；调用方必须立即把同一 offer 交给对应
`StateReplica`，再回报 Core result。只有 `MEISO_STATE_RECEIVE_NEW` 绑定新的 desired reference
或提交 actual production state；duplicate/stale/conflict/invalid 不改变 gate。任一 intervening
non-const Runtime 调用都会丢弃 pending candidate。Desired 的完整 canonical payload
（8-byte prefix + Profile suffix）受 `max_desired_profile_bytes_per_endpoint` 限制并被完整复制；
Actual 的 Profile suffix 受 `max_actual_profile_bytes_per_endpoint` 约束。两者均在 commit 时
逐字节复核，不能以相同 State identity 替换 desired 参数或 effective Profile 后改变 gate。

Host 发送侧只有在 desired `STATE_OFFER` 成功排入当前 Session TX 后才绑定该 State
owner/version；Edge 接收侧只在 desired 被 `StateReplica` 接受为 `NEW` 后绑定。Actual 发送侧
只有在 offer 成功排入当前 Session TX 后才提交本地 gate；接收侧只有在对应 actual 被
`StateReplica` 接受为 `NEW` 且 commit 通过后才提交 gate。`STATE_ACCEPT` 只承载 Core
delivery acceptance identity，不表示 adapter apply 或 production success。

提交 actual 还必须满足：

- observed desired 等于 Runtime 当前已绑定 desired；applied desired 可以等于当前 desired，
  或等于仍在运行的上一份 applied desired，因此新 desired pending/rejected 时旧生产可继续；
- 同一 active production epoch 只能继续相同 applied desired 与逐字节相同 effective Profile；
- 每次从 inactive 重新进入 active，或发生不连续 reconfigure，epoch 必须大于该 endpoint
  incarnation 的 high-water；`STOPPED` 后复用旧 epoch 被拒绝；
- data gate 只在 actual 为 present、effective enabled、状态为
  `RUNNING / DEGRADED / STOPPING` 且 epoch 非零时开放。

因此 desired disable 本身不关门；旧 actual/epoch 在 Edge 停止 producer 并发布 `STOPPED` 前仍
有效。`send_stream_sample` 和 receive 两侧都要求 DATA/Stream route、Edge producer、当前
production epoch 与合法 `MeisoStreamSample` metadata；Runtime 返回 sample event 后，调用方再
交给 `MeisoStream`。

## Source API 与内存

下表省略 `meiso_runtime_` 前缀：

| API | 责任 |
|---|---|
| `desired_state_* / actual_state_*` | canonical payload size/encode/decode；decoded Profile 借用输入 |
| `alignment_required / memory_required / init` | 在 caller-owned arena 中建立固定容量、Session-bound handle |
| `offer_catalog / accept_catalog / reject_catalog` | 完整 snapshot 与 decision；accept 后原子换 bank |
| `endpoint_* / route_* / find_*` | active/staged catalog 审查与查询 |
| `send_state_offer / send_state_accept` | 传递 Core State offer 与 delivery acceptance metadata |
| `send_stream_sample` | 在已开放 production gate 上传递 Core Stream sample |
| `receive / commit_state_receive` | 解码 Runtime event；把 Core StateReplica 结果提交给 gate |
| `snapshot / production_snapshot` | Session/catalog 状态与每 endpoint actual/effective/gate view |

Arena 固定包含两套 endpoint/route/capability/actual-Profile bank、一个
`max_endpoint_history` identity-pair ledger、一个
`8 + max_desired_profile_bytes_per_endpoint` pending desired payload buffer、一个 pending actual
Profile buffer，以及一个
`max_runtime_payload_bytes` TX buffer；实现不调用 heap、thread、clock、socket、filesystem 或
driver。一个 owner event loop 串行调用同一 Runtime/Session pair；无内部 locking。
Runtime arena、完整 Session arena 与 out object 不得重叠。

Catalog capability 与 production effective Profile view 借用 Runtime arena，到该 handle 下一次
non-const 调用前有效。State/Stream event 中的 byte view 借用 Session RX message，到相关
Session 或 Runtime 下一次 non-const 调用前有效；需跨调用则复制。

## Runtime wire v2

Runtime frame 是 Session `RUNTIME` payload，不重复 SessionId。所有整数为 big-endian，C struct
不得直接复制到 wire。

```text
offset  size  field
0       4     magic = "MRT0"
4       1     wire_version = 2
5       1     type
6       2     reserved = 0
8       4     total Runtime frame bytes
12      ...   body
```

| type | value | fixed body/prefix | direction / body |
|---|---:|---:|---|
| `CATALOG` | 1 | 16 | Edge→Host；generation、endpoint count、route count；每 endpoint 为 48-byte record 后跟 inline capability；每 route 为 72-byte record |
| `CATALOG_ACCEPT` | 2 | 8 | Host→Edge；generation |
| `CATALOG_REJECT` | 3 | 12 | Host→Edge；generation、reason、3-byte reserved |
| `STATE_OFFER` | 4 | 96 | Resource owner→peer；RouteKey、delivery generation、State owner/version、payload length/reserved、State payload |
| `STATE_ACCEPT` | 5 | 88 | State receiver→owner；RouteKey、delivery generation、State owner/version |
| `STREAM_SAMPLE` | 6 | 144 | Edge→Host；RouteKey、production epoch、Core Stream producer/epoch/sequence/source time/deadline、dependency/payload lengths 与 bytes |

共同 `RouteKey` 为 56 bytes：catalog generation、EndpointId、endpoint incarnation、ResourceId、
facet 与 7-byte zero reserved。Endpoint record 为 48 bytes；route record 为 72 bytes，内含完整
`MeisoResourceDescriptor` 的 id/kind/owner/schema/max payload。所有 reserved、length、role、
identity、kind/facet、schema、capacity 和 current binding 均在生成 event 前验证。

## Evidence 与明确边界

当前 Host tests 分别支持以下结论：

- `meiso_runtime_test`：desired/actual canonical codec、v0 catalog subset/bundle conflict、catalog
  activation/generation、StateReplica commit gate、forged desired/actual payload 拒绝、desired
  disable 不直接关门、stopped 关门、epoch high-water/reuse 拒绝、endpoint identity pair 跨
  catalog 缺席/替换/退役与 history capacity、catalog replacement 与 Core terminal invalidation；
- `meiso_runtime_session_test`：两个真实 Session arena 上，`StatePublisher -> Runtime ->
  StateReplica -> acceptance` 和 `Runtime -> Stream` primitive 的完整调用链；旧 actual 在 disable
  cut 中继续、停止后关门、新配置使用新 production/stream epoch，以及新 Session 必须重建
  catalog；
- `meiso_runtime_wire_test`：六类 v2 frame 的独立 literal golden bytes、每个截断点、header 与
  body reserved 字段、big-endian decode，以及 malformed 路径不改变公开 Runtime snapshot。

这些结果只支持当前 portable Host semantic/wire evidence，不证明 transport、并发、fuzz、性能、
断线恢复或 target 行为。Source v0 明确尚未实现：

- Action、Blob 与 MediaStream Runtime carriage；capability State/Blob route；
- 产品 endpoint direction/QoS/permission schema、typed dispatcher、catalog fragmentation 或
  duplicate response cache；
- Profile-specific admission、private config/probe/hotplug adapter、Host orchestration、SDK /
  Virtual Edge；
- direct-IP bootstrap、socket connect/listen、认证/加密、安全 transport；
- K230D/Linux service、真实 sensor adapter 或任何 Runtime `TARGET_VERIFIED / BOARD_VERIFIED`
  evidence。

现有 four-kind lab、POSIX loopback transport、Resource Core 板测或独立 Profile codec 不能提升
上述 Runtime evidence 等级。
