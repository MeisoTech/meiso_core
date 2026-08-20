# Host 集成回执 v0

> 受众：Host Runtime / SDK 开发者。状态：当前可执行互操作基线；不是新的产品规范。
> 产品语义以[文档入口](index.md)列出的五份规范为准。可复现实装基线为
> [`7e53bc9`](https://github.com/anantheparty/meiso_glass/commit/7e53bc9)：包含既有 direct-IP
> Session/Runtime、R1 Blob carriage、R2a combined builder/fair scheduler，以及R2b0真实loopback
> happy-path增量。

## 结论

Host 现在可以直接复用 Meiso 的 portable C11 Resource、Session、Runtime、Wi-Fi Profile codec
与 POSIX connected-stream adapter；在其上封装自己的本地 object/SDK API。不要把 Host 本地
handle、C struct、Dashboard NDJSON 或旧 lab envelope 当作跨端协议。

当前已实现的portable Session对象链为：

```text
numeric IPv4/TCP connected fd（locator）
  -> Core Session: OPEN -> ACCEPT -> CONFIRM -> READY
  -> Runtime: Edge CatalogSnapshot -> Host ACCEPT
  -> Host desired Core State
  -> Edge actual Core State
  -> actual RUNNING 后 Edge Core Stream
  -> exact Composition Profile v2/MRT0 v3 下：Host Blob -> Edge Core BlobStore publication
  -> explicit Scene visibility -> Scene/Plan State -> C2 ACTIVE
  -> exact Wi-Fi / RenderResult Stream routes
```

没有 subscription。Host 不需要数据时发布 `desired.enabled=false`，Edge 停止生产并通过
actual `STOPPING -> STOPPED` 报告结果；不是继续采集后选择“不发送”。

## 最小连接流程

1. Host 从本地配置读取 numeric IPv4、port、local PeerId 与 expected Edge PeerId；地址只定位
   socket，PeerId 才参与协议 identity。部署值不得进入应用对象或 telemetry。
2. Host 建立 TCP 连接，设为 nonblocking；把 already-connected fd 交给
   `meiso_posix_tcp`。adapter 不负责 DNS、connect、close、重连或认证。
3. 为每次连接创建新的 `MeisoSession`、local incarnation 与 nonce；调用
   `meiso_session_start`，按 writable/readable 顺序驱动四步握手。协议/Profile/limits 必须
   exact match；`READY` 前不得发送 Runtime payload。
4. Core `READY` 后按exact Profile创建绑定该Session的Runtime：`wifi-telemetry-v0`和Composition
   Profile v1使用MRT0 v2，Composition Profile v2显式使用MRT0 v3；不得混用或fallback。Edge首先
   发送完整catalog；Host
   按 Profile、容量、endpoint/resource identity、owner、kind、schema 与 facet 验证后原子
   `ACCEPT` 或完整 `REJECT`，不得部分接受。
5. Host 用 `MeisoStatePublisher` 发布 desired，经 Runtime 发送 State offer；Edge 的 delivery
   acceptance、observed、apply、actual `RUNNING` 与首个 sample 是五个不同进展。
6. Host 收到 actual State 时，先交 `MeisoStateReplica`；只有 `RECEIVE_NEW` 再调用
   `meiso_runtime_commit_state_receive`。随后才接收同一 production epoch 的 Stream sample。
7. 重配置使用新的 desired State version；旧 actual/epoch 在 cut 前仍有效，新 actual 切换到
   新 epoch 后拒绝旧样本。停止后 Session 仍 `READY`；只有 Core normal close 才关闭连接。
8. transport loss、Core close 或新 TCP 连接都创建新 Session。新 Session 必须重新 catalog、
   desired、actual；v0 没有 resume、隐式 desired 恢复或旧 handle 复活。

同一 event-loop owner 必须串行驱动 Session/Runtime。Session RX payload 是短期 borrow；先同步
解码/复制并完成对应 Resource mutation，再进行下一个 non-const Session 调用。

## Host 对象管理

Host 可以保留自己的 opaque API handle，但至少把下列作用域纳入 handle validation；不要压成
一个通用 `object_id`：

| Host 本地对象 | 必须绑定 | 失效条件 |
|---|---|---|
| connection backend | locator、connected fd、event-loop owner | fd terminal；不代表 peer identity |
| Edge session | local/expected PeerId、SessionId、role、negotiated limits | close/loss；不可 reset |
| endpoint view | SessionId、catalog generation、EndpointId/incarnation | 新 Session、catalog replacement、incarnation retired |
| Resource route | endpoint view、ResourceId、kind/schema、facet | 任一 route identity 改变 |
| State view | route、owner scope、version | stale/conflict/new owner；delivery ACK 不等于 actual |
| production view | desired ref、actual effective config、production epoch | actual STOPPED、new epoch、catalog/Session invalidation |
| Stream cursor | producer incarnation、stream epoch、sequence、source clock epoch | continuity loss/new epoch；gap 必须显式暴露 |
| Blob transfer | SessionId、catalog/route、Runtime transfer ID、ContentId/total/offset | terminal RESULT、abort、catalog/Session discontinuity |
| Blob visibility | Core publication + current Profile/owner scope | cache 命中不恢复新 Session/world/desired/visibility |

建议 Host SDK 对应用暴露 `EdgeHandle / endpoint / typed State / typed Stream`，内部再映射到上述
tuple。应用不应看到 socket、PeerId、catalog wire、driver path 或 K230D 细节。

## 对 Host 项目对象的映射

| Host/SDC 概念 | Meiso 对接方式 |
|---|---|
| runtime connection / endpoint session | 本地 facade；底层绑定一次性 Core Session，不把本地 handle 编进 wire |
| object/descriptor query | accepted Runtime catalog 与 typed Resource descriptor；Edge 是 capability/actual truth owner |
| control object | Host desired State；有副作用且需要结果时才使用 Action，不创建通用 RPC |
| sensor/telemetry queue | Stream；producer/epoch/sequence/source time 不可省略 |
| peripheral resource mirror | 由 Edge catalog、capability 与 actual 驱动；Host 可以缓存镜像但不是第二 truth |
| logical layer / presentation batch | Host SDK 的 Composition plan producer；当前scene lab映射`PresentationPlan` State与Mesh Blob，MediaStream仍后置 |
| presentation receipt | Edge-owned `PresentationStatus`；Link ACK 不能冒充 prepared/active/presented |
| reset epoch | 分别映射 peer/Session、endpoint incarnation、production/stream/plan generation；不得合并 |

Host 项目的 preliminary C packing、remote wire 与 numeric IDs 尚未冻结，不能作为 Meiso codec。
最短路径是在 Host backend 直接链接当前 Meiso C 实现；若独立重写，必须逐字节复用 golden、
truncation、reserved/length、stale Session/catalog/epoch 与 failure vectors。

## 协议规范入口

| 层 | 当前权威/实现契约 |
|---|---|
| Resource 语义 | [Resource 契约](resource.md)；State / Stream / Action / Blob |
| Session | [Session Core v0](session-core-v0.md)：`MSL0`、wire v1、big-endian、七类 frame |
| Runtime | [Runtime 产品契约](runtime.md)与[Runtime Core v0](runtime-core-v0.md)：strict `MRT0` v2/v3 |
| 首个 Profile | [Wi-Fi telemetry v0](../profiles/wifi-telemetry-v0/README.md) |
| direct-IP 实装 | [Direct-IP Wi-Fi Runtime v0](wifi-runtime-direct-ip-v0.md) |
| Composition | [Composition 契约](composition.md)；[scene lab](../profiles/composition-scene-lab-v0/README.md) 已提供 C0--C2、R1 exact Blob、R2a portable 3/9/920 builder/fair scheduler与R2b0真实AF_INET loopback happy-path evidence；没有Host editor、target interoperability或combined fault/recovery evidence |

当前 Runtime source v0 承载 Host-owned desired State、Edge-owned actual State、Edge-owned data
Stream，以及MRT0 v3的Host-owned `IMMUTABLE/Blob` Host→Edge carriage。Action、MediaStream、
Edge→Host/并行/resumable Blob、typed dispatcher与稳定Host SDK/Virtual Edge尚未实现；R2a scheduler
是Composition Profile v2的独立Runtime--SDK对象，不是通用Host SDK。缺失能力必须明确`unsupported`，
不能用私有消息旁路。

## 已验证与禁止外推

K230D Linux Edge 与 macOS Host 已在真实 Wi-Fi direct-IP 上完成同一 Session 内启动、运行中
重配置、停止、再次启动和 normal close；还验证了 100 ms 下 64 个连续 sample，以及显式进程
重启后的完整重协商。准确 artifact、参数和 failure 边界见[系统进展](system-report.md)。

另一个独立的R2b0 Host test在真实`127.0.0.1` AF_INET `SOCK_STREAM`上使用双方nonblocking POSIX
adapter、17-byte I/O step与双方R2a scheduler，贯通Profile v2/MRT0 v3 exact 3/9/920 catalog、cube
Blob→Core publication→显式visibility、Scene/Plan→C2 `ACTIVE`，以及exact `WIFI_TLM/DATA_STR`与
`K230_SCN/RES_STR` route。production epoch为Wi-Fi `77`、Scene `42`、Presentation `41`；Stream分别为
epoch `177`/sequence `1`与epoch `417`/sequence `1`。fresh Debug与ASan+UBSan CTest均25/25，
clang analyzer为0 finding；GitHub Actions run `32316527393`四个job全部success。

R2b0结论严格为`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED / TRANSPORT_VERIFIED`（loopback happy
path）。这不证明稳定SDK、自动重连、wrong Profile、catalog replacement、backpressure、fair-contention
stress、长稳/性能、MediaStream、Composition target、discovery或安全transport。R1只证明portable Blob
semantic/wire与Core publish bridge；R2a只证明portable/in-process scheduler与builder；R2b0也不证明
K230D或renderer/physical output。
当前 plain TCP 的 PeerId exact check 只能防误连，不能提供认证、机密性或完整性；产品连接必须
另行绑定经审查的安全 transport。

## Host 提交门

- 使用公开 header/codec 或通过全部独立 golden/failure vectors；不复制 Dashboard/lab wire；
- app object 与 Session/catalog/endpoint/Resource/epoch 生命周期分离；旧 handle fail-closed；
- catalog 完整原子接受；desired delivery、actual 与 sample 分段可观察；无 subscription；
- new Session 零继承，重配置/stop 的旧、新 epoch cut 精确；
- simulated/loopback 与 physical Edge 使用相同 Profile 和业务 API；
- 不记录或输出 locator、PeerId、SSID/MAC、凭据和原始私有配置。
