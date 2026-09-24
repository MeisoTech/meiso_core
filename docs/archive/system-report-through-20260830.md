> 历史记录：保留原日期与范围；当前使用见[快速开始](../host-edge-quickstart.md)。

# 当前系统状态

> 状态：`CURRENT IMPLEMENTATION AND EVIDENCE SUMMARY`，更新于 2026-08-30。
> 本页只报告实现与证据；产品语义只由[文档入口](../index.md)列出的五份规范定义。

## 基线

- pushed source baseline：`main@ecc12b3988d8a0c3f7a84f67f2ec3b69af431074`；当前 SDK
  重组工作树尚未生成最终 commit；
- 专项门已覆盖 53-test inventory、Release、sanitizer、header 与 install consumer；
  全部并发修改冻结后的 fresh unified gate 仍待主任务收口；
- 实现语言：主体为caller-owned、bounded C11；原始硬件日志、配置和凭据不进公开仓。

## 已实现的垂直路径

| 层 | 当前实现 | 证据边界 |
|---|---|---|
| Resource | `State / Stream / Action / Blob`、descriptor snapshot、bounded arena | portable semantic，不冻结 C ABI |
| Session | `MSL0` v1，`OPEN/ACCEPT/CONFIRM/READY`，sticky terminal | 无resume、无安全transport、无应用层心跳 |
| Runtime | atomic catalog、desired/actual、production gate、正式 Composition 路径 MRT0 v4 | v2/v3仅内部 compatibility fixture；非通用QoS/plugin系统 |
| Composition | `COMPOSE_/_PROFILE:v1`、MRT0 v4、6 endpoint/18 route、Blob/Scene/C2、RenderResult | 固定 Profile，非任意scene/media协商 |
| Host | opaque `EdgeHandle`、typed command/snapshot、media borrow，simulator/trace | 固定HostRuntime backend，开放backend ABI与通用watch/publish未完成 |
| Edge | K230D Linux Wi-Fi/Device/GC2093、Scene/C2、CPU raster、双media | off-screen reference compositor，不是DRM/DSI物理present |
| Editor | native Composition Host Bridge + loopback Dashboard + CMFB | browser不持wire；非远程Web control plane |

## K230D compositor 与渲染结论

K230D 上的**集成 CPU offscreen compositor 已运行验证**：Host 传递304-byte canonical
Mesh Blob和完整Scene/Presentation State，Edge BlobStore发布后经SceneStore/C2激活world，
CPU raster生成320×240 RGBA8888 target、RenderResult SHA/thumbnail与RenderPreviewMedia。真实GC2093
CameraMedia、Wi-Fi和DeviceTelemetry在同一control Session中继续进展。

这不是physical compositor验证：当前没有DRM/KMS、DSI、display plane、fence、page-flip/
vblank或面板像素证据。历史standalone K1a offline ELF仍只是build-only artifact；它与
后来在`K230dCompositionEdgeRuntime`内实际运行的integrated CPU backend是两个不同证据对象。

## 当前 K230D 证据

- 单adapter camera：4-buffer V4L2，3帧NV12 `320x240/115200B`，零mapping/queue债；
- combined/editor：Blob、Scene/C2 ACTIVE、CPU render、render/camera CMFB、Wi-Fi/Device和clean shutdown；
- 2026-08-26对`ecc12b3`的fresh复核：18个render、33个连续camera record、6路Actual、
  Edge rc0且camera/media ledger全零；脱敏summary SHA-256为
  `81758307e111da177caa8dbc88ff22b71054cebec7e79e984f4a45e1f6d6a713`。

以上是旧身份历史证据；当前正式身份的fresh board复验尚未完成。验收字段与不外推边界见
[K230D Composition E2E](https://github.com/MeisoTech/meiso_edge/blob/main/docs/k230d-composition-e2e.md)。

## 连接与恢复现状

- control和media均为numeric direct-IP plain TCP；TCP locator不是peer identity；
- Session handle一次性；EOF/RST/protocol failure后sticky terminal，新TCP必须新nonce、
  `SessionId`、arena和handle；
- 新Session必须重新catalog、desired、actual和media bind；旧view/token/route/frame不复活；
- Composition Editor只有host-local fresh-child supervisor：它丢弃旧child truth，退避创建新Host并重放
  browser拥有的最新完整intent；这不是Session resume或Edge restart；
- Session v1没有`PING/PONG`、协商idle deadline或可执行heartbeat failure。当前仅从合法输入、
  EOF/RST/socket/protocol error得知liveness；SSE keepalive仅属于browser HTTP。

## 尚未完成

- authenticated/encrypted/integrity-protected transport与replay protection；
- 可协商、有界、可测的Link heartbeat/liveness timeout；
- target-in-loop断网、Edge restart、通用fresh-Edge supervisor、第五media slot恢复；
- physical display/scanout、实际fence/vblank/panel、codec与performance/power/soak；
- Looking Glass HostRuntime到 Meiso `EdgeHandle`/`COMPOSE_/_PROFILE:v1` 的当前 adapter
  与 mainline evidence。

过去的逐commit记录已移到[归档报告](system-report-through-ecc12b3.md)。
