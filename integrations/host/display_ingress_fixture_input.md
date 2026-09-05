# Display Ingress 本机测试画面输入

> 状态：`CURRENT HOST CONTRACT`。本页唯一拥有 `DIFI/v1` 的字节布局、codec、所有权与失败语义。

## 用途与边界

`DIFI/v1` 是调用方送给模拟 Host 进程的一次性、本机测试画面记录。它只搬运一张完整画面，
不是 Meiso 会话或资源运行时协议，不是 `DIFR` 媒体 packet、网络协议、呈现证据或送达确认，
也不得复用 Composition Host Bridge 的 `CMFB/v1` 预览输出格式。

它不携带或推导 Session、catalog、endpoint、resource、production/media epoch、nonce、Runtime
媒体绑定身份、prepared token、Looking Glass 私有身份或 source-release 权限。成功解码也只表示
一张 Host 测试输入可提交，不表示网络送达、Edge 接收或物理呈现。

## `DIFI/v1` 字节布局

记录以 128-byte canonical big-endian header 开始，随后紧跟一张完整 RGBA8888 画面：

```text
0..3      magic `DIFI`                                      4 bytes
4..5      record version (=1)                               u16
6..7      header bytes (=128)                               u16
8..11     complete record bytes (=128+rgba bytes)           u32
12..15    flags：bit 0=deadline present，其余必须为零       u32
16..19    input width                                       u32
20..23    input height                                      u32
24..27    row pitch bytes (=width*4)                        u32
28..31    RGBA payload bytes (=width*height*4)              u32
32..39    frame sequence                                    u64
40..47    source clock epoch                                u64
48..55    source monotonic time                             u64
56..63    sender deadline clock epoch                       u64
64..71    sender deadline monotonic time                    u64
72..79    producer whole-frame drop high-water              u64
80..111   complete RGBA payload SHA-256                     32 bytes
112..127  reserved                                           必须全零
128..     tight top-left opaque RGBA8888                    exact payload
```

`flags` 只有 bit 0 已定义。sequence 与 source clock epoch 必须非零。deadline absent 时 deadline
clock epoch 与 time 必须都为零；deadline present 时 deadline epoch 必须非零且等于 source
epoch，deadline time 可以为零。producer drop high-water 是调用方提供的累计整帧丢弃上界，
本无状态 codec 不判断其跨记录单调性。

source time 是调用方在所声明 source clock epoch 中记录的 producer/source 单调时间戳。
service 必须原值传入媒体帧，不从 display target time 推导，也不替换成本进程的
local now。该 source clock epoch 与 production epoch、media stream epoch 及 Edge 证据时钟
是不同的身份。

宽高各为 1..4096，row pitch 必须紧密等于 `width*4`，payload 必须精确等于
`width*height*4` 且不超过 67,108,864 bytes；完整记录最大 67,108,992 bytes。像素顺序是
top-left row-major R、G、B、A，每个 alpha byte 必须为 255。所有乘加使用 checked arithmetic。

## 公开 codec

公开头文件是 `<meiso/host/display_ingress_fixture_input.h>`，CMake target 是
`Meiso::DisplayIngressHostFixtureInput`，installed component 是
`DisplayIngressHostFixtureInput`。这组 C11 API 提供：

- typed frame 到精确 128-byte header 的编码；codec 从实际 payload 自己计算 header 中唯一的
  SHA-256，调用方不再提供第二份摘要真值；
- 精确 128-byte header 的解码；
- header 与完整 payload 的长度、SHA-256、opaque alpha 校验，成功后返回借用 caller payload
  的 `MeisoDisplayIngressMediaFrame`；
- 对完整 payload 计算同一 SHA-256，供接收侧对实际收到的画面形成测试证据。

codec 不分配内存、不读写 fd、不保留 pointer。encoder 不复制 payload；validator 返回的 frame
只在 caller payload 仍有效期间可用。媒体 transport 的 `submit()` 会在返回前复制整帧；
只有它返回成功后，codec 的 payload borrow 才可结束。所有 typed input、output、size output 和
payload alias 必须遵守 public header 的 disjoint 规则。失败不改 caller 的输出 bytes；encoder
在可安全写入且互不重叠的 `out_size` 中报告零。

公开 status 的失败向量固定如下；先命中的结构门决定结果：

| API | 条件 | 结果 |
| --- | --- | --- |
| encode | null/invalid buffer，或 typed input、完整 RGBA range、output、out-size 任意别名 | `INVALID_ARGUMENT` |
| encode | output 小于 128 bytes | `CAPACITY` |
| encode | 任一维大于 4096，或 payload/record 超上限 | `FRAME_TOO_LARGE` |
| encode | 零尺寸、row pitch 非 `width*4`、零 sequence/source epoch、非 canonical deadline，或未超上限但 payload size 与几何不符 | `INVALID_VALUE` |
| encode | 任一 alpha 不是 255 | `NON_OPAQUE_ALPHA` |
| decode | 非零 size 却为 null 的 invalid view，或 input 与 typed output 别名 | `INVALID_ARGUMENT` |
| decode | 少于/多于 128 bytes | `TRUNCATED` / `EXTRA_BYTES` |
| decode | magic 不是 `DIFI` | `MALFORMED` |
| decode | version 不是 1 | `UNSUPPORTED_VERSION` |
| decode | header-size/flags/reserved 不合法 | `MALFORMED` |
| decode | 尺寸或声明 byte 上限越界 | `FRAME_TOO_LARGE` |
| decode | 其余 size/metadata/deadline 不 canonical | `INVALID_VALUE` |
| validate | 非零 size 却为 null 的 invalid view，或 header/payload/output 任意别名 | `INVALID_ARGUMENT` |
| validate | 手造 header 不 canonical | 与 decode 的 value/size 分类相同 |
| validate | payload 少于/多于 header 声明 | `TRUNCATED` / `EXTRA_BYTES` |
| validate | 实际 SHA 与 header 不同 | `DIGEST_MISMATCH` |
| validate | SHA 相同但 alpha 非 255 | `NON_OPAQUE_ALPHA` |

SHA helper 只接受非空且不超过 64 MiB 的 payload，并要求 digest output 与 payload disjoint；
失败返回 `false` 且不写 digest。

## 模拟 Host 的一次性 fd reader

`meiso_display_ingress_simulated_service --role host` 必须额外收到且只收到一次
`--frame-fd N`；Edge 角色禁止这个参数。该 fd 必须大于等于 3、不同于 control/media fd，
且是调用方建立并已连接的 nonblocking `AF_UNIX SOCK_STREAM`。service 不改变 fd flags，
不向它写回逐帧 ACK。调用方拥有父进程连接端；service 只拥有并关闭 exec 后继承的子进程
fd 副本。

Host 在向 Session/control socket 写任何 byte 以前，使用一个不会因分段读取而重置的总期限，
完整读取 header 与 payload，再探测明确 EOF。一次 read 只返回部分内容是正常 stream 行为；
`EINTR` 重试，`EAGAIN` 回到 poll，`POLLHUP` 仍先 drain 内核中可读 byte。下列情况都在媒体
submit 前 fail closed：空输入、header 或 payload 中途 EOF、声明超上限、payload 后多一个
byte、完整记录后未及时 EOF、错误 fd 类型/family/flags、hard I/O、codec 拒绝、SHA 不同或
alpha 非 255。

当前模拟 service 在公开上限之上再要求固定 64x32、row pitch 256、RGBA 8192 bytes、完整
记录 8320 bytes，并要求当前全新 production epoch 的第一帧 sequence 为 1；它还使用不支持
deadline 的媒体配置。因此 codec 合法但尺寸不同、sequence 不是 1 或携带 deadline 的输入也会
以 application/input failure 结束；service 不 resize、不重编号、不删 deadline。

成功 stdout 收据使用 `DISPLAY_INGRESS_SIMULATED_SERVICE_RECEIPT_V2`，Host 明确写
`frame_input="DIFI/v1"`，并从实际读取或实际收到的 frame 填写几何、metadata 与 SHA-256。
这条收据不是 ACK，且仍固定 `physical_evidence=false`。

每个角色的 stdout 恰好是一条终态 NDJSON；字段及顺序固定如下：

```text
Host success: schema, role, status, frame_input, session_id,
              catalog_generation, applied_desired_version, actual_version,
              production_epoch, media_stream_epoch,
              input_width, input_height, row_pitch_bytes, rgba_bytes,
              frame_sequence, source_clock_epoch, source_time_ns,
              sender_deadline_present, sender_deadline_clock_epoch,
              sender_deadline_ns, producer_dropped_frames,
              input_sha256, evidence_input_sha256, evidence_class,
              connector, carrier, physical_evidence, link_security,
              clean_shutdown
Edge success: schema, role, status, session_id,
              catalog_generation, applied_desired_version, actual_version,
              production_epoch, media_stream_epoch,
              input_width, input_height, row_pitch_bytes, rgba_bytes,
              frame_sequence, source_clock_epoch, source_time_ns,
              sender_deadline_present, sender_deadline_clock_epoch,
              sender_deadline_ns, producer_dropped_frames,
              received_sha256, evidence_input_sha256, evidence_class,
              connector, udp_armed_before_actual_write, carrier,
              physical_evidence, link_security, clean_shutdown
Failure:      schema, role, status, plane, stage, carrier,
              physical_evidence, link_security, clean_shutdown
```

只有 Host 声明自己读取了 `frame_input="DIFI/v1"`；Edge 收据只声明它从媒体 transport
实际收到的 frame，不能冒充 DIFI reader。成功 `status` 是 `SUCCESS`，失败是 `FAILURE`。
`session_id` 是双方协商出的完整 SessionId 的 64 个小写十六进制字符；所有 SHA 字段是
32-byte digest 的 64 个小写十六进制字符。`evidence_class` 固定 `SIMULATED`，`connector`
固定 `UNSPECIFIED`，`carrier` 固定 `CONTROLLED_RAW_UDP`。input failure 使用
`plane="application"`、`stage="input"`；收据刻意不增加会持续扩张的诊断 reason 字符串。
精确拒绝类别由 codec status 与测试矩阵证明，不成为第二套 wire 合同。已识别角色运行失败
返回 1；参数非法仍只在 stderr 输出 `E_ARGUMENT`、不产生 stdout 并返回 2。

## 明确未完成

本合同只服务受控 fixture E2E。它不建立产品级 Host ingress、连续 frame queue、背压策略、
线程或 scheduler；不替代 Looking Glass 到 Meiso 的正式依赖授权、版本 pin 与 sender adapter；
也不满足 Link 的身份认证、机密性、完整性或防重放要求。
