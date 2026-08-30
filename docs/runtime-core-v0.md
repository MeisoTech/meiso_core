# Runtime Core compatibility

> 状态：`INTERNAL COMPATIBILITY NOTE`。当前正式 Composition Profile 只使用
> MRT0 v4；本页记录 portable Runtime 中仍由测试保护的 MRT0 v2/v3 边界。
> 详细开发历史、wire table 和旧证据已[归档](archive/runtime-v2-v3-compatibility-history.md)。

## Authority

产品 Runtime 语义只由 [Runtime 契约](runtime.md) 拥有。当前 Composition 的
ProfileRef、MRT0 v4 graph 和容量只由
[Composition Profile](../profiles/composition/README.md) 拥有。MRT0 v2/v3 是不安装的
内部 protocol fixture 所依赖的兼容面，不得作为正式 Profile 的 fallback、alias
或新 API 命名来源。

## Shared Runtime invariants

每个 Runtime handle 永久绑定 init 时已 READY 的 Session、SessionId、role 和
negotiated limits。Session close/loss 或 binding 改变使 handle 永久失效；不 reset、
不 resume、不复活旧 catalog/State/Blob/Stream truth。

- memory 全部来自 caller-owned arena，不分配 heap、不建线程、不做 socket I/O；
- catalog 是完整 snapshot，使用 active/staged 双 bank 原子 accept/reject；
- generation、endpoint incarnation、route、owner、kind、facet、schema 和容量全部验证；
- desired/actual 使用 Core State，data 使用 Core Stream；不新增 subscription truth；
- desired delivery、StateReplica commit、actual RUNNING 和 data gate 是不同阶段；
- 新 production epoch 的 data 只能在对应 actual 被 Core State 接受后通过；
- 所有 pending event/view 都是有界 borrow，只在合同规定的下次 mutation 前有效；
- failure、malformed、capacity、alias 和 `TX_BUSY` 不得部分修改输出或内部状态。

## Compatibility surface

MRT0 v2 保留六类 canonical big-endian Runtime frame：Catalog offer/accept/reject、
State offer/accept 和 Stream sample。它仅接受 State/Stream route，拒绝 Blob immutable route。

MRT0 v3 保留 v2 的 type 1..6 body layout，并增加 Host→Edge Blob
`BEGIN / CHUNK / COMMIT / ABORT / RESULT`。它只允许 Host-owned Blob + immutable facet，
不因此开放 Edge→Host Blob 或任意双向 Stream。每个 handle 只有一个全局
Blob transfer，chunk 上限 4096 bytes，没有 per-chunk ACK、并行 transfer 或 resume。

```text
Host BEGIN(ContentId,total) -> Edge RESULT(BEGIN,READY,next=0)
  -> Host CHUNK(offset,bytes)*
  -> Host COMMIT
  -> Edge length/SHA/Profile verification
  -> Core BlobStore publication
  -> Edge terminal RESULT
```

Blob 在完整长度、SHA-256 与 Profile verifier 通过前不可见。Session/catalog
discontinuity 取消未完 transfer 并清 scratch；Core 已发布 cache 不自动获得新
Session 中的 route 或 Profile visibility。

Strict v2 与 strict v3 handle 互相拒绝 wire version，没有协商后 fallback。
MRT0 v4 的 delivery-class route 和当前 Composition graph 不由本页的 compatibility
surface 定义。

## Validation boundary

Compatibility tests 保留：

- canonical desired/actual prefix、catalog staging/activation 和 production gate；
- v2 六类 frame 与 v3 Blob frame 的 literal golden、每个 truncation/reserved 失败；
- StateReplica commit、desired disable/reconfigure、epoch high-water 与 stale endpoint history；
- Blob 0/4096/boundary length、offset/gap/replay、publish failure、Session/catalog invalidation；
- capacity、overlap、borrow lifetime、`TX_BUSY` 与 no-partial mutation。

这些测试只证明内部兼容语义，不证明正式 Composition Profile、transport、
K230D、SDK、security、performance 或 recovery。
