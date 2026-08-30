# Runtime 契约

> 状态：产品级技术契约。本页定义 Resource Graph、catalog、
> desired/actual、production gate、route 与恢复边界；不定义具体 sensor、
> driver ABI、SDK facade 或 transport。开发期分版和逐 commit 证据已
> [归档](archive/runtime-development-history.md)。

## 边界

Core 建立 peer Session 并交付 `State / Stream / Action / Blob`。Runtime 拥有
当前 Edge 的 Resource Graph、Host desired、Edge actual 与 production lifecycle。
SDK 只投影 Runtime truth，不得根据 socket 或 ACK 推断业务成功。

```text
Core READY
  -> Edge offers complete CatalogSnapshot
  -> Host atomically accepts it
  -> Host publishes desired State
  -> Edge admits and applies through the truth-owner adapter
  -> Edge publishes actual State
  -> accepted RUNNING actual opens the data gate
  -> disable/failure drains production and publishes STOPPED/FAILED
```

desired/actual 是 catalog 声明的普通 Core State，小数据是 Stream，连续媒体是
MediaStream。不存在平行的 subscribe wire 或隐藏 delivery switch。IP、port、fd、
bus address 和 native buffer 不进入 Runtime identity。

## Catalog 与 endpoint bundle

Edge 从以下交集构建 catalog：

```text
Profile envelope
  ∩ private configuration
  ∩ current adapter probe
  ∩ policy / permission / resource budget
```

`CatalogSnapshot` 是完整、不可变、原子接受的 Resource Graph，至少包含：

- 非零且在当前 Runtime incarnation 中单调的 `catalog_generation`；
- 稳定 `EndpointId`、当前 `endpoint_incarnation`、capability 与 schema；
- Resource descriptor、facet、owner/direction、payload limit 和 delivery class；
- desired、actual、data、operation 与 immutable Resource 的明确关联；
- entry/capability 容量与完整性边界。

Host 对整个 snapshot `ACCEPT` 或 `REJECT`；子集 endpoint 不可见。Catalog
接受只证明 route identity 有效，不证明 producer 已启动。

## Identity 与代际

- `EndpointId` 是 Profile 内的逻辑槽，不是硬件序列号或 locator；
- 设备替换、adapter 重启或 continuity 无法证明时更新 `endpoint_incarnation`；
- endpoint/resource/schema/mode envelope 改变才更新 catalog generation，瞬时
  health 优先由 actual State 表达；
- actual 必须指出 observed/applied desired owner+version；迟到结果不得覆盖新意图；
- 每次成功启动或不连续重配置产生新的非零 `production_epoch`；
- SessionId、catalog generation、endpoint incarnation、State version、production epoch、
  stream epoch 和 sequence 不互相替代。

## Desired → actual → production

1. Core 未 READY 时 Runtime 不产生任何 control/data mutation。
2. Host 在接受 catalog 后发布完整 desired State。
3. Edge 按 capability、availability、permission、budget 和安全 policy 验收，然后
   调用唯一 truth-owner adapter。
4. actual 正交报告 availability、apply result、effective parameters、reason、
   observed/applied desired 与 production epoch。
5. 唯一 data gate 是 Host Core State 已接受的 actual 中的 effective-enabled +
   production status + epoch。
6. Host Runtime 再校验 catalog/endpoint/schema/epoch、freshness 和 gap，才向 SDK 暴露。

本地 desired receipt、Core delivery accepted、Edge observed、adapter applied、actual
accepted、production RUNNING 与首个 sample 是不同进展，不得压成一个成功布尔值。

重配置时旧 actual/epoch 可继续 RUNNING 到明确 cut；新 effective 配置必须使用
新 production epoch。Disable 先停止新生产并有界 drain，再发布 STOPPED。
不支持的参数必须 REJECTED/DEGRADED 并说明 reason，不得静默替换 format/rate/field。

## Route delivery 与有界性

Route 显式声明 delivery class：

- `INLINE_RUNTIME`：State、普通 Stream 和 Blob FSM 由 Runtime/Session 搬运；
- `EXTERNAL_MEDIA`：只用于 MediaStream。完整 unit 可超过 Runtime frame，但必须有
  Profile 固定的 unit/fragment/buffer/backpressure 上限和 Session-bound binding。

External media 仍校验 exact SessionId、catalog generation、route、endpoint incarnation、
production/media epoch 和 nonce。任一 scope 变化先使旧 bind 失效；partial unit
不得对 SDK 可见。Media failure 通过 control actual/failure 可观测；control
Session terminal 关闭 media transport。

当前正式 Composition Profile 为 `COMPOSE_/_PROFILE:v1`，只选择 MRT0 v4、
6 endpoints / 18 routes，并为 RenderMedia/CameraMedia data route 选择 external delivery。
具体合同见 [Composition Profile](../profiles/composition/README.md) 和
[Direct-IP MediaStream](media-stream-direct-ip.md)。MRT0 v2/v3 仍可用于内部 compatibility
fixture，但不是该正式 Profile 的 fallback。

所有 queue、history、reassembly、cache 和 retry 必须有界。Backpressure 只能
throttle、停止 source、替换有限 latest 或产生可观测 drop/gap/failure；不得无限缓存
或将丢失写成“未订阅”。

## Outbound scheduler

Core、Session wire 与 direct Runtime API 不拥有 route scheduling。集成层可为一个
Runtime/Session pair 创建 caller-owned scheduler，并成为该 pair 的唯一 TX owner。
Host 和 Edge 的 slot/counter/round 相互独立。

`TX_BUSY` 不是成功 slot，不得推进 scheduler、Runtime、Session 或 source。
State commit 和 Blob publication 的 RX obligation 必须先完成，不得被继续 TX 掩盖。
“成功 slot”只表示 Runtime frame 已排入 Session outbound，不表示 socket write、
peer receive、application commit 或时延保证。

## Blob 可见性

Blob 是有限、immutable、content-addressed 内容。在完整长度、ContentId、schema
和 Profile verifier 通过前，partial bytes 不可见。

```text
Runtime transfer complete
  -> Core BlobStore atomic publication
  -> Profile/SDK visibility in the current owner scope
```

Session loss、catalog replacement、offset discontinuity、abort 或 capacity failure 必须清理
未完 transfer。Content cache 可依 Profile 复用，但 cache hit 不恢复新 Session 的
route、desired、world 或 visibility。Blob route 不受 production gate 限制；publication
本身也不启动 producer 或激活 world。

## Catalog change、Session 与恢复

Catalog replacement 成功后，旧 route/control binding 全部失效，Host 重新发布
desired。Unplug 关闭 production 并在可交付时发布 unavailable/stream end；
replug 或 continuity 不可证明时使用新 endpoint incarnation 和 epoch。

每个新 Core Session 都重新交换 catalog 并发布 desired。地址相同不代表可复用
binding。Session 丢失后，Host-driven production 默认停止；只有 Profile 显式声明
bounded offline behavior 时才能暂时继续。State truth 可以重发，Action 不自动
replay，Blob bytes 可按 content identity 复用，但 transfer/visibility 不继承。

## 失败与验收

Runtime 至少区分：Core/Session 未就绪；catalog capacity/malformed/stale；
unknown/stale endpoint 与 resource；schema/parameter/QoS mismatch；desired conflict/stale；
production not running；Blob busy/discontinuity/length/integrity/capacity/receiver failure；permission/
admission/apply failure；payload capacity、backpressure 和 protocol error。

任一失败不得部分激活 catalog、desired、actual、Blob 或 production。
正式实现必须证明：READY 前零 Runtime mutation；原子 catalog；desired/actual
分层；actual 前零新 epoch data；disable/reconfigure cut；wrong scope 全拒绝；
queue/payload 过载有界可观测；Blob 完整验证；new Session 不恢复旧对象。

## 层级责任

| 层 | 责任 |
|---|---|
| Core | peer/Session identity、handshake、limits、Resource delivery/failure |
| Runtime | catalog、desired/actual、production lifecycle、route 与 Blob transfer |
| Profile | endpoint/schema、parameters、capacity、QoS、delivery 与 recovery selection |
| Adapter | 真实 probe/apply/production、timestamp 和 native handle |
| Host SDK | typed discovery/configure/view；不暴露 transport |
| Application | 表达 desired 并消费 actual/data；不推断硬件拓扑 |
