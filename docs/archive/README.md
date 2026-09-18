# 文档归档

> 状态：`HISTORICAL / NON-AUTHORITATIVE`。这些文件保留开发过程、commit、资源账本和
> 当时证据；不再表示当前实现状态或产品权威。

| 归档 | 截止点 | 当前入口 |
|---|---|---|
| [系统进展累积报告](system-report-through-ecc12b3.md) | 历史切片、CI、target receipt 至 `ecc12b3` | [当前系统状态](../system-report.md) |
| [Edge compositor 开发历史](edge-local-compositor-development-history-v0.md) | C0–C2、R1–R2b5、K1 分片过程 | [当前 Edge compositor](../edge-local-compositor-v0.md) |
| [Host 集成历史](host-integration-history-v0.md) | MRT0 v2/v3 与早期 facade 证据 | [当前 Host 集成](../host-integration-receipt-v0.md) |
| [Composition Profile 正式化前历史](composition-profile-pre-formal-history.md) | 旧 Profile ID、分版、逐 commit 证据 | [当前 Profile](../../profiles/composition/README.md) |
| [Runtime 开发历史](runtime-development-history.md) | 旧 Profile/wire 分版、逐 commit 证据与资源账本 | [当前 Runtime](../runtime.md) |
| [Runtime v2/v3 compatibility 历史](runtime-v2-v3-compatibility-history.md) | 内部旧 wire/FSM 详细与逐 commit 证据 | [当前 compatibility 说明](../runtime-core-v0.md) |
| [Direct-IP MediaStream 开发历史](media-stream-direct-ip-v0.md) | 旧 Profile 身份下的 transport/media 分片 | [当前 MediaStream](../media-stream-direct-ip.md) |
| [Composition 正式化前 K230D E2E](k230d-composition-pre-formal-e2e.md) | 旧 Web/receipt 身份下的 exact 实机记录 | [当前 E2E 门](../k230d-composition-e2e.md) |
| [Composition 工程地图](composition-runtime-map-through-20260902.md) | 截至 2026-09-02，原折叠段 | [当前实现地图](../system-runtime-map.md) |
| [Display Ingress bring-up](display-ingress-bringup-through-20260906.md) | 原 v1、单帧 service 与早期 DRM 研究 | [当前 Profile](../../profiles/display_ingress/README.md)、[DRM 研究](../../research/k230d-drm/README.md) |
| [K230D 早期服务与视频记录](k230d-linux-bringup-through-20260907.md) | 旧 wrapper/Composition、单帧及 2026-09-07 视频实测 | [当前 target](../../targets/k230d-linux/README.md) |
| [K230D Linux target 开发历史](k230d-linux-target-development-history.md) | 旧 cross artifact、逐 commit receipt 与 target 实验 | [当前 target 入口](../../targets/k230d-linux/README.md) |

五份通用规范、当前 Profile/wire 合同和源码 API 不归档。历史文件中的
`CURRENT`、`NOT IMPLEMENTED`、`TARGET NOT RUN` 等标记只对它的截止点成立。

- [截至2026-08-30的阶段状态](system-report-through-20260830.md)：已被当前应用快速开始替代。
- [C SDK结构审计](c-sdk-organization-report.md)：已完成的结构整理与当时欠账。
