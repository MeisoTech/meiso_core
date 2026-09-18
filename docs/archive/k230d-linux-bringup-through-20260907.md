# K230D Linux 早期服务与显示记录

> 归档来源：Meiso `e9f555e` 中保留的历史正文（2026-09-18 整理）。
> `HISTORICAL / NON-AUTHORITATIVE`。保存早期 Composition、单帧与 2026-09-07 视频验收，
> 原命令/版本选择按记录时解释，不代表当前默认构建或验收范围。
> 当前连续视频、scene、相机、安装与资源边界见[K230D 产品入口](../../targets/k230d-linux/README.md)。
> 旧 Composition 目标需要显式选择 `MEISO_BUILD_LEGACY_COMPOSITION=ON`。

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


## Display Ingress 单帧实机路径（早期诊断）

`MEISO_BUILD_DISPLAY_INGRESS_DRM_SERVICE=ON` 在 Linux 构建
`meiso_display_ingress_drm_service`。它复用现有 service 协议源文件，新增一个窄的 libdrm 后端；
不依赖下节的拓扑准入/TEST_ONLY supervisor，也不替换最终 Composition 的层策略。
合同及失败向量见 [Profile](../../profiles/display_ingress/README.md#受控单帧-drm-service)，
实测边界见 [2026-09-06 结果](../../targets/k230d-linux/evidence/display-ingress-page-flip-2026-09-06.md)。

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
[K230D Composition E2E](../k230d-composition-e2e.md)。Cross-build 不证明板端运行；
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
waiting for presentation. Edge has two bounded media slots, two input DMA buffers, a bounded pool of NV12
capture surfaces and a 16-entry identity/digest ledger. V4L2 timestamps carry the submitted frame
identity into capture; only an actual DRM page-flip produces success evidence. Display uses the
video's native dimensions in the top-left of the current output mode, with no extra RGB surface.
Driver requirements include MVX DMABUF import/coherency and correct VO per-plane DMA addresses.

The private video backend also exposes an optional Edge-rendered ARGB8888
screen region. It selects a free RGB overlay plane under the same DRM master,
lazily allocates two 256 KiB DMA surfaces, and copies at most 256 KiB per update.
Coordinates use the active output mode without scaling. Video and overlay
commits share one CRTC and are serialized; their exact page-flip identities
are polled separately. A pending update returns busy, and source bytes are no
longer read after admission. Disable restores the original plane state;
close restores both planes before releasing buffers. No buffers are added to
video-only use. This private adapter API does not add or advertise a HUD
Resource in Display Ingress v2. `--native-overlay on` explicitly selects
[Display Ingress v4](../../profiles/display_ingress/README.md)
and accepts Host plans through the same Runtime. It publishes physical output
dimensions, 16×16 minimum region and 2×1 alignment, 65536 pixels, 48 built-in
label characters and 10 updates/s. A 256 KiB CPU scratch region is allocated
only in this mode. Hidden plans flip a transparent 16×16 region. HUD Actual
and exact-plan page-flip evidence have their own endpoint/production epoch;
video stays on its original binding. Both peers must select the same Profile.
With `--video-layout side-by-side`, both select v5: two native-size video views
share the decoder framebuffer and are centered in the physical eye regions.
This requires the common-plane source crop/stride driver fix. The Profile owns
the layout equations; it does not imply scaling or optical calibration.

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
