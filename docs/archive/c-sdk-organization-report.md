> 历史记录：保留原日期与范围；当前使用见[快速开始](../host-edge-quickstart.md)。

# Meiso Glass C SDK 组织与重构审计报告

> 状态：结构整理已完成，待最终统一验证的工作树。
> 审计日期：2026-08-30。
> Git 基线：`main@ecc12b3988d8a0c3f7a84f67f2ec3b69af431074`。
> 范围：C11 SDK、CMake target/package、原生进程入口、测试组织和 K230D Linux 构建边界。

本报告描述当前大重构后的代码事实，不替代产品规范，也不把尚未提交的工作树写成发布版本。
最终 fresh full、五个 K230D cross target 和准确板端 E2E 仍由主任务统一执行。

## 1. 结论

本轮已经把原型期的“名字、目录和巨型单文件”问题转成可评审的 SDK 结构：

- 正式 API、C symbol、header、target 和文件名完成零 legacy 硬迁移；
- 正式组件名不再带 `debug`、`App` 或 Profile/wire 历史版本；
- 公共头统一进入 `include/meiso/...`，没有旧根头或 forwarding shim；
- package 固定为 `MeisoGlass 0.1.0`，imported target namespace 为 `Meiso::`；
- Core、Runtime、Profile、Composition、Client、Host、Simulation 和 EdgeLinux 已有物理边界；
- CMake 是 Host 与 K230D Linux source graph 的唯一构建权威；
- 产品大单体已拆成真实独立编译的多 TU，没有用 `.inc` 伪拆；
- 当前全部非测试 C/H 文件均少于 1000 行，最大为 991 行；
- 测试语义未因整理删除，CTest 定义按稳定 lane 分文件；
- install/export 已能表达 Core-only 与 Core+custom Runtime 等消费方式。

这仍不是可对外分发的 release：

- 仓库没有 `LICENSE`、`COPYING` 或 `NOTICE`；
- 最新组合工作树尚未完成统一 sanitizer、cross 和板端 E2E；
- RT-Smart 完整 SDK、当前 Windows preset 和物理显示链路均未验证。

## 2. 权威与非目标

产品语义只由 [System](../system.md)、[Resource](../resource.md)、[Runtime](../runtime.md)、
[Link](../link.md) 和 [Composition](../composition.md) 拥有。

本报告只审计代码所有权、命名、构建、安装和验证边界。
正式 Composition Profile 已在 pre-public 窗口硬切换为
`COMPOSE_/_PROFILE:v1`，不保留旧身份 alias。Numeric wire version、canonical bytes、
枚举值、arena 规则、terminal、borrow 与 no-partial 合同仍由 exact 协议和测试保护。

本次结构重构也不会自动获得 Session resume、通用 discovery、backend plugin ABI、
安全 transport、协商 heartbeat、自动 Edge restart、物理显示、性能或长稳保证。

## 3. 最终责任边界

| 边界 | 拥有内容 | 不得拥有 |
|---|---|---|
| Core | identity、Resource value、Action、Blob、Session | Runtime policy、Profile、socket、线程、板卡 |
| Runtime | catalog、route、desired/actual delivery、Blob transfer、Scheduler | payload 业务解释、platform I/O |
| Profiles | Wi-Fi、Device、Composition schema/codec/catalog/media contract | process main、fd、camera driver、renderer policy |
| Composition | SceneStore、Coordinator、reference/CPU renderer | Host process、network bootstrap、Linux driver |
| Adapters | POSIX TCP/media 与 Linux Wi-Fi/Device/V4L2 边界 | Host/Edge role policy |
| Control | Runtime/Scheduler 与 POSIX control transport 的组合 | payload codec、camera/renderer truth |
| Client | portable opaque `MeisoEdgeHandle` facade | Host 或 simulator backend 实现 |
| Host | Host desired/actual/Blob/media lifecycle 与 Remote Edge backend | Edge truth 制造、Linux target policy |
| Simulation | deterministic Edge backend 与 trace | Host transport、K230D truth |
| EdgeLinux | K230D Composition Runtime、Linux adapters 和 foreground service | Host role、portable Core 语义 |
| Applications | 参数、构造、run、cleanup 和 evidence runner | 第二套 SDK、wire 或状态机权威 |
| Devtools | Dashboard、Four-kind/Wi-Fi 实验工具 | 正式 public API ownership |

依赖方向是单向的：`Core <- Runtime <- Profiles`；Profiles/Runtime 向上支撑
Composition，Profiles 支撑 Client，Client/Composition 支撑 Simulation；
Runtime/Profiles/Posix 支撑 Control，Client/Control/Posix 支撑 Host，
Control/Composition/Linux adapters 支撑 EdgeLinux。process entry point 只位于可复用 target 之上。

`*_private.h` 只服务同一 target 内的 TU，不安装、不导出，也不形成跨 target ABI。

## 4. 正式命名与兼容策略

当前正式名称如下：

| 职责 | 正式名称 |
|---|---|
| Composition Profile | `CompositionProfile` / `Meiso::CompositionProfile` |
| shared control | `CompositionControlSession` |
| Host integration | `CompositionHostRuntime` |
| portable client | `CompositionEdge` |
| remote backend | `CompositionRemoteEdge` |
| simulator backend | `CompositionEdgeSimulator` |
| trace | `CompositionEdgeTrace` |
| K230D integration | `K230dCompositionEdgeRuntime` |
| Host process | `CompositionHostBridge` |
| K230D process | `K230dCompositionEdgeService` |

迁移是硬切换：不保留旧 header、typedef、macro、C symbol、target alias 或 forwarding header，
仓外实验 consumer 必须直接迁移。numeric Profile/wire version 仍是协议数据，不能为无数字而改 wire。
`include/meiso/internal/composition/session_v2.h` 和 Four-kind 均为未安装的内部实现；
`composition_editor` 是独立开发工具，不属于正式 SDK 组件。

本轮保持 source-level contract，但没有宣称稳定二进制 ABI。

## 5. Public headers 与安装包

公共 include 只使用 namespaced 路径，如 `<meiso/core/session.h>`、
`<meiso/runtime/runtime.h>`、`<meiso/profiles/composition/profile.h>`、
`<meiso/composition/coordinator.h>`、`<meiso/host/edge.h>` 和
`<meiso/simulation/composition_edge.h>`。

源码树当前有 35 个 `include/meiso` 头；其中 internal protocol、lab 和 EdgeLinux
build-only 头不进入 portable package。`meiso/version.h` 由 CMake 配置生成并安装。

`MeisoGlassConfig.cmake` 支持九个组件：

| Component | 主要 imported target | 责任 |
|---|---|---|
| Core | `Meiso::Core`、`CoreResources`、`CoreSession` | 最小 Resource/Session SDK |
| Runtime | `Meiso::Runtime`、`RuntimeScheduler` | Core 上的 runtime policy |
| Profiles | Wi-Fi、Device、Composition schema/profile/media | typed contracts |
| Client | `Meiso::CompositionEdge` | portable Edge facade |
| Posix | POSIX Session/media adapters | UNIX transport |
| Composition | SceneStore、Coordinator、两个 renderer | portable composition |
| Simulation | Edge simulator、trace | deterministic local backend |
| Control | `Meiso::CompositionControlSession` | connected control integration |
| Host | Host Runtime、Remote Edge | native Host integration |

依赖闭包由 package config 检查：Runtime 拉 Core，Profiles 拉 Runtime，Simulation 拉
Client+Composition，Control 拉 Posix+Profiles+Runtime，Host 拉 Client+Control+Posix+Profiles。

`cmake --install --component` 本身不递归安装依赖。
组合 prefix 必须显式安装依赖组件，或不传 `--component` 安装完整 portable package。

已经建立 Core-only、Core+custom Runtime、Client、portable Composition、Simulation 和 Host
六类消费边界。

K230D/Linux adapters、K230D service、internal protocol fixture 和 lab target 均不 install/export。
详细接入方式见 [SDK guide](../sdk-guide.md)。

## 6. CMake 是唯一构建权威

顶层 [CMakeLists.txt](../../CMakeLists.txt) 只设置 project、语言、生成头、选项和模块顺序。
产品图按责任拆为 `MeisoBuild/Core/Runtime/Profiles/Composition/Adapters/Control`，
`MeisoClient/Host/Simulation/EdgeLinux`，以及 `MeisoApplications/Install/Tests` 模块。

旧 `MeisoIntegrations.cmake` 已删除；Client、Host、Simulation、EdgeLinux 不再混在一个模块。

`BUILD_TESTING=OFF` 不创建测试 target。
`MEISO_BUILD_APPLICATIONS=OFF` 提供 library-only 构建。
`MEISO_BUILD_K230D_LINUX_APPS=ON` 还要求 Linux/riscv64 toolchain contract。

五个 `targets/k230d-linux/build_*.sh` 只做参数检查并调用共享
`_build_cmake_target.sh`；source list 与 link graph 不再手写在 shell 中。
Xuantie toolchain 位于 `cmake/toolchains/k230d-linux-xuantie.cmake`。

RT-Smart overlay 因外部 SDK 约束仍有 Makefile。
它只能作为受 manifest gate 约束的集成副本，不能反向定义 canonical product source graph；
完整 RT-Smart SDK 构建仍是待验项。

## 7. 真多 TU 拆分结果

下表的 TU 数只计算对应 target 的 `.c` source，最大行数来自当前工作树 `wc -l`；
private header 不计入 TU。所有条目均为 CMake 独立编译源，不使用 `.inc`。

| Target/区域 | TU | 最大行数 | 主要拆分轴 |
|---|---:|---:|---|
| Core Action | 4 | 348 | facade/private/caller/executor |
| Core Session | 4 | 495 | init/wire/receive/lifecycle |
| Runtime | 12 | 977 | wire/catalog/route/state/stream/blob/query |
| Runtime Scheduler | 4 | 574 | facade/boundary/work/step |
| Composition Scene Schema | 8 | 517 | desired/mesh/presentation/result/identity/hash |
| Composition Profile | 5 | 573 | catalog/media state/media stream/private |
| MediaTransport | 4 | 493 | facade/outbound/receive/private |
| SceneStore | 3 | 805 | state/query/render |
| Coordinator | 4 | 529 | facade/desired/frame/private |
| Reference renderer | 2 | 646 | lifecycle/render |
| CPU raster renderer | 4 | 706 | lifecycle/codec/geometry/render |
| ControlSession | 3 | 577 | facade/pump/work |
| portable Edge facade | 1 | 526 | validation/backend dispatch only |
| HostRuntime | 6 | 675 | control/commands/events/media/pump |
| RemoteEdge | 3 | 427 | facade/backend/snapshot |
| Edge simulator | 6 | 756 | commands/composition/render/step/telemetry |
| Edge trace | 3 | 733 | codec/reader/writer |
| HostBridge process | 6 | 781 | entry/config/commands/media/truth/process |
| K230D EdgeRuntime | 8 | 958 | control/desired/transfer/composition/media/platform/pump |
| K230D EdgeService | 5 | 424 | 13-line entry/config/run/loop/lifecycle |
| K230D camera adapter | 4 | 533 | lifecycle/diagnostics/frames/syscalls |
| Wi-Fi Edge process | 7 | 750 | 14-line entry/config/bootstrap/truth/telemetry/pump/lifecycle |
| Wi-Fi Host process | 8 | 618 | 119-line entry/config/bootstrap/catalog/truth/commands/pump/lifecycle |
| Four-kind protocol | 3 | 621 | core/message/order |
| Four-kind application | 9 | 653 | system/network/connection/host/edge/output/thermal/selftest |

全仓非测试 C/H 当前最大文件是 991 行的 offscreen runner。
测试源码不在该 1000 行治理门内；大型 fixture 仍需后续按语义抽取，不能为过线而机械切碎。

拆分时采用的保护方式包括：

- 公开 symbol owner 集合前后比较；
- opaque handle size/alignment/field offset 比较；
- function-body 与 CLI output 基线比较；
- strict warnings、clang analyzer 和 ASan+UBSan 专项门；
- lifecycle、AF_INET/POSIX 和 failure-path semantic tests。

这些比较证明拆分没有观察到结构性回归，不等于对未来冻结 ABI。

## 8. 测试组织

`cmake/MeisoTests.cmake` 现在只按顺序加载 `Support`、`WebInstall`、`CoreRuntime`、
`ProfilesRenderers`、`IntegrationServices`、`TransportServices` 和
`TargetPolicyConformance` 七个模块。

在当前 macOS 工具依赖齐全的配置中，CTest inventory 为 53 个，分 13 个 label：

- `core`、`session`、`runtime`、`profile`、`renderer`；
- `adapter`、`integration`、`service`；
- `target-policy`、`conformance`、`compiler-variant`；
- `web`、`install`。

本轮保持原 test identity 与语义 case，没有用减少测试数量掩盖结构问题。
必须保留 capacity/alias/failure atomicity/terminal/borrow、wire golden 与 malformed、
catalog/scheduler/backpressure、renderer、adapter fault matrix、service lifecycle、
`-fshort-enums` 和 aggregate conformance 合同。

仍可整理但不能删语义的是 conformance/standalone 重复执行、大型 renderer/TCP/Runtime fixture、
部分 `NO_MAIN`/syscall macro seam，以及 native bridge 的 deadline/外部并发敏感性。

native bridge 和 installed-consumer gate 标为 `RUN_SERIAL`；
该属性只约束一个 CTest 进程，不能防止两个外部 CTest 同时操作同一 build tree。

## 9. 已有验证与证据边界

当前可引用的专项结果：

- public header：35 个源码树头加 1 个生成头，strict C11/C++17 共 72 次编译通过；
- package：Core、Runtime、Client、Composition、Simulation、Host relocation consumer 已通过；
- Runtime、Scheduler、ControlSession、HostRuntime、RemoteEdge、simulator、trace、
  HostBridge、K230D Runtime/Service、camera 与 Composition 各子模块完成 symbol/layout/semantic 专项门；
- Wi-Fi Edge：7 TU，body/layout/CLI exact，Debug/Release/ASan/analyzer/AF_INET 重复门通过；
- Wi-Fi Host：8 TU，body/layout exact，Debug/Release/ASan/analyzer/POSIX+Wi-Fi TCP 通过；
- Four-kind：protocol 3 TU、application 9 TU，symbol/layout/body/CLI exact，
  Debug/Release/ASan+UBSan、strict header 与 analyzer 专项门通过；
- CMake 四模块拆分：target/help、CTest identity 和依赖边集合前后 exact，
  一次 Debug full 53/53 与 Release tests/apps-off 通过；
- K230D 五个 wrapper 的 shell syntax、CMake manifest 引用和 RT-Smart overlay 路径门已通过。

仍不得把以上专项结果写成“最新组合候选已全部验证”。
所有并发编辑结束后的 fresh full、最终五个 Xuantie cross artifact 和准确 K230D board E2E
由主任务统一收口。

历史 `ecc12b3` 已有 K230D combined camera/render/media/telemetry 证据，见
[K230D Composition E2E gate](https://github.com/MeisoTech/meiso_edge/blob/main/docs/k230d-composition-e2e.md)。
它证明历史准确源码，不证明当前未提交重构候选。

证据层必须保持分离：

| 证据 | 可以证明 | 不能证明 |
|---|---|---|
| hosted unit/integration | C 语义、failure path、fake adapter | K230D driver 与真实资源 |
| install relocation | package 不依赖源码树绝对路径 | target runtime |
| cross-build | riscv64 source/link closure | 板端运行、camera、cleanup |
| board offscreen E2E | 指定板、镜像、网络和进程组合 | physical display、性能、soak |
| simulator/trace | deterministic Host-facing behavior | network、camera 或 Edge truth |

## 10. 剩余阻断与债务

提交和推送前必须完成：

1. 所有并发修改结束后的 fresh Debug、Release、ASan+UBSan 和 53/53 CTest；
2. public-header、install relocation、old-name、source-manifest 和 diff-check 复跑；
3. 五个 K230D Linux target 的准确 Xuantie cross-build、ELF hash 与静态审计；
4. 同一准确 commit 的 Blob/Scene/C2/render/camera/Wi-Fi/Device combined board E2E；
5. SIGTERM/LOCAL_ABORT、camera/media zero-debt、ISP PID 保持与临时文件清理。

发布前的硬阻断：

- 项目所有者选择并提交许可证与必要 notice；
- 不得在缺少许可证时宣称 package 可公开分发。

尚未关闭的工程债：

- 完整 RT-Smart SDK overlay 未执行；
- 当前 Windows MSVC x64 v143 preset 未在本轮执行；
- Linux/K230 adapters 仍以 `UNIX` 条件暴露，platform gate 可进一步精确；
- 链接器已有重复 `libmeiso_runtime.a` 警告，虽非本轮回归仍应定位；
- 测试 fixture 仍大，lane 去重策略尚未进入 CI policy；
- 内部 Composition v2 与 Four-kind fixture 仍保留 numeric protocol identity，但只存在于
  `profiles/internal/`，不安装也不进入正式组件命名；
- heartbeat、fresh-Edge supervisor、安全 transport 和 physical presentation 是产品后续工作，
  不是本轮代码整理完成项。

## 11. Reviewer 清单

- [ ] 正式 component/Profile 名称是否无旧 debug/App 或版本后缀，且必要的
  MRT0 wire-version API 保持显式；
- [ ] 是否没有 compatibility alias、旧根头或 forwarding shim；
- [ ] numeric wire/Profile data 是否保持不变；
- [ ] Core 是否仍不依赖 Runtime、Profile 或平台；
- [ ] Runtime 是否仍不解释 payload；
- [ ] Host、Simulation 和 EdgeLinux link closure 是否互不泄漏；
- [ ] private header 是否只在 target 内使用且不安装；
- [ ] CMake 是否是五个 Linux wrapper 的唯一 source graph；
- [ ] install component 是否只携带声明的 target/header；
- [ ] test identity 与 semantic cases 是否保持；
- [ ] 当前 full/cross/board 证据是否来自同一最终 commit；
- [ ] 报告是否没有把 hosted、cross、board 与 physical evidence 混写；
- [ ] LICENSE 阻断是否在发布前关闭。

## 12. 审计判定

当前结构已经具备继续开发“保留 Core、自定义 Runtime”和独立 Host/Simulation/EdgeLinux
实现的技术基础，代码组织不再是原型单体。

在最新统一门、准确 cross/board E2E 与许可证完成前，判定仅为：
`SDK STRUCTURE READY FOR FINAL VALIDATION — NOT RELEASE READY`。
