# K230D Linux targets

当前产品入口是 `K230dVideoRuntime`：Display Ingress 视频/原生 2D/scene 与可选相机上行。
本页维护构建、安装、运行和资源边界；协议由[Display Ingress](../../profiles/display_ingress/README.md)拥有。
旧 Composition、单帧诊断和视频 bring-up 记录见[历史服务记录](../../docs/archive/k230d-linux-bringup-through-20260907.md)，
更早的 artifact/receipt 见[target 开发历史](../../docs/archive/k230d-linux-target-development-history.md)。

## Build authority

CMake 是 K230D Linux source/link graph 的唯一权威。各个 `build_*.sh` 只验证参数、
调用共享 `_build_cmake_target.sh` 并复制单个 ELF；它们不维护源文件列表。

`cmake/toolchains/k230d-linux-xuantie.cmake` 要求 Linux/riscv64 交叉编译器和
`MEISO_BUILD_K230D_LINUX_APPS=ON`。RT-Smart overlay 因外部 SDK 约束保留 Makefile，
但只是受 manifest gate 约束的集成副本，不能反向定义 product graph。

## 连续视频与原生2D：构建、安装和启动

`MEISO_BUILD_K230D_VIDEO_SERVICE=ON` 构建已有 MVX/DRM 视频服务。
产品入口、会话与控制共用实现位于 [`applications/display_ingress/`](../../applications/display_ingress/)，
平台解码/显示与 launcher 位于本目录；源码和 CMake 标注为 `PRODUCT`。
`devtools/display_ingress_*` 仅保留标注为 `REFERENCE` 的模拟/单帧诊断入口，
它们复用产品服务逻辑，产品 Edge 不编译任何 `devtools/` 源文件。
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

GitHub Actions 的 `K230D product cross-build` 在每个 PR 上构建并检查这个安装组件，
不接触板卡。输入 URL/SHA-256 只在 [CI workflow](../../.github/workflows/ci.yml) 的
`jobs.k230d.env` 中维护：Xuantie 3.0.2 自带 libc sysroot，加上从源码构建的
libdrm 2.4.124 / IJG JPEG 9f；不依赖本地供应商 rootfs 或私有下载凭据。
这三个 archive 在解包前校验。工具链来源见[官方 SDK 文档](https://www.kendryte.com/k230_linux/en/main/app_develop_guide/ai_develop/aidemo_doc.html)，
库摘要沿用 Buildroot 2025.02.1 的 [libdrm](https://github.com/buildroot/buildroot/blob/2025.02.1/package/libdrm/libdrm.hash)
和 [libjpeg](https://github.com/buildroot/buildroot/blob/2025.02.1/package/libjpeg/libjpeg.hash)记录。

在 Linux x86_64 上可以运行相同脚本；需 curl、tar、xz、make、CMake、Ninja、pkg-config
与 Meson 1.7.2。把 workflow 上述六个环境变量导出后运行：

```sh
.github/scripts/k230d-cross.sh
cat build/k230d-ci/SHA256SUMS
```

`K230D_CI_WORK` 可指定独立构建目录，下载缓存仍会逐次验 hash。
CI artifact 包含 `K230dVideoRuntime.tar.gz`、`SHA256SUMS` 和源码/输入身份；
精确同一提交/输入可与本地 SHA 对照。此门只证明交叉编译与安装，不证明板端运行或屏幕可见。

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
驱动需支持NV12源裁剪/步长；示例 `MEISO_NATIVE_OVERLAY=on` 选择 Profile v9 的单区域 HUD。
选择 `MEISO_NATIVE_OVERLAY=scene`（前台参数 `--native-overlay scene`）时改为 v10 多区域 scene，
Host 必须选择相同版本；两者均可组合视频与相机，包含可选 CJK16 能力。
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

## Camera safety boundary

V4L2 adapter 固定 progressive NV12 320×240、stride 320、sizeimage 115200，至少需要
4 个 single-planar MMAP buffer。Dequeue 后的 borrow 必须保持到 exact QBUF release。
失败路径分别保留 primary operation 和 cleanup failure，不得为了重试而破坏
ISP owner、卸载 module 或重启板卡。

板端运行前先做 memory/CMA admission。不足时不启动 camera/compositor，记录当次
`ADMISSION_BLOCKED`，但不将瞬时资源不足写成永久产品失败。

## Decoder and buffer limits

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
in response to queue pressure. With `--native-overlay scene`, the two reserved
ARGB4444 scene surfaces instead share this 10 MiB cap with decoder capture;
their page-rounded sizes are deducted before capture allocation. See the
[v10 scene selection](../../profiles/display_ingress/README.md#application-v10-native-scene-selection).

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
原 `vvcam_video_register.c` 含 `MIT OR GPL-2.0-or-later` 双许可声明；这是该文件的许可范围，
不把整个 BSP 标成 MIT，也不由本仓 Apache 许可覆盖。补丁历史及上游声明见根 [NOTICE](../../NOTICE)。

## 诊断与历史入口

- [旧 wrapper、Composition 与视频实测](../../docs/archive/k230d-linux-bringup-through-20260907.md)。这些目标不是当前产品的统一验收前置门。
- 一次相机诊断入口为 `build_camera_v4l2_capture_once.sh`；它不证明视频/scene/相机并发。
- 构建、模拟、物理 page-flip 和肉眼画面分别提供对应证据；按改动范围验证，交叉编译不代替板端运行。

<a id="display-ingress-单帧实机路径"></a>

### Display Ingress 单帧实机路径（早期诊断）

见[历史单帧入口](../../docs/archive/k230d-linux-bringup-through-20260907.md#display-ingress-单帧实机路径早期诊断)。

### Composition Edge service

见[旧 Composition 服务](../../docs/archive/k230d-linux-bringup-through-20260907.md#composition-edge-service)。

### Controlled H.264 Display Ingress service

见[2026-09-07 视频记录](../../docs/archive/k230d-linux-bringup-through-20260907.md#controlled-h264-display-ingress-service)。

## Historical DRM research

The opt-in [DRM scanout research](../../research/k230d-drm/README.md) retains
policy experiments, TEST_ONLY probes, injected-operation tests and historical
evidence. It is outside the default build and installation. The product video
runtime above uses `display_ingress_video_backend.c` directly.
