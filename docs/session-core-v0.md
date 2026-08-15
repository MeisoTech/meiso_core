# Session Core v0 实现契约

> 状态：`IMPLEMENTED / HOST_SEMANTIC_VERIFIED / HOST_WIRE_VERIFIED`。本页冻结
> portable Session Core v0 的 source/wire 行为；当前不声明 TCP、认证、
> 安全 transport 或 K230D Session evidence。

## 边界

Core 拥有 connection：peer/boot/Session identity、exact protocol/Profile、handshake、
bounded wire、lifecycle 与 Runtime-message gate。Runtime 拥有 Resource Graph：capability、
descriptor/instance、owner/schema/QoS、endpoint 和控制逻辑。Host SDK 拥有 Virtual Edge。

`meiso_resource_core` 的四类 primitive 保持 session-agnostic；它们允许本地 truth、capture
或已发布 Blob 在离线时继续存在。跨 peer 路由由 Runtime 强制：只有 Core
`READY` 且 Registry route 有效时，remote message 才能进入对应 typed primitive。

## Identity 与配置

| 值 | 形式 | lifecycle |
|---|---|---|
| `PeerId` | non-zero 128-bit | peer 的稳定 identity，不随 IP 或重启变化 |
| `Incarnation` | non-zero `u64` | peer Runtime 每次重启必须改变 |
| `SessionNonce` | 128-bit | 每次 transport connection 由 bootstrap/platform 生成 |
| `SessionId` | `HostNonce || EdgeNonce` | 256-bit；每次连接唯一，v0 不 resume |
| `ProtocolVersion` | `major:u16, minor:u16` | v0 只接受 exact match |
| `ProfileRef` | `id:128-bit, version:u32` | v0 只接受 exact match |

Validity：两个 `PeerId`、nonce、Profile id/version 与 protocol major 均 non-zero；local 与
expected `PeerId` 必须不同。`CLOSE` reason 固定为 `1 NORMAL / 2 SHUTDOWN /
3 PROTOCOL_ERROR`。

Host bootstrap 可从 gitignored 本地配置读取
`expected_peer_id -> IP:port`，再向 direct-TCP adapter 注入 typed options。Core 不解析文件、
IP、port、DNS 或 socket；IP 只回答“连到哪里”，`PeerId` 才回答“对方是谁”。
Bootstrap 必须为每个新 handle 注入 fresh、不得复用的 local nonce；Core 只验证 non-zero，
不生成 nonce，也不跨 handle 持久追踪 uniqueness。

## Limits

Handshake 协商三个 non-zero `u32` 上限：

| limit | 含义 |
|---|---|
| `max_frame_bytes` | header 在内的单帧上限 |
| `max_runtime_payload_bytes` | `RUNTIME` 中除 `SessionId` 外的 payload 上限 |
| `max_registry_entries` | Core 协商并交给 Runtime 的 Registry admission ceiling；Core 不解析 Registry |

双方在 `OPEN/ACCEPT` 中各发 local limits，再按字段取 `min(local, remote)`。
Local limits 必须满足 `max_frame_bytes >= 104`、`max_runtime_payload_bytes <=
max_frame_bytes - 44` 且 `max_registry_entries != 0`，否则配置或 handshake 失败。
Resource payload、queue/history/retry 的更细上限属于 Runtime/descriptor。实现使用调用方
arena 和显式 capacity，不分配无界内存。

## Source API

| API | 职责 |
|---|---|
| `meiso_session_alignment_required / memory_required / init` | 在 caller-owned arena 内建立一次性 handle；不 heap allocate |
| `meiso_session_start` | Host 排队 `OPEN`；Edge 进入 wait-`OPEN` |
| `meiso_session_next_outbound` | 借用当前完整 outbound frame；同一 handle 下一次 non-const API 调用前有效 |
| `meiso_session_mark_outbound_sent` | 仅在 adapter 完整接管该帧后推进状态 |
| `meiso_session_receive` | 验证一帧完整 bytes；只在 `READY` 交付 `(SessionId, borrowed Runtime payload)`；同一 handle 下一次 non-const API 调用前 payload 失效，需跨调用则复制 |
| `meiso_session_submit_runtime` | 在 `READY` 且 TX idle/容量允许时排队一帧 `RUNTIME` |
| `meiso_session_close / transport_lost` | 正常关闭或 sticky terminal failure |
| `meiso_session_snapshot` | 读取 identity、state、failure、negotiated limits 和 TX 状态 |

Session Core 不打开 socket，也不处理 stream reassembly 或 partial write。Transport adapter
用自己的有界 buffer 组成完整 frame，对 outbound frame 循环写入，完整成功后
才调用 `mark_outbound_sent`。同一 Session handle 的全部调用由 owner event loop 串行化；
不同 handle 可独立。实现不提供内部 locking，reader/writer 线程不得并发调用同一 handle。
Loopback 也必须经过同一 codec，不得绕过 wire。

## Wire

Core 编解码显式字段，禁止把 C struct 直接复制到 wire。整数使用 big-endian。

```text
offset  size  field
0       4     magic = "MSL0"
4       1     wire_version = 1
5       1     message_type
6       2     reserved = 0
8       4     total_frame_bytes, includes header
12      ...   payload
```

| type | value | payload bytes | payload（按表中顺序） |
|---|---:|---:|---|
| `OPEN` | 1 | 92 | Host `PeerId, Incarnation, Nonce`；expected Edge `PeerId`；`ProtocolVersion, ProfileRef, Limits` |
| `ACCEPT` | 2 | 92 | Edge `PeerId, Incarnation, Nonce`；echo Host `Nonce`；相同 `ProtocolVersion, ProfileRef`；Edge local `Limits` |
| `CONFIRM` | 3 | 32 | `SessionId` |
| `READY` | 4 | 32 | `SessionId` |
| `REJECT` | 5 | 17 | echo Host `Nonce, handshake failure 1..11:u8` |
| `CLOSE` | 6 | 33 | `SessionId, reason:u8` |
| `RUNTIME` | 7 | `32+n` | `SessionId, opaque Runtime payload` |

读取 payload 前必须检查 magic、version、reserved、完整长度和本地
`max_frame_bytes`。未知 type、trailing bytes、错误 SessionId 或超限 payload 都是明确
failure。`RUNTIME` payload 对 Session Core 不透明；Registry 和四类 Resource envelope
由 Runtime codec 处理。

## Handshake

v0 不做 range negotiation、fallback 或隐式降级：

```text
NEW -> HANDSHAKING -> READY -> CLOSING -> CLOSED
          |            |
          +----------> FAILED
```

1. Host 在 transport 连接后发 `OPEN`。
2. Edge 验证 role、identity、exact protocol/Profile 和 limits，发 `ACCEPT`。
3. Host 验证 expected Edge、nonce echo、exact protocol/Profile 和 limits，发
   `CONFIRM(SessionId)`。
4. Edge 验证 SessionId，发 `READY(SessionId)`。Edge 在完整 `READY` 帧被有序
   transport 接管后进入 Core `READY`；Host 收到并验证该帧后进入 Core `READY`。

任一方在 Core `READY` 前发出 `RUNTIME`，或按错误顺序发 handshake message，Session
进入 sticky `FAILED`。Transport writer 接受 bytes 只表示本地 transport 接管，不表示
peer 接受、Resource actual、Action result 或物理效果。

`REJECT` 只在当前 handshake 的合法等待阶段有效，并必须回显该 attempt 的 Host nonce。
nonce 不匹配是 `SESSION_MISMATCH`；READY 或其他非法阶段收到 `REJECT` 是
`UNEXPECTED_MESSAGE`，当前 Session fail-closed，但不接受该帧携带的 failure reason，也不回发
另一个未绑定 SessionId 的 `REJECT`。adapter 随后关闭 transport。

## Core READY 与 Runtime READY

```text
transport connected
  -> Core OPEN / ACCEPT / CONFIRM / READY
  -> Runtime RegistrySnapshot / RegistryAccepted
  -> per-endpoint READY
  -> Resource delivery
```

Core `READY` 只开放 opaque Runtime message gate；每次交付同时返回已经验证的
`SessionId`。未来 Runtime 契约必须另行冻结 route 与当前 Session、版本化 Registry snapshot、
ResourceId 和 kind 的绑定；未声明 ID、kind 不匹配或旧 route 不得进入 typed primitive。
本页不定义 Registry generation 字段或产品 route codec。Core 关闭时
Runtime 显式执行各 Resource lifecycle：State 的有效 truth 在新 Session 重新 offer；Stream
默认新 epoch；未知副作用的 Action 进入 `indeterminate`；partial Blob transfer 取消，
已验证 publish 的 Blob 不删除。

## 终止与失败

Session handle 一次性。正常 `CLOSE`、transport loss、protocol failure 或 local close 后
不得 reset；重连创建新 handle、nonce 和 SessionId。v0 不 reconnect、resume、replay
或 migration。双方同时 `CLOSE` 是正常竞态；当前 SessionId 验证通过后进入 `CLOSED`，
snapshot 分别保留 local/peer close reason。最小稳定 failure 分类为：

```text
1  MALFORMED_FRAME        2  UNEXPECTED_MESSAGE    3  ROLE_MISMATCH
4  PEER_MISMATCH          5  INVALID_INCARNATION   6  INVALID_NONCE
7  PROTOCOL_MISMATCH      8  PROFILE_MISMATCH      9  LIMITS_INCOMPATIBLE
10 SESSION_MISMATCH      11  EARLY_RUNTIME         12  TRANSPORT_LOST
13 LOCAL_CLOSED          14  PEER_CLOSED
```

## 安全与证据

`PeerId` 比对可以防止误连，但不会自动证明身份。本协议不实现 authentication、
confidentiality、integrity 或 replay protection；产品 Profile 必须用经审查的安全
transport 将 authenticated peer 绑定到 `PeerId`。

证据标签必须逐层使用：

| 标签 | 最多证明 | 不证明 |
|---|---|---|
| `HOST_SEMANTIC_VERIFIED` | Host 状态机与 failure vectors | wire bytes、network、security、target |
| `HOST_WIRE_VERIFIED` | Host codec、golden bytes、truncation/limit vectors | TCP、security、target |
| `TRANSPORT_VERIFIED` | 指定 adapter 与条件下的数据移动 | authentication、K230D、产品 Profile |
| `BOARD_VERIFIED` | 指定 image/board/adapter 的实测 | 未声明 transport 或 endpoint |

在对应测试和报告存在前不得使用上述验证标签。当前 fresh AppleClang
Debug build 的 CTest `11/11` 通过；其中 `meiso_session_test` 对七种 frame 提供独立
golden bytes，并验证状态机、nonce/Session binding、failure、truncation、length 和 limits；
`meiso_session_four_kind_test` 使用明确的 test-only envelope 验证 READY/SessionId gate、
undeclared ResourceId、wrong kind、旧 Session copy，以及四类 primitive 集成。它不验证
Registry generation 或产品 Runtime codec。该结果只支持两个 HOST 标签。

## 最小验收

- identity/incarnation/nonce/Session 分离，exact protocol/Profile/limit 每轴独立失败；
- `OPEN/ACCEPT/CONFIRM/READY` 正常完整路径；duplicate `OPEN` 与 Host/Edge 各一条
  skip/out-of-order 向量；adapter 另行验证 partial write/would-block 时不提前 mark；
- 七种 frame 的独立 golden bytes、大小端、逐字节 truncation、错误
  length/reserved/type/nonce/SessionId；
- `RUNTIME` 在 Core `READY` 前不发出、不交付，close 后旧 Session route 失效；
- 同 Peer/incarnation 重连仍得到新 Session，旧 frame 被拒绝；
- 相同 codec 经 bounded loopback 端到端验证，并在 Runtime gate 后分别进入
  State/Stream/Action/Blob primitive；四类失败互不污染。

## 非目标

v0 不定义 discovery、IP 以下网络、socket API、TLS/密码系统、clock estimator、
Registry/capability schema、endpoint control、Virtual Edge、resume、multipath 或 transport migration。
