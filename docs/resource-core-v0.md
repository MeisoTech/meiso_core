# Resource semantic core v0 实现设计

> 状态：`ACCEPTED_IMPLEMENTATION_BASELINE`
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

meiso_resource_catalog.h + meiso_resource_catalog.c
    fixed descriptor catalog; no generic state-machine dispatcher

tests/
    per-kind conformance + cross-kind in-process fixture
```

每种 Resource 使用 typed opaque handle 和自己的 outcome enum。catalog 只把 identity 映射到
immutable descriptor；它没有 universal vtable、QoS enum 或通用 transition engine。

## 共同值类型与内存规则

- `ResourceId`、`SchemaId` 与 `ActionId` 在 v0 source API 中各用独立的 128-bit POD 类型，
  防止意外互换；这个宽度不是冻结的 wire/ABI 决定。
- owner scope 是 `(OwnerRole, OwnerIncarnation)`。State version 只在
  `(ResourceId, owner scope)` 内比较；Session 不进入 Resource identity。
- Stream sample key 是 `(producer incarnation, stream epoch, sequence)`；clock epoch 与
  source timestamp 是独立字段。
- Blob `ContentId` 是 `(algorithm, digest length, digest bytes)`；v0 提供 SHA-256 算法标识，
  但不包含 hash 实现。adapter/verifier 计算摘要，Core 只比较 identity，并在调用者明确给出
  integrity success 后 publish。
- `ByteView {data,size}` 与 `MutableBuffer {data,capacity}` 可表达零长度 payload；非零长度时
  pointer 必须有效。Core 不保存临时 input view，只有完整复制成功才改变状态。
- runtime handle 通过 `*_memory_required()`、`*_alignment_required()` 和
  `*_init(caller_memory, ...)` 创建；payload/ring/slot storage 同样由调用者提供。失败的 init
  不产生可用 handle。
- 任何 operation 在 `INVALID / CAPACITY / SCOPE_MISMATCH / CONFLICT` 等失败后保持先前可见
  状态不变。计数器饱和在 `UINT64_MAX`，不得 wrap 后伪装成较小值。

`ResourceDescriptor` 只包含 portable primitive 必须知道的 identity、kind、schema、owner 和
最大 payload。权限、endpoint 格式、native handle、transport QoS 与 Profile capability 留在
外层 descriptor 扩展；Core 不通过一个巨型通用结构假装理解它们。

## State 状态机

State publisher 与 replica 分开初始化，二者都持有调用者提供的一份完整 snapshot storage。

- key 为 `(ResourceId, OwnerIncarnation, version)`；version 从 1 开始并在 owner scope 内严格
  单调，`UINT64_MAX -> small` 是 wrap failure。
- 同 key 同 bytes 是 duplicate；同 key 异 bytes 是 conflict；旧 version 是 stale。
- publisher 的新 truth 原子覆盖 current；若旧 current 尚待交付，返回 `REPLACED_PENDING`。
- `next_offer` 只借用 current snapshot；`mark_delivery_accepted` 只清除对应 delivery scope
  中的 pending，不表示 desired 已实际应用。
- `CONVERGENT` publisher 在新的 delivery scope 中重新 offer current；`BEST_EFFORT` 不自动
  伪装成需要最终收敛。
- replica 显式开始 owner incarnation。切换后清空旧 current，新 incarnation 可以从 version 1
  开始；旧 scope 的 receive 被拒绝。
- desired 与 actual 必须是两个 descriptor/State instance；primitive 不把它们合成布尔值。

## Stream 状态机

Stream v0 是 best-effort append/classification 与固定 ring，不实现可靠重传。

- sample key 为 `(producer incarnation, epoch, sequence)`；sequence 从 1 开始，只在同 epoch
  内比较。producer incarnation 或无法连续恢复时由调用者显式开始新 epoch。
- 同 key 同 payload 是 duplicate，同 key 异 payload是 conflict；跳号接受并报告 `GAP`，
  旧 epoch/旧 sequence 明确报告 stale。
- ring payload slot 为固定 stride。满时按 descriptor 选择 `DROP_OLDEST` 或
  `REJECT_NEWEST`；两者都增加可观察 counter，失败不能破坏既有 sample。
- deadline 由调用者传入当前 monotonic time 判断；clock epoch 不匹配单独报告，Core 不估计
  两端时钟。
- `pop` 只消费已接收 sample。MediaStream 使用相同 identity/lifecycle 分类，但 codec、
  packetization 和大 buffer 路径由 adapter/Profile 实现，不复制进本 ring。

## Action 状态机

Action caller 与 executor 分开。delivery acceptance、executor acceptance 与 terminal result
是三件不同的事。

- request key 为 `(ActionId, executor incarnation)`，并包含 target ResourceId、operation
  SchemaId、arguments、可选 deadline 与 idempotency declaration。
- caller `submit -> next_offer -> mark_delivery_accepted` 不产生成功结果；只有 executor 的
  result 能进入 `SUCCEEDED / FAILED / REJECTED / EXPIRED / INDETERMINATE`。
- executor `ingest` 同 ID 同完整 request 返回 replay-same，不再次交给 application；同 ID
  异内容返回 conflict。
- dedup/result table 固定容量且不静默逐出。满时拒绝新 request；只有显式 `release` 才缩短
  当前 executor incarnation 的 replay horizon。
- deadline 在 application 接受前已过则 terminal `EXPIRED`。terminal result 不可改写。
- scope 中断或 executor restart 后无法证明副作用结果的未决 request 变为
  `INDETERMINATE`；Core 不自动重放 non-idempotent Action，也不承诺跨重启持久 exactly-once。

## Blob 状态机

Blob catalog 维护 fixed slots。每个 slot 有 caller-owned staging/published capacity；v0 不把
transport chunk protocol放进 Core。

- `stage_begin` 声明 ContentId 与完整 length，并返回 staging buffer；调用者填充后交给外部
  verifier。
- 只有 `commit_verified(..., integrity_ok=true)` 且长度完整时，内容才原子可见。
- incomplete、capacity、cancel、session loss 或 integrity failure 都 abort staging，lookup
  永远看不到 partial bytes。
- 已 publish 的同 ContentId 同 bytes 是 duplicate；同 ContentId 异 bytes/length 是 conflict。
- zero-length Blob 合法；schema/Profile 决定它是否有业务意义。
- 已发布 Blob 不因 Link Session 消失而自动删除；当前授权与引用仍由外层 State/Session
  判断，缓存命中不能冒充授权。

## 固定 catalog

catalog 接受调用者提供的 descriptor slot 数，允许在 seal 前注册，seal 后只读。以下情况
显式失败：invalid descriptor、重复 identity 的相同 descriptor、重复 identity 的不同 kind/
schema/owner、capacity exhausted 和 seal 后 mutation。catalog 不拥有四种 runtime handle，
也不决定实现必须支持全部 kind。

## 正交 conformance 矩阵

| 轴 | State | Stream | Action | Blob |
|---|---|---|---|---|
| truth / identity | owner + version | producer + epoch + sequence | action + executor | content identity |
| 原子可见 | 完整 snapshot replace | 完整 sample 入 ring | 完整 request/result slot | verified complete publish |
| delivery / actual | ACK != actual State | 接收 != 消费/物理输出 | ACK != accepted/result | transfer != activation |
| duplicate | 同 key 同 bytes | 同 key 同 bytes | 同 ID 同 request/result | 同 ID 同完整内容 |
| conflict | 同 version 异 bytes | 同 sample key 异 bytes | 同 ID 异 request/result | 同 ID 异内容 |
| bounded failure | payload capacity | ring/stride/overflow | slot/args/result capacity | slot/content capacity |
| time | freshness 由外层解释 | clock epoch/deadline | optional deadline | 不适用 |
| discontinuity | owner/delivery scope | producer/epoch | executor/scope | staging abort |
| terminal | 不适用；current truth | ended/failed 可扩展在 lifecycle | 六种明确 terminal | published/aborted/failed |

“不适用”是刻意结论，不为追求矩阵对称而给 State 虚构 terminal、给 Blob 虚构 sequence，
或把已发生的 Stream event 变成 Action。

## 必须通过的测试门

1. common：null/zero/exact-capacity、错误 kind/owner/schema、整数边界、失败后不变、无 heap；
2. State：lost ACK、newest replaces pending、duplicate/conflict/stale/wrap、owner reset、wrong
   delivery scope、best-effort 与 convergent 分离；
3. Stream：in-order、duplicate/conflict/gap、old epoch、sequence boundary、两种 overflow、
   clock/deadline mismatch、慢消费者仍有界进展；
4. Action：single execution、conflict、ACK/result 分离、pre-exec expiry、terminal immutable、
   dedup full 不逐出、unresolved -> indeterminate、non-idempotent 不自动 replay；
5. Blob：incomplete invisible、length/integrity/id mismatch、atomic success、duplicate/conflict、
   capacity rollback、abort/session loss 与 zero-length；
6. cross-kind：相同 ResourceId 不得注册为不同 kind；一个 kind 的容量失败不污染另一个；
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
