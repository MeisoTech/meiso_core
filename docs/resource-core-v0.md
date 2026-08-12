# Resource semantic core v0 实现设计

> 状态：`DRAFT_IMPLEMENTATION_BASELINE`
> 更新：2026-08-12
> 兼容性：本工作分支上的 C11 source API；不冻结二进制 ABI，也不兼容旧实验 API。

本文把[Resource 契约](resource.md)映射为第一份可执行的 portable primitive 与 conformance
package。产品语义仍由 `system.md / resource.md / link.md / composition.md` 定义；本文拥有
模块边界、内存模型、v0 API 选择和测试映射，不能反向修改产品含义。

## 完成定义与非目标

本工作单元完成的是 **Resource semantic core**，包括四个相互独立的
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

## Prototype-level API 形状

下表固定职责和 lifetime，不冻结 v0 ABI 的 struct layout；header 实现时可将同一职责拆成
publisher/replica 或 caller/executor typed handle。

| primitive | arena/init | 主要 mutation | 只读 observation |
|---|---|---|---|
| State publisher | `state_publisher_memory_required(desc,cfg)`；`state_publisher_init(...)` | `publish(owner,version,bytes)`；`begin_delivery(generation)`；`mark_accepted(...)`；`mark_delivery_failed(...)`；`end_delivery(...)` | `current()`；`next_offer()`；`stats()` |
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
- `next_offer` 只借用 current snapshot；`mark_delivery_accepted` 只清除匹配 local delivery
  generation、owner scope 和 version 的 pending，不表示 desired 已实际应用。
- `BEST_EFFORT` 与 `CONVERGENT` 是 `MeisoStateConfig` 的两种明确模式。新 publish 在已打开
  delivery generation 中都成为 pending；best-effort 的显式 `mark_delivery_failed` 清除该
  offer 并增加 drop counter，convergent 则保留 pending 并返回 required-delivery failure。
- `end_delivery` 在 convergent current 仍 pending 时返回 `CONVERGENCE_FAILED` 并增加 failure
  counter；新的 generation 重新 offer current。best-effort 结束时只记录/drop 未送达 offer，
  新 generation 不重发旧 truth。wrong-generation acceptance 或 failure 永远不能清 pending。
- replica 显式开始 owner incarnation。切换后清空旧 current，新 incarnation 可以从 version 1
  开始；旧 scope 的 receive 被拒绝。
- desired 与 actual 必须是两个 descriptor/State instance；primitive 不把它们合成布尔值。

## Stream 状态机

Stream v0 是显式声明的 `STRICT_MONOTONIC` best-effort capability、完整 sample classifier 与
固定 ring；它不代表所有 Stream Profile，也不实现乱序缓存或可靠重传。

- sample key 为 `(producer incarnation, epoch, sequence)`；sequence 从 1 开始，只在同 epoch
  内比较。producer incarnation 或无法连续恢复时由调用者显式开始新 epoch。
- 同 key 的完整 immutable sample（source clock/time、mapped deadline、dependency metadata 与
  payload）相同才是 duplicate；任一字段不同都是 conflict。跳号接受并报告 `GAP`；已超出
  fixed retained history 的旧 key 报 stale，v0 不把 late/reorder 冒充已支持。
- `MeisoStreamConfig` 明确 `STRICT_MONOTONIC`、slot count、payload stride、retained history、
  deadline support 与 `DROP_OLDEST / REJECT_NEWEST`。满时 drop-oldest 是
  `ACCEPTED_WITH_DROP`，会移除最旧 sample 并计数；reject-newest 才是保持 ring 不变的失败。
- sample 保留原 sender clock metadata；deadline 则必须是外层已经映射并带 uncertainty gate
  结论的 local comparable deadline。clock epoch 不匹配单独报告，Core 不做 offset/uncertainty
  估计。
- `peek` 的 view 在下一次成功 receive/pop/begin-producer 前有效；`pop` 消费最旧 sample。
  v0 fixed-stride ring **尚未实现 MediaStream payload path**。未来 MediaStream adapter 可复用
  key/classification 规则，但必须另证 external-buffer、codec dependency 与 recovery，不能把
  本模块测试冒充 MediaStream conformance。

## Action 状态机

Action caller 与 executor 分开。delivery acceptance、executor acceptance 与 terminal result
是三件不同的事。

- `ActionId` 自身是稳定 request identity；request 另带 expected executor incarnation、target
  ResourceId、operation SchemaId、arguments、可选的已映射 local deadline 与 idempotency
  declaration。executor incarnation 只做 scope validation，不把旧 ID 变成“新 Action”。
- caller `submit -> next_offer -> mark_delivery_accepted` 不产生成功结果；只有 executor 的
  result 能进入 `SUCCEEDED / FAILED / REJECTED / EXPIRED / INDETERMINATE`。
- executor `ingest` 同 ID 同完整 request 返回 replay-same，不再次交给 application；同 ID
  异内容返回 conflict。
- dedup/result table 固定容量且不静默逐出。v0 没有 `release`；满时显式拒绝新 request，table
  的 replay horizon 就是当前 executor handle/incarnation 的完整 lifetime。
- old-incarnation request 始终 `SCOPE_MISMATCH`，绝不进入 application。caller API 不允许把
  已有 ActionId retarget 到新 executor；重试一次业务操作必须使用新 ActionId。双方同时丢失
  dedup state 后不承诺 exactly-once，这一限制必须由 Profile/persistent operation design 补足。
- deadline 在 application 接受前已过则 terminal `EXPIRED`。`ACCEPTED` 是非终态；
  `SUCCEEDED / FAILED / REJECTED / EXPIRED / INDETERMINATE` 是 v0 的五种 terminal。
  terminal immutable，迟到 result 只能报告 `TERMINAL_ALREADY_SET`，不悄悄改写知识状态。
- 外层显式报告 delivery discontinuity 或 executor restart 时，无法证明副作用结果的 caller/
  executor entry 由 `mark_indeterminate` 进入终态。Core 不观察 Session，也不自动重放
  non-idempotent Action。

## Blob 状态机

一个 Blob Resource descriptor 表示一个 bounded content store endpoint（ResourceId），store
内的每个 immutable value 由 ContentId 定位。ResourceId 决定 capability/authorization scope，
ContentId 决定内容 identity；二者不能互相替代。v0 store 有 fixed published slots 和一个独立
staging buffer，不把 transport chunk protocol 放进 Core。

- `stage_begin` 声明 ContentId 与完整 length；`staging_buffer` 返回借用 mutable view。一个 store
  同时只允许一个 staging candidate，published slot 不会被 staging 覆盖。
- `commit_verified(written_size, verifier, context)` 首先要求 written size 等于 declared length，
  再让 verifier 对 Core 提供的精确 staging bytes 与 expected ContentId 计算/确认。只有 callback
  返回 true 才能 publish；Core 信任该 callback 的密码学与实现正确性，但不信任调用者单独
  传入的 boolean 或另一块 buffer。
- length、ContentId/algorithm、capacity 或 integrity failure 明确失败并保持 published set
  不变；staging 是否保留供诊断由 outcome 定义。只有外层显式调用 `abort_staging(reason)` 才
  取消 partial candidate，transport/Session 变化本身不会被 Core 观察或自动 abort。
- 已 publish 的同 ContentId 同 bytes 是 duplicate；同 ContentId 异 bytes/length 是 conflict。
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

## 提交与退役顺序

1. common types、descriptor validator 与 memory contract；
2. State，并迁移 `meiso_latest_state` 的行为向量；
3. Stream；4. Action；5. Blob；6. catalog 与 cross-kind fixture；
7. target toolchain/build evidence；8. 行为向量全部覆盖后再单独删除旧
   `meiso_latest_state` 与不再适用的 `meiso_slice` demo。

旧实现会暂时并存，避免把“新文件能编译”和“旧行为已迁移完整”混为同一个不可审阅
提交。每个切片都必须 clean build + test 后独立提交。
