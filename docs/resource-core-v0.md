# Resource semantic core v0 实现设计

> 实现状态：`IMPLEMENTED`
> 验证状态：`HOST_VERIFIED`
> target build：`HOST_VERIFIED`
> K230D execution：`BOARD_VERIFIED`（仅 Resource semantic core v0）
> 更新：2026-08-13
> 兼容性：本工作分支上的 C11 source API；不冻结二进制 ABI，也不兼容旧实验 API。

本文把[Resource 契约](resource.md)映射为第一份可执行的 portable primitive 与 conformance
package。产品语义仍由 `system.md / resource.md / link.md / composition.md` 定义；本文拥有
模块边界、内存模型、v0 API 选择和测试映射，不能反向修改产品含义。

## 完成定义与非目标

本工作单元的完成边界是 **Resource semantic core**，包括四个相互独立的
`State / Stream / Action / Blob` 状态机、共同值类型、descriptor 校验、固定容量 catalog
和纯进程内 conformance fixture。

它不是完整的 Meiso Core。以下内容明确留在后续工作单元：

- Virtual Edge / `EdgeHandle` 与 application-facing API；
- LinkSession、handshake、wire、serialization、socket、重传和 clock estimator；
- Composition plan/activation 实现；
- OS、线程、锁、timer、持久化、allocator、driver、codec、硬件 buffer 和 system image；
- K230D/CanMV、Mac、Orin 或任何 sensor/endpoint 特例。

Core 不读取系统时钟，不分配 heap，不启动线程，不访问文件或网络。调用者提供所有内存、
当前 monotonic 值与外部 scope 变化。一个纯 C11 target 只要具备 `stdint.h / stddef.h /
stdbool.h / string.h` 即可编译 primitive。

## 模块图

```text
meiso_resource.h + meiso_resource.c
    value types / views / descriptor validation
        ├── meiso_state.h + meiso_state.c
        ├── meiso_stream.h + meiso_stream.c
        ├── meiso_action.h + meiso_action.c
        └── meiso_blob.h + meiso_blob.c

meiso_descriptor_set.h + meiso_descriptor_set.c
    fixed immutable registry snapshot; no generic state-machine dispatcher

tests/
    per-kind conformance + cross-kind in-process fixture
```

每种 Resource 使用 typed opaque handle 和自己的 outcome enum。descriptor set 只把 identity 映射到
immutable descriptor；它没有 universal vtable、QoS enum 或通用 transition engine。

## 共同值类型与内存规则

- `ResourceId`、`SchemaId` 与 `ActionId` 在 v0 source API 中各用独立的 128-bit POD 类型，
  防止意外互换；这个宽度不是冻结的 wire/ABI 决定。
- owner scope 是 `(OwnerRole, OwnerIncarnation)`。State version 只在
  `(ResourceId, owner scope)` 内比较；Session 不进入 Resource identity。
- Stream sample key 是 `(producer incarnation, stream epoch, sequence)`；clock epoch 与
  source timestamp 是独立字段。
- Blob `ContentId` 是 `(algorithm, digest length, digest bytes)`；v0 只接受 SHA-256 与精确
  32-byte digest，但不包含 hash 实现。Core 在 commit 时把自己持有的完整 staging view 交给
  调用者注入的 verifier callback；verifier 是明确的信任边界。
- `ByteView {data,size}` 与 `MutableBuffer {data,capacity}` 可表达零长度 payload；非零长度时
  pointer 必须有效。Core 不保存临时 input view，只有完整复制成功才改变状态。
- runtime handle 通过 `*_memory_required(descriptor, config)`、`*_alignment_required()` 和
  `*_init(caller_arena, arena_size, descriptor, config, **out)` 创建。每个 typed arena 同时容纳
  opaque handle 与其 payload/ring/slot storage，避免多个 caller buffer 的重叠歧义；地址必须
  满足返回 alignment，size 必须至少是精确 required size。descriptor/config 在 init 时复制，
  caller 随后可释放原对象；arena 在 handle 最后一次使用前保持存活且不能移动。
- 任何 operation 在 `INVALID / CAPACITY / SCOPE_MISMATCH / CONFLICT` 等失败后保持先前可见
  状态不变。计数器饱和在 `UINT64_MAX`，不得 wrap 后伪装成较小值。

`ResourceDescriptor` 只包含共同 identity、kind、schema、owner 和最大 payload。每种 primitive
另有 typed config：State 的 delivery mode；Stream 的 ordering/history/overflow/deadline
support；Action 的 slot/arguments/result/dedup 限制；Blob 的 slot/content/integrity 限制。
权限、endpoint 格式、native handle 和 transport arrangement 仍留在外层；Core 不通过一个
巨型 descriptor 或 universal QoS enum 假装理解所有 kind。

成功返回的 borrowed `ByteView` 只在同一 handle 下一次成功的 mutating operation 前有效；
失败调用不得使它失效。`*_stats()` 复制饱和计数器 snapshot，不返回内部指针。所有 init 和
operation 都有 typed outcome；`memory_required()` 对 invalid config 或 size overflow 返回 0。
handle 不提供内部同步，调用者必须串行化同一 arena 的 mutation。init 允许 descriptor/config
位于 arena 内（实现先复制再清零），但 output-handle pointer 不能与将被初始化的 arena 重叠。

## v0 source API 形状

下表记录已实现 API 的职责和 lifetime，不冻结 v0 ABI 的 struct layout；State 和 Action
分别使用 publisher/replica 与 caller/executor typed handle。

| primitive | arena/init | 主要 mutation | 只读 observation |
|---|---|---|---|
| State publisher | `state_publisher_memory_required(desc,cfg)`；`state_publisher_init(...)` | `publish(owner,version,bytes)`；`begin_delivery(generation)`；`mark_offer_complete(...)`；`mark_delivery_accepted(...)`；`mark_delivery_failed(...)`；`end_delivery(...)` | `current()`；`next_offer()`；`stats()` |
| State replica | `state_replica_memory_required(desc)`；`state_replica_init(...)` | `begin_owner(owner)`；`receive(owner,version,bytes)` | `current()`；`stats()` |
| Stream receiver | `stream_memory_required(desc,cfg)`；`stream_init(...)` | `begin_producer(producer,epoch)`；`receive(sample,comparable_now)`；`pop()` | `peek()`；`stats()` |
| Action caller | `action_caller_memory_required(desc,cfg)`；`action_caller_init(...)` | `submit(request)`；`mark_delivery_accepted(...)`；`observe_result(...)`；`mark_indeterminate(...)` | `next_offer()`；`query()`；`stats()` |
| Action executor | `action_executor_memory_required(desc,cfg)`；`action_executor_init(...)` | `ingest(expected_executor,request,comparable_now)`；`accept()`；`record_result()`；`mark_indeterminate()` | `next_application_request()`；`query()`；`stats()` |
| Blob store | `blob_store_memory_required(desc,cfg)`；`blob_store_init(...)` | `stage_begin(content_id,length)`；`commit_verified(written,verifier,context)`；`abort_staging(reason)` | `staging_buffer()`；`lookup(content_id)`；`stats()` |
| Descriptor set | `descriptor_set_memory_required(capacity)`；`descriptor_set_init(...)` | `register()`；`seal()` | `lookup()`；`count()` |

`generation` 是 Link adapter 分配的本地非零 delivery generation；它只用于防止迟到 acceptance
清除新一轮 pending，不是 SessionId、transport identity 或 Resource identity。`comparable_now`
必须已由外层映射到 sample/action deadline 所在的同一 local clock epoch；Core 对不能比较的
输入返回 `CLOCK_SCOPE_MISMATCH`，绝不直接比较 sender clock 与 receiver monotonic。

## State 状态机

State publisher 与 replica 分开初始化，二者都持有调用者提供的一份完整 snapshot storage。

- key 为 `(ResourceId, OwnerIncarnation, version)`；version 从 1 开始并在 owner scope 内严格
  单调，`UINT64_MAX -> small` 是 wrap failure。
- 同 key 同 bytes 是 duplicate；同 key 异 bytes 是 conflict；旧 version 是 stale。
- publisher 的新 truth 原子覆盖 current；若旧 current 尚待交付，返回 `REPLACED_PENDING`。
- publisher 第一次成功 publish 绑定 owner incarnation，直到该 handle 销毁；本地 owner 自身
  重启必须新建 publisher handle，不能在同一 version namespace 内偷偷换 incarnation。
- `next_offer` 只借用 current snapshot；`mark_delivery_accepted` 只清除匹配 local delivery
  generation、owner scope 和 version 的 pending，不表示 desired 已实际应用。
- `BEST_EFFORT` 与 `CONVERGENT` 是 `MeisoStateConfig` 的两种明确模式。新 publish 在已打开
  delivery generation 中都成为 pending；best-effort 在 adapter 接管一次 offer 后用
  `mark_offer_complete` 清除 pending，这既不是 peer acceptance 也不是 actual application。
  显式 `mark_delivery_failed` 同样可清除并增加 drop counter；convergent 则保留 pending 并
  返回 required-delivery failure。
- `end_delivery` 在 convergent current 仍 pending 时返回 `CONVERGENCE_FAILED` 并增加 failure
  counter；新的 generation 重新 offer current。best-effort 结束时只记录/drop 未送达 offer，
  在同一 generation 不自动 retry；新的 generation 仍 offer 当时最新 current 一次，以允许
  owner 在新 Session 重新发布仍有效 truth。wrong-generation acceptance 或 failure 永远不能
  清 pending。
- 对一次 best-effort offer，adapter 必须在已知最终同步结果时选择 `mark_offer_complete`、
  `mark_delivery_accepted` 或 `mark_delivery_failed` 之一；`complete` 只表示本地接管/尝试已
  完成。若 peer acceptance 随后异步到达，Core 仍可独立记录它一次；重复 acceptance 幂等。
- replica 显式开始 owner incarnation。切换后清空旧 current，新 incarnation 可以从 version 1
  开始；旧 scope 的 receive 被拒绝。
- desired 与 actual 必须是两个 descriptor/State instance；primitive 不把它们合成布尔值。

## Stream 状态机

Stream v0 是显式声明的 `STRICT_MONOTONIC` best-effort capability、完整 sample classifier 与
固定 ring；它不代表所有 Stream Profile，也不实现乱序缓存或可靠重传。

- sample key 为 `(producer incarnation, epoch, sequence)`；sequence 从 1 开始，只在同 epoch
  内比较。producer incarnation 或无法连续恢复时由调用者显式开始新 epoch。
- 同 key 的完整 immutable sample（source clock/time、sender deadline metadata、dependency
  metadata 与 payload）相同才是 duplicate；任一字段不同都是 conflict。动态 clock estimate
  得到的 local projection 不是 sample identity。跳号接受并报告 `GAP`；已超出
  fixed retained history 的旧 key 报 stale，v0 不把 late/reorder 冒充已支持。
- `MeisoStreamConfig` 明确 `STRICT_MONOTONIC`、slot count、payload stride、retained history、
  deadline support 与 `DROP_OLDEST / REJECT_NEWEST`。满时 drop-oldest 是
  `ACCEPTED_WITH_DROP`，会移除最旧 sample 并计数；reject-newest 才是保持 ring 不变的失败。
- queue slot 必须至少一个，history slot 可以是零。每个真正提交的 forward identity（进入 queue
  或因过期而丢弃）都按接收顺序进入 latest-N history；queue 另外保留尚未消费的完整 sample，
  因此慢消费者仍可识别已滑出 history 但尚在 queue 的 duplicate/conflict。`pop` 与
  drop-oldest 只移除 queue，不把旧 sample 倒写进 history，以免消费时机改变 retained window。
- intrinsic sample 结构、producer scope 与 epoch 先校验；随后先在 queue/history 分类同 key 的
  duplicate/conflict，再将未保留且不大于 high-water 的 key 分类为 stale/wrap。只有新的 forward
  key 才检查配置容量、deadline admission 和 ring overflow。这样动态 clock projection 或当前
  ring 状态不会改写一次已提交 sample 的 identity 结论。
- sample 保留原 sender clock/deadline metadata；外层另给本次 admission 使用的 local comparable
  deadline 与 `now`。重传时 clock estimate 变化不能把同一 sample 变成 conflict。clock scope
  不匹配单独报告，Core 不做 offset/uncertainty 估计。`now == deadline` 仍可接收，只有
  `now > deadline` 才过期；过期 sample 不占 queue、不会触发 overflow，但会推进 high-water、
  保留 identity 并显式报告 gap。reject-newest 不推进 high-water，调用者消费后可重试同 sample。
- `begin_producer` 把 epoch 当非零 opaque identity，不根据数值大小猜测先后。producer 或 epoch
  改变会显式清空 queue/history/high-water；旧 scope/epoch 的迟到 sample 被拒绝。
- `peek` 的 view 在下一次成功 receive/pop/begin-producer 前有效；`pop` 消费最旧 sample。
  v0 fixed-stride ring **尚未实现 MediaStream payload path**。未来 MediaStream adapter 可复用
  key/classification 规则，但必须另证 external-buffer、codec dependency 与 recovery，不能把
  本模块测试冒充 MediaStream conformance。

## Action 状态机

Action caller 与 executor 分开。delivery acceptance、executor acceptance 与 terminal result
是三件不同的事。

- `ActionId` 自身是稳定 request identity；request 另带 expected executor incarnation、target
  ResourceId、operation SchemaId、arguments、sender deadline metadata 与 idempotency
  declaration。executor incarnation 只做 scope validation，不把旧 ID 变成“新 Action”。本地
  mapped expiry 是 ingest context，不进入 duplicate identity；同一 ActionId 首次 admission 后
  保留原判断，clock estimate 更新不能让 replay 改写结果。
- caller `submit -> next_offer -> mark_delivery_accepted` 不产生成功结果；只有 executor 的
  result 能进入 `SUCCEEDED / FAILED / REJECTED / EXPIRED / INDETERMINATE`。
- caller 的 local delivery generation 严格递增；同 ActionId 不能通过下一 generation 重新
  submit/requeue。`end_delivery` 若本 generation 仍有 `PENDING / ACCEPTED` 会明确返回
  unresolved，但不擅自 replay、失败或标记 indeterminate；terminal entry 即使没有 transport
  ACK 也不算 unresolved。带 generation、ActionId 和 executor scope 的迟到 ACK，以及迟到的
  executor observation，在 generation 结束后仍可独立记录。
- executor `ingest` 同 ID 同完整 request 返回 replay-same，不再次交给 application；同 ID
  异内容返回 conflict。replay outcome 不复制出第二份 request/result；调用者通过 `query` 读取
  原 dedup slot 中持久的 PENDING、ACCEPTED 或 terminal/result。
- dedup/result table 固定容量且不静默逐出。v0 没有 `release`；满时显式拒绝新 request，table
  的 replay horizon 就是当前 executor handle/incarnation 的完整 lifetime。
- old-incarnation request 始终 `SCOPE_MISMATCH`，绝不进入 application。caller API 不允许把
  已有 ActionId retarget 到新 executor；重试一次业务操作必须使用新 ActionId。双方同时丢失
  dedup state 后不承诺 exactly-once，这一限制必须由 Profile/persistent operation design 补足。
- deadline 在 application 接受前已过则 terminal `EXPIRED`。`ACCEPTED` 是非终态；
  `SUCCEEDED / FAILED / REJECTED / EXPIRED / INDETERMINATE` 是 v0 的五种 terminal。
  executor 在 ingest 保存首次 local deadline projection，并在 application 接管副作用前用相同
  local clock epoch 再检查；replay 不更新该 projection，`now == deadline` 仍可接受，只有
  `now > deadline` 才过期。`next_application_request` 只是可重复 observation；只有一次串行的
  `executor_accept -> ACCEPTED` 返回授权 application 开始副作用，duplicate accept 不授权第二次
  执行。executor 只允许 `PENDING -> REJECTED/EXPIRED` 与
  `ACCEPTED -> SUCCEEDED/FAILED/INDETERMINATE`；caller 可从 PENDING 直接观察任一 terminal 以
  容忍 accepted observation 丢失，但一旦观察 ACCEPTED，就拒绝矛盾的 REJECTED/EXPIRED。
  terminal immutable，迟到 result 只能报告 `TERMINAL_ALREADY_SET`，不悄悄改写知识状态。
- 外层显式报告 delivery discontinuity 或 executor restart 时，无法证明副作用结果的 caller/
  executor entry 由 `mark_indeterminate` 进入终态。Core 不观察 Session，也不自动重放
  non-idempotent Action。
- executor handle 在 init 时永久绑定 role/incarnation。外层 lifecycle 必须保证同一
  incarnation 不通过清空并重建 arena 丢弃 dedup table；这是调用者/外层前置条件，primitive
  无法在 arena 之外检测这种重建。新 executor incarnation 拒绝旧 request，新的业务尝试必须
  使用新 ActionId。

## Blob 状态机

一个 Blob Resource descriptor 表示一个 bounded content store endpoint（ResourceId），store
内的每个 immutable value 由 ContentId 定位。ResourceId 决定 capability/authorization scope，
ContentId 决定内容 identity；二者不能互相替代。v0 store 有 fixed published slots 和一个独立
staging buffer，不把 transport chunk protocol 放进 Core。

- `stage_begin` 声明 ContentId 与完整 length；`staging_buffer` 返回借用 mutable view。一个 store
  同时只允许一个 staging candidate，published slot 不会被 staging 覆盖。同一 ContentId 与
  length 的重复 begin 保留已写 bytes 并返回 unchanged；另一 candidate 显式 busy；已发布的
  同 ContentId 若 length 冲突则在 begin 时直接报告 conflict。
- `commit_verified(written_size, verifier, context)` 首先要求 written size 等于 declared length，
  再让 verifier 对 Core 提供的精确 staging bytes 与 expected ContentId 计算/确认。只有 callback
  返回 true 才能 publish；Core 信任该 callback 的密码学与实现正确性，但不信任调用者单独
  传入的 boolean 或另一块 buffer。
- length、ContentId/algorithm、capacity 或 integrity failure 明确失败并保持 published set
  不变。length mismatch、integrity failure、content conflict 与 published-slot exhaustion 都保留
  staging candidate 和 mutable borrow；publish、verified duplicate 或显式 abort 才结束
  staging。只有外层显式调用 `abort_staging(reason)` 才取消 partial candidate，transport/
  Session 变化本身不会被 Core 观察或自动 abort。
- 已 publish 的同 ContentId 同 bytes 是 duplicate；同 ContentId 异 bytes/length 是 conflict。
- verifier 执行期间，同一 store 的 staging access 与 mutation 会被 reentrant guard 拒绝；
  callback 只借用 canonical expected ContentId 的临时副本和 Core-owned staging bytes。v0 不
  release/evict published slot，因此成功 lookup 的 immutable content borrow 与 store arena 同寿命。
- zero-length Blob 合法；schema/Profile 决定它是否有业务意义。
- 已发布 Blob 不因 Link Session 消失而自动删除；当前授权与引用仍由外层 State/Session
  判断，缓存命中不能冒充授权。

## 固定 descriptor set snapshot

descriptor set builder 接受调用者提供的 descriptor slot 数，允许在 seal 前注册，seal 后作为
一个 immutable registry snapshot。以下情况
显式失败：invalid descriptor、重复 identity 的相同 descriptor、重复 identity 的不同 kind/
schema/owner、capacity exhausted 和 seal 后 mutation。snapshot 不拥有四种 runtime handle，
不表达 endpoint 当前 availability，也不阻止 runtime 在下一 capability generation 发布新的
snapshot。

v0 允许 capacity 为零，以表达可 seal、可查询但永远为空的 snapshot；builder 阶段 lookup
显式返回 not-sealed，避免消费者观察半构造集合。seal 是幂等边界，重复 seal 返回
already-sealed。lookup 只复制 descriptor value，不返回内部 borrow；duplicate/conflict 在容量
检查前分类，因此满集合仍能准确识别已有 ResourceId。相同 ResourceId 的 descriptor 任一字段
不同都是 conflict，不能因 kind 不同而成为第二项。

## 正交 conformance 矩阵

| 轴 | State | Stream | Action | Blob |
|---|---|---|---|---|
| truth / identity | owner + version | producer + epoch + sequence | ActionId；executor 是 target scope | ResourceId store + ContentId |
| 原子可见 | 完整 snapshot replace | 完整 sample 入 ring | 完整 request/result slot | verified complete publish |
| delivery / actual | ACK != actual State | 接收 != 消费/物理输出 | ACK != accepted/result | transfer != activation |
| duplicate | 同 key 同 bytes | 同 key 同 bytes | 同 ID 同 request/result | 同 ID 同完整内容 |
| conflict | 同 version 异 bytes | 同 sample key 异 bytes | 同 ID 异 request/result | 同 ID 异内容 |
| bounded failure | payload capacity | ring/stride/overflow | slot/args/result capacity | slot/content capacity |
| time | freshness 由外层解释 | clock epoch/deadline | optional deadline | 不适用 |
| discontinuity | owner/local delivery generation | producer/epoch | 外层显式 indeterminate | 外层显式 abort staging |
| v0 terminal | 不适用；current truth | 不适用；ring/current epoch | 5 terminal；accepted 非终态 | published 或 staging 不可见 |

“不适用”是刻意结论，不为追求矩阵对称而给 State 虚构 terminal、给 Blob 虚构 sequence，
或把已发生的 Stream event 变成 Action。

## 必须通过的测试门

1. common：null/zero/exact-capacity、错误 role+incarnation/kind/schema/digest、整数边界、
   misaligned/undersized arena、memory-size overflow、borrow invalidation、counter saturation、
   失败后不变和无 heap；
2. State：lost ACK、newest replaces pending、duplicate/conflict/stale/wrap、owner reset、wrong
   delivery scope、best-effort 与 convergent 分离；
3. Stream：in-order、完整 metadata duplicate/conflict、gap、late/reorder -> stale、producer
   restart/new epoch、sequence boundary、两种 overflow、clock/deadline mismatch、慢消费者仍
   有界进展；
4. Action：single execution、conflict、ACK/result 分离、pre-exec expiry、terminal immutable、
   replay-result、same ID old executor scope、dedup full 不逐出、unresolved -> indeterminate、
   late result rejected、non-idempotent 不自动 replay；
5. Blob：incomplete invisible、length/integrity/id mismatch、atomic success、duplicate/conflict、
   capacity rollback、invalid algorithm/digest、external abort、外部 Session 变化后 published
   lookup 不变与 zero-length；
6. cross-kind：相同 ResourceId 的 snapshot descriptor 不得变成不同 kind；duplicate/seal
   mutation 可见；一个 kind 的容量失败不污染另一个；
   test-only in-process fixture 覆盖四类，但不冒充 LinkSession 或 remote backend；
7. clean build 在 warnings-as-errors 下通过，并以源码/链接审计确认 portable library 不引用
   socket、syscall、filesystem、thread、clock service 或旧 wire symbol。

## Host 验证快照

截至 payload source 提交
`add9b8ac49a86cb0fb10b8f8cff7cbb8f3739b27`，common、四类 primitive、descriptor snapshot、纯进程内
four-kind fixture 与复用同一测试源码的 conformance runner 已在 AppleClang 17、CMake 4.2、
Ninja 1.13 的 clean out-of-tree Debug、Release 和 ASan/UBSan 配置中完成
warnings-as-errors build 与 CTest，并通过 AppleClang 静态分析和 portable-library
forbidden-symbol 审计。测试 inventory 以该提交的
`ctest --test-dir <build> -N` 与实际 `ctest --output-on-failure` 输出为准，不在文档中固化测试
总数。

four-kind fixture 使用真实 public contracts 证明 catalog identity、State delivery/actual、
Stream receive/consume、Action delivery/accepted/result 与 Blob staging/publish 彼此独立，并
逐类验证容量失败不污染其他 kind。它不包含 serialization、socket、LinkSession、remote
backend、MediaStream external-buffer path 或板级 adapter，因此 `HOST_VERIFIED` 不能解释为
完整 Meiso Core、Resource Runtime、Profile 或 K230D 通过。

后续 [Session Core v0](session-core-v0.md) 保持这些 primitive API 不变，并以独立
`session_four_kind` fixture 证明同一 codec 的当前 SessionId/READY gate 后才能执行
test-only remote dispatch；未声明 ResourceId、wrong kind 与旧 Session copy 均被拒绝。
该新增证据不包含 Registry generation，也不把测试 envelope 晋升为产品 Runtime wire；
它不改变本页 primitive 可离线运行的契约。

| 层级 | 状态 | 边界 |
|---|---|---|
| common、四类 primitive、descriptor snapshot | `IMPLEMENTED / HOST_VERIFIED` | portable、in-process、caller-owned arena |
| four-kind fixture | `HOST_VERIFIED` | test-only orthogonality，不是 runtime dispatcher |
| Session-gated four-kind fixture | `HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED` | test-only route envelope；不是 TCP 或产品 Runtime |
| target build | `HOST_VERIFIED` | exact CanMV v1.8 graph 的 RISC-V ELF 与 fresh full image 已完成 host inspection |
| K230D execution | `BOARD_VERIFIED` | 锁定 candidate 的 boot-critical prefix/payload identity 在 `CanMV-K230D-Zero V1.1` 上人工执行；七套 aggregate suite 全部 `rc=0`，仅覆盖本页 semantic core |

## 实施状态与后续工作

common types/descriptor validation、State、Stream、Blob、descriptor set、Action、counter
saturation vector 与 four-kind fixture 已分别完成并验证。同一 runner 已由锁定 target toolchain
构建成 RISC-V ELF，在 fresh full image 中逐字节复核，并在带明确 FAT 变更边界的 K230D
candidate 上人工运行两次；两次之间有一次物理断电重启，每次七套 aggregate suite 均通过。
只有第二次 conformance invocation 保留了完整原始 RX capture；它不包含此前的上电过程。
串口工具可能改变 control line，且没有取得从上电前开始的完整无干扰
cold-boot capture，因此该结果不证明启动时序、LinkSession reset 或自动执行。`.kdimg` 被证明
不含该 payload，仍不能作为 Meiso OTA candidate。具体介质、启动与证据限制见
[系统进展](system-report.md)。

重基线前的 prototype 已在逐项处置审阅后从活动树退役；删除的旧行为不构成现行兼容承诺，
Composition/Rendering workload 在对应新结构实现时重新建立。Git 历史保留其来源，不形成
现行 API、ABI、构建目标或兼容面。
