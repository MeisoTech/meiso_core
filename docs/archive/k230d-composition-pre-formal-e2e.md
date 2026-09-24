# K230D Composition 正式化前 E2E 实机证据

> 状态：`HISTORICAL / NON-AUTHORITATIVE`。本页只证明下述 exact 旧 source/
> artifact/board run，不证明当前 Profile 身份或当前工作树。当前验收门见
> [K230D Composition E2E](https://github.com/MeisoTech/meiso_edge/blob/main/docs/k230d-composition-e2e.md)。

> 这是一次准确 source commit、准确 K230D/GC2093 实例和受控局域网条件下的
> target evidence。它记录四个独立验收门，不改变 [Composition](../composition.md)、
> [Runtime](../runtime.md) 或 [MediaStream](../media-stream-direct-ip.md) 契约。

## 固定输入

- public source commit：`0125afc11d6a1a218200376408eebd35f704a4b8c`；
- Edge ELF：254824 bytes，SHA-256
  `b991df0b74407211ede162bdfb9ee7255ce8308ce0478c9ee00792dd15eed82c`；
- macOS/Linux Host service SHA-256：
  `b9190450619589202e6e14fc2f7fdc3f106a75967b846c4e3cc8152b6e44a727`；
- combined board archive SHA-256：
  `710274f5aa923ed861980980c05b16d6debbf16e3b16e3d5e1b6af31d2d340f3`；
- direct-IP control 与 media 各使用一条独立 plain-TCP connection；SSH 只负责部署和
  只读取证，不进入 Meiso wire。

板端启动前发现 `/etc/init.d` 中保留了一个以 `S` 开头的旧 ISP script backup；BusyBox
启动时把它与当前 script 都执行，造成两个 `isp_media_server` 进程。旧文件按原 SHA-256
`a920fa128d8f12293a02f8ccb1fe31a1327227d2c07baccaf10e476a2484d3ed`
移动到 `/root/meiso-backups`，不修改当前 GC2093 script。重新冷启动后只有一个 ISP
进程，后续四个门都在该 fresh boot 内完成。备份文件不得放在 `/etc/init.d/S*` 名称下。

## 四个验收门

### G1 — fresh GC2093 camera

当前 source 构建出的 three-frame probe 为29112 bytes，SHA-256
`4248d76b6ac993b4f0aa271b89665a952b1a188763945ee0723807af0fbb5ee8`。
它在 fresh boot 上只运行一次，`rc=0`、stderr为空，并产生1479-byte
`THREE_FRAME_CAPTURE_RECEIPT`，receipt SHA-256为
`baba7181efd4df60f9e0fe09142b7609293259bc3b92865e951cae8e25ea78bb`。

4个MMAP buffer取得3帧NV12 `320x240/stride320/115200 bytes`；effective sequence为
`1/2/3`，driver sequence仍为absent，drop均为0。三帧SHA-256为：

1. `3a20f25f033bc6f86d991076705c79f8f3d2ab1e3850f9a9edc5a2e45d62f500`
2. `137a2e38c005338dd6062e607890f51333a688f61347d922d9ecb3b09d3d297a`
3. `086995eca70610a9127dae47e7891bf0ec41fc848bacafb154fd556f286eb74d`

退出后phase为`STOPPED`、mapped/queued为0，唯一ISP进程仍持有camera device。该门复验
single-adapter physical capture，不以此前receipt替代本次combined run的camera frame。

### G2 — combined render + camera media

同一 control Session 先建立render-only media slot，后在CameraMedia Desired/Actual准确收敛后
终止旧scope并用fresh slot建立`mask=both`。最终control snapshot为`ready`、terminal reason
`none`、catalog generation 3、media slots consumed 2；RenderMedia与CameraMedia Actual都为
`applied/present/running`，production epoch均非零。

Host bridge取得并验证两个完整CMFB/v1 record：

| source | header/payload | sequence | record SHA-256 |
| --- | --- | ---: | --- |
| render | 80B + RGBA8888 320x240 307200B | 7980 | `3d6a0cd6de7d97c56d0218fad4a580684247f5e5aa16a6a6ed552f5d9d03a8ab` |
| camera | 80B + NV12 320x240 115200B | 399 | `b4bb0e7b2f310843cf6e2f926d532ab43047637790963cb27baef75c0266194e` |

4秒后的持续性快照仍为`ready/no terminal/mask=both`，render/camera sequence已分别推进到
9519/1163；Wi-Fi和DeviceTelemetry sample仍更新，说明媒体没有阻塞control或inline Stream。

### G3 — local Web bridge 与编辑器

loopback-only Dashboard通过唯一native Host child投影`NATIVE_HOST / READY / ACTIVE`，页面显示
`DEVICE CONNECTED`与`MEDIA BOUND`。本次还修复了两个tooling问题：

- `actual`或`media.connected`后去重请求fresh native snapshot，避免页面长期停留在bootstrap
  `HANDSHAKE` truth；
- 只有browser revision变化时才同步表单，避免高频native render/pump SSE覆盖用户尚未提交的
  checkbox和数值。

相机、Wi-Fi、DeviceTelemetry和scene transform都通过Dashboard的fixed CSRF/Origin Web API进入
native bridge；浏览器/Python没有分配State version、ObjectId、epoch、sequence或解析Meiso wire。
Wi-Fi requested/present fields为`3/3`，DeviceTelemetry为`7/7`。把cube移动到x=1并绕Y轴45°后，
SceneStatus、RenderResult与render CMFB的world generation同时由1推进到2；新的full-target digest为
`6a2dc3fd5a583f921c7075975f53568290fcfc6220a9010c7054f6eaf4f1d9c6`。

浏览器自动化受本地URL policy限制，没有把最后四个POST冒充“自动点击”证据；可见页面状态、
表单抗SSE覆盖和同一Web API的真实native chain分别记录。标签限于
`HOST_LOCAL_BRIDGE_VERIFIED`，不是远程Web control plane或产品SDK。

### G4 — safe shutdown 与资源账本

保持Host/control在线，只向已核实的Edge PID发送一次`SIGTERM`。Edge输出唯一close receipt：

```text
status=SHUTDOWN reason=LOCAL_ABORT snapshot=true
camera_phase=1 camera_driver_buffers=0 camera_mapped=0
camera_mapped_bytes=0 camera_queued=0 camera_borrowed=false
camera_stream_on=false media_bound=false
```

Edge进程随后消失，control/media监听端口释放；`/dev/video1`只剩启动前同一ISP PID持有。
运行中`CmaFree`最低观察为360 KiB，退出后恢复到15000 KiB，`MemAvailable`为29340 KiB；
dmesg只有预期camera open/alloc/release，没有event4、allocation failure或segfault。最后才关闭
Dashboard/Host，loopback listener和native child均消失。独立shell wait status没有另行落盘；上述
`SHUTDOWN`只由runner clean-success分支产生，且process/driver/resource ledger已独立核为零。

## 2026-08-26 `main@ecc12b3` fresh re-audit

为排除旧 source/Host artifact 对结论的影响，又对当前 main 完成一次 fresh、单 Session、
`media_mask=both` 的脱敏复核：

- source：`ecc12b3988d8a0c3f7a84f67f2ec3b69af431074`；
- Edge ELF SHA-256：
  `b991df0b74407211ede162bdfb9ee7255ce8308ce0478c9ee00792dd15eed82c`；
- 6 路 Actual 均收敛，304-byte canonical cube Blob 发布，Scene/C2 进入 `ACTIVE`；
- 18 个完整 render CMFB 与 33 个连续 camera CMFB 通过离线一致性审计；
- RGBA payload SHA 与同 world 的 RenderResult 对应，Wi-Fi/Device 继续进展；
- Edge `rc=0`，camera/media 最终 ledger 全零，原 ISP owner 保持存活；
- 58 项 artifact 的脱敏 summary SHA-256 为
  `81758307e111da177caa8dbc88ff22b71054cebec7e79e984f4a45e1f6d6a713`。

这次复核直接驱动 native bridge，没有独立重做真实浏览器点击；它不新增 physical display、
安全 transport、断网恢复、第五 media slot、性能或 soak 证据。

## 证据边界

这次运行证明准确实例上的Blob 304B publish、Scene/C2 ACTIVE、CPU offscreen RenderResult、
RenderPreviewMedia、真实GC2093 CameraMedia、Wi-Fi/DeviceTelemetry、local Web bridge和clean shutdown
能在同一Session中组合工作。它不证明physical display、fence/vblank、codec/encoder、TLS/auth、
Internet transport、自动reconnect/resume、第五个media slot、吞吐上限、长稳、thermal/power、
通用K230板型或产品级Web安全。原始archive和CMFB保留在受控lab evidence目录，public repo只记录
脱敏hash与闭合字段。
