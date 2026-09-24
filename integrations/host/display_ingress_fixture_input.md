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

（该程序与其测试在 [meiso_edge](https://github.com/MeisoTech/meiso_edge)，2026-09-24 随 Display Ingress 服务拆出。）

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

## 受控 Host/Edge pair 模式

同一个未安装的本机证据程序还提供一条更窄的公开入口：

```text
/absolute/path/meiso_display_ingress_simulated_service --pair --frame-fd N
```

程序路径必须是绝对路径。`N` 必须大于等于 3，并且是调用方已经连接、已经设为非阻塞的
`AF_UNIX SOCK_STREAM` 读端。调用方继续拥有另一端，负责写入恰好一条 `DIFI/v1` 记录并用
EOF 结束；pair 模式从参数验证成功开始拥有传入的读端，在所有成功或失败路径恰好关闭一次，
也不会向调用方写逐帧确认。

pair 父进程不读取、不缓存也不重编码 DIFI 像素。它先创建全新的 IPv4 回环 TCP 连接、全新的
connected UDP 四元组和一条内部单向 readiness pipe，再把调用方的 frame fd 唯一转移给 Host
子进程并立即关闭自己的副本。Host 仍使用上一节唯一 reader；它完整验证记录、看到明确 EOF、
关闭 frame fd 后，并且在调用 `service_run()` 或写任何 control byte 以前，向 readiness pipe
写一个固定字节。这个内部字节只表示 Host 输入已验证且 fd 已关闭，不是画面、送达或业务 ACK，
也不出现在 stdout 或收据里。

pair 父进程只有收到该字节以后才 `fork` Edge，因此父进程和 Edge 从未持有或读取 RGBA，Edge
也不可能继承 frame fd。父进程自己不创建 Session、Runtime、Profile binding 或媒体对象。
它在没有 worker thread、没有待刷新的 stdio 输出时 `fork` 两个真实子进程；子进程直接调用
同一已加载程序映像中的共享 role entry，不在 pair 内再次 `exec`。现有 `--role host|edge`
入口及其独立 `fork/exec` 测试仍保留。

新建 socket、capture pipe 和 readiness pipe 初始都带 close-on-exec；直接 `fork` 不会执行
close-on-exec，所以每个子进程仍必须显式关闭不属于自己的全部描述符。父进程用一个不会因输入
或输出进展而重置的总期限，在等待 readiness 时也监控 Host 退出并 drain 其 stdout/stderr；
Edge 启动后继续同时 drain 两个子进程，并以非阻塞 `waitpid` 回收。超时或任何中途错误都终止
并回收所有已启动的子进程。read、write、poll 与 waitpid 等等待/收集调用遇到 `EINTR`
时重试；close 仍只调用一次。`POLLHUP` 先 drain，`SIGPIPE` 不得终止父进程。

成功以前，pair 必须确认两侧都以 0 退出、stderr 为空、stdout 各恰好一条本页定义的 V2
收据；随后交叉核对完整 SessionId、catalog/Desired/Actual/production/media epoch、画面几何、
sequence、source time、deadline、drop high-water、输入/接收/证据 SHA、模拟证据类别、
connector、UDP 武装顺序和 clean shutdown。只有全部相等且符合固定值以后，pair stdout 才写
一条 canonical 聚合收据：

```text
Success: schema, status, host, edge, fixture_only,
         physical_evidence, link_security, clean_shutdown
Failure: schema, status, stage, fixture_only,
         physical_evidence, link_security, clean_shutdown
```

成功 schema 是 `DISPLAY_INGRESS_SIMULATED_PAIR_RECEIPT_V1`，`status="SUCCESS"`；`host` 与
`edge` 是已验证并去掉末尾换行的完整 V2 JSON 对象，顺序固定为 Host 后 Edge；最后四项固定
为 `true, false, false, true`。已识别的运行失败使用同一 schema、`status="FAILURE"`，stage
只允许 `input`、`bootstrap`、`child`、`receipt` 或 `timeout`，最后四项固定为
`true, false, false, false`。失败不得透传子进程的半条或未验证 stdout。参数错误仍不产生
JSON，只输出 `E_ARGUMENT` 并返回 2；运行失败返回 1，成功返回 0。

这条聚合收据只是把两个本机 fixture 结果做严格配对，不是业务 ACK，也不是物理呈现证据。
pair 模式不安装、不导出公共库，不声明内部使用了 `exec`，也不声明当前可执行文件的 SHA-256、
ELF 身份或“由某个源码 commit 构建”。这些 artifact 与源码的因果关系必须由外部构建证明和
后续调用方单独校验。

## 明确未完成

本合同只服务受控 fixture E2E。它不建立产品级 Host ingress、连续 frame queue、背压策略、
线程或 scheduler；不替代 Looking Glass 到 Meiso 的正式依赖授权、版本 pin 与 sender adapter；
也不满足 Link 的身份认证、机密性、完整性或防重放要求。
