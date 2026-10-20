# Jetson → K230D：视频、消息与应用 SDK

> 本页主要保留 `v0.1.0-alpha.1` / 2026-09-08 视频与单区域 HUD 的配对命令和实测范围。
> 当前 scene、多区域与相机入口见[实现地图](system-runtime-map.md)、[K230D 产品说明](https://github.com/MeisoTech/meiso_edge/blob/main/targets/k230d-linux/README.md)及
> [Looking Glass Host SDK](https://github.com/MeisoTech/looking_glass/blob/main/sdc/integrations/meiso/presentation_sink/README.md)；
> 使用当前 main 时跟随其源码锁与对应 README，不把本页旧范围当成今天的全部能力。

这是注明版本的构建与使用入口，不另立产品契约。跨端行为由
[Display Ingress Profile](../profiles/display_ingress/README.md)拥有；Host 应用集成由独立
Looking Glass 仓库的 `sdc/integrations/meiso/presentation_sink` 拥有。

应用经 Host 本地 SDK/IPC 交付像素和屏幕内容，HostRuntime 编码视频并通过 Meiso 连接交付；
K230D 硬件解码视频、本地绘制框和短消息，再回传实际呈现结果。纯消息不要求先播放视频。
当前每次服务接入一个本地应用；退出会清除它拥有的画面，可再启动新应用。

## 已验证范围与环境

- Host：Jetson Orin NX、aarch64 Linux、GNU C++ 13、CMake ≥3.21、Python 3、OpenSSL Crypto
  开发包、pkg-config、GStreamer app ≥1.20 及 Jetson 编码插件。
- Edge：K230D 128 MiB，匹配的 Linux MVX/DRM 驱动、libdrm 和 glibc loader。
  驱动须正确处理 NV12 多平面 DMA-BUF、解码器 flush 和源裁剪/stride；原始 BSP 不等于
  已满足这些条件。内核、DTB、屏幕时序和恢复镜像属于板级交付，本用户态包不替换它们。
- 当前负载：合并视频 1280×720，H.264 独立帧，每 AU 上限 30720 字节；物理输出
  1920×1080，左右眼原生像素居中，共用解码缓冲。输入/输出尺寸分别参数化，不自动缩放。
  30 Hz 是输入负载，非最低帧率或延迟承诺。Edge 原生 2D 表面最多 65536 像素。
- 消息支持矩形、边框、填色及一行有限字符集文字，可按物理眼界重复；CJK16 字库可选。
  多区域、复杂排版、6DoF、Edge 姿态跟踪/微调和完整 XR 设备接口不在此版能力内。
- OpenXR 视频入口另需 OpenXR/Vulkan/OpenGL 开发包，以及运行时的 Monado、Xvfb、xauth、
  xwininfo、GStreamer X11 插件和可用 Vulkan 驱动。源 runtime 负责定位和输入；源每眼姿态/FOV与
  可转换的目标时间随帧传递，不表示 Edge 已具备跟踪。

两端需要可双向路由的 IPv4；允许经过一台有线/无线中转机。当前部署的中转必须保留两端地址
或提供对应配置，不能让未配对的 NAT 源地址被当作 Edge。端口、地址、PeerID 都是部署参数。
这些显式 PeerID 不是密码学认证；安全 carrier 尚未交付。Linux连接入口启用TCP存活探测与
15秒未确认发送等待上限，将失联交给已有fresh Session重连；这不提供应用级健康检查。
中转部署还应在Host链路暂失时阻止流量转向其他网关，避免对端缓存临时ICMP重定向。

## 从固定源码生成两个包

配对源码使用两仓的 `v0.1.0-alpha.1` tag；Looking Glass 的
`sdc/integrations/meiso/meiso.lock.json` 固定所需 Meiso 完整 commit，构建时自动检查。
仓库、选择版本与升级步骤见[版本配对与发布](releases.md)。使用干净源码分别放在 `meiso/` 与
`looking_glass/`；Host 构建只需要后者的 `sdc/` 子树。

板测行为基线仍为 Meiso `d5da243631edd1d472776707d2ecfe51ea2417cd` 与
Looking Glass `be0f725f037c3d266c560ae712dfc84879009d7d`；本次预发布增加配对元数据和构建检查，
没有改变媒体运行代码或重新宣称板测。hello_xr 的单眼近景红色物体仍待源图对照，双眼光学/内容正确性未完整验收。

2026-09-08基础包按下列命令从空构建目录生成并迁移，在同一Host服务中通过纯消息、
仓外SDK消息应用及真实OpenXR视频+消息的显示、更新、隐藏和退出清理；摄像头观察两类实屏内容。
当前配对版本随后只更新两侧连接启动器，补充空闲失联收敛；受控TCP丢包后应用恢复、
配合中转路由保护的Host普通重启及重启后真实视频/消息/清屏已通过。
验证沿用已配置的板级固件和中转网络，不代表未准备的任意BSP可直接运行，也不是长期性能测试。
当前组合（v9/v10）是 Wi-Fi 量级 Profile。为 BLE 量级链路写的
[Display Ingress v11 草案](../profiles/display_ingress/capability-tiers-v11.md)已被取代（S-024），不影响本部署步骤。

在 Jetson 的工作目录执行；`host-package/` 同时包含运行程序和客户端 SDK：

```sh
cmake -S meiso -B build-sdk -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DMEISO_BUILD_APPLICATIONS=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/host-package"
cmake --build build-sdk -j4
for component in Core Runtime Profiles Posix; do
  cmake --install build-sdk --component "$component"
done
cmake -S looking_glass/sdc/integrations/meiso/presentation_sink -B build-host \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/host-package" \
  -DCMAKE_INSTALL_PREFIX="$PWD/host-package" \
  -DSDC_STAGE3_BUILD_OPENXR_LAYER=ON -DSDC_MEISO_PRESENTATION_BUILD_TESTS=ON
cmake --build build-host --target sdc_meiso_host_runtime_video \
  sdc_meiso_native_application sdc_openxr_capture_layer sdc_meiso_overlay_content_test -j4
ctest --test-dir build-host -R '^sdc_meiso_overlay_content_test$' --output-on-failure
for component in MeisoHostRuntime LookingGlassClientSDK MeisoHostSDK; do
  cmake --install build-host --component "$component"
done
```

只需要原生应用时可将 OpenXR 选项改为 `OFF`，并从构建目标中移除 `sdc_openxr_capture_layer`。
应用包保留现有 C++20 源接口，跨版本不保证 C++ 二进制 ABI；应用与安装包应一起重建。

Edge 在有 Xuantie Linux 工具链的构建机交叉编译，源码在 [meiso_edge](https://github.com/MeisoTech/meiso_edge)
（2026-09-24 从本仓拆出）。把 meiso_edge 检出到 `meiso_edge/`，本仓按其 `meiso.lock.json` 的修订检出到
`meiso/`；`CC` 指定目标 C 编译器，`DRM_ROOT` 指向匹配板端的 sysroot，`FONT_FILE` 指向已生成的可选字库：

```sh
MEISO_K230_LINUX_CC="$CC" cmake -S meiso_edge -B build-edge \
  -DMEISO_CORE_SOURCE_DIR="$PWD/meiso" \
  -DCMAKE_TOOLCHAIN_FILE=meiso_edge/cmake/toolchains/k230d-linux-xuantie.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DMEISO_BUILD_K230D_VIDEO_SERVICE=ON \
  -DMEISO_DISPLAY_DRM_INCLUDE_DIR="$DRM_ROOT/usr/include" \
  -DMEISO_DISPLAY_DRM_UAPI_INCLUDE_DIR="$DRM_ROOT/usr/include/libdrm" \
  -DMEISO_DISPLAY_DRM_LIBRARY="$DRM_ROOT/usr/lib/libdrm.so" \
  -DMEISO_K230D_OVERLAY_FONT_FILE="$FONT_FILE" \
  -DCMAKE_INSTALL_PREFIX="$PWD/edge-package"
cmake --build build-edge --target meiso_display_ingress_video_edge \
  meiso_display_ingress_edge_launch -j4
cmake --install build-edge --component K230dVideoRuntime
```

不装字体时令 `FONT_FILE` 为空，只使用内建字符。CJK16 使用
[字体工具](https://github.com/MeisoTech/meiso_edge/blob/main/targets/k230d-linux/make_overlay_font.py)中固定下载地址及 SHA-256 的 Unifont 输入：
`python3 meiso_edge/targets/k230d-linux/make_overlay_font.py unifont-17.0.05.hex.gz overlay-cjk16.bin`。
字符范围见 Display Ingress Profile，授权见[字体声明](https://github.com/MeisoTech/meiso_edge/blob/main/targets/k230d-linux/overlay-font-NOTICE.txt)。
保留安装组件交付的字体声明。Host 开发包不携带供应商库，Edge 包不携带内核/驱动/系统镜像。

## 安装并连接

将两个安装目录分别复制到对应设备的版本前缀；记录源码版本、包校验和及匹配的固件版本。
先停止应用并完成 drain，再停止 Host、Edge 服务后切换版本；保留上一前缀方便恢复。
不要把新包直接覆盖到正在使用的目录。

下面是首次安装配置；升级时保留已有配置和部署所需的路由字段。先在各终端设置
`HOST_IP`、`EDGE_IP`、`HOST_PEER`、`EDGE_PEER`；地址须可双向路由，两个 PeerID
各为非零、互不相同的32位十六进制。两端互换 local/expected 配置。
`HOST_PREFIX` 与 `EDGE_PREFIX` 为对应设备的绝对安装前缀。

Host 在服务用户的终端执行：

```sh
install -d -m 700 "$HOME/.config/meiso" "$HOME/.config/systemd/user"
install -m 644 "$HOST_PREFIX/share/meiso/meiso-host.service" "$HOME/.config/systemd/user/"
cat > "$HOME/.config/meiso/host.env" <<EOF
MEISO_SERVE=$HOST_PREFIX/bin/meiso-host-serve
MEISO_RUNTIME=$HOST_PREFIX/bin/sdc_meiso_host_runtime_video
MEISO_HOST_IP=$HOST_IP
MEISO_EDGE_IP=$EDGE_IP
MEISO_LOCAL_PEER=$HOST_PEER
MEISO_EXPECTED_PEER=$EDGE_PEER
MEISO_CONTROL_PORT=4440
MEISO_MEDIA_PORT=4441
MEISO_EDGE_MEDIA_PORT=4442
MEISO_WIDTH=1280
MEISO_HEIGHT=720
MEISO_AU_BYTES=30720
MEISO_VIDEO_LAYOUT=side-by-side
EOF
chmod 600 "$HOME/.config/meiso/host.env"
```

Edge 以 root 执行：

```sh
mkdir -p /etc/meiso
chmod 700 /etc/meiso
cp "$EDGE_PREFIX/share/meiso/edge.conf.example" /etc/meiso/edge.conf
# 编辑这四项：MEISO_PREFIX、MEISO_HOST_IP、MEISO_LOCAL_PEER（Edge）、MEISO_EXPECTED_PEER（Host）。
vi /etc/meiso/edge.conf
chmod 600 /etc/meiso/edge.conf
```

k230d-bsp #1 起的 K230D 镜像（release `looking-glass-k230d-0.2.0` 及以后）根文件系统只读，`/etc/meiso` 位于数据分区。镜像自带
`/etc/init.d/S90meiso-edge`，它按 `edge.conf` 的 `MEISO_PREFIX` 执行
`$MEISO_PREFIX/share/meiso/init.d/S90meiso-edge`，不需要再复制启动脚本。
较早的可写根镜像仍按原方式安装：

```sh
cp "$EDGE_PREFIX/share/meiso/init.d/S90meiso-edge" /etc/init.d/S90meiso-edge
chmod 755 /etc/init.d/S90meiso-edge
```

默认宽高、布局和端口与上面一致；脚本会选择包内已安装的字库。详情见
[BusyBox 服务说明](https://github.com/MeisoTech/meiso_edge/blob/main/targets/k230d-linux/README.md#busybox-开机服务)。
预先配置双向路由；服务安装不改 Wi-Fi、转发或防火墙。

相机上行是可选的。确认目标板 ISP 与采集节点可用后，在 Edge `edge.conf` 同时设置
`MEISO_CAMERA_DEVICE=/dev/video1` 和 `MEISO_CAMERA_PORT=4443`（节点按板型确认）；
在 Host `host.env` 设置 `MEISO_CAMERA_PORT=4443`。当前 K230D NV12 路径还需 Host 设置
`MEISO_CAMERA_COLORIMETRY=bt601-limited`；桌面 RGBA 相机不设置此项。
相机窗口组合的已测配置还将 Host `MEISO_AU_BYTES` 设为 `131072`，避免沿用简单演示图的
30720 字节预算。它是单个压缩帧的上限，不是实际帧大小或已承诺的链路吞吐。
相机 TCP 端口必须不同于控制 TCP 端口，允许当前路由双向通过。
当前 v9 应用 Profile 可在同一 Session 中组合相机、双眼视频和原生 HUD；升级需同时安装
匹配的两端版本，不能仅替换一侧。未配置相机时继续使用视频/HUD。

Host 服务也读取可选 `MEISO_NATIVE_OVERLAY=on|off`、
`MEISO_PRESENTATION_EVIDENCE=physical|simulated` 和 `MEISO_STATIC_CALIBRATION` 路径。
默认仍是 physical 与原生 HUD；只有桌面输出环境显式选择 simulated。
这些值经启动器同一参数检查，显式命令行参数优先；实际设备的标定不能从示例或另一块板复制。

```sh
# Host（使用已配置的服务用户）
systemctl --user daemon-reload
systemctl --user enable --now meiso-host.service
# Edge
/etc/init.d/S90meiso-edge start
```

Host 日志出现 `HOST_SERVICE_READY` 后才启动应用。systemd active/Edge status 只表示进程存活，
不代替应用发现和实际呈现结果。无交互登录启动还需管理员为 Host 服务用户配置 linger。
故障后服务重新连接会建立新 Session；应用须重新启动，不自动重放旧句柄和内容。

## 显示内容与开发应用

以下在 Host 上运行；`HOST_PREFIX` 为安装前缀，服务默认 socket 是
`$XDG_RUNTIME_DIR/meiso/runtime.sock`：

```sh
RUNTIME_SOCKET="$XDG_RUNTIME_DIR/meiso/runtime.sock"
# 只有 Edge 绘制的消息：提前创建文件；原子替换文件内容更新，空文件隐藏。
printf '你好，Meiso\n' > "$PWD/message.txt"
"$HOST_PREFIX/bin/sdc_meiso_native_application" --socket "$RUNTIME_SOCKET" \
  --video off --message-file "$PWD/message.txt"
# Ctrl-C 正常退出后，再运行左右眼动画与 Edge 消息。
"$HOST_PREFIX/bin/sdc_meiso_native_application" --socket "$RUNTIME_SOCKET" \
  --frames 120 --message 'Hello Meiso'
# 已安装源 runtime 和 hello_xr 时，运行真实 OpenXR 投影视频。
"$HOST_PREFIX/bin/meiso-openxr-preview" --presentation api-source \
  --message-file "$PWD/message.txt" -- hello_xr -g Vulkan2
```

消息和视频的来源均在 Host，消息像素在 Edge 生成。应用 receipt 中的 `executed` 对应远端实际
结果，视频 source release 区分 presented/unobserved。正常退出还需确认 drain/close 和清屏，
不能只看提交计数。应用异常断开走服务端清理；正常关闭不一定打印 disconnected cleanup 日志。

开发自己的应用，可把安装的例子复制到仓外，仅依赖安装 SDK：

```sh
cp -R "$HOST_PREFIX/share/meiso/examples/screen_message" ./my-message-app
cmake -S my-message-app -B my-message-app/build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOST_PREFIX"
cmake --build my-message-app/build -j4
./my-message-app/build/meiso_screen_message "$RUNTIME_SOCKET" '你好，Meiso'
```

`native_video` 例子同样可独立构建；两者使用 `find_package(MeisoHost 0.1 CONFIG REQUIRED)`，
链接 `LookingGlass::HostClient` 与 `MeisoHost::NativeOverlay`。应用不链接具体 HostRuntime、
Jetson 编码器或 K230D 驱动。更多图元参数、外部 RGBA 输入和 X11 窗口入口见 Looking Glass
presentation README。公开发行许可和渠道另行确定；技术包可运行不等于已经正式发布。
