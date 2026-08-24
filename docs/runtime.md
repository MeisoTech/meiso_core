# Runtime 契约

> 状态：产品级技术契约。定义 Resource Graph、endpoint catalog、Host desired、Edge actual、
> production lifecycle、动态插拔与 data route；不定义具体 sensor 型号、driver ABI、SDK
> facade 或 transport。

## 边界

Core 建立并约束 peer Session；Runtime 决定当前 Edge 有什么、Host 想让 endpoint 做什么、
Edge 实际应用了什么，以及由此产生哪些 Resource。SDK 把 Host Runtime 的结果投影成
Virtual Edge。

```text
Core READY
  -> Edge CatalogSnapshot
  -> Host CatalogAccept
  -> Host desired State(enabled + parameters + granularity)
  -> Edge admission + adapter apply
  -> Edge actual State(applied desired version + effective configuration)
  -> production RUNNING: every produced data item -> Runtime -> Core -> Host
  -> desired disabled: stop production -> actual STOPPED -> no new data
```

不存在独立 subscribe/unsubscribe control plane。若 Host 不需要某 endpoint 的数据，它修改
desired State 使 Edge 停止生产；不能让 Edge 继续生产，再在 Runtime 中静默丢弃或不发送。

`desired` 与 `actual` 不是 Runtime 自造的 control message：它们是 catalog 中声明的普通
Core State，由 `StatePublisher / StateReplica` 传递；sensor data 是普通 Core Stream，连续
媒体是 MediaStream。Runtime 只增加 endpoint/catalog/production route 的关联与门禁，不得
另建平行 desired/actual truth、subscription slot 或 delivery switch。

Core 不声明 sensor、参数或 endpoint。Runtime 不重新实现 peer identity、Session handshake
或四类 Resource 语义。IP、socket、OS path、I2C address、file descriptor 和 native buffer
handle 不进入本契约。

## Catalog 与 endpoint bundle

Edge Runtime 从以下交集构造 catalog：

```text
Profile 允许范围
  ∩ Edge 私有配置
  ∩ adapter 当前探测结果
  ∩ policy / permission / resource budget
```

`CatalogSnapshot` 是完整、不可变、原子接受的 Resource Graph，至少携带：

- Edge Runtime incarnation 内单调且非零的 `catalog_generation`；
- 每个稳定逻辑 `EndpointId`、当前 `endpoint_incarnation` 与 capability/schema；
- endpoint 所属 Resource descriptor、facet、owner、direction、limits 与 QoS/Profile 引用；
- desired、actual 与 data Resource 的明确关联，不靠名称或 Host 硬编码推断；
- actual/availability facet 的 identity，以及较大 capability/mode table 或 calibration 所引用的
  Blob；动态值由 actual State 表达；
- snapshot 的 entry count、容量边界和完整性信息。

Catalog 被接受后，其 Resource route 可以承载对应 owner 产生的 Resource message；这只表示
route identity 有效，不表示 sensor 正在采集。是否生产、以何种参数生产，只由 desired→actual
控制链决定。

Edge 私有配置决定逻辑 endpoint、允许的 mode/parameter envelope、driver owner 与本地
policy；adapter probe 决定当前是否真实存在、可用和能否 apply。它们不是第二套公开协议，
只能通过 catalog、capability 和 actual State 影响 Host 可见结果。Host 不依赖配置文件路径、
driver 名、bus address 或固定 sensor 数量。

Sensor 不是第五种 Resource。一个 endpoint bundle 可以包含：

| facet | Resource | owner / 含义 |
|---|---|---|
| capability | State 或 Blob | Edge；支持的 mode、range、format、rate、clock、limits |
| desired | State | Host；enabled、生产参数、颗粒度与 QoS intent |
| actual | State | Edge；availability、apply 状态、effective 参数、health、production epoch |
| data | Stream / MediaStream | Edge；在 actual 允许时产生的 sample、event、audio 或 frame |
| operation | Action | 确有副作用的 calibrate、reset、self-test 等离散操作 |
| immutable | Blob | calibration、mode table、model、有限 capture 等不可变内容 |

一个 endpoint 可以有零个或多个 data/operation/Blob Resource。

## Identity 与版本

- `EndpointId` 表示 Profile 内稳定的逻辑槽，不是总线地址或硬件序列号；
- `endpoint_incarnation` 在设备替换、adapter 重启或 continuity 无法证明时更新；
- `catalog_generation` 只在 endpoint/resource/schema/mode envelope 改变时更新；瞬时 health
  或 unavailable 优先由 actual State 表达；
- generation 一旦形成可判定的 offer（包括被拒绝的 offer）便不得用不同内容复用；
- desired 与 actual 各自遵循 State owner/version；actual 必须指出它观察或应用的 desired
  owner/version，迟到结果不能覆盖更新的 desired；
- 每次成功启动或不连续的重配置产生非零 `production_epoch`；data 必须引用当前 production
  epoch，Stream 继续携带 producer incarnation、stream epoch、sequence、source clock epoch/time；
- SessionId、catalog generation、endpoint incarnation、State version、production epoch 与
  stream epoch 不互相替代。

## Desired → actual → production

1. Core 未 `READY` 时，Runtime control/data 均不得发送或接受；
2. Edge 发布完整 catalog；Host 在容量内验证 Profile、schema、identity、facet、owner、limits
   和 endpoint bundle 一致性；
3. Host 只对完整 snapshot 返回 `ACCEPT` 或带明确原因的 `REJECT`，部分 entry 不可见；
4. catalog 双方接受后，Host 才发布 endpoint desired State；没有当前 desired 时，Host-driven
   production 默认不启动；
5. Edge 按 capability、permission、budget、availability 与本地安全 policy 做 admission，再
   调用唯一 truth-owner adapter；
6. Edge actual State 正交报告 availability、apply result、production status、observed/applied
   desired owner+version、effective 参数、reason 与 production epoch；因此“新 desired 被拒绝，
   旧配置仍在 RUNNING”可被准确表达。禁止静默换 format、rate、fields 或 range；
7. 唯一 data gate 是已被 Core State 语义接受的 actual 所声明的 effective-enabled、production
   status 与 production epoch；新 desired 未应用或被拒绝时，旧 applied desired/epoch 可以继续
   `RUNNING / DEGRADED / STOPPING`，其全部样本仍须发送；
8. Host Runtime 校验 catalog/endpoint/schema、actual/production epoch、freshness、gap 与 QoS
   后再交给 SDK。

Edge 为新 production epoch 发布的 actual State 成功进入有序 Core TX 前，不得产生该 epoch
的 public sample；Host 也只有在对应 actual State 经 `StateReplica` 接受后才开放接收 gate。
解码一帧 State 但尚未通过 Core State version/owner/conflict 检查，不得改变 gate。

Host desired 的本地提交、Core delivery acceptance、Edge observed、adapter apply、actual
`RUNNING` 与首个有效 sample 是不同进展，不能合并成一个成功布尔值。

### Disable 与重配置 cut

- desired `enabled=false` 只改变意图，不直接关闭 route；Edge 先发布仍引用旧 applied desired
  和 production epoch 的过渡 actual，停止 producer 后再发布 `STOPPED` actual；
- 重配置时，Edge 可以继续按旧 actual 生产，直到明确 cut；已经产生的数据仍必须发送，并带
  旧 production epoch；`STOPPING` 若仍允许产生数据，旧 gate 必须保持有效；
- 新 effective 配置必须使用新 production epoch；Host 不把旧 epoch sample 解释为新配置；
- 若参数不能支持，Edge 发布 `REJECTED` 或 `DEGRADED` 与明确 effective/reason，不部分静默应用。

## 数据交付与有界性

对于 catalog 中公开的 data Resource，“产生”和“向 Host 交付”不是两个独立业务开关。
本地 safety/control 使用的私有采样可以留在 adapter 内，但一旦被声明并物化为 public Resource
数据，就必须进入 Runtime/Core。

当 Core backpressure、deadline 或 transport failure 阻止交付时，Edge 必须按 Profile 的有界
规则 throttle、停止 source、保留有限 history，或产生可观察的 drop/gap/failure；不能无限缓存，
也不能把静默丢弃伪装成“未订阅”。MediaStream 可以使用专业化数据面，但仍共享相同 endpoint、
desired/actual、production epoch、Session 与 failure 语义。

### Inline 与 external media route

能够选择外部数据面的catalog revision必须为route显式声明delivery class；MRT0 v2/v3没有该字段，
其现有route按各自固定wire语义隐式为`INLINE_RUNTIME`：

- `INLINE_RUNTIME` 由当前 Runtime/Session payload 搬运 State、普通 Stream 或 Blob。State/Stream
  unit必须落在协商的Runtime frame上限内；Blob total可以更大，但只能通过Profile有界的
  begin/chunk/commit FSM传递，每个wire chunk仍受frame上限约束；
- `EXTERNAL_MEDIA` 只允许 Stream 的 MediaStream 专业化。完整 media unit 可以超过 Runtime
  payload，但 catalog 必须另行声明 Profile-bounded maximum unit、format/codec、buffer/backpressure
  与 transport arrangement；generic Runtime Stream send/receive 不得截断或分片冒充该 route。

external route 仍由相同 accepted catalog、Resource/endpoint identity、desired/actual、production
epoch 与 Session lifecycle约束。Profile-owned media binding必须验证exact SessionId、catalog generation、
route、endpoint incarnation、production epoch与media stream epoch；任一边界变化先使旧bind失效，
partial media payload不得对SDK可见。media transport failure必须通过control plane actual/failure变得
可观察；control Session terminal则关闭media transport。IP/socket/native buffer只属于adapter，
不会因此进入Runtime业务identity。

Composition scene Profile v1/v2继续分别选择MRT0 v2/v3且byte-exact不变。Profile v3选择MRT0 v4，
用新增catalog delivery class注册DeviceTelemetry、RenderPreviewMedia与CameraMedia；其选定的direct-IP
media arrangement见[Direct-IP MediaStream v0](media-stream-direct-ip-v0.md)。这是规范选择，当前公开
实现仍未提供MRT0 v4或MediaStream data path。

### Runtime--SDK outbound scheduler

Core、Session wire 与 direct Runtime API 不拥有 route scheduling。集成层可以为本地 peer 的 exact
Runtime/Session pair 建立一个 caller-owned scheduler，并成为该 pair 唯一 TX owner；Host 与 Edge 的
outbound slot、counter 和 round 必须彼此独立，full-duplex 不能合成全局序列。`TX_BUSY` 不算成功
slot，不能推进 scheduler、Runtime、Session 或 source；收到需要 State commit 或 Blob publication 的
Runtime event 时，scheduler 必须先完成对应 RX obligation，不能继续发送掩盖未决交付。这里的
successful slot 只表示 direct Runtime send 返回`OK`且frame已经排入Session outbound；不表示socket
write、peer receive、application commit或任何时间/延迟保证。

public commit `5d92ff6` 的 `meiso_runtime_scheduler` 是 Composition scene Profile v2 的一个 portable
实现：control source 持久 RR，最多连续 4 个成功 control slot；data round 冻结本轮 ready membership，
每个成员至多一次发送机会，`data_probe_due` 使 nonbusy control reject 后下一 step 先探测 data。
Profile v2 builder 无部分输出地固定 selected limits `8236/8192/12`、3 endpoint、9 route 与 canonical
920-byte catalog。确定性 vector 得到 Host 单 Blob data route 等待 `<=4+1=5` 个 Host outbound 成功
slot；Edge 的 Wi-Fi 与 RenderResult 同时 ready 时，每条 route 等待 `<=4+2=6` 个 Edge outbound成功
slot。这是该 Profile 的 in-process semantic evidence，不是通用 QoS、AF_INET combined transport、
K230/K1 target、Hz/latency/throughput 或性能保证。

RX obligation 只在 exact 条件成立时建立：State `OFFER`要求对应 commit；Blob只在接收`COMMIT`后
Runtime snapshot为`publish_pending && !result_pending`时要求Core publication。Blob length/SHA precheck
失败会直接形成terminal `RESULT`，没有publish obligation。active catalog generation变化进入
`REBIND_REQUIRED`；Session terminal则永久进入`SESSION_TERMINAL`并exact-once通知sources，旧对象不能
rebind，必须创建新Session/Runtime/Scheduler。

public commit `7e53bc9` 的R2b0把该Composition Profile v2组合接入真实`127.0.0.1` AF_INET
`SOCK_STREAM`：Host/Edge各自使用nonblocking POSIX adapter与R2a scheduler，I/O step固定17 bytes，
双方Runtime frame均观察到多次`WRITE_PROGRESS`和`READ_PROGRESS`。同一MRT0 v3 Session接受exact
3 endpoint/9 route/920-byte catalog，随后传递cube Blob、Wi-Fi/Scene/Presentation State与两条
Stream；没有新增wire、Core或direct Runtime API。该结果是loopback happy-path
`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED / TRANSPORT_VERIFIED`，不证明wrong Profile、catalog
replacement、reconnect、backpressure、fair-contention、K230/renderer、性能或transport security。
fresh Debug与ASan+UBSan CTest均为25/25，clang analyzer为0 finding；GitHub Actions run
`32316527393`四个job全部success。

public commit `9daa7f9` 的R2b1沿用上述同一真实TCP Session/catalog/Plan/cube：所有desired/actual
State都完成offer→replica commit→accept→publisher confirmed闭环并保持byte-exact。Scene从world 1/
RenderResult sequence 1依次经过TRS world 2/sequence 3、camera world 3/sequence 4、第二个共享cube
object world 4/sequence 5及删除首对象world 5/sequence 6；version 2 pending阶段仍可发布old-world
sequence 2。Scene actual epoch `43..46`使old/new data gate严格在actual前后切换，Plan 1 ref/digest、
Blob count/bytes/publication/transfer high-water全程不变。prefix-only Presentation desired v3先以drain
未完成返回`WAITING_DRAIN`且coordinator/SceneStore原字节不变，drained后退役；Presentation与Scene
actual精确进入`STOPPED`，RenderResult被拒为`ROUTE_NOT_ACTIVE`，Wi-Fi仍以epoch `77`发送sequence
`2`。Debug与ASan+UBSan full CTest均25/25，analyzer 0 finding；Actions run `32317542841`四个job
全部success。该证据只证明same-Session happy lifecycle，不证明catalog replacement、RST/reconnect、
max-Blob、wrong Profile、backpressure、K230/renderer、性能或transport security。

public commit `369ff4d` 的R2b2仍沿用同一真实TCP Session。catalog generation 2 offer只建立staged/
pending catalog，active generation 1与scheduler source boundary不变；accept后Host/Edge Runtime原子切到
generation 2，双方scheduler对每个source exact-once发出`CATALOG_CHANGED`并进入`REBIND_REQUIRED`。
旧generation 1 queued work返回`SOURCE_INVALID`且Session outbound保持空；所有route-bound source只接受
generation 2 rebind。SessionId、Session handle、fd、peer identity、negotiated limits与`READY`
均保持。

replacement清除三endpoint的actual/data gate但保留旧production high-water `77/41/46`。新的
capability-generation-2 coordinator推进control scope，清SceneStore world/visibility并使旧coordinator/
token/world失效；Core Blob cache仍命中，但Scene desired v6先得到`MISSING_BLOB`，g2 Blob transfer在
Core返回`DUPLICATE`后再显式`mark_visible`。Wi-Fi/Plan/Scene desired version为`2/6/6`，Wi-Fi/
Presentation/Scene actual version为`2/3/7`且production epoch为`88/51/52`；Plan generation 2使用
新token。旧production epoch `77/46`在scheduler拒绝，旧Stream epoch `177/417`在新gate下由Stream
拒绝；新`WIFI_TLM/DATA_STR` epoch `178`与`K230_SCN/RES_STR` epoch `418`均从sequence 1恢复。
Debug与ASan+UBSan full CTest均25/25，analyzer 0 finding；Actions run `32318799466`四个job全部
success。该证据只证明same-Session catalog replacement，不证明RST/reconnect、max-Blob、wrong
Profile、backpressure、K230/renderer、性能或transport security。

public commits `9df5268`、`e4a726f`与`43ae3ce` 完成R2b3的explicit same-process RST/reconnect slice。
`meiso_runtime_refresh_bound_session`在绑定Session仍为READY且SessionId、role、limits exact时返回`OK`
并保持完整Runtime arena byte-for-byte；Session unavailable/unknown/non-READY时首次原子清两代catalog、
pending State/Blob与Edge reassembly scratch，保留Blob transfer-id high-water并返回`CORE_NOT_READY`，
以后返回`STALE_SESSION`。live identity/role/limits mismatch首次即`STALE_SESSION`；Blob verifier callback内
refresh返回`CONFLICT`且Runtime arena零变化。Scheduler每次step在poll/TX前refresh；terminal先清Runtime，
再对每个source exact-once报告`SESSION_TERMINAL`，不产生额外Session outbound。

真实loopback transcript从g2 ACTIVE出发：一个distinct Mesh完成`BEGIN/READY/CHUNK`但不发`COMMIT`，
Edge adapter以`SO_LINGER` RST/close中止，Host read检测transport loss；两端Session均为
`FAILED/TRANSPORT_LOST`，旧Runtime/Scheduler/route/epoch永久失效，未完成Blob未进入Core。SceneStore
旧active world在transport loss后仍保持；新连接使用不同AF_INET sockets、SessionId及Session/Runtime/
Scheduler/coordinator arenas，新coordinator开始新control scope后才清world/visibility并使旧coordinator/
token/world失效。同process保留Core Blob cache和SceneStore，但desired、visibility、world、actual/data
gate均不自动恢复。
显式提高State delivery generation并重发cube后，Core返回`DUPLICATE`，`mark_visible`后再以Plan
generation 3/new token、Wi-Fi/Presentation/Scene actual epoch `99/61/62`、Wi-Fi/RenderResult epoch
`179/419` sequence 1恢复。
Linux CI首次暴露测试把scratch误认作arena tail的padding假设；`43ae3ce`改为从真实COMMIT Runtime-scratch
borrow校准offset并在同配置fixture重验范围，产品代码未变。Actions run `32321051775`四个job全部success。
public commit `a392850` 在R2b3的新AF_INET Session上完成R2b4 max-Blob/resource boundary。Host先把
262145-byte Mesh `BEGIN`本地拒绝为`PAYLOAD_CAPACITY`：source完成一次rejected completion且
`data_probe_due`置位，但successful slot、Session outbound、Runtime transfer/high-water、两端
Runtime/Session arena、Edge scheduler、Core BlobStore与SceneStore均不变。随后exact 262144-byte
canonical `MSH0`以64个4096-byte `CHUNK`传输；只有`BEGIN+READY`和`COMMIT+PUBLISHED`，没有逐块
ACK。第32块后Edge独立发送Wi-Fi epoch `179`/sequence `2`及old-world RenderResult epoch `419`/
sequence `2`；publish→显式`mark_visible`→Scene desired v8/swap→actual v10/production epoch `63`后，
RenderResult sequence `3`引用新world。

当前macOS arm64 ABI的R2b4五arena ledger为Host Runtime 17,384、Edge Runtime 279,528（含
262,144-byte reassembly scratch）、2-slot Mesh BlobStore 1,049,056、SceneStore 31,656、Coordinator
1,520 bytes，均alignment 8，合计1,379,144 bytes。逻辑账为BlobStore中cube 304 + max Mesh 262,144 =
262,448 bytes；active world只引用max Mesh的262,080-byte vertex+index区域，target/job仍为0。该五项
是当前HEAD/R2b4 fixture口径；上文R1 arena数字仍是`a8a777a` commit snapshot，两组不能混合相加。
该口径不含Session、scheduler、POSIX adapter、State/Stream、TCP kernel buffer、Host外部asset buffer或GPU/CMA；
2-slot是此fixture配置，不等于SCAP声明的16-slot aggregate。Actions run `32322475279`四个job全部
success。该证据不证明自动resume、fresh Edge process、wrong Profile、backpressure、K230/renderer、
16-slot aggregate、性能或transport security。

public commit `6488fef` 完成R2b5 adversarial TCP slice。真实AF_INET连接分别以Composition Profile v1
和Wi-Fi Profile对接Composition Profile v2；Edge在handshake中以`PROFILE_MISMATCH`失败，双方均未得到
SessionId，未进入catalog/Runtime frame，随后各自fail closed。匹配的Composition Profile v2正例仍到达
READY。

同一测试分别制造Edge→Host RenderResult和Host→Edge max-Blob `CHUNK`的真实nonblocking
`WRITE_WOULD_BLOCK`。Session保持READY且保有一个outbound frame时，下一次scheduler step返回
`TX_BUSY`；完整Scheduler/Runtime/Session/POSIX-adapter arena、source、output以及poll/completion原字节
不变。接收端恢复读取后，已排队RenderResult或Blob CHUNK按原payload/offset各接收一次，没有重发或
重复；Blob随后以显式ABORT终结且Core BlobStore仍为空。

direction-local fairness也只按本peer successful slot计数：Host本方向4个control后，等待的Blob CHUNK
精确成为第5个slot；Edge本方向4个control后，等待的Wi-Fi和RenderResult精确成为第5、6个slot并属于
同一冻结data round。每段中对端scheduler successful-slot计数不变。这两个方向是同一Session内依次
验证，不是同时socket saturation，也不是wall-clock、latency、throughput或performance上界。Actions run
`32324328746`四个job全部success。

## Blob route 与可见性

Blob route 只传递有限、immutable、带 ContentId 的完整内容；它不是连续数据、State snapshot 的
替代物或 subscription。Catalog 必须声明 owner、direction、schema 与 total-size capacity，接收端
在完整长度、content identity 与 Profile/schema verifier 同时通过前不得发布部分内容。

Blob transfer identity 属于当前 Session/catalog/route scope；Session loss、catalog replacement、
offset discontinuity、abort 或 capacity failure 必须清理未完成 transfer。已完整发布的 content cache
可以按 Profile 保留，但 cache 命中不恢复新 Session 的 route、desired、world 或 SDK visibility。
因此至少存在三层不可合并的 truth：

```text
Runtime transfer complete
  -> Core BlobStore atomic publication
  -> Profile/SDK current owner-scope visibility
```

Blob route 不受 endpoint production gate 控制，否则 scene/model 等资产无法在 desired prepare 前
到达；但 publication 本身也不启动 producer、不应用 State 或激活 Composition world。具体 wire
version、chunk、并行度、ACK/FSM 与 Core bridge 由 exact Profile/实现契约冻结。当前 portable
实现中仅`composition-scene-lab-v0`的Profile v1→MRT0 v2、Profile v2→MRT0 v3；该选择不泛化到
其他Profile。Composition scene的单transfer细节见
[Runtime negotiation core v0](runtime-core-v0.md)。

## 动态插拔与更新

- Profile 已知的可插拔逻辑槽可以保留在 catalog 中，由 actual State 表达 `ABSENT`；
- 真正新增/删除 Resource、schema 或 mode envelope 时，Edge 发布新的完整 generation；
- v0 catalog 改变时所有旧 route/control binding 失效，Host 必须接受新 snapshot 并重新发布
  desired；
- unplug 时 Edge 停止 production，发布 actual unavailable/stream end（仍可发送时），拒绝新
  Action，并使 SDK last-known view 带 stale/freshness；
- replug 或 continuity 无法证明时使用新的 endpoint incarnation、production epoch 与 stream
  epoch；不得复活旧 handle、desired binding 或 sample；
- catalog build/reject 不改变当前 active generation；只有完整新 snapshot 被接受后才原子切换。

## Session 与恢复

每个新 Core Session 都重新交换并接受 catalog，Host 随后重新发布 desired State；地址相同不
表示可以复用旧 control/data binding。Session 丢失时，Host-driven production 默认停止；Profile
只有在明确声明 bounded offline retention 时才能继续。State truth 可以重新 offer，Action 不
自动重放，Blob bytes 是否复用由 content identity/Profile 决定，但 transfer 与当前 visibility
不得从旧 Session 继承。

若 Host 首次连接时接收一个连续 producer 的 Stream，首样本 sequence 可以大于 1，
`missing_before` 必须显式暴露；不能为新 Host 重置 Edge sequence 或静默吞掉 gap。

## 失败

Runtime 至少区分：

- `CORE_NOT_READY / STALE_SESSION`；
- `CATALOG_CAPACITY / MALFORMED_CATALOG / STALE_CATALOG`；
- `UNKNOWN_ENDPOINT / STALE_ENDPOINT_INCARNATION / UNKNOWN_RESOURCE`；
- `SCHEMA_MISMATCH / UNSUPPORTED_PARAMETER / QOS_UNSUPPORTED`；
- `STALE_DESIRED / DESIRED_CONFLICT / ACTUAL_MISMATCH / PRODUCTION_NOT_RUNNING`；
- `BLOB_BUSY / BLOB_DISCONTINUITY / BLOB_LENGTH_MISMATCH / BLOB_INTEGRITY_FAILED /
  BLOB_CAPACITY / BLOB_RECEIVER_FAILURE`；
- `PERMISSION_DENIED / ADMISSION_DENIED / UNAVAILABLE / APPLY_FAILED`；
- `PAYLOAD_CAPACITY / BACKPRESSURE / PROTOCOL_ERROR`。

任一失败不得部分激活 catalog、desired、actual 或 production。catalog staging、State、route、
queue、history 和 pending apply 都有协商或 Profile 固定上限，并暴露 capacity/drop/failure。

## 层级责任

| 层 | 责任 |
|---|---|
| Core | peer/Session identity、handshake、limits、bounded delivery/failure；不理解 sensor |
| Edge Runtime | catalog、desired apply、actual、production lifecycle、route 与 telemetry |
| Host Runtime | catalog mirror、desired truth、actual/gap/freshness 处理 |
| Profile | endpoint schema、参数/颗粒度、QoS、capacity、production cut、hotplug、recovery |
| Adapter | 私有映射、动态检测、真实生产/控制、timestamp 与 native handle |
| Host SDK | typed discovery/configure、Virtual Edge、stale handle；不暴露 transport |
| App | 表达 desired 并消费 actual/data；不推断硬件拓扑 |

## 验收条件

正式 Runtime 实现至少证明：

- Core `READY` 前零 Runtime mutation；新 Session 必须重新协商 catalog 与 desired；
- catalog snapshot 原子接受，stale/conflict/capacity 不部分提交；
- Host desired 可以启停 endpoint、选择参数和数据颗粒度，unsupported 参数明确 reject；
- desired delivery acceptance 不冒充 Edge actual；actual production gate 未激活时不产生 data；
- desired disable 后停止 production；已经产生的数据不因独立 delivery 开关被静默丢弃；
- reconfigure 使用新 production epoch，旧数据不能冒充新 effective 配置；
- unplug/update 关闭旧 production，replug 使用新 endpoint incarnation/epoch；
- wrong Session/catalog/endpoint/resource/kind/schema/State version/production epoch 全部拒绝；
- queue、State、route 与 payload 过载仍有界且可观察；
- Blob 完整验证前不可见；wrong Session/catalog/route/owner/ContentId/offset/phase 以及 Core publish
  failure 均不产生部分 publication，新 Session 不继承 transfer 或 visibility；
- simulated Edge、真实 Edge 和 Host SDK 使用同一 catalog/desired/actual/data 语义。
