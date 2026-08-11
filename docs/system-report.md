# 系统进展

> 日期：2026-08-12
> 状态：公开、已脱敏的 evidence report；不是产品规范，不包含原始日志、地址、凭据、
> 唯一设备标识或本机路径。

## 架构重基线

产品规范现已直接使用 `State / Stream / Action / Blob`、Virtual Edge 和 transport-
independent Session boundary。旧公共 API、旧私有 wire 和旧术语没有兼容承诺；当前 C11
代码及双板实验只作为可复用行为与失败证据。

规范入口是 [docs/index.md](index.md)。本报告不规定 platform、runtime、sensor、frontend、
transport 或实现顺序。

## 当前公共代码

| 内容 | 已证明 | 未证明 |
|---|---|---|
| `meiso_latest_state` | 固定容量完整 snapshot、newest-wins、pending、精确 ACK、duplicate/conflict、version wrap gate 与新 session 重发 | 新 State API、Resource Registry、Session identity、wire、scheduler、concurrency、persistence 或 actual application |
| `meiso_slice` | 确定性 frame identity、pose/freshness/uncertainty gate、lease、session isolation 与 fail-closed workload | Virtual Edge、通用 Resource Runtime、真实 MediaStream、platform adapter 或产品 Composition |
| synthetic demo | 固定 camera-like frame、模拟姿态/检测与 PPM 输出 | 真实 camera、sensor、codec、network、display 或 target performance |

这些模块早于当前规范。其文件名、C symbol、slot layout 和 ABI 都是 legacy evidence，不是
新公共 SDK。迁移可以提取 behavior/conformance vector，也可以在更小实现中重建；不需要
兼容旧调用面。

本次纯文档重基线在 AppleClang 17、CMake 4.2 与 Ninja 1.13 环境重新以 warnings-as-errors
构建，现有 3 个 CTest 全部通过。它只证明文档改动没有破坏 legacy baseline，不证明新
Resource/Link/Virtual Edge 契约已经实现。

## Resource evidence

| Resource | 当前 evidence | Gap |
|---|---|---|
| State | 公共 fixed-capacity newest-wins primitive；私有代理实验验证 desired/actual、ACK 丢失、幂等、重启重发与 lease safe state | 尚无按新 descriptor/ownership/actual contract 实现的公共 Runtime |
| Stream | 私有代理实验验证 timestamped sensor-like sample、deadline-bound result、bounded history 和 failure isolation | 尚无公共 Stream endpoint/epoch/backpressure API |
| MediaStream | 私有 i.MX8MM/Orin workload 验证持续 H.264/RTP、recovery、硬解码、identity-bound result 和本地预览 | 私有 wire/codec/平台路径不是公共 transport/Profile，也不能外推到其他平台 |
| Action | 只有旧 fixture 的 ordering/duplicate 行为，没有当前驻留 operation | 尚无公共 Action request/result/idempotency runtime |
| Blob | 旧实验验证 chunk、integrity、manifest、有限 retry/failure 和恢复思路 | 尚无公共 Blob content identity/atomic publish runtime |

## 代理平台实验

一个 640×480 lab workload 曾在 i.MX8MM Edge proxy 与 Orin Host 间得到：

- H.264 media、Host decode/detection、source-frame identity 与 Edge-side bounded correction；
- 验收窗口约 27 fps 的 encode/decode/preview 进展；
- 媒体发送失败时，capture、encode 与本地 preview 继续进展；
- desired 与 actual 分离、ACK 丢失重试、session 重发、lease safe state；
- bounded history、payload integrity、Dashboard 和资源额度停止行为。

这些结果只证明当时 adapter/workload 的形状和失败门。Hantro、RTP/UDP、Linux process、
设备节点、私有 packet、Orin 计算路径和约 27 fps 数字都不是新架构约束。

## K230D lab evidence

一块 `CanMV-K230D-Zero V1.1` 已使用
[官方 CanMV v1.8 Zero release](https://github.com/kendryte/canmv_k230/releases/tag/v1.8)
从 TF/SD 完整启动到 CanMV REPL；启动链实证 U-Boot SPL、DDR training、OpenSBI、
RT-Smart/CanMV 与 nncase runtime。对应
[Zero 官方规格](https://www.kendryte.com/k230_canmv/en/main/userguide/boards/canmv_k230d.html)
声明 128 MiB LPDDR4，但本次启动并未测得可供 Meiso 使用的容量或预算。这个结果只证明
准确镜像实例能够启动。

当前公开 evidence 尚未证明该板实例的 camera module、display panel、touch、microphone、
IMU、codec/KPU workload、network data path、clock quality、usable memory budget、持续温度、
功耗或任何产品 Profile。firmware 中包含 driver/API 也不等于物理 endpoint 存在。

系统通过通用 capability/endpoint discovery 表达这些组件，不为 K230D Zero 或某种 sensor
建立 Core 特例。CanMV、native runtime、SDK sample 或其他镜像都可以用于后续实验；结果
必须按具体版本和条件记录。

## 已验证的负面结论

- 把每帧 raw camera 当成可靠完整 transfer 会造成高带宽与旧数据阻塞，不适合连续 Stream；
- per-frame ACK/retry 会让过期媒体妨碍实时进展；
- 软件 JPEG 在当时代理 workload 中未达到目标帧率；
- driver start/success 不能证明首个真实 sample 或 physical output；
- 在现有 CPU workload 尚有余量时先建设通用 GPU/DLA framework，没有证据支持其复杂度；
- private wire 之间已经出现不兼容语义，不能作为新公共 wire 的迁移基础。

负面结论同样带平台与 workload 范围；它们用于设计 failure vector，不用于禁止所有未来
实现。

## 未实现与未知

- 稳定的 Host-facing `EdgeHandle`/Virtual Edge API；
- Resource descriptor/registry 和 State/Stream/Action/Blob runtime；
- 生产 LinkSession handshake、安全 transport、公共 wire、time/reconnect 参数；
- capability/endpoint discovery 和真实 platform adapter；
- MediaStream 与 Resource lifecycle/telemetry 的公共集成；
- Composition API、prepared generation 与真实 presentation endpoint；
- 任一完整、声明并验证过的产品 Profile；
- fuzz/sanitizer/CI、资源预算和长期 fault/soak evidence。

这些 gap 是无序能力集合。实际下一项由当前产品问题、可用硬件、风险和可执行验收选择，
不在本报告中固化开发路线。
