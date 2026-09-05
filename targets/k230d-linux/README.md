# K230D Linux targets

> 状态：构建目标由 CMake 管理；Linux 显示选择与资源状态逻辑已通过主机测试；原生
> libdrm 拓扑探针只枚举真实设备并给出安全准入判断，不提交画面。真正的 framebuffer、
> page-flip 与屏幕验收仍待完成。DRM/KMS 指 Linux 内核的显示管理和模式设置接口。
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

例：

```sh
MEISO_K230_LINUX_CC=/path/to/riscv64-unknown-linux-gnu-gcc \
  targets/k230d-linux/build_composition_edge_service.sh \
  /tmp/meiso_k230d_composition_edge_service
```

输出路径必须为 fresh、受控的仓外路径。工具链路径、SDK archive、ELF、
原始日志、配置、IP 与凭据都不进入公开仓。

## DRM scanout policy core

本节补充术语：libdrm 是用户态调用库；connector 是物理输出口，CRTC 是当前扫描输出的
显示时序，plane 是可叠加的图层，zpos 是图层上下顺序。

`meiso_k230d_drm_scanout` 是只供仓内构建的目标板专用库，内容是与 libdrm 无关的
设备选择和资源状态逻辑。它不安装、不 export，也不链接进普通 application target
或 wrapper。当前 macOS 测试会注入一组假设备操作（测试代码模拟设备返回，不打开真实
`/dev/dri`），用来验证 connector/CRTC/plane 选择、RGB565 转换、提交结果三态、
page-flip 事件身份和恢复/释放账本。所有收据固定为
`INJECTED_OPS_UNVERIFIED / physical_evidence=false`，明确不能作为实机证据。

拓扑选择现在只有一份纯判定器：现有 scanout `begin()` 和板端拓扑探针都调用它。判定
顺序固定为 topology 容量、canonical 形式、page-flip 单调时钟、唯一 active route、
可用 RGB565 overlay 与 immutable zpos、current mode 尺寸、最小 RGB565 buffer 大小。
探针不调用 `begin()`，所以不会走后面的 master、dumb buffer、framebuffer、TEST_ONLY 或
真实提交路径。

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
没有显式写调用改写成“显示状态没有变化”；这一点必须由同次开机的前后状态对照证明。

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

策略只接受运行时枚举出的唯一有效 active 显示路径，以及 unused、RGB565、immutable
OVERLAY plane。这里“有效路径”指 connector 已连接、已有 encoder/CRTC 绑定，并且该
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

当前正式身份的 fresh cross + board E2E 尚待收口。之前的 exact board evidence
只作历史，不替代当前 commit。验收定义见
[K230D Composition E2E](../../docs/k230d-composition-e2e.md)。Cross-build 不证明板端运行；
CPU off-screen 不证明 DRM/DSI/fence/vblank/panel；单 camera probe 不证明 combined E2E。
