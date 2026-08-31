# K230D Composition E2E

> 状态：`CAMERA PASS / COMBINED PENDING`。当前正式身份下的独立 camera probe 已闭合；
> Composition combined run 仍待完成。正式化前的实机记录已
> [归档](archive/k230d-composition-pre-formal-e2e.md)，不能替代本轮复验。

## 当前身份

- Host service 的 `service.ready.profile` 为 `composition`；
- Session ProfileRef 为 `COMPOSE_/_PROFILE:v1`，Runtime wire 为 `MRT0 v4`；
- local Web 工具为 `devtools/web/composition_editor/`，页面名称为 `Composition Editor`；
- K230D camera/offscreen receipt 的 `build_target` 为 `k230d_linux`；
- offscreen receipt 以 `contract=composition-scene` 标识所验合同，不把 scene schema 冒充为
  Session Profile。

这些字段已经进入当前源码与本地测试。Camera 现已绑定 exact commit、Xuantie ELF 和 fresh
K230D probe；完整 Composition 身份仍须由同一代源码的 combined run 闭合。

## 已闭合的 source、cross 与 fresh camera evidence

- product commit `4628f6905867f18ab34c42a2dbecefe415d14d0b`，tree
  `5bafa890ee351924d49dd88e979fb860ea0ce47e`；
- Xuantie V2.6.0 / GCC 10.2.0 的五个公开 wrapper 均从 fresh source 通过，构建日志无
  warning/error；ELF 均为 RISC-V LP64D，exact artifacts 如下：

| Target | Bytes | SHA-256 |
|---|---:|---|
| Camera capture once | 34,560 | `72458524691eaf9f2c8a68f7d3fb8a25a6559809f3e5035798f7fe8d46827800` |
| Composition Edge service | 265,856 | `333e67722278d060f404f9722d1209ec597e2cf13873b952a8acad2e6cc53121` |
| Composition offscreen runner | 75,248 | `003d6fc9b23550b37be6d3da808ff0651b8e66532c2992875734d0d8bced83ec` |
| Four-kind lab | 91,576 | `e976e9dcf67b99c712ab082ca70763e2920600fbda31ff5063e47f18693a1ea3` |
| Wi-Fi Runtime Edge | 100,504 | `c2deaadc769e5b1d7b6b77dc1c00ec73c374c9e01b9175926c3fd48243eb4983` |

- fresh boot 后只执行一次 probe、无 retry：三帧均为完整 320×240 NV12 / 115,200 bytes，
  effective sequence 为 1/2/3，payload SHA-256 各不相同；
- vvcam 保留了真实 raw timestamp：三帧均为 zero，同时 receipt 明确标记
  `ADAPTER_DEQUEUE_MONOTONIC`，effective monotonic time 严格递增；没有把 fallback 冒充成
  driver capture timestamp；
- 最终 phase 为 STOPPED；stream、borrow、driver/mapped/queued buffer、mapped bytes 与
  primary/cleanup failure ledger 全部归零；ISP owner 保持不变，vvcam Used 为零，SD ext4
  error count 与 CMA allocation-failure count 均为零；
- 41-file 私有 evidence manifest 的 SHA-256 为
  `75901a31e31482039e8fbefd36e2b00cbc488ca9088b69dd4d333576f94acc4e`。Public repo 不保存
  raw frame、设备标识或本机路径。

该 probe 只关闭 camera capture、timestamp provenance 与 cleanup 门；它不替代下面的
Composition combined Session、Editor、CMFB 和 shutdown 证据。

## Fresh run 必须闭合的门

1. **Source / cross**：记录 exact public commit、工具链、五个 K230D target 的 ELF size 与
   SHA-256；所有 receipt 只接受上述当前身份。
2. **Camera**：fresh boot 后只运行一次 GC2093 three-frame probe；三帧完整、effective sequence
   与 effective monotonic time 严格递增、raw timestamp 与 provenance 原样记录；退出后 phase、
   mapped、queued、borrowed 和 stream ledger 全部归零。该门已由上述 probe 关闭。
3. **Composition**：同一 fresh Session 内完成 Blob、Scene、Presentation、CPU offscreen render、
   RenderMedia、CameraMedia、Wi-Fi 和 DeviceTelemetry 收敛；Host 验证完整 CMFB/v1 record。
4. **Editor**：loopback-only Composition Editor 通过唯一 native Host child 提交固定编辑意图；
   State version、ContentId、Session、generation、epoch 和 sequence 只来自 native read-only truth。
5. **Shutdown**：Host/control 保持在线时只向已核实 Edge PID 发送一次 `SIGTERM`；Edge `rc=0`、
   terminal reason 为 local abort，camera/media 账本归零，原 ISP owner 与系统服务保持不变。

任一 camera、cleanup、identity 或 resource-ledger 门失败后都不重跑以挑选成功样本；先保存失败
证据并修复根因。

运行前必须检查 board memory/CMA admission；不足时标记为该次运行的
`ADMISSION_BLOCKED`，不启动 camera/compositor、不重试挑样，也不将瞬时资源不足
写成永久产品失败。资源条件恢复后可在新的 fresh run 中重新评估。

## 不外推边界

本轮即使全部通过，也只证明准确 K230D/GC2093 实例、准确源码和受控局域网中的组合工作流。
它不证明 physical display/fence/vblank、codec、TLS/auth、Internet transport、板端重启恢复、
吞吐上限、长稳、thermal/power、通用 K230 板型或产品级远程 Web control plane。

原始日志、帧和设备信息只保存在受控 lab evidence 目录；public repo 只记录脱敏摘要、哈希与
可复现输入。完成 fresh run 后，本页再追加 exact commit、artifact hashes 和闭合结论。
