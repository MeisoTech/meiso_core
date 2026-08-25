# Host 集成回执 v0

> 受众：Host Runtime / SDK 开发者。状态：当前可执行互操作基线；不是新的产品规范。
> 产品语义以[文档入口](index.md)列出的五份规范为准。可复现实装基线为
> [`6488fef`](https://github.com/anantheparty/meiso_glass/commit/6488fef)：包含既有 direct-IP
> Session/Runtime、R1 Blob carriage、R2a combined builder/fair scheduler，以及R2b0真实loopback
> happy path、R2b1 same-Session lifecycle、R2b2 atomic catalog replacement与R2b3 explicit
> same-process RST/new-Session recovery、R2b4 max-Blob/resource boundary及R2b5 adversarial TCP增量。

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

当前首个Composition Profile v3 Host SDK切片已经用opaque `MeisoEdgeHandle`把accepted endpoint/
resource view、typed desired/actual/Stream/Blob/Composition snapshot与external-media latest/release
映射到上述tuple。它的integration adapter仍借用并驱动既有HostApp；应用不看到socket、Session/
Runtime pump、route key、nonce、catalog wire、driver path或K230D细节。该实现不是通用backend ABI；
State watch、Stream publish、Action invoke与Blob lookup仍以capability false明确缺失。

现有macOS Composition Debug Host已作为首个真实消费者迁移到该facade：业务命令、typed snapshot与
media borrow不再直接读取HostApp；HostApp只留在fd/pump/slot/abort integration。既有NDJSON与CMFB
输出保持不变，因此此迁移验证的是应用边界，不新增协议或证据标签。

## 对 Host 项目对象的映射

| Host/SDC 概念 | Meiso 对接方式 |
|---|---|
| runtime connection / endpoint session | 本地 facade；底层绑定一次性 Core Session，不把本地 handle 编进 wire |
| object/descriptor query | accepted Runtime catalog 与 typed Resource descriptor；Edge 是 capability/actual truth owner |
| control object | Host desired State；有副作用且需要结果时才使用 Action，不创建通用 RPC |
| sensor/telemetry queue | Stream；producer/epoch/sequence/source time 不可省略 |
| peripheral resource mirror | 由 Edge catalog、capability 与 actual 驱动；Host 可以缓存镜像但不是第二 truth |
| logical layer / presentation batch | Host SDK 的 Composition plan producer；scene lab映射`PresentationPlan` State与Mesh Blob，Profile v3 facade另投影两路external media read；physical present仍后置 |
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
| Composition | [Composition 契约](composition.md)；[scene lab](../profiles/composition-scene-lab-v0/README.md) 已提供 C0--C2、R1 exact Blob、R2a portable scheduler、R2b0--R2b5 loopback lifecycle/failure evidence，Profile v3另有local Host editor与一次受控target combined evidence；没有physical presentation、自动resume、通用fresh Edge或长期target evidence |

当前 Runtime source v0 承载 Host-owned desired State、Edge-owned actual State、Edge-owned data
Stream，以及MRT0 v3的Host-owned `IMMUTABLE/Blob` Host→Edge carriage。Composition Profile v3
已有固定HostApp backend的typed SDK/Virtual Edge facade及两路external MediaStream read；另一个
caller-owned deterministic simulator通过同一handle实现Wi-Fi desired→Actual/Stream和render/camera
media replay，并可由config显式启用deterministic DeviceTelemetry desired→Actual/Stream；固定v1 trace只把
Wi-Fi/media synthetic子集编码为bounded canonical bytes，并以fresh simulator
逐record重放。trace不打开文件、不持fd，也不能替代network/target/physical evidence。两者均明确不实现
Blob/Composition。Action、通用typed dispatcher、Edge→Host/并行/
resumable Blob与开放plugin backend ABI仍未实现；R2a scheduler
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

public `9daa7f9` 在同一Session中保持Plan 1 ref/digest与cube Blob publication不变，顺序验证world/
RenderResult `1/1`、pending old `1/2`、TRS `2/3`、camera `3/4`、共享cube第二对象`4/5`、删除首对象
`5/6`，以及Scene actual epoch `43..46`前后的data gate。所有State均闭环且byte-exact；Blob没有重传，
count/bytes/publication/high-water不变。prefix-only disable先`WAITING_DRAIN` raw zero-diff，drained后
Presentation/Scene actual进入`STOPPED`、RenderResult得到`ROUTE_NOT_ACTIVE`，同时Wi-Fi epoch `77`/
sequence `2`继续。Debug与ASan+UBSan full CTest均25/25，analyzer 0；Actions run `32317542841`
四个job全部success。

public `369ff4d` 在同一Session内把catalog g1 staged后atomic accept为g2；双方scheduler每个source
exact-once收到`CATALOG_CHANGED/REBIND_REQUIRED`，stale g1 work为`SOURCE_INVALID`且不写socket，
所有route再绑定g2。SessionId/handle/fd/peer/limits/READY不变，Runtime gate清除但high-water保留。
fresh capability-generation-2 coordinator重建scope/visibility/world；Blob cache保留但不能恢复
visibility，Scene v6先`MISSING_BLOB`，g2 transfer在Core得到`DUPLICATE`后才显式`mark_visible`。
Plan generation 2/new token与actual epoch `88/51/52`恢复控制；旧production/Stream epoch拒绝，
新Wi-Fi/RenderResult epoch `178/418`均从sequence 1恢复。Debug与ASan+UBSan full CTest均25/25，
analyzer 0；Actions run `32318799466`四个job全部success。

public `9df5268`、`e4a726f`与`43ae3ce` 完成explicit same-process RST/reconnect：READY exact Runtime
refresh byte-preserve；terminal首次在scheduler poll/TX前清catalog、pending State/Blob和Edge scratch，
保留Blob transfer-id high-water，随后旧Runtime永久stale，source只收到一次terminal boundary。g2 ACTIVE
后的distinct Mesh只到`BEGIN/READY/CHUNK`，Edge以`SO_LINGER` RST/close中止；双方Session均为
`FAILED/TRANSPORT_LOST`，第二Blob未发布，旧Runtime/Scheduler/route/epoch失效；SceneStore旧world
保持到新coordinator开始新control scope，此时才清world/visibility并使旧coordinator/token/world失效。
新AF_INET Session使用不同SessionId和新arena；Core cache/SceneStore虽在同process保留，desired/
visibility/world/gate仍须显式重建。cube重发得到Core
`DUPLICATE`后再`mark_visible`，Plan generation 3、Wi-Fi/Presentation/Scene actual epoch `99/61/62`及
Wi-Fi/RenderResult epoch `179/419` sequence 1恢复。Actions run `32321051775`四个job全部success。

public `a392850` 在该新Session上传输exact 262144-byte canonical `MSH0`：64个4096-byte `CHUNK`只有
BEGIN `READY`与COMMIT `PUBLISHED`，没有逐块ACK；262145-byte `BEGIN`在Host本地capacity拒绝且无
wire/transfer/Core/Scene副作用。第32块后Edge反向Wi-Fi epoch `179`/sequence `2`及old-world
RenderResult epoch `419`/sequence `2`仍各自前进；publish、显式visibility及scene swap后RenderResult
sequence `3`引用新world。Actions run `32322475279`四个job全部success。

public `6488fef` 又在真实AF_INET上完成R2b5：Composition v1或Wi-Fi Profile对Composition v2均在
handshake fail closed且不进入SessionId/catalog/Runtime；Edge→Host RenderResult与Host→Edge Blob
`CHUNK`分别触发真实`WRITE_WOULD_BLOCK`，随后的`TX_BUSY`对完整scheduler/runtime/session/adapter
arena、source与output原字节零推进，排空后已排队frame各到达一次。方向独立fairness精确为Host
4 control + Blob的5 slots、Edge 4 control + 同一data round的Wi-Fi/RenderResult共6 slots；两段依次
执行，不表示同时socket saturation或时延/性能界。Actions run `32324328746`四个job全部success。

R2b0--R2b5结论严格为`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED / TRANSPORT_VERIFIED`
（loopback same-Session lifecycle/catalog replacement及explicit same-process reconnect）。后续首个
Composition Profile v3 `EdgeHandle`切片另以真实AF_INET HostApp/Edge fixture验证exact 6/18 discovery、
typed Wi-Fi State/Stream、camera media borrow/release与terminal清理，但这仍不证明通用SDK backend、
自动resume、fresh Edge、SCAP 16-slot aggregate、未覆盖的fair-contention/negative matrix、长稳/性能、通用或physical MediaStream、Composition
target、discovery或安全transport。R1只证明portable Blob
semantic/wire与Core publish bridge；R2a只证明portable/in-process scheduler与builder；R2b0--R2b5也不证明
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
