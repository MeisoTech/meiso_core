# Meiso Glass C SDK 指南

> 状态：当前实现与接入指南。产品语义只由[System](system.md)、
> [Resource](resource.md)、[Runtime](runtime.md)、[Link](link.md) 和
> [Composition](composition.md) 五份规范拥有。

## 1. 边界

```text
Locate / Echo / native application
  ↓ installed Looking Glass Host SDK (separate repository)
Looking Glass HostRuntime / presentation + camera services
  ↓ Meiso Display Ingress + CameraMedia / Runtime / Core
POSIX transport ↔ Edge platform capture / native scene / video backend
```

- Core 拥有identity、Session与有界Resource传输；Runtime管理catalog、endpoint、desired/actual和transfer。
- Display Ingress的scene/overlay/video和CameraMedia仍走已有Profile与传输语义。
- 旧Composition facade、ControlSession/Scheduler、HostBridge、EdgeService只在显式历史构建中可用，
  不代表当前Looking Glass应用入口。协议规范仍保留；构建选项不会另立协议。

IP、port、TCP、NDJSON 与 CMFB 都是 bootstrap/adapter 边界，不是 peer identity 或产品 Resource。

## 2. 源码所有权

| 目录 | 责任 |
|---|---|
| `include/meiso/core/` | Resource 与 Session 公开头 |
| `src/core/` | portable Core 实现 |
| `include/meiso/runtime/`, `src/runtime/` | Runtime 与 Scheduler |
| `include/meiso/profiles/`, `profiles/{wifi,device,composition,display_ingress}/` | schema、codec、Profile contract |
| `profiles/internal/` | 不安装的历史协议与垂直 fixture；不得成为产品依赖 |
| `include/meiso/composition/`, `composition/` | Store、Coordinator 与 renderer API/实现 |
| `integrations/composition/` | portable Edge facade 与 Control Session |
| `integrations/host/` | Host Runtime 与 Remote Edge backend |
| `simulation/` | deterministic simulator 与 trace |
| `adapters/posix/` | POSIX Session/media transport |
| `targets/k230d-linux/` | K230D Runtime、Linux adapter、service 与 CMake wrapper |
| `applications/display_ingress/` | 当前产品进程与共享会话/媒体接线；不编译 `devtools/` 源文件 |
| `applications/host/`, `devtools/` | 历史 Composition 入口与明确的开发/诊断工具；不等于当前 Host SDK |

`*_private.h` 只属于同一 target 内部：不安装、不提供跨 target ABI、不得被应用 include。

## 3. CMake package 与组件

Package 名为 `MeisoGlass 0.1.0`，imported target namespace 为 `Meiso::`。

| Component | 主要 target | 用途 |
|---|---|---|
| `Core` | `Meiso::Core`, `CoreResources`, `CoreSession` | 只保留 Core 二次开发 |
| `Runtime` | `Meiso::Runtime` | Core上的catalog/route/policy；Scheduler仅历史构建 |
| `Profiles` | Wi-Fi、Device、Composition schema/profile/media、Display Ingress | 当前typed contract与CameraMedia依赖 |
| `Client`（历史） | `Meiso::CompositionEdge` | portable opaque Edge facade |
| `Posix` | POSIX Session/media adapters | UNIX transport adapter |
| `Composition`（历史） | SceneStore、Coordinator、两个 renderer | portable composition implementation |
| `Simulation`（历史） | Edge simulator、trace | deterministic local backend |
| `Control`（历史） | `Meiso::CompositionControlSession` | connected control fd integration |
| `Host`（历史） | Host Runtime、Remote Edge | native Host integration |

默认 `MEISO_BUILD_LEGACY_COMPOSITION=OFF`：不生成上述历史组件、RuntimeScheduler、internal
profiles、旧Composition进程/renderer/simulation及web/macOS/four-kind外壳的专属测试；默认CI
也不运行旧dashboard。需要追溯时在独立构建目录显式打开该选项，CI可手动选择legacy。
历史K230D构建脚本在请求旧目标时显式选入；`runtime.h`仅聚合当前Runtime API，使用旧
scheduler的消费者需要直接包含 `meiso/runtime/scheduler.h`。源码、规范和[历史证据](archive/README.md)继续保留。默认安装不导出历史组件，旧prefix不会
被安装命令自动卸载，切换默认产品包请用干净prefix，避免遗留旧头文件/库。

**必须保留的相机依赖清单：** CompositionSceneSchema → CompositionProfile（CameraMedia类型和
catalog）→ CompositionMediaTransport → CompositionMediaPosixTcpAdapter → DisplayCameraChannel；
DisplayIngressProfile仍链接CompositionProfile。V4L2、显示后端与当前桌面Edge保留。这里保留
schema/catalog的链接闭包，不代表重新启用旧Composition应用栈；本次不重写或复制CameraMedia。

K230D/Linux adapter 不进入 portable 库导出；视频程序与启动入口可通过独立的
`K230dVideoRuntime` 组件安装。两端应用包和构建依赖见[快速开始](host-edge-quickstart.md)。
安装包同时导出源码 revision；Looking Glass 用固定清单检查它，见[版本配对](releases.md)。
内部协议 fixture 和 Four-kind lab 也不安装。

`cmake --install --component` 只安装指定分量，不递归安装依赖；构造组合 prefix 时必须
按表中依赖逐个安装，或不传 `--component` 一次安装完整 portable package。

### Core-only / 自定义 Runtime 基座

```sh
cmake -S . -B build-core \
  -DBUILD_TESTING=OFF -DMEISO_BUILD_APPLICATIONS=OFF \
  -DCMAKE_INSTALL_PREFIX=/tmp/meiso-core
cmake --build build-core --parallel
cmake --install build-core --component Core
```

```cmake
find_package(MeisoGlass 0.1 CONFIG REQUIRED COMPONENTS Core)
target_link_libraries(my_custom_runtime PRIVATE Meiso::Core)
```

这个 prefix 只包含 Resource/Session Core；仓外实现可以在其上定义自己的 catalog、policy、
storage 与调度，而不会链接仓库的 reference Runtime、Profile、Host 或 platform target。

### 仓库 reference Runtime

Runtime prefix 必须同时包含 Core：

```sh
cmake --install build-core --prefix /tmp/meiso-runtime --component Core
cmake --install build-core --prefix /tmp/meiso-runtime --component Runtime
```

```cmake
find_package(MeisoGlass 0.1 CONFIG REQUIRED COMPONENTS Runtime)
target_link_libraries(my_runtime PRIVATE Meiso::Runtime)
```

### 历史 Host 或 simulator（显式选入）

以下不是Looking Glass Host SDK。先以 `-DMEISO_BUILD_LEGACY_COMPOSITION=ON` 构建并安装旧组件：

```cmake
find_package(MeisoGlass 0.1 CONFIG REQUIRED COMPONENTS Host)
target_link_libraries(my_host PRIVATE Meiso::CompositionRemoteEdge)

find_package(MeisoGlass 0.1 CONFIG REQUIRED COMPONENTS Simulation)
target_link_libraries(my_sim PRIVATE Meiso::CompositionEdgeSimulator)
```

Package gate 会把 prefix 移动到新路径，然后从仓外 fresh configure/build/run，防止偶然依赖源码树。

## 4. 公开头与兼容策略

公开 include 只使用 namespaced 路径：

```c
#include <meiso/core/state.h>
#include <meiso/runtime/runtime.h>
#include <meiso/profiles/composition/profile.h>
#include <meiso/profiles/display_ingress/profile.h>
```

早期公开头迁移已完成，以下记录该次迁移边界；当前跨版本兼容范围见[配对与发布](releases.md)：

- 不保留旧 root header、typedef、macro、C symbol 或 CMake alias；
- 正式组件名不含 `debug`、`App` 或 Profile/wire 历史版本；
- wire/Profile numeric version、枚举值和 canonical bytes 保持不变。

仓外实验 consumer 必须直接迁移，不得靠 forwarding shim 继续使用旧路径。

## 5. Arena、borrow 与 failure

- 所有 `memory_required/init` 先计算并验证 caller-owned arena；不隐式分配 heap。
- 公开 output 与 arena/input 的 alias 规则属于 API contract；failure 不得伪造 partial success。
- queued work 对 payload 的 borrow 必须持续到 matching completion/boundary/terminal。
- State receipt、delivery accepted、Actual observed/applied、production RUNNING 和 media frame 是不同证据层。
- Session terminal 后旧 handle、RouteKey、view、token、epoch 和 frame 都不可复活；fresh transport 必须 fresh Session/arena/handle。
- remote 与 simulator 共用同一 `MeisoEdgeHandle` 业务表面，但 simulator evidence 不外推为 target truth。

## 6. Resource 选择

| 业务含义 | Resource |
|---|---|
| 可替换的当前事实、desired/actual、capability/status | State |
| 追加的 sample、event、telemetry 或 deadline-bound result | Stream |
| 连续 raw/encoded camera、audio、projection | MediaStream |
| 有副作用的请求与明确 result | Action |
| 有限、不可变、完整校验后可见的资产 | Blob |

不要为覆盖 API 虚构业务，也不要用 Blob 代替连续 frame、用 Action 代替 State ACK。

## 7. 开发与验证

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`BUILD_TESTING=ON` 的 CTest 按 core/session/runtime/profile/renderer/adapter/integration/service/
target-policy/web/conformance/compiler-variant lane 组织。`cmake/MeisoTests.cmake` 只负责按序加载
`cmake/tests/` 模块；添加测试时保持稳定 test name 和所属 lane。

代码改动按涉及的边界选择检查；仅文档/许可元数据修改做文字、链接与差异检查，不重跑无关构建。相关代码门包括：

1. 对应 target 的 warnings-as-errors Debug/Release build；
2. 相关 semantic/golden/failure tests；
3. 涉及 arena、borrow、parser 或 lifecycle 时的 ASan+UBSan；
4. 跨 TU 拆分时的公开 symbol、opaque layout 和 analyzer 比较；
5. public header 独立 C11/C++17 编译；
6. install component 的 relocate consumer；
7. K230D 修改的 Xuantie CMake cross-build，必要时再做物理板端门。

K230D 五个 `build_*.sh` 只是 CMake target 的参数校验/拷贝 wrapper；CMake 是唯一
Linux source graph authority。RT-Smart overlay 因外部 SDK 约束保留 Makefile，但必须经 manifest gate。

## 8. 当前非目标与阻断

- 无 Session resume、通用 discovery、安全 transport、协商 heartbeat 或 backend plugin ABI；
- CPU offscreen/render-media evidence 不等于 DRM/KMS/DSI physical presentation；
- simulator/hosted/cross-build 不等于 K230D camera/media/cleanup E2E；

当前代码见[实现地图](system-runtime-map.md)，使用入口见[Host/Edge 快速开始](host-edge-quickstart.md)。
旧 Composition 的[E2E 门](k230d-composition-e2e.md)只适用于该栈；历史记录从[archive](archive/README.md)追溯。

## 9. 许可与分发材料

本仓原始代码使用 [Apache-2.0](../LICENSE)，组件例外与来源见根 [NOTICE](../NOTICE)。
可选 CJK16 字体数据沿用已选择的 OFL-1.1；已迁出的 VVCAM/BSP 修改保留上游许可，
不因根 Apache 许可而重新授权。安装组件不会自动收集根 NOTICE 与所有外部依赖许可证；
交付时按实际包含内容保留 LICENSE、NOTICE、字体及依赖声明。许可范围与技术预览的功能成熟度是两个独立问题。
