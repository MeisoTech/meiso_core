# K230D Linux targets

> 状态：`CMAKE-OWNED TARGETS / FINAL CROSS + BOARD GATE PENDING`。本页只描述
> 当前 source graph、入口与验收边界。旧 artifact hash、逐 commit receipt 和开发记录
> 已[归档](../../docs/archive/k230d-linux-target-development-history.md)。

## Build authority

CMake 是 K230D Linux source/link graph 的唯一权威。五个 `build_*.sh` 只验证参数、
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

例：

```sh
MEISO_K230_LINUX_CC=/path/to/riscv64-unknown-linux-gnu-gcc \
  targets/k230d-linux/build_composition_edge_service.sh \
  /tmp/meiso_k230d_composition_edge_service
```

输出路径必须为 fresh、受控的仓外路径。工具链路径、SDK archive、ELF、
原始日志、配置、IP 与凭据都不进入公开仓。

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
