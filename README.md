# Meiso Glass

Meiso Glass 是面向分体式 XR/AR 的开放 Resource 与 Composition 契约。

- `Edge` 拥有实际设备 truth、platform adapter、本地安全行为和已声明的物理输出；
- `Host` 运行应用与重计算，通过 SDK 的 Virtual Edge 操作远端或模拟 Edge；
- 公共 Resource 为 `State / Stream / Action / Blob`；MediaStream 是 Stream 的专业化；
- Session、runtime、sensor、frontend、system image 和 transport implementation 可替换，
  具体有效组合由 capability 与经过验证的 Profile 表达。

项目不绑定某颗 SoC、开发板、操作系统、图形 API 或网络载体，也不规定组件实现顺序。

公开边界为三层：Core 拥有 identity、connection、handshake、Session 与 Resource delivery
primitive；Runtime 拥有 Resource Graph、capability、endpoint 和控制逻辑；Host SDK 拥有
Virtual Edge 与应用 API。IP、port 和 TCP adapter 属于 bootstrap/transport，不是 peer identity。

## 当前代码状态

当前 `main` 包含 portable C11 `meiso_resource_core`：共同 identity 与 descriptor 校验、
固定且可 seal 的 descriptor snapshot，以及 caller-owned arena 上的 bounded
`State / Stream / Action / Blob` primitive。它们是 Resource semantic core v0 的
source-level implementation API，不冻结二进制 ABI。

当前还包含 caller-owned arena 上的 `meiso_session_core`：exact protocol/Profile、
`OPEN/ACCEPT/CONFIRM/READY`、bounded wire 与 Runtime-message gate。命名的 Session
状态机、wire 与固定 Profile 的 four-kind 纵向 integration CI 支持
`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED`。

当前 portable `meiso_runtime` 已实现 State/Stream-driven negotiation source v0，以及严格分离的
MRT0 wire v2/v3。v2 保留原六类 frame 与既有 Profile；v3 在相同 Session Runtime gate 内增加
Host→Edge、Host-owned `IMMUTABLE/Blob` carriage。每个 Runtime handle 全局只允许一个 transfer，
完整长度、SHA-256 与 Profile verifier 在同一 Core BlobStore verification snapshot 上通过后才发布；
Blob publication 与 SceneStore/SDK visibility 仍是两层。commit `a8a777a` 的独立 literal/FSM/Core
bridge vectors 支持 `HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED`，不构成 transport、scheduler
fairness 或 target evidence。

POSIX build 另含 platform-layer connected-stream adapter。它借用 already-connected、
nonblocking `SOCK_STREAM` fd，以 caller-owned bounded RX/TX buffer 对接完整 Session frame；
真实 macOS `AF_INET` loopback TCP unit test 与 Dev Tool 支持该条件下的
`TRANSPORT_VERIFIED`。这不证明 remote/direct-IP、Wi-Fi、K230D 或安全 transport。

首个正式 Runtime 垂直切片已选择 `wifi-telemetry-v0`：本机真实 `AF_INET` 测试贯通 Core
READY、catalog、Host desired State、Edge actual State、production gate 与 Core Stream；macOS
Host Dev Tool 和 K230D Linux Edge target 使用 numeric direct-IP 私有配置；同一路径已在
K230D Linux 与 macOS 间验证单一 Core Session 内的启动、运行中重配置、停止与正常关闭。
native Host 的 `--interactive` 模式在同一 READY Session 中以
`apply(enabled=true/false)` 和 `close` 实现启动、停止与关闭；
desired/actual 都是 Core State，数据是 Core Stream，不存在 subscription control plane。
构建、运行与严格证据边界见
[Direct-IP Wi-Fi Runtime v0 实装规则书](docs/wifi-runtime-direct-ip-v0.md)。
同一正式 Host 可由 loopback-only
[Wi-Fi Runtime dashboard](devtools/web/wifi_runtime_dashboard/README.md) 启动并可视化；Web 只把
`connect/apply/stop/close` 固定意图交给 native Host，并投影 Host 已校验的脱敏事件。
它不解析 wire，不实现 Runtime/SDK/Virtual Edge，也不是 subscription 或第二套
协议 truth。

这套实现仍不是 application-facing 完整产品 Runtime 或产品系统；它不包含 endpoint
facet/direction/QoS/permission schema、通用 multi-endpoint probe/hotplug orchestration、typed
dispatcher、Virtual Edge/SDK、Composition Runtime、MediaStream data path、通用 discovery/
reconnect/service、认证或安全 transport。所选 Wi-Fi target 的 config/probe 与 connect/listen
是 platform slice，不进入 portable Core。
K230D conformance target 已完成 host-side cross-build、image inspection，并在
`CanMV-K230D-Zero V1.1` 上得到七套 suite 全部通过的 `BOARD_VERIFIED` evidence。这个状态只
覆盖 Resource semantic core v0 的单进程 target runner，不构成 remote backend、LinkSession、
Composition、MediaStream、完整冷启动或产品 Profile evidence。各层验证范围见
[系统进展](docs/system-report.md)。

当前 Host 团队接入顺序、handle 生命周期与协议入口已收敛为
[Host 集成回执 v0](docs/host-integration-receipt-v0.md)。Edge local compositor 的边界是
Runtime--SDK/application integration：Host 产生 logical plan，Edge 独占 prepare/activate/safety/
scanout，renderer 只产生 candidate/failure，Core 不变。模块与 K230D backend 计划见
[Edge local compositor v0](docs/edge-local-compositor-v0.md)。首个
[Composition scene lab v0](profiles/composition-scene-lab-v0/README.md) 的 C0 codec、C1 bounded
SceneStore 与 C2 portable coordinator 已完成；commit `a8a777a` 又完成 R1 Runtime v3 Blob carriage。
该 Profile 的 v1 ProfileRef 继续选择 MRT0 v2，v2 ProfileRef 才显式选择 MRT0 v3；现有 Wi-Fi
Profile 及 v2 golden 不受影响。R1 已验证 Blob wire/FSM、Session/catalog discontinuity 时清 Runtime
transfer/scratch，以及 Core BlobStore publication 与 SceneStore visibility 的分层；catalog-only
replacement 的外部 visibility 策略留给后续 Profile/SceneStore integration。R1提交自身不包含scheduler；public
commit `5d92ff6` 已在独立 Runtime--SDK 层完成 R2a portable combined builder/scheduler evidence：
Profile v2 builder 无部分输出地固定 3 endpoint、9 route 与 920-byte catalog；
Host、Edge 分别按本 peer outbound successful slot（direct Runtime send返回`OK`且frame已排入Session
outbound）执行control RR、最多4帧control burst与冻结的data round，确定性上界分别为Host `<=5`、
Edge两条data route各 `<=6`；该slot不表示socket write、peer receive或时延。R2a还验证State/Blob RX
obligation：State `OFFER`须commit；Blob仅在`COMMIT`后`publish_pending && !result_pending`时须publish，
length/SHA precheck失败直接产生terminal `RESULT`。`TX_BUSY`时Scheduler/Runtime/Session三个arena原字节
不变；它没有修改Core、Session wire或direct Runtime API。public commit `7e53bc9` 又完成R2b0
happy-path loopback：真实`127.0.0.1` AF_INET `SOCK_STREAM`两端使用nonblocking POSIX adapter与
17-byte I/O step，双方Runtime frame均实测multi `WRITE/READ_PROGRESS`；同一Profile v2/MRT0 v3
Session贯通3 endpoint/9 route/920-byte catalog、双方R2a scheduler、cube Blob→Core publish→显式
Scene visibility、Scene/Plan State→C2 `ACTIVE`、独立Wi-Fi/RenderResult Stream。它只支持loopback
`HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED / TRANSPORT_VERIFIED`；fresh Debug与ASan+UBSan CTest均
25/25，clang analyzer为0 finding，GitHub Actions run `32316527393`四个job全部success。public commit
`9daa7f9` 又在同一真实TCP Session中完成R2b1 lifecycle：保持PlanRef/digest与cube Blob publication
不变，依次得到world/RenderResult sequence `1/1`、TRS `2/3`、camera `3/4`、共享cube的第二对象
`4/5`、删除首对象`5/6`，并验证pending old result `1/2`、actual epoch `43..46`前后gate、drained
prefix-only disable及Wi-Fi epoch `77`/sequence `2`继续。Debug与ASan+UBSan均25/25，analyzer 0；
Actions run `32317542841`四个job全部success。public commit `369ff4d` 又完成R2b2：同一TCP Session
内catalog g2 staged不改变active g1，accept后原子切换；双方scheduler全部route source重新绑定，
旧g1 work无socket输出。Session identity/handle/fd/limits/READY保持，Runtime gate清除但high-water
保留；capability generation 2重建coordinator scope/visibility/world，复用Core Blob cache时先得到
`MISSING_BLOB`，经g2 transfer `DUPLICATE`后显式恢复visibility，再以Plan generation 2、新actual/
Stream epoch恢复两条sequence 1。Debug与ASan+UBSan均25/25，analyzer 0；Actions run
`32318799466`四个job全部success。public commits `9df5268`、`e4a726f`与`43ae3ce` 又完成R2b3的
explicit same-process RST/reconnect slice：Runtime可显式refresh其永久绑定Session，READY exact匹配时
byte-preserve；terminal首次原子清catalog、pending State/Blob与Edge scratch并返回`CORE_NOT_READY`，
Blob transfer-id high-water保留，随后永久`STALE_SESSION`。Scheduler在poll/TX前自动refresh并exact-once
通知terminal。真实g2 ACTIVE连接在一个未`COMMIT`的distinct Mesh transfer中由Edge发RST；RST使旧
Session/Runtime/Scheduler/route/epoch fail closed，SceneStore旧world保持到新coordinator开始新control
scope，随后旧coordinator/token/world失效。新AF_INET连接使用新SessionId与全新Session/Runtime/
Scheduler arena，只复用同process Core cache与SceneStore；desired、visibility、world和gate均未自动继承，
显式重发后以Core `DUPLICATE`+`mark_visible`、Plan generation 3、
Wi-Fi/Presentation/Scene actual epoch `99/61/62`及Wi-Fi/RenderResult epoch `179/419` sequence 1恢复。Actions run
`32321051775`四个job全部success。public commit `a392850` 又在该新Session上完成R2b4 max-Blob/resource
boundary：一个exact 262144-byte canonical `MSH0`以64个4096-byte `CHUNK`完成传输，只有
`BEGIN/READY`和`COMMIT/PUBLISHED`两次结果、没有逐块ACK；262145-byte `BEGIN`在Host本地以
`PAYLOAD_CAPACITY`拒绝且不产生wire/Runtime transfer/Core或Scene副作用。传输中点Edge反向发送Wi-Fi
epoch `179`/sequence `2`及old-world RenderResult epoch `419`/sequence `2`，证明两个peer outbound
scheduler独立；publish、显式visibility与scene swap后RenderResult sequence `3`引用新world。Actions run
`32322475279`四个job全部success。public commit `6488fef` 又完成R2b5 adversarial TCP slice：真实
AF_INET上Composition v1与Wi-Fi Profile连接Composition v2均在handshake中fail closed，未建立
SessionId、catalog或Runtime frame；Edge→Host RenderResult与Host→Edge Blob `CHUNK`分别把本方向
nonblocking adapter推进到`WRITE_WOULD_BLOCK`，此后的scheduler `TX_BUSY`对Scheduler/Runtime/Session/
adapter arena、source与output原字节零推进，排空后每个已排队frame只接收一次。direction-local
fairness在两个独立阶段验证：Host的4个control后Blob data是第5个successful slot；Edge的4个control后
Wi-Fi与RenderResult是同一冻结data round的第5/6个slot，另一方向scheduler计数不动。Actions run
`32324328746`四个job全部success。该结果不表示两方向同时达到socket saturation，不给出wall-clock、
latency、throughput或performance保证，也不证明自动resume、fresh Edge process、SCAP 16-slot
aggregate、K230D/renderer backend、安全transport、offline lease执行或physical presentation。

public commit `c3b4770` 已加入 caller-owned、bounded 的 portable synthetic reference renderer，
状态为 `HOST_OFFSCREEN_REFERENCE_VERIFIED`。它固定320×240 RGBA8888与implementation-depth32、两个
target set和两个job；当前macOS arm64最小arena为1,268,872 bytes/alignment 8，其中target
1,228,800 bytes、metadata 40,072 bytes。source-commit-pinned输出只带exact `0x3`
off-screen/thumbnail evidence，不是geometry-correct rasterizer、Profile oracle、Stream、driver、CMA、
scanout或physical evidence；Actions run `32326481615`四个job全部success。public commit `b9ec6a3`
又加入offline `baseline_cube_once` runner，在Host上串起BlobStore→SceneStore→Coordinator→renderer，
并输出`HOST_OFFSCREEN_REFERENCE_VERIFIED` receipt。tracked cross脚本已用官方K230 SDK默认Xuantie
Linux toolchain两次fresh构建出byte-exact的83,152-byte RISC-V ELF（SHA-256
`05accb66e621c0bef981e78312fae851a8f0cb8b233e1a9c89e4d648340972f8`），因此target状态分层为
`TARGET_BUILD_VERIFIED / TARGET_NOT_RUN`；neutral `k230d_linux_candidate`尚未执行，不能自证
`TARGET_OFFSCREEN_REFERENCE_VERIFIED`或K1完成。

Windows 配置入口仍保留；其结果必须按实际 toolchain 和当次 CTest evidence 记录：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

## 文档

- [文档入口与权威关系](docs/index.md)
- [系统设计](docs/system.md)
- [Resource 契约](docs/resource.md)
- [Runtime 契约](docs/runtime.md)
- [Link 契约](docs/link.md)
- [Session Core v0 实现契约](docs/session-core-v0.md)
- [Runtime negotiation core v0 实现契约](docs/runtime-core-v0.md)
- [Wi-Fi telemetry Profile v0](profiles/wifi-telemetry-v0/README.md)
- [Direct-IP Wi-Fi Runtime v0 实装规则书](docs/wifi-runtime-direct-ip-v0.md)
- [Wi-Fi Runtime dashboard](devtools/web/wifi_runtime_dashboard/README.md)
- [Composition 契约](docs/composition.md)
- [Host 集成回执 v0](docs/host-integration-receipt-v0.md)
- [Edge local compositor v0 开发方案](docs/edge-local-compositor-v0.md)
- [Composition scene lab v0 实验合同](profiles/composition-scene-lab-v0/README.md)
- [SDK 设计与开发指南](docs/sdk-guide.md)
- [Resource semantic core v0 实现与验证基线](docs/resource-core-v0.md)
- [系统进展](docs/system-report.md)
- [Host/Edge 并行开发模型](docs/development-model.md)
- [K230D Linux Edge 开发基线](docs/k230d-linux-edge-development-baseline.md)
