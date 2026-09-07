# K230D Linux targets

> 状态：构建目标由 CMake 管理；Linux 显示选择与资源状态逻辑已通过主机测试；原生
> libdrm 拓扑探针保留只读职责。另一个受控 Display Ingress service 已完成真实 framebuffer、
> page-flip 与跨机呈现反馈；肉眼屏幕验收仍待完成。DRM/KMS 指 Linux 内核的显示管理和模式设置接口。
> 本页只描述
> 当前 source graph、入口与验收边界。旧 artifact hash、逐 commit receipt 和开发记录
> 已[归档](../../docs/archive/k230d-linux-target-development-history.md)。

## Build authority

CMake 是 K230D Linux source/link graph 的唯一权威。各个 `build_*.sh` 只验证参数、
调用共享 `_build_cmake_target.sh` 并复制单个 ELF；它们不维护源文件列表。

`cmake/toolchains/k230d-linux-xuantie.cmake` 要求 Linux/riscv64 交叉编译器和
`MEISO_BUILD_K230D_LINUX_APPS=ON`。RT-Smart overlay 因外部 SDK 约束保留 Makefile，
但只是受 manifest gate 约束的集成副本，不能反向定义 product graph。

## Targets

| wrapper | CMake target | 用途 |
|---|---|---|
| `build_composition_edge_service.sh` | `meiso_k230d_composition_edge_service` | 正式 Composition Edge foreground service |
| `build_camera_v4l2_capture_once.sh` | `meiso_k230d_camera_v4l2_capture_once` | 一次 bounded V4L2/GC2093 probe |
| `build_composition_offscreen_runner.sh` | `meiso_composition_offscreen_runner` | 无网络的 reference renderer evidence runner |
| `build_wifi_runtime_edge.sh` | `meiso_k230d_wifi_runtime_edge` | 独立 Wi-Fi telemetry Runtime Edge |
| `build_four_kind_lab.sh` | `meiso_four_kind_lab` | 不安装的内部 protocol fixture |
| `build_drm_topology_admission_once.sh` | `meiso_k230d_drm_topology_admission_once` | 显式启用的 libdrm 拓扑观察与安全准入探针 |
| `build_drm_single_plane_test_only_once.sh` | `meiso_k230d_drm_single_plane_test_only_once` | 显式启用、无像素的单 plane `TEST_ONLY` 诊断探针 |

例：

```sh
MEISO_K230_LINUX_CC=/path/to/riscv64-unknown-linux-gnu-gcc \
  targets/k230d-linux/build_composition_edge_service.sh \
  /tmp/meiso_k230d_composition_edge_service
```

输出路径必须为 fresh、受控的仓外路径。工具链路径、SDK archive、ELF、
原始日志、配置、IP 与凭据都不进入公开仓。

## Display Ingress 单帧实机路径

`MEISO_BUILD_DISPLAY_INGRESS_DRM_SERVICE=ON` 在 Linux 构建
`meiso_display_ingress_drm_service`。它复用现有 service 协议源文件，新增一个窄的 libdrm 后端；
不依赖下节的拓扑准入/TEST_ONLY supervisor，也不替换最终 Composition 的层策略。
合同及失败向量见 [Profile](../../profiles/display_ingress/README.md#受控单帧-drm-service)，
实测边界见 [2026-09-06 结果](evidence/display-ingress-page-flip-2026-09-06.md)。

交叉构建在既有 CMake 工具链外显式传入：

```text
-DMEISO_BUILD_DISPLAY_INGRESS_DRM_SERVICE=ON
-DMEISO_DISPLAY_DRM_INCLUDE_DIR=<target directory containing xf86drm.h>
-DMEISO_DISPLAY_DRM_UAPI_INCLUDE_DIR=<target directory containing drm.h>
-DMEISO_DISPLAY_DRM_LIBRARY=<target libdrm.so file>
```

使用完整的工具链 sysroot，保证 ELF interpreter 与目标系统一致。显式 library 文件避免选入
不同 BSP 的 libc linker script。Host 角色使用同一个源版本的本机 Linux 构建；只有 Edge
传入 `--drm-device`。调用方先建立 connected/nonblocking TCP、UDP socket，再继承到两个角色：

```text
host: --role host --control-fd N --media-fd N --frame-fd N
edge: --role edge --control-fd N --media-fd N --drm-device PATH
```

Host frame fd 仍按 DIFI 合同提供 64×32 图像和 EOF。service 一帧完成后退出，显示时间很短；
需单独保持画面作线下观察时使用 lab 静态显示工具。

## DRM scanout policy core

本节补充术语：libdrm 是用户态调用库；connector 是物理输出口，CRTC 是当前扫描输出的
显示时序，plane 是可叠加的图层，zpos 是图层上下顺序。

`meiso_k230d_drm_scanout` 是只供仓内构建的目标板专用库，内容是与 libdrm 无关的
设备选择和资源状态逻辑。它不安装、不 export，也不链接进普通 application target
或 wrapper。当前 macOS 测试会注入一组假设备操作（测试代码模拟设备返回，不打开真实
`/dev/dri`），用来验证 connector/CRTC/plane 选择、RGB565 转换、提交结果三态、
page-flip 事件身份和恢复/释放账本。所有收据固定为
`INJECTED_OPS_UNVERIFIED / physical_evidence=false`，明确不能作为实机证据。

现有 scanout `begin()` 和板端拓扑探针共用一份纯判定器。判定
顺序固定为 topology 容量、canonical 形式、page-flip 单调时钟、唯一 active route、
可用 RGB565 overlay 与 immutable zpos、current mode 尺寸、最小 RGB565 buffer 大小。
探针不调用 `begin()`，所以不会走后面的 master、dumb buffer、framebuffer、TEST_ONLY 或
真实提交路径。

另有一份彼此独立的 `single_plane_test_candidate` 纯判定器，专门回答一个更窄的问题：
给定调用方钉住的 topology SHA-256、connector/CRTC/plane ID，这份同一 DRM fd 上得到的
完整枚举结果，是否足以构造**未来一次诊断性 TEST_ONLY 的候选请求**。它先要求唯一 active
route 与请求 ID 精确一致、全 topology 恰有一个 active CRTC 且该 CRTC 的 legacy
`framebuffer_id=0`，再要求每个枚举 plane 的 `FB_ID=0 && CRTC_ID=0`。兼容该 CRTC、支持
RGB565 且 type 为 immutable `OVERLAY` 的候选必须恰好一个；请求中的 plane ID 只能核对
这个唯一候选，不能在多个候选里挑一个。最后才检查 current mode envelope，并计算最小
RGB565 buffer byte 数。这个判定器不使用 zpos，因为它不判断叠放关系，也不授权真实提交。

返回 `CANDIDATE_IDENTIFIED` 只表示候选身份闭合，可供未来调用方在持有同一 DRM master fd
时重新枚举、重新判定并构造一次 TEST_ONLY 请求；当前函数本身不构造原子请求、不打开设备、
不调用系统接口，也没有执行 TEST_ONLY。它不证明物理硬件层独占、画面可见、真实 commit、
Display Actual RUNNING 或任何物理证据。完整枚举以外的内部硬件层仍不可见，所以即使所有枚举 plane 都未使用，
`visibility/exclusivity` 仍是 unproven。现有 `begin()` 的 immutable-zpos 门完全不变，也
不得消费这份结果来绕过 real-commit policy。既有受控板端观察记录了两个未使用且兼容
RGB565 的 overlay，因此尚未证明“候选恰好一个”；具体的新判定原因还没有由未来同一
master fd 上的新鲜枚举与判定证据封印，在那以前不得尝试 TEST_ONLY。

这个私有 API 的结果状态只有 `CANDIDATE_IDENTIFIED`、`BLOCKED` 和
`INVALID_ARGUMENT`。空指针、零 object ID 或输入/输出内存范围重叠属于参数错误，并保持
输出逐字节不变。其余完整输入按以下顺序只报告第一个阻断原因：容量、canonical 形式、
期望 topology hash、唯一 active route、route ID、active CRTC 数量、legacy framebuffer、
任一枚举 plane 仍 active、零个或多个兼容候选、唯一候选 ID、mode envelope。
`BLOCKED` 会返回已经完成的纯判断字段；这些字段不是 TEST_ONLY 状态或呈现收据。由于
mode 每边先限制为 4096，计算出的 RGB565 最小 byte 数最大为 32 MiB，不另造一个当前
不可到达的 buffer failure reason。

另一个 additive `derive_unique_plane` 私有入口只省略调用方无法安全猜测的 plane ID；调用方
仍须钉住 expected topology SHA-256 与 connector/CRTC route。它和显式入口共用上述同一
policy 与顺序，但在“兼容候选恰好一个”后直接返回该候选的 topology index，不执行“唯一
候选 ID 等于请求值”这一步。零候选或多候选仍分别为 `NO_TEST_TARGET` 与
`MULTIPLE_TEST_TARGETS`，不能按 ID 或枚举顺序挑选。derived request 类型根本没有 plane
字段；零 plane ID 也不会作为旧显式 API 的哨兵。空指针、零 connector/CRTC ID 或三段对象
范围重叠仍是 `INVALID_ARGUMENT` 且输出不变。IDENTIFIED 后的执行器才可从这份 topology
复制非零 plane ID，并立即用原显式判定器自校一个冻结请求。

### 便携的 TEST_ONLY-only 执行器合同

下一层仍是仓内私有、同步、一次性的诊断执行器；本段先固定合同和 API 边界。在假设备
回调的失败矩阵与可执行测试完成前，不把它写成“已经尝试过 TEST_ONLY”。原入口消费上节
精确的 `single_plane_test_candidate` 请求；additive derived 入口消费 expected hash 与
connector/CRTC，不接收 plane ID。二者不会复用现有 `begin()`，也不会改变 `begin()` 的
真实提交规则。

这层使用一套单独的窄回调表。它只能表达：打开 primary DRM 节点、启用 universal-plane
和 atomic 枚举能力、取得并核验 master、读取完整 topology、创建未映射的 RGB565 dumb
buffer、建立 framebuffer、执行一次没有 kind/flags/cookie 参数的 `atomic_test_only`、
删除 framebuffer、销毁 dumb buffer、放弃 master 和关闭 fd。它没有 map 或像素写入入口，
也无法表达 page flip、事件等待、恢复、connector/CRTC property、mode blob、
`ALLOW_MODESET` 或真实 commit。该层只检查 KMS 配置请求，不携带像素；结果固定说明
`pixel_payload_present=false`。

同步调用按以下顺序执行：

1. 先验证配置、请求、输出范围互不重叠，以及回调表完整；参数错误时输出逐字节不变；
2. 打开 fd，启用两项枚举能力，取得并再次核验 master；
3. 在同一 fd 上读取 topology #1。显式模式核对请求绑定的 SHA-256 和完整候选身份；derived
   模式核对 expected SHA-256 与 connector/CRTC route，并要求兼容 plane 恰好一个。derived
   模式只有 IDENTIFIED 后才从 #1 复制非零 plane ID，构造并立即自校一个显式冻结请求。
   任一模式被阻断都不创建 dumb/FB、不尝试 TEST_ONLY，直接进入统一清理；derived 模式的
   结果 plane ID 此时保持零；
4. 创建但不映射 RGB565 dumb buffer，并核验 handle 非零、pitch 至少为 `width * 2`、
   size 至少为 `pitch * height` 且不超过 64 MiB；再建立非零 framebuffer ID；
5. 在 TEST_ONLY 前重新读取 topology #2，并用同一请求重新判定；它的规范化 SHA-256、
   route 和唯一候选必须与 topology #1 完全一致。任何漂移都在 TEST_ONLY 调用计数仍为零
   时阻断；
6. 只依据 topology #2 构造选中 plane 的全屏状态：FB/CRTC 使用新对象，source 与 CRTC
   起点为零，source 宽高为 mode 宽高左移 16 位，CRTC 宽高等于 mode；
7. 精确调用一次 `atomic_test_only`。回调只可能报告“接受但未应用”“拒绝且未应用”或
   “操作失败且未应用”，不存在已应用或结果未知的表达；
8. 无论 TEST_ONLY 报告接受、拒绝还是操作失败，都必须尝试读取 topology #3，再用同一
   请求判定；#3 必须与 #2 的规范化 SHA-256、route 和候选完全一致。#3 读取、判定或对照
   失败均记为 post-test operation failure，但仍保留 TEST_ONLY 已尝试及其 disposition 的
   事实。这里只证明枚举到的 DRM 状态没有观察到变化，不证明屏幕像素、不可见硬件层或
   显示状态绝对未变；
9. 从 framebuffer、dumb buffer、master 到 fd 逆序清理，并分别记录主操作首错和清理首错。

这是一份刻意受限的一次性所有权合同。`atomic_test_only` 永远不能应用状态，所以无论
remove framebuffer、destroy dumb 或 drop master 中哪一步失败，执行器都会记录首个清理
错误并继续后续清理，最后对每个成功打开的 fd 精确调用一次 `close_device`。这里的
`close_device` 只是一份**新 TEST_ONLY Ops** 对未来 Linux DRM native adapter 的专用合同，
不能推及现有 scanout Ops 或任意平台：第一次调用就是 primary fd 的终止性所有权栅栏，
消费该 fd，以及这个 file description 仍持有的 master、GEM handle 和 framebuffer
所有权；即使它报告错误，也不重试。`open_primary` 返回的非负 fd（包括 fd 0）必须是
本次执行器调用唯一持有的用户态 descriptor reference，回调不得复制、外借或共享
fd/GEM/framebuffer；除 close 外的回调
失败必须保留该 fd 供统一清理。函数返回时不能留下调用方拿不到的
fd/master/framebuffer/dumb 所有权位。结果会分别记录 close 是否尝试、是否成功，以及
所有权是否已经消费；任一清理债务都把最终状态升级为 `CLEANUP_FAILURE`，不能伪装成
普通拒绝。

API 调用状态与执行结果严格分开。调用状态只有 `RESULT_AVAILABLE`、`INVALID_ARGUMENT`
和 `INVALID_CONFIG`；后两者不调用任何回调并保持结果输出逐字节不变。只有
`RESULT_AVAILABLE` 才写执行结果。结果只分为 `NOT_TESTED`、`TEST_REJECTED`、
`TEST_ACCEPTED_NOT_APPLIED`、`OPERATION_FAILURE` 和 `CLEANUP_FAILURE`。前三种只有在
统一清理完全成功后才是正常结果。TEST_ONLY 已经接受或拒绝后，如果 topology #3 失败，
结果升级为 `OPERATION_FAILURE`，但结果中的 test disposition 仍保存先前事实；若随后清理
失败，则再升级为 `CLEANUP_FAILURE`，同时保留主操作首错、postcondition 首错和清理首错。
接受 TEST_ONLY 仍不授权 real commit、page-flip 事件、Display Actual RUNNING、画面可见或
物理证据；结果类型根本没有 real commit、unknown outcome、event/poll、restore、Actual 或
physical 的可置真字段/枚举值，只保留 `pixel_payload_present=false` 这一条显式边界。便携
core 自身不链接 native libdrm、命令行 runner 或板端收据，也不安装或 export 新 API；
下一节的独立 opt-in target 才把这份私有合同绑定到 libdrm。

调用状态的分类与优先级固定如下，混合错误只返回先命中的一类：

1. `INVALID_ARGUMENT`：config/request/result 任一为空；这三个定长对象范围重叠；两种
   request 的 connector/CRTC ID 任一为零，或者仅在 explicit request 中 plane ID 为零；
2. `INVALID_CONFIG`：device path、Ops 或 context 为空；path 不是容量内的绝对 NUL 结尾
   字符串；任一必需 callback 缺失；Ops/path 的已知范围与 config/request/result 或彼此
   重叠；或者 context 基地址落在任一已知范围内。由于 core 不知道 context allocation
   长度，完整 context-range 隔离仍由回调实现保证。

以上两类都在首个 callback 前结束，result 逐字节不变。普通 `int` callback 只能返回零或
正 errno；负数属于回调协议违约。带输出的 callback 先收到 sentinel/canary，失败时写过
输出、成功时返回非法 fd/handle/ID/dumb contract，同样属于协议违约。core 不会把失败分支
写出的 fd/handle/ID 猜成已取得所有权。`atomic_test_only` 的枚举越界、accepted 却带非零
error，或 rejected/operation-failed 却没有正 error，也都按 `EPROTO` 记为
`OPERATION_FAILURE`。只要 TEST_ONLY callback 已被调用，即使它违反返回合同，topology #3
仍必须尝试；此时 test observation 固定为 `TEST_OPERATION_FAILED_NOT_APPLIED`，
`test_error=EPROTO`。cleanup callback 的负数也规范化为 `EPROTO`，但不能中断后续清理
和 terminal close。所有 callback 参数指针仅在该次 callback 调用期间借用，尤其不得保留
path、topology/dumb/framebuffer 输出、局部 atomic state 或 error 输出指针。

topology #1 或 topology #2 的候选判定出现任意 `BLOCKED`（包括但不限于
hash/route/target 漂移），都得到清理干净的 `NOT_TESTED`：保留对应 candidate reason，operation stage 为
`NONE`、operation error 为零，并且 TEST_ONLY 调用计数为零。候选判定器若对 core 自己构造
的互不重叠对象返回 `INVALID_ARGUMENT`，属于内部协议违约，以 `EPROTO` 升级为
`OPERATION_FAILURE`。如果 #2 返回 `CANDIDATE_IDENTIFIED`，结果却与同 hash 的 #1 候选
语义不一致，也按 `EVALUATE_PRETEST_CANDIDATE / EPROTO` 处理，不能产生无原因的
`NOT_TESTED`。只有 TEST_ONLY 已调用后的 topology #3 读取、候选复核或不一致使用
独立 postcondition stage；它们把 outcome 升为 `OPERATION_FAILURE`，但不会覆盖先前记录的
test disposition。具体映射固定为：#3 读取失败使用 `POSTCONDITION_READ_TOPOLOGY` 与
callback 的正 errno（协议违约则为 `EPROTO`）；判定器返回内部不可达的
`INVALID_ARGUMENT` 使用 `POSTCONDITION_EVALUATE_CANDIDATE / EPROTO`；合法 `BLOCKED`
或与 #2 的 topology hash、route、target 语义不一致使用
`POSTCONDITION_TOPOLOGY_CHANGED / ESTALE`。cleanup 再失败时，最终 outcome 继续升级为
`CLEANUP_FAILURE`，三个错误轴仍分别保留。

### 原生 TEST_ONLY-only adapter 与一次性探针

`MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RUNNER=ON` 单独开启一个 K230D
Linux/riscv64 原生 target；默认是 OFF，不进入普通 application aggregate，也不安装或
export。它与只读拓扑探针共用调用方显式给出的
`MEISO_K230D_LIBDRM_INCLUDE_DIR` 和 `MEISO_K230D_LIBDRM_LIBRARY`，不查 pkg-config、
不回退到 Host 库，也不接受符号链接 library 路径。

原生 adapter 只把上一节的窄 Ops 绑定到这些操作：以
`O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW` 打开并核验 primary DRM character
node；启用 universal-plane/atomic client cap；显式取得并核验 master；在同一 caller-owned
fd 上最多三次调用一份共享的 libdrm topology translation；创建 16-bpp RGB565 dumb buffer，
但不 map、不写像素；用 `drmModeAddFB2(..., DRM_FORMAT_RGB565, ..., flags=0)` 建立
framebuffer；为选中 plane 精确加入便携 core 给出的十个 property，并仅以
`DRM_MODE_ATOMIC_TEST_ONLY`、`user_data=NULL` 调用一次 `drmModeAtomicCommit`。它不传
`DRM_MODE_ATOMIC_ALLOW_MODESET`、`DRM_MODE_PAGE_FLIP_EVENT` 或任何其他 flag。至多一次的
commit API 调用只有 `EINVAL` 是“拒绝且未应用”；libdrm 最终返回给本程序的其他正 errno
都是“操作失败且未应用”。本 one-shot 不发起
第二次 libdrm API 调用；libdrm 内部的 syscall 尝试或重试不在这份 ledger 的观测范围内。
两类都必须继续做 topology #3 和统一清理。

native run result 同时携带 portable executor result 和一份独立的 libdrm call ledger，不能
从前者反推后者。ledger 分别记录 primary-node 核验、两项 client cap、set/is/drop master、
每次 topology read 的 libdrm object seen/released、dumb/FB 创建、进入 portable atomic
callback、atomic request allocation、十个 plane property add、实际
`drmModeAtomicCommit` API 调用与 exact flags/NULL user data、atomic request free，以及逆序清理
与 terminal close。特别地，进入 callback 不等于已尝试内核 TEST_ONLY：request allocation
或 AddProperty 先失败时，commit API 调用次数必须仍为零。该计数不声称底层 ioctl 恰好
发生一次，因为 libdrm 包装层可能在内部处理或重试。ledger 还把 map/pixel write、
connector/CRTC property、mode blob、legacy modeset、non-TEST_ONLY commit 和禁止的 atomic
flags 计数固定为零。context 保存成功交给 portable core 的唯一 fd；其后每个 callback 都先
核对同一数值，ledger 分开记录 same-fd 是否适用、核对次数和是否全部通过。若 open 已取得
native fd，却在 fstat/primary-node 核验阶段失败，open callback 必须自己做一次 Linux
terminal close；这次内部 close 的尝试、errno 与 ownership consumed 也单独记账，不能被
portable `fd_was_opened=false` 掩盖。只有走到 TEST_ONLY callback 的分支才必须执行第三次
读取。未尝试的 topology-read slot 由独立 attempted=false
标识，不能因 read-status 的零值是 `COMPLETE` 而伪报完成。

共享的 caller-owned-fd translation 不改变 client cap/master、不关闭 fd、不创建对象，也
不提交 atomic request；每次调用只借用 fd，并在返回前释放该次取得的所有 libdrm object。
容量超限以原始 count 交给便携 evaluator clean-block；无法忠实表达的原生 noncanonical
结构以 `EPROTO` 作为读取操作失败，且 topology 输出保持不变。既有 path-owned 只读拓扑
探针继续通过同一个 translation 实现，不改变其不取显式 master、不创建 buffer/FB 的合同。

板端入口只有：

```sh
meiso_k230d_drm_single_plane_test_only_once \
  --device /dev/dri/card0 \
  --topology-sha256 LOWERCASE_SHA256 \
  --connector-id CANONICAL_NONZERO_U32 \
  --crtc-id CANONICAL_NONZERO_U32 \
  --plane-id CANONICAL_NONZERO_U32
```

或者显式选择 derived mode；该 grammar 不接受 `--plane-id`：

```sh
meiso_k230d_drm_single_plane_test_only_once \
  --derive-unique-plane \
  --device /dev/dri/card0 \
  --topology-sha256 LOWERCASE_SHA256 \
  --connector-id CANONICAL_NONZERO_U32 \
  --crtc-id CANONICAL_NONZERO_U32
```

十进制 ID 必须是无前导零的 canonical 非零 `uint32_t`；SHA-256 必须是 64 个小写十六进制
字符。参数错误只向 stderr 写 `E_ARGUMENT`、stdout 为空并退出 2。内部 provider/result
违反私有合同只写 `E_PROVIDER_CONTRACT`、不输出半条收据并退出 1。合法
`NOT_TESTED`、`TEST_REJECTED`、`TEST_ACCEPTED_NOT_APPLIED` 在清理无债时输出一条收据并
退出 0；`OPERATION_FAILURE` 或 `CLEANUP_FAILURE` 也输出完整收据但退出 1。SIGPIPE 或
stdout 写失败不能改写成成功。

stdout 的唯一 JSON 行固定为
`k230d_drm_single_plane_test_only_probe` /
`meiso.k230d_drm_single_plane_test_only_probe.v1`。字段顺序固定为：

1. type、schema、verification=`NATIVE_LIBDRM_TEST_ONLY_ONLY`、build target、
   scope=`single_plane_test_only_no_pixels`；
2. final outcome、primary outcome、test observation；
3. 请求的 topology SHA-256、connector/CRTC/plane ID；
4. initial/pretest/posttest candidate reason、三份 observed topology SHA-256、read/atomic
   次数以及候选与前后不变标志；
5. portable atomic callback 是否进入、callback 是否报告 accepted-not-applied、test errno；
6. ordinary operation、postcondition、cleanup 三条独立的首错 stage/errno；
7. 创建过的 dumb/FB 诊断值、最小输出 byte 数、portable cleanup/ownership 账本，以及上述
   独立 native call/object/atomic flags ledger；
8. `pixel_payload_present=false`、`real_commit_attempted=false`、
   `framebuffer_mapped=false`、`presented=false`、
   `visibility_exclusivity_unproven=true`、`allow_modeset_attempted=false`、
   `page_flip_event_requested=false`、
   `display_state_unchanged_unproven=true`、`display_actual_authorized=false`、
   `physical_evidence=false`。

上述 V1 只属于显式 plane 请求；旧 CLI、API、schema 与所有合法 V1 receipt bytes 保持
不变，common provider validator 只加严不可达的 native/result ledger 组合。
derived mode 使用同一 type 的 additive
`meiso.k230d_drm_single_plane_test_only_probe.v2`；它在 scope 后固定加入
`selection_mode="UNIQUE_POLICY_DERIVED"`，在原请求字段位置固定写
`requested_plane_id=0`，随后写 `derived_plane_id`，其余账本字段保持 V1 的顺序与含义。
这个零只表示“调用方没有请求 plane”，从不传入显式 evaluator。topology #1 未识别唯一候选
或更早失败时，`derived_plane_id=0`，dumb/FB/atomic 调用计数均为零；一旦 #1 IDENTIFIED，
`derived_plane_id` 必须非零且在 #2、#3、操作失败与清理结果中保持固定。V2 provider 必须
把 expected hash、connector/CRTC 与 derived request 精确交叉核对；initial reason 不得为
`TEST_TARGET_IDENTITY_MISMATCH`。合法 clean `NOT_TESTED` 的退出语义与 V1 相同。
这里的 execute provider 是 exact runner artifact 内链接的受信进程内实现，不是外部证据源；
runner 不接收 topology bytes，validator 只能拒绝不可达或内部不自洽的 result/native ledger，
不能单独证明 plane 的派生过程。V2 的派生事实因而必须与该次运行所加载的 native provider
artifact identity 一起使用，不能脱离 artifact provenance 解读。

### derived TEST_ONLY 的板端监督封套

Host 侧 SSH 超时不能安全替代板端子进程回收：连接或回执丢失时，Host 既不能证明远端
`TERM`/`KILL` 已送达，也不能安全重发同一种信号。为消除这项歧义，后续单独、默认关闭的
`meiso_k230d_drm_single_plane_test_only_supervised_once` target 把本节既有 V2 runner 源和
native provider 链入同一个 ELF。它不是通用进程监督器：CLI 没有 runner path、任意 argv、
environment、PID、signal、timeout、device 或 plane 参数；device 固定为 `/dev/dri/card0`，
child 只可直接调用 derived-V2 seam。旧 runner ELF、显式 V1/V2 CLI/API/schema 和合法收据
字节不因此改变。

CLI 顺序固定为：

```sh
meiso_k230d_drm_single_plane_test_only_supervised_once \
  --run-dir ABSOLUTE_PRIVATE_EMPTY_DIRECTORY \
  --nonce LOWERCASE_HEX_64 \
  --boot-id-sha256 LOWERCASE_HEX_64 \
  --artifact-sha256 LOWERCASE_HEX_64 \
  --topology-sha256 LOWERCASE_HEX_64 \
  --connector-id CANONICAL_NONZERO_U32 \
  --crtc-id CANONICAL_NONZERO_U32
```

run-dir 必须是调用者拥有、此前为空、非符号链接且 mode 0700 的绝对目录。supervisor 只在
持有的 dirfd 下通过 `openat(O_NOFOLLOW | O_EXCL)` 创建固定的
`launch.consumed.v1.json`、`run.ready.v1.json`、`runner.stdout`、
`runner.stderr`、可选的 `run.quarantine.v1.json` 和最后的 `run.result.v1.json`；调用方
不能改名或增加输出路径。每个记录都必须完整写入、同步文件并同步 parent directory，禁止
覆盖已有对象或跟随链接；可见的 JSON 名称只可由同目录内已经同步的 private body 通过
no-clobber atomic publication 产生，不能让读取者把半写文件当成完成记录。全局 per-boot
tombstone 使用固定 root-owned `/run` 名称，且与
run-dir 内 mirror 绑定同一 nonce/boot/artifact/topology/route。artifact hash 对应当前这个
combined ELF，而不是旧 runner ELF。
固定全局名称是
`/run/meiso-k230d-drm-single-plane-test-only.launch.consumed.v1.json`；
`--boot-id-sha256` 是对 `/proc/sys/kernel/random/boot_id` 原始完整字节（包括文件自带的末尾
LF）计算的 SHA-256，不能用去掉换行或重新格式化的 UUID 代替。

启动这个 supervisor 本身就是一次且唯一的执行授权。Host 此后只可等待或读取固定证据
文件，绝不向 child 发送 `CONT`、`TERM` 或 `KILL`，也不得因 SSH/回执丢失重启 supervisor。
supervisor 在任何 DRM 回调前先完成 boot、combined-artifact、primary-node、全量 holder 和
单线程 preflight，再先后、分别耐久取得本次开机全局及 evidence-dir 内的 `O_EXCL`
no-rerun claim；全局 claim 是唯一 authority，evidence mirror 失败不得删除或改写它。任一步
只要可能创建过对象就耗尽本次授权、不得重试；只有两个 claim 都完成 file 与 parent-dir
sync 的唯一写入者可以 fork。取得 direct child 后，child 必须设置
`PR_SET_PDEATHSIG=SIGKILL`、复核 parent 未在设置窗口内改变、隔离继承 fd、恢复子进程
signal 状态并停在本地 gate。fork 成功便立即把这个 exact direct
child 的所有权交给 parent；固定 `0x52` ready byte 与 exact
`waitpid(..., WUNTRACED)` 的 `WIFSTOPPED/SIGSTOP` 由另一步确认，并在 child 停止时绑定
其 PID 与 `/proc/PID/stat` start ticks。parent 随后再次做 preflight，并在 gate 前耐久发布
launch-ready 记录。任何 handshake、记录或二次 preflight 失败都发生在 derived seam 前；
若仍持有未回收 child，必须先发出至多一次 KILL、尝试一次 quarantine 记录并持续 exact wait 到
回收，不能因 signal/记录错误返回。tombstone 一经写入便不删除，不能用失败后的第二次调用
补救。
portable 状态机只能校验 callback 返回的 PID 字节，不能从一个恶意、隐藏 fork 的 provider
恢复未知 child；因此 no-child 结论明确依赖 production Linux spawn callback 的 side-effect
合同，并须由 source gate 与 harmless-child test 证明。zero-return 但 PID 非 canonical 只会记为
`EPROTO`/no authority，绝不能被解读为对 Byzantine provider 的隐藏进程证明。

parent 成功发送唯一一次 `SIGCONT` 后立即取得的 `CLOCK_MONOTONIC` 时刻记为 T0；若
`SIGCONT` 已成功而取时失败，不能重发 CONT，只能进入 KILL-once 与无限 exact-reap 的失败
收敛。放行后到首次 wait 之间不再做任何同步持久化。parent 每次都先用 exact `waitpid` 检查
direct child 是否已退出，再在绝对 T0+15 秒至多调用一次 `SIGTERM`，T0+17 秒仍未回收时
至多调用一次 `SIGKILL`。signal syscall 报错也不得重发同类信号。T0+20 秒仍未回收不允许
猜测 SIGKILL 已生效：只能尝试一次耐久 `QUARANTINED_UNREAPED_AT_DEADLINE` 记录，随后继续
持有 no-rerun tombstone 并等待同一个 child；它只声称最后一次 exact wait 尚未回收且
observed-now 不早于 deadline，不声称 child 当时仍在运行。即使记录失败也不能重试记录；
在 exact reap 前不能退出或发布 terminal result。
在 gate、preflight、ready-record、CONT/T0 或时钟协议失败触发的较早 ownership fence 中，
同一文件的 state 固定为 `OWNERSHIP_FENCE_PENDING_EXACT_REAP`；它只声称 parent 仍持有尚未
exact-reap 的 direct child，不虚构已到达 +20 秒或做过最后一次 deadline wait。
HUP/INT/TERM 等 supervisor 终止请求
只进入同一板端收敛状态机；若 supervisor 被不可捕获地终止，child 的 parent-death signal
负责阻止其继续运行。direct parent 在回收前持有 child PID，因此 signal 不依赖可复用的
`/proc/PID` 身份猜测。任一后续单调时钟读取失败、回退或越过可表示范围都会立即转入
KILL-once/quarantine/infinite-reap，不能让 wait 错误或信号洪泛把 15/17/20 秒边界无限拖延。
最终只有 exact `waitpid` 已回收、runner stdout/stderr 已同步、同一
boot/device/artifact 复核和完整 holder=0 postflight 全部完成，才可最后发布 terminal
supervisor result。

进程通道也固定：参数或静态配置错误发生在 stdio 隔离前，stdout 必须为空，stderr 只写一行
`E_ARGUMENT`，退出 2；一旦执行被接纳，process seam 必须 `setsid`、接管相关 signal 并把
自身 SSH stdio 隔离到 `/dev/null`，child 的原始 V2 stdout/stderr 只进入上述两个 fresh 文件。
此后任何 no-child failure、监督债务、forced signal、quarantine、child rc=1、postflight 或
terminal-record 债务在能够安全返回时都退出 1。只有 child 未经 TERM/KILL 自然退出 0、
postflight clean、所有计数未饱和且 terminal record durable 才退出 0；它只说明 lifecycle 与 raw receipt 已可供
Host 继续验证，不等于 TEST_ONLY accepted，也不授权任何 real commit/Actual/physical 结论。

首版不依赖尚未在同一 K230 内核封印的 seccomp filter。combined artifact 的 source、依赖
闭包和 ELF 必须另行证明 child 路径没有 `fork`、`vfork`、`clone`、`clone3`、`exec*`、
shell、network 或第二进程入口；监督 TU 是唯一允许调用 `fork`/`prctl`/`kill`/`waitpid` 的
范围。该静态证据不能冒充内核强制。supervisor 的固定证据文件只约束并记录 exact direct
child 的内核观测生命周期与 holder snapshot、TERM/KILL 调用次数与 errno、deadline、输出
byte/hash 和 pre/postflight ledger；其中不存在 TEST_ONLY `SUCCESS`、accepted、显示、像素、
Actual 或 physical 结论。
只有 Host 另行验证原始 V2 runner receipt、combined ELF provenance、运行时依赖和外部
postflight 后，才能把一次自然退出分类为 accepted-not-applied、rejected 或 not-tested。
任何 forced signal、quarantine、记录债务、holder 债务或连接不确定性都固定为 UNKNOWN 且
不得重跑。连接不确定本身不证明 child 或 DRM 已被隔离；durable quarantine 只证明 no-rerun
fence，只有 exact reap 加 holder=0 postflight 才支持相应清理结论。

本段、默认关闭的实现和 Linux Host harmless-child 测试面仍不证明 combined ELF 已为 K230
构建、封印或适合板端运行；Host 测试只能约束当前 Host kernel/libc 下的直接 child 生命周期。
旧 `a2e4373` runner artifact、旧 cross 收据和既有板端 observation 都不能替代该 combined
target 的新 source/dependency/ELF 与最终板端 preflight 门。

收据不含 device path，也不声称源码 commit、artifact 身份、像素、可见性或物理呈现。
即使三份枚举 hash 相同，open/set/drop/close 与驱动内部副作用仍不由本程序证明为零，所以
`display_state_unchanged_unproven` 恒为 true。当前已封存的板端 topology 只证明有
两个兼容的未使用 RGB565 overlay；它没有封存新 evaluator 的其余候选门，
也不能预先声称会得到“多候选”或任何其他精确 reason。只有同一 master fd 上
最多三次的新鲜枚举（只有到达 TEST_ONLY callback 才执行 #3）与该 runner 的新收据，
才可能说明此次是否真的到达
`TEST_ONLY`。Host 单元测试只注入 execute result 验证 CLI/收据，不冒充 libdrm；原生代码
必须另经显式 target cross compile/link 门。

原生 target 由 `MEISO_BUILD_K230D_DRM_TOPOLOGY_ADMISSION_RUNNER=ON` 单独开启；默认构建
为 OFF。调用方必须同时给出绝对的 `MEISO_K230D_LIBDRM_INCLUDE_DIR` 和
`MEISO_K230D_LIBDRM_LIBRARY`。CMake 不查 pkg-config、不回退到 Host 系统 libdrm，target
不加入默认 K230D application aggregate，也不安装或 export。板端唯一入口是：

```sh
meiso_k230d_drm_topology_admission_once --device /dev/dri/card0
```

它打开一次设备，为该 fd 启用 universal-plane 与 atomic **枚举视图**，读取 driver、单调
时钟能力、connector、encoder、CRTC、plane、format 与属性，再立即释放所有 libdrm 对象
并关闭 fd。它只允许 `drmGet*`/`drmModeGet*`/对应 Free、`drmSetClientCap`、`drmIsMaster`
和 `close`；严禁 `drmSetMaster`/`drmDropMaster`、dumb/map/AddFB、atomic request/commit、
property write、mode blob、legacy modeset 或 vendor `vo_init`。primary node 的首个 opener
可能被内核隐式视为 master，关闭最后一个 fd 也可能触发驱动回调；因此收据只写
`explicit_set_master_called=false`、实际 `master_observed_while_open`、
`explicit_kms_mutation_calls=0` 和 `display_state_unchanged_unproven=true`。程序不把用户态
没有显式写调用改写成“显示状态没有变化”。同次开机的外层前后对照最多说明所选观察面
未见持久差异；它仍不能证明没有瞬态 master/last-close 影响或像素连续性。

stdout 只允许一条 canonical JSON 收据，type/schema 固定为
`k230d_drm_topology_admission_probe` /
`meiso.k230d_drm_topology_admission_probe.v1`。字段顺序固定为：

1. type、schema、verification=`NATIVE_LIBDRM_TOPOLOGY_PROBE`、build_target；
2. overall `status`、`policy_status`、scope=`topology_policy_only`、`admission_reason`、
   operation stage/error；
3. driver/version、page-flip 时钟、raw object counts、topology complete/hash；
4. route/connector/CRTC/current mode 与 plane selection；没有完成的 selection 用 false 与零值；
5. active-plane、unused RGB565 overlay、immutable-zpos candidate 和十属性完整性计数；
6. 两个 client-cap 结果、master observation、显式 KMS mutation 调用为零，以及显示状态
   是否不变仍未由程序自身证明的边界；
7. primary-node 验证、libdrm object 释放与 close-once 账本，以及
   `actual_running_authorized=false`、`physical_evidence=false`。

`status` 只允许 `ADMISSION_BLOCKED`、
`TOPOLOGY_ELIGIBLE_NOT_COMMIT_TESTED` 或 `OPERATION_FAILED`；`policy_status` 额外允许
`NOT_EVALUATED`，用于保留 close 等晚失败发生前是否已完成纯策略判断。
`admission_reason` 只允许 `NONE`、`CLIENT_CAPABILITY`、`TOPOLOGY_CAPACITY`、
`TOPOLOGY_NONCANONICAL`、`PAGE_FLIP_CLOCK_UNPROVABLE`、`NO_ACTIVE_ROUTE`、
`MULTIPLE_ACTIVE_ROUTES`、`NO_UNUSED_RGB565_OVERLAY`、`ZPOS_UNPROVABLE`、
`MODE_ENVELOPE` 或 `BUFFER_ENVELOPE`。完整观察得到 blocked 也是诊断成功，退出 0；
open/enumeration/translation/master observation/close/receipt 失败退出 1；参数错误只在 stderr
写 `E_ARGUMENT` 并退出 2。即使 topology eligible，也只表示可以进入未来 TEST_ONLY，
不表示 admitted、KMS latched、HDMI、屏幕像素或 Display Actual RUNNING。

现有供 scanout `begin()` 与拓扑探针使用的 real-commit eligibility 判定器只接受运行时
枚举出的唯一有效 active 显示路径，以及 unused、RGB565、immutable OVERLAY plane。这里
“有效路径”指 connector 已连接、已有 encoder/CRTC 绑定，并且该
CRTC 明确 ACTIVE 且带 current mode；已连接但没有 active CRTC 的 connector 会被忽略。
规范化 topology 按 object ID 排序；
`possible_crtcs` 必须由原生 translation 从 libdrm resource 顺序重映射到 canonical
CRTC 下标。若 zpos 不能证明严格高于全部 active plane，probe 安全阻断。策略 API 不能写
connector/CRTC、不能 modeset、不能表达 `ALLOW_MODESET`，也不调用 vendor `vo_init`。

这是当前已知的真实硬件风险：仓内 K230 vendor 示例和现有板卡材料没有给出可验证的
immutable zpos 事实。因此真板首次运行很可能合法返回 `ADMISSION_BLOCKED`；这表示安全
条件无法证明，不是代码崩溃，也不能通过猜 plane 顺序、改用 primary 或强制 modeset
绕过去，更不能预先承诺一定能显示。

真实板验收不能用主机假设备测试代替，必须分别保存四类新证据：

1. 构建证据：source commit/tree、Xuantie 编译器版本、精确 sysroot、libdrm header/library
   身份，以及最终 ELF 的 ABI、size、SHA-256；
2. 同一次开机的拓扑观察证据：运行前后 holder、单次 canonical probe 收据及其 topology
   hash，以及调用方选择的外部状态面在运行前后的逐字节对照；probe 内部不会把设备路径、
   PID、boot ID 或 raw log 写入收据；
3. 同一次开机的写入前环境证据：`boot_id`，运行前后 DRM owner，以及 connector、CRTC、
   primary、overlay 的状态；
4. 单次显示证据：`TEST_ONLY`、真实 plane-only commit、exact cookie+CRTC 的单调时钟事件、
   原 plane state 恢复、逐字段 readback 和零资源债收据。

只有四类都通过，未来受控 native factory 才能产生真实 DRM/KMS 证据；它仍不证明面板
像素、光学校正或最终 safety-layer policy。公开摘要必须脱敏，不能写本机或设备 locator。

真实设备上若 present/restore 提交结果未知，或恢复后的 readback 不一致，必须保留当前
runner 进程、DRM fd/master、framebuffer、mapping 和 dumb buffer，记录 owner、topology、
kernel log 与资源账本，等待操作者单独授权恢复。不得强制 rmFB/close，不得再跑 `vo_init`
或重复 probe，也不得 kill driver、卸载 module 或重启板卡来制造一次“通过”。

### 2026-09-05 受控板端拓扑收据

精确实现提交 `07a365696a32a294be31805df6a78d873267e022` 已从 fresh `git archive`
重新交叉构建；生成的 42648-byte RISC-V/LP64D ELF SHA-256 为
`e9c5941806be848e58c12c57fe839da1711729e95847d6706e1be3bca780657f`，与板上实际
执行 artifact 逐字节相同。完整的脱敏[构建与运行证据](evidence/drm-topology-admission-2026-09-05.md)
已保存；2021-byte canonical 脱敏收据按仓库卫生规则保留在公开 Git 树外，摘要记录其
SHA-256 与所有准入关键字段，不提交原始运行 artifact。

单次真实探针退出 0、stderr 为空；它观察到 `canaan-drm` 1.0.0、一个 connector、一个
CRTC、七个 plane，以及 DSI connector 48 → CRTC 46 的 1920x1080 current mode。规范化
topology SHA-256 为
`4924deeef96f4f828accd9b85f104256acc011f1895bdeb0e48f88d2c3f5f96b`。虽然有两个当前
未使用且支持 RGB565 的 overlay，但没有一个候选具备可验证的 immutable zpos；因此结果
精确为 `ADMISSION_BLOCKED / ZPOS_UNPROVABLE`，没有选择 plane，也没有授权 Actual
RUNNING。

`active_plane_count=0` 只表示本次顺序枚举时，没有 plane 的 `CRTC_ID` 等于 46；它不证明
面板关闭、黑屏、扫描停止或可以安全接管。运行前后处于同一 boot，DRM holder 都为零，
所选 connector 与 kernel-log 快照逐字节相同；framebuffer sysfs 观察输入在前后都为空，
因此不能提供 framebuffer 状态对照。这些事实仍不能证明没有瞬态 implicit-master/
last-close 影响或像素连续性，所以 canonical 脱敏收据保留
`display_state_unchanged_unproven=true`。这也是 DSI 拓扑证据，不是 HDMI、TEST_ONLY、
真实 commit/page-flip、恢复或物理显示证据。

正式探针前的一次 wrapper 检查因 BusyBox 缺少 `timeout` 以 127 退出，binary 未启动、
stdout 为空，记为 `NOT_EXECUTED`，不算第二次设备探针。不能通过重复运行、猜 plane ID/
枚举顺序，或把 RGB565 支持当成 z-order 证明来绕过当前阻断。

## Composition Edge service

`K230dCompositionEdgeService` 组合：

- `COMPOSE_/_PROFILE:v1` + MSL0 v1 + MRT0 v4 control Session；
- Wi-Fi、DeviceTelemetry、Presentation、Scene/Blob/RenderResult、RenderMedia 和 CameraMedia
  的 exact 6 endpoints / 18 routes；
- caller-owned Runtime/Scheduler、BlobStore、SceneStore、Coordinator 与 CPU raster renderer；
- Linux Wi-Fi、device telemetry 与 V4L2 camera adapter；
- 独立但 Session-bound 的 control/media plain-TCP adapter。

它是 foreground service；进程启动、config 解析、socket 创建、signal 处理与 cleanup
位于 application 层，不进入 portable SDK 或 wire。Host service 对外 readiness 字段为
`service.ready.profile=composition`。

本地 config 必须从 tracked example 复制到 ignored path。Locator 只用于 bootstrap，
不是 peer/endpoint identity。Control/media 均是 plain TCP，只能运行于受控开发网络。

## Camera safety boundary

V4L2 adapter 固定 progressive NV12 320×240、stride 320、sizeimage 115200，至少需要
4 个 single-planar MMAP buffer。Dequeue 后的 borrow 必须保持到 exact QBUF release。
失败路径分别保留 primary operation 和 cleanup failure，不得为了重试而破坏
ISP owner、卸载 module 或重启板卡。

板端运行前先做 memory/CMA admission。不足时不启动 camera/compositor，记录当次
`ADMISSION_BLOCKED`，但不将瞬时资源不足写成永久产品失败。

## Validation boundary

每个最终 source commit 必须重新完成：

1. 五个 wrapper 的 fresh Xuantie CMake cross-build；
2. ELF header、size、SHA-256 与 source commit 记录；
3. 只对准确 Edge PID 的受控部署和启动；
4. 同一 fresh Session 的 Blob/Scene/C2/render/camera/Wi-Fi/Device E2E；
5. Host 保持在线时 Edge SIGTERM 退出 `rc=0/LOCAL_ABORT`，camera/media ledger 归零，
   原 ISP owner 与系统服务不变。

当前拓扑观察已有上述 exact-commit fresh cross 与真板收据；Composition 的 fresh cross +
combined board E2E 仍待收口。拓扑收据不能替代 frame、TEST_ONLY、真实 commit 或恢复门。
验收定义见
[K230D Composition E2E](../../docs/k230d-composition-e2e.md)。Cross-build 不证明板端运行；
CPU off-screen 不证明 DRM/DSI/fence/vblank/panel；单 camera probe 不证明 combined E2E。

## Controlled H.264 Display Ingress service

`meiso_display_ingress_video_host` and optional `meiso_display_ingress_video_edge`
exercise [Display Ingress v2](../../profiles/display_ingress/README.md) through the existing
Session/Runtime catalog, Desired/Actual, DIFR UDP adapter and asynchronous presentation stream.
This is the controlled 1280×720@30 workload; it is not the Looking Glass application integration.
RAW v1 and the single-frame service remain available.

Build Host with `MEISO_BUILD_APPLICATIONS=ON`. Cross-build Edge with
`MEISO_BUILD_K230D_VIDEO_SERVICE=ON`, the target C compiler and caller-supplied
`MEISO_DISPLAY_DRM_INCLUDE_DIR`, `MEISO_DISPLAY_DRM_UAPI_INCLUDE_DIR`,
`MEISO_DISPLAY_DRM_LIBRARY`. Both executables accept inherited, connected, nonblocking
`--control-fd` TCP and `--media-fd` UDP descriptors. Host additionally takes
`--source animation.h264`; `--frames 1800` is the default for both endpoints.
Edge accepts `--video-device /dev/video0 --drm-device /dev/dri/card0`.
Socket connection/peer selection belongs to the launcher; SSH carries maintenance commands only.

The source is an Annex B file with SPS/PPS and one Baseline IDR slice per picture,
up to 4096 pictures/128 MiB on Host and 128 KiB per AU. The file reader splits on repeated SPS;
AUD-delimited files should be normalized first. The Host loops that source at 30 frames/s without
waiting for presentation. Edge has two bounded media slots, two input DMA buffers, seven NV12
capture surfaces and a 16-entry identity/digest ledger. V4L2 timestamps carry the submitted frame
identity into capture; only an actual DRM page-flip produces success evidence. Display uses the
video's native dimensions in the top-left of the current output mode, with no extra RGB surface.
Driver requirements include MVX DMABUF import/coherency and correct VO per-plane DMA addresses.

On 2026-09-07, real aarch64 Jetson Host and K230D ran two 1800-frame sessions with independent-IDR
animation (mean 9.38 Mbps, largest AU 62078 bytes). The first presented/verified 1788 frames; the
second 1795, ending at source frame 1800. Both sides exited successfully with display restoration,
no pending decoder ledger and no new MVX error. In the second run, 38 datagrams were rejected,
3 partial frames discarded and 2 unread latest frames replaced; loss did not create a decode
reference dependency. Mac camera observations confirmed changing cube/airplane images and
recognizable colors on the physical screen. The output mode was 1920×1080 with a 1280×720 video area.

Edge ELF BSS was 606160 bytes. A 15-second in-run sample reached MemAvailable 11860 KiB and
CmaFree 0 KiB; no concurrent camera/compositor budget is established. This result uses a routed
USB/server/Wi-Fi path, not direct Jetson Wi-Fi. It does not establish full-screen stereo,
pose correction, OpenXR integration, a secure carrier or an end-to-end latency bound.
Validation for this display unit is the five Display Ingress tests, native/target builds and the
same two-endpoint physical display/evidence/cleanup result; the separate Composition checks above
remain applicable to Composition changes.
