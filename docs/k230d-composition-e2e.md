# K230D Composition E2E

> 状态：`PENDING FRESH BOARD EVIDENCE`。本页定义当前正式身份下的验收边界；
> 正式化前的实机记录已[归档](archive/k230d-composition-pre-formal-e2e.md)，不能替代本轮复验。

## 当前身份

- Host service 的 `service.ready.profile` 为 `composition`；
- Session ProfileRef 为 `COMPOSE_/_PROFILE:v1`，Runtime wire 为 `MRT0 v4`；
- local Web 工具为 `devtools/web/composition_editor/`，页面名称为 `Composition Editor`；
- K230D camera/offscreen receipt 的 `build_target` 为 `k230d_linux`；
- offscreen receipt 以 `contract=composition-scene` 标识所验合同，不把 scene schema 冒充为
  Session Profile。

这些字段已经进入当前源码与本地测试，但尚未绑定到一个新的 exact commit、Xuantie ELF 和
fresh K230D run，因此目前不能声称新身份已获实机验证。

## Fresh run 必须闭合的门

1. **Source / cross**：记录 exact public commit、工具链、五个 K230D target 的 ELF size 与
   SHA-256；所有 receipt 只接受上述当前身份。
2. **Camera**：fresh boot 后只运行一次 GC2093 three-frame probe；三帧完整、不同时间戳，退出后
   phase、mapped、queued、borrowed 和 stream ledger 全部归零。
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
