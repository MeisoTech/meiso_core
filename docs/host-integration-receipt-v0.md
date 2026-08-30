# Host 集成回执 v0

> 状态：`CURRENT COMPOSITION HOST INTEGRATION`。本页是Host接入指南，不是新的产品规范。
> 产品语义见[文档入口](index.md)，逐commit证据已[归档](archive/host-integration-history-v0.md)。

## 当前可接入面

Host application 只依赖 opaque `MeisoEdgeHandle`、typed command/snapshot 和 media borrow/release。
当前 `COMPOSE_/_PROFILE:v1` 固定：

- `MSL0` Session wire v1 + `MRT0` Runtime wire v4；
- 6 endpoints / 18 routes：Wi-Fi、Presentation、Scene/Blob/RenderResult、DeviceTelemetry、
  RenderPreviewMedia、CameraMedia；
- control走Session-bound Runtime，两路大帧走独立但同Session scope的media TCP；
- browser/Dashboard只调native Host的typed面，不持Session、route、epoch、nonce或wire。

## 建立顺序

```text
numeric TCP connected
  -> OPEN / ACCEPT / CONFIRM / READY
  -> exact MRT0 v4 catalog 6/18 accepted
  -> Host desired State delivered
  -> Edge actual State observed + applied + accepted
  -> endpoint production RUNNING
  -> selected external-media OPEN/BIND
  -> Stream / Blob / complete media units
```

1. locator只来自本地config，不作为peer identity。
2. already-connected nonblocking fd交给POSIX adapter；adapter不负责connect/close/reconnect/auth。
3. 每次control TCP创建fresh Session arena、nonce、`SessionId`和handle；`READY`前零Runtime。
4. catalog整体accept/reject；不接受endpoint子集或wire fallback。
5. desired delivery、Edge observed、apply、actual accepted、production和首个sample是独立进展。
6. media只在两个media desired/actual都与当前truth精确收敛后建立；新epoch/config使旧bind失效。
7. disable停止新生产并有界排空；`STOPPED`不等于Session close。

## 终止与重连

- Session/Runtime/HostRuntime terminal 是sticky；原handle不reset、不resume、不复活旧view。
- 新TCP必须fresh Session并重做catalog、desired、actual、Blob visibility和media bind。
- State owner可重发仍有效的完整truth；Stream从新epoch/recovery point起步；Action不自动重放；
  content-addressed Blob bytes可复用，但transfer/visibility不继承。
- Dashboard remote mode仅实现host-local supervisor：可恢复browser拥有的最新完整scene/intent，
  不恢复旧Session或保证Edge进程已重启。
- 当前Session v1无产品心跳；黑洞但不关闭的TCP可能长时间不被发现。SSE `keepalive`
  只维护HTTP client，与Meiso Link liveness无关。

## Host 必须保留的scope

| 对象 | 最小绑定 | 失效条件 |
|---|---|---|
| Edge handle | peer expectation、fresh Session/backend lifetime | terminal/close；不reset |
| endpoint/route | SessionId、catalog generation、endpoint incarnation、kind/schema/facet | 新Session/catalog/incarnation |
| State | owner/incarnation/version | stale/conflict/new owner |
| production | exact desired + applied actual + production epoch | stop/failure/new epoch |
| Stream/media | route、producer、stream epoch/sequence/dependency | gap/new epoch/bind terminal |
| Blob | content identity + current transfer/visibility scope | terminal result/new catalog/Session |
| Composition | capability/plan/world generation + State refs/token scope | supersede/new scope/terminal |

Host本地object、C++ layout、IPC、buffer/fence handle和editor revision不编入Meiso wire。

## Looking Glass 接入边界

Looking Glass HostRuntime应将一个backend映射到上述`EdgeHandle`：local scene/lane/buffer/fence仍属于
Host，跨端只使用当前Profile声明的State/Stream/Blob/MediaStream。Host不根据TCP ACK推断
actual、render complete或physical present。

## 尚未承诺

discovery、通用backend/plugin ABI、Session resume、自动Edge restart、安全transport、产品Link
heartbeat、State watch、Stream publish、Action invoke、Blob lookup、physical presentation和长期性能均未由本页承诺。
