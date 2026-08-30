# Meiso Glass C SDK 指南

> 状态：当前实现与接入指南。产品语义只由[System](system.md)、
> [Resource](resource.md)、[Runtime](runtime.md)、[Link](link.md) 和
> [Composition](composition.md) 五份规范拥有。

## 1. 边界

```text
Application
  ↓ typed commands / snapshots
Portable Edge facade (MeisoEdgeHandle)
  ↓
Host Runtime                 K230D Edge Runtime
  ↓                              ↓
Composition Control Session / Runtime / Scheduler
  ↓                              ↓
Core Session + Resource primitives
  ↕ transport adapters           ↑ platform adapters
```

- Core 拥有 identity、Session 和 bounded Resource delivery，不依赖 socket、线程、Profile 或板卡。
- Runtime 拥有 catalog、endpoint、desired/actual、production route 和 Blob transfer，不解释业务 payload。
- Profile 选择 schema、codec、route graph 和上限；当前 Composition 为
  `COMPOSE_/_PROFILE:v1` + MRT0 v4 + 6/18 graph。版本号是协议数据，不进入正式组件名。
- Control Session 组合 Runtime/Scheduler 与 POSIX Session adapter，不解 codec、不持有 camera/renderer truth。
- Host Runtime 拥有 desired、actual、Blob 和 media lifecycle；Edge Runtime 只在真实 apply/produce 后发布 actual。
- `CompositionHostBridge` 和 `K230dCompositionEdgeService` 是正式进程入口，不是第二套 SDK 或 wire owner。

IP、port、TCP、NDJSON 与 CMFB 都是 bootstrap/adapter 边界，不是 peer identity 或产品 Resource。

## 2. 源码所有权

| 目录 | 责任 |
|---|---|
| `include/meiso/core/` | Resource 与 Session 公开头 |
| `src/core/` | portable Core 实现 |
| `include/meiso/runtime/`, `src/runtime/` | Runtime 与 Scheduler |
| `include/meiso/profiles/`, `profiles/{wifi,device,composition}/` | schema、codec、Profile contract |
| `profiles/internal/` | 不安装的历史协议与垂直 fixture；不得成为产品依赖 |
| `include/meiso/composition/`, `composition/` | Store、Coordinator 与 renderer API/实现 |
| `integrations/composition/` | portable Edge facade 与 Control Session |
| `integrations/host/` | Host Runtime 与 Remote Edge backend |
| `simulation/` | deterministic simulator 与 trace |
| `adapters/posix/` | POSIX Session/media transport |
| `targets/k230d-linux/` | K230D Runtime、Linux adapter、service 与 CMake wrapper |
| `applications/`, `devtools/` | 薄进程入口与明确的开发工具 |

`*_private.h` 只属于同一 target 内部：不安装、不提供跨 target ABI、不得被应用 include。

## 3. CMake package 与组件

Package 名为 `MeisoGlass 0.1.0`，imported target namespace 为 `Meiso::`。

| Component | 主要 target | 用途 |
|---|---|---|
| `Core` | `Meiso::Core`, `CoreResources`, `CoreSession` | 只保留 Core 二次开发 |
| `Runtime` | `Meiso::Runtime`, `RuntimeScheduler` | Core 上的 catalog/route/policy |
| `Profiles` | Wi-Fi、Device、Composition schema/profile/media | 当前 typed contract |
| `Client` | `Meiso::CompositionEdge` | portable opaque Edge facade |
| `Posix` | POSIX Session/media adapters | UNIX transport adapter |
| `Composition` | SceneStore、Coordinator、两个 renderer | portable composition implementation |
| `Simulation` | Edge simulator、trace | deterministic local backend |
| `Control` | `Meiso::CompositionControlSession` | connected control fd integration |
| `Host` | Host Runtime、Remote Edge | native Host integration |

K230D/Linux adapter 与 service 是 build-only target，不进入 portable install export。
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

### Host 或 simulator

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
#include <meiso/host/edge.h>
```

当前 API 尚未对外发布，本轮因此选择零 legacy 硬迁移：

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

一个改动至少需要：

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
- 仓库尚无 `LICENSE`/`COPYING`/`NOTICE`，因而 install package 技术上可迁移，但尚不能声称具备对外分发授权。

当前实现/证据摘要见[system report](system-report.md)；K230D 分层证据见
[current E2E gate](k230d-composition-e2e.md)；历史记录只在[archive](archive/README.md)中追溯。
