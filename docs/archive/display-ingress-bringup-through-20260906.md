# Display Ingress 早期 bring-up 与 DRM 研究记录

> 归档来源：Meiso `e9f555e` 中保留的历史正文（2026-09-18 整理）。
> `HISTORICAL / NON-AUTHORITATIVE`。以下为截至 2026-09-06 的工程路径与原 v1 范围，
> 从 Profile 入口迁出；“当前”“下一步”均按当时理解，不约束后续 v2–v10 产品。
> 现行协议见[Display Ingress](../../profiles/display_ingress/README.md)，
> 研究构建与探针合同见[DRM research](../../research/k230d-drm/README.md)。
> DIFI 输入合同仍由[原入口](../../integrations/host/display_ingress_fixture_input.md)拥有。

### 受控双进程模拟 service

模拟 Host 的画面不是从 Composition 预览输出倒灌，而是先读取公开的一次性本机测试画面
记录（magic `DIFI`，版本 1）。它的精确布局、codec、fd 所有权与输入失败规则由
[Host 本机测试画面输入合同](../../integrations/host/display_ingress_fixture_input.md) 唯一拥有；
本 Profile 只规定验证成功后的 typed frame 如何进入当前媒体对象。这个输入不是 Meiso Link
或 `DIFR` wire，也不是送达确认。

当前模拟 service 只接受固定 64x32 opaque RGBA8，并且必须在任何 Session/control byte 发出
前完整读入、校验且看到 frame fd 的明确 EOF。随后才进入下述真实 Session、Runtime、Profile
binding 和 connected UDP 路径。成功收据升级为
`DISPLAY_INGRESS_SIMULATED_SERVICE_RECEIPT_V2`，但仍只是 fixture 运行结果，不是业务 ACK
或实屏证据。

本机证据程序 `meiso_display_ingress_simulated_service` 的 `--role` 入口只接受调用方传入的
两个已连接、非阻塞 socket：`--control-fd` 是 TCP 控制通道，`--media-fd` 是 UDP 画面通道；
`--role` 精确选择 `host` 或 `edge`。在这个入口中，角色进程不监听端口、不发现对端、不重连、
不创建线程；程序也不安装为产品可执行文件。测试启动器为每次运行创建新的 IPv4 回环 TCP
连接和全新 UDP 四元组（本地 IP/端口与对端 IP/端口），然后 `fork/exec` 两个真实进程。

同一程序另有 [Host 输入合同](../../integrations/host/display_ingress_fixture_input.md) 拥有的
`--pair --frame-fd` 本机入口。它先建立尚未运行协议的私有 TCP/UDP 描述符，实际 DIFI 读取仍
只发生在 Host 子进程。Host 在任何 control byte 前用内部单向 pipe 通知父进程输入已验证且
外部 fd 已关闭，父进程收到后才 `fork` Edge。在没有 worker thread 的父进程中，两个
子进程直接调用同一已加载程序映像的共享角色入口；pair 内部不再次 `exec`，父进程也不创建
Runtime 或媒体对象。成功只输出一条内嵌两份已验证 V2 角色收据的 pair 聚合结果；它仍是
fixture-only、非物理、非安全证据。

两个角色都使用真实会话协议（wire magic `MSL0`，定义见
[Session Core](../session-core-v0.md)）、资源运行时协议（wire magic `MRT0`，定义见
[Runtime](../runtime.md)）、本页 Display Ingress Profile 和 POSIX socket adapter。
所谓“模拟”只表示 Host 读取一张调用方提供的测试画面、Edge 用内存显示接收端验证其 SHA-256，并发布
`SIMULATED + UNSPECIFIED` 呈现证据；它不允许手工拼接控制帧或媒体 packet。Edge 收到并
提交合法 Desired 后，先把本地 RUNNING Actual 排入 Runtime；随后必须从同一个 Runtime
导出媒体绑定身份（binding）并成功初始化 UDP 接收对象，才允许事件循环把该 Actual 写入 TCP。
Host 收到 Actual 后严格执行 Profile 校验、本地状态副本（StateReplica）的 `RECEIVE_NEW` 和
Runtime 提交，再从 Runtime 导出媒体绑定身份并发送媒体；本证据程序不发送也不等待
State 送达确认（wire event `State ACCEPT`）。

这个一次性证据程序不发送 Desired/Actual 的 State 送达确认；这些确认不是画面或呈现事实，
也不能占用控制通道的唯一待发送位置而拖住媒体或证据。通用 Core 测试仍单独覆盖 State
送达确认语义；本程序以已提交的 Actual 和带类型的模拟呈现证据作为业务事实。

成功收据必须让 Host 输入、Edge 完整帧和 Host 收到的模拟呈现证据携带同一个 SHA-256，
同时明确 `physical_evidence=false`、`link_security=false` 和
`carrier=CONTROLLED_RAW_UDP`。控制或媒体硬错误必须给出终态收据并以失败退出；一次成功
UDP `send` 仍不算远端交付。本自动测试结合启动器创建的回环连接和两侧配对收据，最多证明
同机双进程、真实协议和指定 socket 的接线；单条收据只证明该进程所见的协议、哈希与关闭
终态。两者都不证明 K230D、真实网络、屏幕像素或产品级安全承载。

stdout 仍只允许每个角色输出一条终态 NDJSON 收据。引入外部画面来源以后，schema 唯一升级为
`DISPLAY_INGRESS_SIMULATED_SERVICE_RECEIPT_V2`；精确字段顺序、输入失败聚合字段与退出码由
[Host 本机测试画面输入合同](../../integrations/host/display_ingress_fixture_input.md) 定义。
Profile 仍要求两侧 frame metadata 与 SHA 对齐、`media_stream_epoch == production_epoch`，并且
Edge 只有在 UDP 接收对象完成武装以后才能把 RUNNING Actual 首字节写入 TCP。所有 stdout
收据固定 `physical_evidence=false`，不得当作物理呈现证据。

### 受控单帧 DRM service

`meiso_display_ingress_drm_service` 是显式启用的 Linux 实验程序，复用上述 service 的
Session、Runtime、DIFI 输入、固定 64×32 RGBA 帧和 connected TCP/UDP 接线。它支持跨机
调用方提供的 socket；不提供发现、重连或安全 carrier。它没有 `--pair` 入口，不安装为 SDK。
Host 参数与模拟角色相同；Edge 另要求 `--drm-device PATH`，由 Edge 本地选择显示设备，
该路径和 KMS 对象 ID 不进入 Host 协议。

这个变体只发布/接受 `DRM_KMS_PAGE_FLIP` 能力。Edge 在开放协议前确认唯一活动输出、
所有平面未绑定、无其他 DRM master，选择兼容的 RGB565 overlay，分配最多 5 MiB 的单缓冲。
它将收到的完整 RGBA 小帧用 nearest-neighbor 铺到当前输出模式，保持输入 SHA 与帧身份；
仅在收到本次提交 cookie/CRTC 匹配的单调时钟 page-flip 事件后，才发送该帧的呈现证据。
证据中的 output 尺寸来自实际 CRTC，source_time 来自内核事件。没有事件不能冒充呈现。

显示准入失败、分配失败、TEST_ONLY/提交失败、事件超时或恢复失败均返回失败收据；
失败不得输出成功的 KMS 证据。恢复使用保存的平面属性，结束后释放本轮缓冲和设备。
已有活动平面时拒绝这个实验，而不是绕过最终 Composition 层的叠放/安全层要求。
这里的单帧生命周期没有加入逐帧 ACK，也没有证明持续视频吞吐或完整应用兼容。

变体终态收据使用 `DISPLAY_INGRESS_DRM_SERVICE_RECEIPT_V1`，带角色、Session、输入帧与
媒体 epoch、SHA、实际输出尺寸、KMS 事件时刻及关闭结果。`physical_observation=false`
表示肉眼核验独立进行；成功的 KMS 事件本身不证明光学或 splitter 正确。


## K230D 本机 DRM probe 的当前实现边界

本节补充术语：libdrm 是用户态调用库；connector 是物理输出口，CRTC 是当前扫描输出的
显示时序，plane 是可叠加的图层，zpos 是图层上下顺序。下文保留这些内核字段名，便于
与驱动枚举结果逐项核对。

当前仓库已有一层**与 libdrm 无关的设备选择和资源状态逻辑**。主机测试会注入一组
假设备操作（测试代码模拟打开、提交和事件，不会打开真实 `/dev/dri`）。fake scanout
收据仍固定写 `INJECTED_OPS_UNVERIFIED` 和 `physical_evidence=false`；即使它让状态进入
`KMS_LATCHED`，也只证明状态逻辑按合同推进。

同一份纯 topology 判定器现在也接到一个显式启用的 K230D 原生拓扑观察程序。它用 libdrm
在固定容量内顺序读取当前 connector、CRTC、plane、format 与属性，规范化后调用共享判定器；
这不是内核原子快照。它不取得显式 master、不建 buffer/FB、不做 TEST_ONLY 或真实 commit。
容量内完整 topology 不满足策略时输出
`ADMISSION_BLOCKED`，满足时最多输出 `TOPOLOGY_ELIGIBLE_NOT_COMMIT_TESTED`。两者都固定
`actual_running_authorized=false / physical_evidence=false`，不能生成 Display Actual
RUNNING 或本 Profile 的 `DRM_KMS_PAGE_FLIP` 呈现证据。精确 CLI、收据字段、退出语义、
默认关闭的 CMake locator 和禁止调用清单由
[K230D target入口](../../research/k230d-drm/README.md#drm-scanout-policy-core)拥有。

仓内还新增了一份独立、无系统调用的 `single_plane_test_candidate` 判定器。它不改变上述
拓扑探针或现有 scanout `begin()`：调用方必须给出期望 topology SHA-256 和精确的
connector/CRTC/plane ID；snapshot 必须只有一条 active route、只有一个 active CRTC，
其 legacy framebuffer 为零，并且完整枚举中的每个 plane 都处于
`FB_ID=0 && CRTC_ID=0`。符合 immutable `OVERLAY`、RGB565 和 CRTC compatibility 的
候选必须恰好一个，plane ID 只用于核对该唯一候选，不能用于二选一。zpos 不参与这个
诊断候选判断，因为结果不声称画面叠放关系或可见性。

`CANDIDATE_IDENTIFIED` 只表示未来 TEST_ONLY 的候选身份闭合；函数没有构造原子请求、尝试
TEST_ONLY，也没有取得 master、创建 framebuffer 或改变显示状态。未来调用方仍须在同一个
master fd 上重新枚举并重新判定，再构造请求并尝试 TEST_ONLY。结果不能授权 real commit、Display Actual
RUNNING、呈现证据或物理输出，也不能证明未枚举硬件层上的独占。现有 real-commit 路径的
immutable-zpos 门不因此放宽。既有板端观察记录了两个未使用且兼容 RGB565 的 overlay，
所以目前没有证明唯一候选；具体的新判定原因尚未由未来同一 master fd 上的新鲜枚举与
判定证据封印，不能提前写成某个精确结果，更不能按请求 ID 猜选其中之一。

为避免 operator 猜 plane ID，另有 additive derived selector。它仍要求调用方给出 expected
topology SHA-256 和 connector/CRTC route，只在同一份 topology 通过全部共同门且兼容
immutable RGB565 overlay 恰好一个后派生 plane ID。零候选或多个候选都 clean block，不能
用 plane ID 二选一；derived request 类型没有 plane 字段，也不把零传进旧显式 API。
IDENTIFIED 后，便携执行器从 topology #1 复制实际 hash/route/plane，立即用旧显式 evaluator
自校并冻结该请求，#2 与 #3 仍按完整显式身份复核。这个入口同样不使用 zpos，也不改变
real-commit eligibility 或任何物理/可见性门。

下一层便携诊断执行器的 explicit 与 derived 合同由
[K230D target 入口](../../research/k230d-drm/README.md#便携的-test_only-only-执行器合同)
拥有。在走到 TEST_ONLY callback 的分支，它计划在同一 master fd 上同步完成三次完整枚举：
第一次核对精确候选，创建但不映射也不写像素的 RGB565 dumb buffer 与 framebuffer 后，
第二次重新枚举并在任何 TEST_ONLY 前阻断漂移；一次 TEST_ONLY 后再做第三次枚举与候选复核。它的专用回调表无法
表达 page flip、事件等待、恢复或真实 commit，TEST_ONLY 回调也没有 kind、flags 或
cookie 参数。

这一层仍只是“可构造并诊断检查单 plane 全屏 KMS 配置”的合同，不是画面路径。它不携带
frame，也不输出像素。独立、默认关闭的 K230D native runner 现在把这份私有 Ops 绑定到
libdrm：同一 fd 上最多三次复用 caller-owned-fd topology translation，创建但不 map/不写的
RGB565 dumb/FB，并只用十个 plane property 与 `DRM_MODE_ATOMIC_TEST_ONLY` 做一次检查；
它没有 `ALLOW_MODESET`、page-flip/event、restore 或 real commit 入口。native result 另有
一份独立 ledger，区分“进入 portable callback”与“调用 `drmModeAtomicCommit` API”，并逐次记录
topology read 的 libdrm object 释放、exact atomic flags 与 cleanup；只有走到 TEST_ONLY
callback 的分支才必须执行第三次读取，未尝试的 slot 不能冒充 COMPLETE。不能从
portable result 硬推这些事实。该调用计数不观测 libdrm 内部的 syscall 尝试或重试。
精确 CLI、收据和 cross-link 门由
[K230D target 入口](../../research/k230d-drm/README.md#原生-test_only-only-adapter-与一次性探针)
拥有。旧显式模式继续输出 byte-exact V1；derived 模式必须由 operator 显式选择
`--derive-unique-plane`，不接收 plane ID，并输出 V2，其中
`selection_mode=UNIQUE_POLICY_DERIVED`、`requested_plane_id=0` 与独立
`derived_plane_id` 不能混称。只有这份 native 运行收据中的 accepted-not-applied 才能归因于该次 libdrm/内核
调用；注入 Ops 的 Host test 仍不能。两者都不能授权 Display Actual RUNNING、真实提交、
可见画面或物理呈现证据；native receipt 还固定声明 framebuffer 未 map、
`presented=false`、`visibility_exclusivity_unproven=true` 与
`display_state_unchanged_unproven=true`。
这里的 execute provider 是 exact runner artifact 内链接的受信进程内实现；runner 不接收
topology bytes，V2 validator 只检查 result/native ledger 的可达性与内部一致性，不能脱离
linked provider artifact identity 单独证明派生事实。
所有成功打开的 fd 都必须经过一次终止性 close；这是新 TEST_ONLY Ops 对未来 Linux DRM
native adapter 的专用合同，不改变现有 scanout Ops。remove/destroy/drop/close 的任一清理
债务都会成为 `CLEANUP_FAILURE`，不得留下调用方不可达的对象所有权。

板端执行还需要一层独立的 one-child 监督封套，不能把 Host SSH timeout 当作远端回收。
这层默认关闭的 K230D target 将 derived V2 runner 与 native provider 链进同一个待重新
封印的 combined ELF；fork 后的唯一 child 在任何 DRM 调用前绑定 parent-death kill、复核 parent 并停在
本地 gate，随后只由板端 parent 按 `CLOCK_MONOTONIC` 的绝对 15/17/20 秒边界执行至多一次
TERM、至多一次 KILL 和最终 quarantine/waitpid。Host 不发送控制信号，也不因回执丢失
重启。只有 child 已回收且 holder=0 等 postflight 完成才有 terminal supervision result；
超时、forced signal 或 +20 秒仍未 exact-reap 一律是 UNKNOWN/no-retry。该封套只约束并记录 exact
direct child 的内核观测生命周期与 holder snapshot，
不解释 V2 收据，也不新增 TEST_ONLY、真实 commit、像素、可见性、Actual 或 physical 权限。
合同、Host fake 或 durable quarantine 本身不证明 Linux 内核行为、child 已终止或 DRM 已
隔离；只有新 combined artifact 门、exact reap 与 clean postflight 才能支持对应结论。

现有供 scanout `begin()` 和拓扑探针使用的 real-commit eligibility 判定器固定以下安全
选择；未来 native translation 只能如实提供事实，不能放宽：

- 在最多 8 个 connector、8 个 CRTC、16 个 plane 的完整规范化 observation 中，只接受唯一一条有效
  显示路径：connector 已连接、已有 encoder/CRTC 绑定，并且该 CRTC 明确处于 `ACTIVE`
  且带 current mode。另一个已连接但未指向 active CRTC 的 connector 不算有效路径；
  所有对象按 ID 严格排序；
- `possible_crtcs` 的 bit 明确定义为排序后的 canonical CRTC 数组下标。libdrm 原始数组若
  顺序不同，native translation 必须先重映射，不能把原始 mask 直接交给策略层；
- 只选择兼容该 CRTC、支持 DRM `RGB565/RG16`、当前 `FB_ID=0 && CRTC_ID=0` 的
  immutable `OVERLAY` plane；primary、cursor 和 unknown type 一律不用；
- 目标 CRTC 上每个 active plane 和候选 overlay 的 type、zpos 都必须是 immutable truth，
  且候选当前 zpos 严格高于所有 active plane。驱动没有 zpos 或不能证明时直接
  `ADMISSION_BLOCKED`，不按数组位置、plane ID 或 K230 示例布局猜测；
- 不写 connector 或 CRTC property，不建 mode blob，不改变 current mode，不调用
  `vo_init`，也没有 `ALLOW_MODESET` 的表达入口；唯一写入对象是上述 unused overlay；
- 输入仍是完整 opaque RGBA8888 或 canonical RGB565 big-endian。策略层同步转换成 DRM
  RGB565 little-endian，以 CPU 最近邻等比缩放并在 current mode 内居中补黑；每边最大
  4096 像素，输入与 dumb buffer 各有公开 64 MiB 硬上限；
- plane-only `TEST_ONLY` 接受后才允许 real `NONBLOCK | PAGE_FLIP_EVENT`。real commit
  明确区分 accepted、definitely-not-applied 和 outcome-unknown；只有 exact cookie + CRTC
  的 monotonic event 才成为 `KMS_LATCHED`。无关 event 被消费但不改变 pending；
- event deadline 由未来 runner 有界管理。持续 would-block/interrupt 必须显式声明
  present 或 restore outcome unknown；unknown 时 fd、master、FB、mapping 和 dumb handle
  全部保留，不能猜测“应该没显示”后释放；
- cleanup 先用原值恢复候选 plane 的十个可写字段：`FB_ID`、`CRTC_ID`、
  `SRC_X/Y/W/H`、`CRTC_X/Y/W/H`。先做 `TEST_ONLY`，再提交带 event 的真实恢复；
  `type` 和 `zpos` 只读校验，绝不改写。匹配恢复事件后还要重读这十个字段和只读的
  immutable type/zpos，逐项相同才按
  `rmFB -> unmap -> destroy dumb -> drop master -> close` 释放。

收据绑定规范化 topology SHA-256、driver name/version、connector type/id、CRTC/current
mode、plane/zpos、FB/handle/pitch/size、输入格式/尺寸/SHA-256、缩放 viewport、test/commit/
event 和资源清理账本；不包含 `/dev/dri/*` 路径、设备序列号或私有日志。Topology hash
输入的前 8 byte 固定为 `KDT0 00 00 00 01`：`KDT0` 在这里首次定义为
“K230D DRM topology”的哈希域标签，末尾的 `1` 只表示后续字段排列的第一种布局；当前
没有第二种布局，它也不是网络 wire 版本。其后字段按公开 topology 数组的 ID 排序结果，
整数用 big-endian，定长字符串空余 byte 补零。这个 hash 只防止验收时混淆输入，不提供
认证、签名或传输完整性保护。

原生 translation、runner 与显式 libdrm cross/link 属于拓扑观察，不是 page-flip。当前
驱动没有公开可验证的 immutable zpos，因此受控板端观察预期安全返回
`ADMISSION_BLOCKED / ZPOS_UNPROVABLE`；不能按 plane ID、枚举顺序、Overlay 类型或
RGB565 支持猜叠放关系。在后续 TEST_ONLY、真实 commit、事件与恢复门全部关闭以前，
Display Actual 不得因本策略内核或拓扑收据存在而报告 RUNNING。

## 明确不在第一版里

- carrier 选择及弱网无队头阻塞证明；TCP 只可作受控 bring-up；
- compressed video、delta frame、重传、逐帧 ACK、远端补帧；
- LG batch、source/import/release milestone、authority、lease 或 prepared token；
- 断线后的 hold-last、fallback、黑屏或安全策略；
- DRM connector/mode 的远程选择、lens correction 或 stereo optics；
- carrier 内的认证、加密、anti-replay 实现，自动 reconnect 或跨 Session resume；但最终
  carrier 仍必须满足 Link 的完整性与认证前提。

双眼画面可以由应用先排成一张左右并列 atlas，再作为普通完整 RGBA frame 发送。只要总
宽高落在已接受 capability 内，媒体 wire、buffer 和 DRM 提交路径都无需改变。眼间布局、
畸变和光学参数属于应用/显示调节配置，不是本帧传输合同的隐藏含义。

## 实现和验收顺序

1. **已完成：** DATA route 的 Profile-local `DIFR` codec、两槽媒体状态机和故障矩阵；
2. **已完成：** 冻结其余三个 schema，实现完整 Catalog、Desired/Actual、evidence
   codec 和只证明协议状态机的双 Runtime fixture；
3. **部分完成：** K230D 的 portable policy/state machine、fake failure matrix，以及原生
   native libdrm topology/admission runner 已接到同一判定器；
   [2026-09-05 真板拓扑收据](../../targets/k230d-linux/evidence/drm-topology-admission-2026-09-05.md)
   已得到 `ADMISSION_BLOCKED / ZPOS_UNPROVABLE`，所以没有选择 plane 或授权 Actual
   RUNNING；TEST_ONLY、真实 page-flip 与恢复仍按独立门推进；
4. **已完成：** connected UDP 行为适配器；它只关闭有界 connected-datagram 搬运与
   丢包恢复行为，并为后续跨进程/网络 bring-up 提供适配器，不关闭安全 carrier；
5. **已完成：** 两个本机进程使用真实 TCP 控制、connected UDP 数据、模拟画面源和内存
   显示接收端，对齐输入、接收和控制通道内小型呈现证据的 SHA-256；
6. integrity-protecting carrier 与 K230D Edge receiver；
7. LG Host latest slot 到 Profile sender 的映射；
8. 真实 Host 输入 SHA-256 与 KMS page-flip evidence 对齐；
9. 可选左右眼 atlas。

第 8 步以前不得声称“Host 应用已经显示到 Edge 实屏”。
