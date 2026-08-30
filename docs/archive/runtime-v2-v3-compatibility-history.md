# Runtime v2/v3 compatibility 开发历史

> 状态：`HISTORICAL / NON-AUTHORITATIVE`。本页保留 MRT0 v2/v3 的详细
> wire/FSM 和逐 commit 证据；它们不是当前正式 Composition Profile 路径。
> 当前说明见 [Runtime Core compatibility](../runtime-core-v0.md)。

> 状态：`INTERNAL COMPATIBILITY CONTRACT`。本页冻结 portable Runtime 中
> MRT0 v2/v3 fixture 的行为；它们不是当前正式 Composition Profile 的连接路径。
> 当前产品语义和 MRT0 v4 路径由 [Runtime 契约](../runtime.md) 与
> [Composition Profile](../../profiles/composition/README.md) 拥有。本页不声明 transport、
> K230D target 或产品 Profile evidence。

## 边界

```text
meiso_core_resources(State / Stream / Blob types and primitives)
                    +
meiso_core_session(READY Session + opaque Runtime gate)
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
- v3 Blob 跨端对象仍由 Core `MeisoBlobStore` 拥有 staging/verification/atomic publication；Runtime
  只负责 Session-scoped carriage、bounded reassembly 和同步 publish bridge，不把 publication
  冒充 Profile/SDK visibility；
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

当前 source v0 对 State/Stream route 的接受不依赖 wire version；Blob route 只在严格 v3 handle
中接受。枚举中的其他产品 facet 不代表已有 carriage：

| facet | Core kind / owner | 当前作用 |
|---|---|---|
| `DESIRED` | State / Host | Host 发布 enabled 与 Profile 参数 |
| `ACTUAL` | State / Edge | Edge 发布 effective state 与 production gate |
| `DATA` | Stream / Edge | Edge 发送当前 production epoch 的 sample |
| `IMMUTABLE`（v3 only） | Blob / Host | Host→Edge 传递有限 immutable content；不受 production gate 控制 |

Capability 直接内联在 endpoint，`CAPABILITY` route 尚不实现。`OPERATION` 与非 Stream 的
`DATA` 均返回 `UNSUPPORTED_KIND`；v2 对 `IMMUTABLE/Blob` 也返回该错误。v3 只接受
`owner=Host, kind=Blob, facet=IMMUTABLE`，不因此开放 Edge→Host Blob 或任意双向 Stream。
每个 endpoint 最多一个 desired 和一个 actual；存在任意 data route 时两者必须恰好各一个。
EndpointId 与 ResourceId 分别唯一，route 必须引用同 incarnation 的已声明 endpoint。

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

v3 另以 `MeisoRuntimeV3Config.max_blob_transfer_bytes` 限制单个 Blob transfer。该限制在 Host
用于 offer/send admission，在 Edge 同时决定完整 reassembly scratch；它不是 BlobStore slot、
Profile visibility 或 GPU resident budget。任一 accepted catalog replacement 或 Session
discontinuity 都取消 Runtime transfer 并清 Edge scratch，已由 Core 发布的 Blob cache 不因此删除。
新 Session/control scope 必须重新建立 SceneStore/SDK visibility；仅 catalog replacement 时外部
visibility 是否重建由后续 Profile/SceneStore integration 定义，Runtime 不持有它，R1 也未验证该策略。

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

## MRT0 v3 Blob carriage

v3 只增加 Host-owned `BLOB/IMMUTABLE` 的 Host→Edge carriage；同一 handle 的多个合法 Blob
route 共享一个全局 transfer slot。它没有 per-route subscription、并行 transfer、resume window
或 per-chunk ACK。Host 成功排入 `BEGIN` 时由 Runtime 生成非零、Session-scoped、严格递增且
不复用的 `transfer_id`；`TX_BUSY` 不写 output，也不消耗 identity/high-water/offset。

确定性 FSM 为：

```text
Host BEGIN(ContentId,total) -> Edge RESULT(BEGIN,READY,next=0)
  -> Host CHUNK(offset,bytes)*             // 无逐块 ACK
  -> Host COMMIT -> Edge length/SHA check
  -> synchronous Runtime→Core publish bridge
  -> Edge RESULT(COMMIT,PUBLISHED|DUPLICATE|terminal failure)

Host ABORT 可在等待 READY 或传输中发生
  -> Edge RESULT(ABORT,ABORTED)
```

- zero-length Blob 由 `BEGIN -> READY -> COMMIT` 表示，不发送 CHUNK；
- non-final CHUNK 必须恰好 4096 bytes，short CHUNK 只能精确结束于 `total_size`；offset 必须连续，
  gap/overlap/out-of-order 产生 terminal `DISCONTINUITY`；
- `BEGIN+READY` 是唯一非终态 RESULT。BEGIN rejection 的 `next_offset=0`；PUBLISHED/DUPLICATE 的
  `next_offset=total_size`；ABORTED/失败返回 receiver 已接受的 prefix；
- Host 对 terminal prefix 只接受 `next_offset <= bytes_sent`，且它必须等于 total 或位于 4096-byte
  chunk boundary。CHUNK failure 可以在 Host 已继续发送后异步到达，但至少已有一个 queued byte；
- Host 在 READY 前发 ABORT 合法；交叉到达的 late READY 只形成 observation，不能重新打开 transfer；
- impossible operation/result pair、错误 route/id/total、reserved、phase 或 prefix 零副作用拒绝，
  不得退役另一条/当前 transfer。

Edge 收到完整 COMMIT 后先在 Runtime scratch 上核对 length 与 SHA-256 ContentId，再由
`meiso_runtime_publish_blob_receive` 同步执行：

```text
Runtime reassembly scratch
  -> Core BlobStore stage_begin
  -> bounded copy
  -> Core commit_verified(
       Runtime SHA-256 identity && caller Profile/schema verifier,
       same immutable Core verification snapshot)
  -> Core PUBLISHED/DUPLICATE 才映射为 wire success
```

bridge 只接受 caller 明确证明与 exact active ResourceDescriptor 独占绑定的 BlobStore；opaque Core
store 本身不能让 Runtime 推断 route。它不覆盖或 abort 外部已有 staging candidate；只清理由本次
bridge 成功建立、但后续 copy/commit 失败而仍可能保留的 candidate。Runtime、BlobStore、output
arena 必须不重叠；Profile verifier 重入同一 Runtime 被拒绝。Core publication 只证明 content 已完整
验证并进入 store；SceneStore/SDK 必须另外执行当前 owner/Session scope 的 `mark_visible` 或等价
授权，cache hit/duplicate 不恢复 desired、world 或 visibility。

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
| `v3_memory_required / v3_init` | 建立严格 v3 handle；v2/v3 frame 不混用、不 fallback |
| `send_blob_begin/chunk/commit/abort` | Host 驱动单一 Session-scoped Blob transfer |
| `publish_blob_receive / send_blob_result` | Edge 把完整 scratch 同步提交 Core BlobStore，并回送 READY/terminal truth |
| `receive / commit_state_receive` | 解码 Runtime event；把 Core StateReplica 结果提交给 gate |
| `snapshot / production_snapshot / blob_snapshot` | Session/catalog、production gate 与当前 Blob transfer/status truth |

Arena 固定包含两套 endpoint/route/capability/actual-Profile bank、一个
`max_endpoint_history` identity-pair ledger、一个
`8 + max_desired_profile_bytes_per_endpoint` pending desired payload buffer、一个 pending actual
Profile buffer，以及一个
`max_runtime_payload_bytes` TX buffer；v3 Edge arena 另含完整 `max_blob_transfer_bytes` reassembly
scratch，Host arena不含该scratch。实现不调用 heap、thread、clock、socket、filesystem 或 driver。
一个 owner event loop 串行调用同一 Runtime/Session pair；无内部 locking。
Runtime arena、完整 Session arena 与 out object 不得重叠。

Catalog capability 与 production effective Profile view 借用 Runtime arena，到该 handle 下一次
non-const 调用前有效。State/Stream event 中的 byte view 借用 Session RX message，到相关
Session 或 Runtime 下一次 non-const 调用前有效；需跨调用则复制。v3 CHUNK event 借用 Session
message，COMMIT event 的完整 Blob 借用 Edge Runtime scratch；二者同样不得跨下一次 mutation。

## Runtime wire v2/v3

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

MRT0 v3 使用同一个 12-byte header，但 `wire_version=3`，严格 v2/v3 handle 拒绝另一版本；没有
协商后 fallback。v3 的 type 1..6 保留上表 body layout，同时增加以下五类 frame。表内 offset
相对 12-byte Runtime header 后的 body；所有 integer 为 big-endian，reserved 必须为零：

| type | value / direction | exact full frame | body layout |
|---|---|---:|---|
| `BLOB_BEGIN` | 7 / Host→Edge | 120 | `0 RouteKey:56, 56 transfer_id:u64, 64 ContentId:36, 100 total_size:u64` |
| `BLOB_CHUNK` | 8 / Host→Edge | `84 + N`, `1 <= N <= 4096` | `0 RouteKey:56, 56 transfer_id:u64, 64 offset:u64, 72 bytes:N` |
| `BLOB_COMMIT` | 9 / Host→Edge | 120 | 与 BEGIN 相同；完整 identity/total 必须回显 |
| `BLOB_ABORT` | 10 / Host→Edge | 84 | `0 RouteKey:56, 56 transfer_id:u64, 64 reason:u8, 65 reserved:7` |
| `BLOB_RESULT` | 11 / Edge→Host | 96 | `0 RouteKey:56, 56 transfer_id:u64, 64 operation:u8, 65 result:u8, 66 reserved:2, 68 next_offset:u64, 76 total_size:u64` |

ContentId 的 exact 36 bytes 为 `algorithm:u16, digest_size:u8, reserved:u8, digest:32`；当前只接受
canonical SHA-256。CHUNK 不重复 ContentId/total，全部由已绑定 transfer ref 确定；最大 Runtime
frame 为 4180 bytes。RESULT value 为：`READY=1, PUBLISHED=2, DUPLICATE=3, ABORTED=4,
BUSY=5, CONFLICT=6, INVALID_CONTENT_ID=7, LENGTH_MISMATCH=8, INTEGRITY_FAILED=9,
CAPACITY=10, DISCONTINUITY=11, RECEIVER_FAILURE=12`。唯一当前 ABORT reason 是
`CALLER_REQUEST=1`。

合法 operation/result pair 只有：`BEGIN+READY`或BEGIN+任一terminal rejection；
`CHUNK+DISCONTINUITY|RECEIVER_FAILURE`；`COMMIT+PUBLISHED|DUPLICATE`或COMMIT+任一terminal
rejection；`ABORT+ABORTED`。其中terminal rejection是value 5..12。其他组合即使字段长度合法也
是phase conflict，不能改变当前transfer。

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
- `meiso_runtime_blob_wire_test`：Profile v1→MRT0 v2、Profile v2→MRT0 v3；五类 v3 Blob frame
  独立 literal bytes；0/4096/4097/262144-byte boundaries；BEGIN/READY、连续 CHUNK、COMMIT/ABORT
  phase；async terminal prefix、replay、alias/TX_BUSY、multi-route global slot、Session/catalog
  invalidation；Runtime SHA 与 Profile verifier 同一 Core snapshot、external staging ownership、
  forced-abort cleanup、duplicate publication 后显式 SceneStore visibility，以及 v3 State/Stream/Blob
  coexistence。

public commit `a392850` 的后续R2b4 integration不改变本页API/wire，并在真实loopback TCP、双方R2a
scheduler上把exact 262144-byte canonical `MSH0`分成64个4096-byte `CHUNK`。Host只接收
`BEGIN+READY`和`COMMIT+PUBLISHED`，Edge没有发送逐块ACK；第32块后Edge反向发送独立Wi-Fi与
RenderResult Stream。262145-byte `BEGIN`在Host Runtime admission处以`PAYLOAD_CAPACITY`拒绝，未
排入Session outbound，也未建立transfer或Core stage。Actions run `32322475279`四个job全部success。
该结果补充AF_INET max-single-Blob evidence，不把Core BlobStore slot总容量、K230 memory或吞吐性能
纳入Runtime合同。

commit `a8a777a` 的 fresh Debug、strict 与 ASan+UBSan build 均 CTest 23/23；clang analyzer 对
Runtime、composition 与新 test 为零 finding，C/C++ strict headers/source 通过。macOS arm64、
composition Profile v2 配置下，Host Runtime arena 为 14,904 bytes，Edge Runtime arena 为
277,048 bytes（含 262,144-byte reassembly scratch），均 alignment 8。这是 caller-owned Host
process CPU-visible heap evidence，不是跨 ABI 常量或 K230 memory budget。

这些结果只支持 direct Runtime 的 portable Host semantic/wire evidence，不证明 transport、并发、
fuzz、性能、断线恢复或 target 行为。public commit `5d92ff6` 另实现 caller-owned
`meiso_runtime_scheduler`；它在 Runtime--SDK 集成层绑定 exact Runtime/Session pair，不修改本页的
Core、wire 或 direct Runtime API。Source v0 本身明确不内建：

- Action 与 MediaStream Runtime carriage；capability State/Blob route；Edge→Host Blob 或并行/resumable
  Blob transfer；
- 产品 endpoint direction/QoS/permission schema、typed dispatcher、catalog fragmentation 或
  duplicate response cache；
- control/data/Blob scheduler；R2a 的独立 scheduler 才为 Composition Profile v2 证明
  `4 + N_ready_routes` successful-slot wait bound 与 no-starvation；
- Profile-specific admission、private config/probe/hotplug adapter、Host orchestration、SDK /
  Virtual Edge；
- direct-IP bootstrap、socket connect/listen、认证/加密、安全 transport；
- K230D/Linux service、真实 sensor adapter 或任何 Runtime `TARGET_VERIFIED / BOARD_VERIFIED`
  evidence。

现有 four-kind lab、POSIX loopback transport、Resource Core 板测或独立 Profile codec 不能提升
上述 Runtime evidence 等级。
