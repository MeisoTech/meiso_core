# K230D Linux targets

> 状态：构建目标由 CMake 管理；Linux 显示选择与资源状态逻辑已通过主机测试；原生
> libdrm 拓扑探针保留只读职责。另一个受控 Display Ingress service 已完成真实 framebuffer、
> page-flip 与跨机呈现反馈；连续压缩视频和原生2D有独立构建/安装入口，见下节。
> DRM/KMS 指 Linux 内核的显示管理和模式设置接口。
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

例：

```sh
MEISO_K230_LINUX_CC=/path/to/riscv64-unknown-linux-gnu-gcc \
  targets/k230d-linux/build_composition_edge_service.sh \
  /tmp/meiso_k230d_composition_edge_service
```

输出路径必须为 fresh、受控的仓外路径。工具链路径、SDK archive、ELF、
原始日志、配置、IP 与凭据都不进入公开仓。

## 连续视频与原生2D：构建、安装和启动

`MEISO_BUILD_K230D_VIDEO_SERVICE=ON` 构建已有 MVX/DRM 视频服务。
`K230dVideoRuntime` 安装组件交付该服务、连接启动器、BusyBox 重连脚本及可选init模板；
库仍由 SDK 的既有组件拥有。固件不包含在这个组件中，板端需预装匹配的
MVX/DRM 驱动、libdrm 和 Linux loader。内核/DTB 配对由板级发行配置负责。

使用现有 Xuantie 工具链及显式目标 libdrm（以下路径由部署环境提供）：

```sh
MEISO_K230_LINUX_CC="$CC" cmake -S . -B build/k230d-video \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/k230d-linux-xuantie.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DMEISO_BUILD_K230D_VIDEO_SERVICE=ON \
  -DMEISO_DISPLAY_DRM_INCLUDE_DIR="$DRM_ROOT/usr/include" \
  -DMEISO_DISPLAY_DRM_UAPI_INCLUDE_DIR="$DRM_ROOT/usr/include/libdrm" \
  -DMEISO_DISPLAY_DRM_LIBRARY="$DRM_ROOT/usr/lib/libdrm.so"
cmake --build build/k230d-video --target \
  meiso_display_ingress_video_edge meiso_display_ingress_edge_launch -j4
cmake --install build/k230d-video --component K230dVideoRuntime --prefix "$EDGE_STAGE"
```

将 `EDGE_STAGE` 中的文件部署到 Edge 的隔离前缀。下例在 Edge 执行，`EDGE_PREFIX`
为实际安装位置，`HOST_IP` 是可路由到的 Host 地址。先启动 Host 的对应监听服务，
再启动 Edge；两侧的端口和输入宽高必须匹配。当前 720p 输入不改变已有物理输出模式。

```sh
"$EDGE_PREFIX/bin/meiso-edge-reconnect" \
  "$EDGE_PREFIX/bin/meiso_display_ingress_edge_launch" \
  "$EDGE_PREFIX/bin/meiso_display_ingress_video_edge" \
  "$HOST_IP" 4440 4441 /dev/dri/card0 4442 \
  --width 1280 --height 720 --native-overlay on --continuous on \
  --video-layout side-by-side \
  --local-peer "$EDGE_PEER" --expected-peer "$HOST_PEER"
```

启动器仅建立 connected/nonblocking TCP、UDP fd，然后 exec 服务。最后允许最多16对
服务选项，不能覆盖启动器拥有的 role、control/media fd 和 DRM 路径。
不需要再生成一个仅为传宽高/overlay选项的 wrapper。IPv4 地址是部署 locator；
PeerID 由 `--local-peer` 与 `--expected-peer` 配置，各为32位十六进制（高64位在前），
均非零且彼此不同，必须成对提供。continuous入口要求显式配置；有限诊断负载在
两项均省略时保留原示例身份。对端将自己的local与另一侧expected交换配置，
匹配由现有Core Session握手检查。这是对等身份声明，不提供密码学认证或设备注册。
Linux连接启动器为控制TCP启用存活探测：空闲5秒开始、间隔2秒、探测计数3，并设置
15秒`TCP_USER_TIMEOUT`（同时约束未确认发送；它会覆盖探测计数对应的关闭时限）。
连接失效由内核报告socket error，沿既有终止/重连路径建立fresh Session；不会在应用字节流插入
心跳，也不证明远端应用健康。参数只属于此Linux部署入口，不改变Core的通用liveness合同或
系统全局TCP设置。行为定义见[Linux TCP文档](https://man7.org/linux/man-pages/man7/tcp.7.html)。
重连脚本只持有一个子进程 PID，退出后等待2秒，参数错误（退出码2）停止重试。
不要在已有平台 supervisor 上再叠加此脚本。
`--continuous on` 让 Edge 媒体服务持续到连接终止或显式停止，不因实验帧数计时退出。
未选此选项时仍保留 `--frames / 30 + 15` 秒的诊断截止（默认75秒）。
continuous 不取消协议错误、解码无进展超时、Session 关闭或有界缓冲检查。

正常停止顺序：应用 drain → Host 服务停止并完成远端清理 → Edge 重连脚本停止。
`VIDEO_RESULT` 中的呈现计数、`restored` 和 `close_clean` 是实际反馈检查入口；
Host 应用应同时收到视频 source release 和原生2D execution receipt。
Host 构建/IPC入口属于 Looking Glass 的 `sdc/integrations/meiso/presentation_sink`。

### BusyBox 开机服务

同一安装组件在 `share/meiso/` 交付 `edge.conf.example` 和 `init.d/S90meiso-edge`。
对于使用 `/etc/init.d/rcS` / `rcK` 的 BusyBox 板，部署者先将配置复制为
`/etc/meiso/edge.conf`（root拥有、目录700、文件600），填写可执行安装前缀、Host地址与
配对PeerID。k230d-bsp #1 起的镜像（release `looking-glass-k230d-0.2.0` 及以后）根文件系统只读，已自带 `/etc/init.d/S90meiso-edge`
转发脚本：它读取 `edge.conf` 的 `MEISO_PREFIX` 并执行前缀里的 `share/meiso/init.d/S90meiso-edge`，
缺配置或脚本时打印 `EDGE_SERVICE_ABSENT` 并退出 0。较早的可写根镜像仍需将启动文件以755权限安装为
`/etc/init.d/S90meiso-edge`。
配置是root维护的shell赋值，不包含Wi-Fi凭据；720p与side-by-side是初步默认值。
驱动需支持NV12源裁剪/步长；当前服务选择Profile v9组合视频、可选原生2D与相机，包含可选CJK16能力。
相机配置同时指定`MEISO_CAMERA_DEVICE`和`MEISO_CAMERA_PORT`，Host配置同一相机TCP端口；
具体采集节点/ISP由板型确认，不从设备名称猜测。未配置时不建立相机上行。
现有物理输出模式不在此修改。完整配对入口见[快速开始](../../docs/host-edge-quickstart.md)。

```sh
/etc/init.d/S90meiso-edge start
/etc/init.d/S90meiso-edge status
/etc/init.d/S90meiso-edge stop
```

安装到该init目录即参与下次启动/关闭；需要停用开机启动时，停止后将脚本移出该目录。
只读根镜像上改为停止后移走 `/etc/meiso/edge.conf`，转发脚本随即不再启动服务。
`start`复用单PID重连器，网络暂未就绪时后台等待；`status`只表示重连进程存活，
真正可用仍由Host应用发现与远端Actual/呈现回执确认。脚本核对PID和脚本参数后停止，
最多等待20秒清理，不强杀显示持有者。更换配置或安装前缀前先正常停止两端。
日志写 `/dev/console`，不向板端内存文件系统无限追加；维护时通过串口保存需要的区间。
这只安装产品进程的生命周期入口，不修改内核、Wi-Fi或路由，也不叠加新的supervisor。

## Display Ingress 单帧实机路径（早期诊断）

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

## Historical DRM research

The opt-in [DRM scanout research](../../research/k230d-drm/README.md) retains
policy experiments, TEST_ONLY probes, injected-operation tests and historical
evidence. It is outside the default build and installation. The product video
runtime above uses `display_ingress_video_backend.c` directly.

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


The compressed-video service accepts `--width` and `--height` (defaults
1280×720) to select the same input dimensions in its decoder, capability,
Desired/Actual and media binding. Width is 16..1920 in multiples of eight;
height is even and 16..1080. These CLI limits are not a claim that every
combination fits the board's memory. The Canaan video plane currently presents
at native input size; it does not implement output scaling. A full output uses
matching input and physical dimensions, subject to decoder buffer admission.

At source change, capture allocation uses the decoder's
`V4L2_CID_MIN_BUFFERS_FOR_CAPTURE` plus one surface for the shown frame.
Decoding may pause during a pending flip until the previous shown frame is
returned. Allocation is capped at seven surfaces and 10 MiB in page-rounded
DMA storage.
The backend rejects a larger requirement before allocating any capture
surface. This replaces the fixed seven-surface allocation; encoded input,
HUD and firmware/system memory remain separate costs. The pool does not grow
in response to queue pressure.

With native overlay enabled, periodic `MEMORY_PROGRESS` lines sample
`MemAvailable` and `CmaFree` inside the existing video process. They avoid a
separate on-board sampler. `CmaFree` is not the allocatable CMA budget; use
actual DMA allocations and board-wide memory observations together.

## Optional JPEG camera keyframes

The video Edge can select `--camera-codec jpeg --camera-colorimetry bt601-limited`
with its existing `--camera-device` input. This advertises CameraMedia JPEG at
320×240, at most 0.5 Hz and 30 KiB per independent frame. The encoder reads the current
NV12 frame using row-sized RGB scratch, preserves its source sequence/time, and
releases the V4L2 buffer even if the encoded frame is too large and dropped.
This does not reduce ISP capture power or enable continuous local video recording.
Omit these options to keep RAW NV12. Cross builds need matching target libjpeg
headers/library (`JPEG_INCLUDE_DIR`, `JPEG_LIBRARY`) in addition to libdrm.
Capability support does not by itself establish board performance or power.

## BSP 来源

第一板（BPI CanMV K230D Zero）的内核、GC2093/VVCAM 适配、DTB 与 rootfs 由
[MeisoTech/k230d-bsp](https://github.com/MeisoTech/k230d-bsp) 单一维护（`patches/sdk/0011` 即原本仓的
vvcam 最小缓冲补丁）。本仓不再保留 BSP 补丁副本；用户态 Edge 包与 BSP 分别交付，配对版本在部署记录中注明。
