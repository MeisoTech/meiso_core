# Pairing v0 两端契约

> 状态：两端契约 v0。编解码已实现（`include/meiso/pairing.h`，CMake target `Meiso::Pairing`），
> 有 golden 测试（`tests/test_pairing.c`）；Host、Edge 尚未接入，板上未验证。

一副 Edge 第一次遇到一台 Host 时，两边怎么认识对方、各自记下什么、以后怎么不靠人改配置就连上。

## 1. 边界

- **配对**：两台设备第一次互相记下对方的 PeerId、名字和“怎么去”。它发生在 Session 之外、
  Session 之前，结果是两端各存一条“已配对”记录。
- **[Session Core v0](session-core-v0.md) 不改**：Host 的 expected `PeerId` 和 Edge 要核对的
  Host `PeerId` 从已配对记录取，不再来自手写配置。配对用的 PeerId 就是 Session Core 的
  `MeisoPeerId`。
- **记录按 PeerId 存**：地址只回答“连到哪里”（[Link](link.md#identity)：连接地址不能替代
  peer identity）。以后换有线或别的网络，只是换一个地址。
- **换 Host**：结束当前 Session，再和另一台已配对 Host 建新 Session（[Link](link.md#建立与恢复)），
  不续接，也不搬应用状态。

不在 v0：

- 密钥身份、带认证和加密的 Session；
- Edge“选主机”界面和 Host 配对界面的样子与文字；
- Host 与 Host 之间的配对；
- BLE、mDNS、WPS，以及 Wi-Fi 关联和地址以外的网络管理。

## 2. 身份与记录

**PeerId**：每台设备第一次启动时自己生成。生成规则：

- 从操作系统的密码学随机源取 128 位，不能为零；
- 存进掉电不丢、只有本机服务能读的存储，以后不变；
- 部署脚本不再写入 PeerId。

两端各记下这些。文件格式由各端自己定：

| | Edge：已配对 Host（最多 8 条） | Host：已配对 Edge |
|---|---|---|
| 身份 | Host PeerId | Edge PeerId |
| 名字 | Host 名字（§4.2） | Edge 名字（§5.1） |
| 怎么去 | SSID、信道、Host 地址、Edge 在该网段的地址、配对口 | Edge 在本网段的地址 |
| 时间 | 配对时刻、上次连上的时刻 | 配对时刻 |

记录的维护：

- **Wi-Fi 口令**：Edge 只把它交给系统的 Wi-Fi 凭据存储（仅 root 可读），不放进这条记录。
- **重配**：同一 PeerId 再配一次时，新记录替换旧记录；Host 侧旧记录占用的地址随之释放。
- **忘掉**：各自在本地删除。对方下次来连时 PeerId 对不上，按 Session Core 失败。
- **Edge 记满 8 条时**：忘掉哪一条由 Edge 自己决定。
- **Host 侧地址**：Host 可以记多副 Edge，每条记录的 Edge 地址互不相同。Host 按连接的来源地址
  找到期望的 Edge PeerId。

## 3. 流程

1. Host 旁边的人打开配对窗口（v0 是 Host 上的一条命令，例如 `meiso-pair open`）。
2. Host 显示 QR 文本（§4）。
3. Edge 用相机读出 QR 文本，加入 Host 的 Wi-Fi，设好地址 `e`，在配对口上发请求（§5）。
4. Host 答复，两端各存记录，窗口关闭。
5. 之后照常建 Session；谁连谁和现在一样。

## 4. 配对窗口与 QR 文本

### 4.1 配对窗口

- **谁开、开多久**：只能由 Host 本机上的人打开，每次最多 120 s，同一时刻只有一个。
- **开窗口时 Host 依次做**：
  - 检查 Host 名字合规（§4.2）；不合规就不开，并说明原因；
  - 用密码学随机源生成 80 位一次性码，写成 16 个 RFC 4648 base32 字符（`A`–`Z`、`2`–`7`，
    无填充）；
  - 选出 Edge 地址 `e`：在 Host 的网段里、DHCP 地址池外，也不是任何已配对 Edge 记录正在用的
    地址；
  - 开始监听配对口：默认 4444/TCP，控制、媒体和相机用的是 4440–4443。窗口外不监听。
- **窗口关闭**：下面任一件事发生就关，关后码作废、配对口停止监听：
  - 配对成功；
  - 到时；
  - 连续 3 次请求的码不对；
  - 人取消。

### 4.2 QR 文本

一行 ASCII，最多 300 字符：

```text
MEISO-PAIR:1;n=<Host 名字>;h=<Host PeerId>;s=<SSID>;p=<口令>;c=<信道>;a=<Host 地址>;e=<Edge 地址>;o=<配对口>;k=<一次性码>;x=<剩余秒数>;;
```

| 键 | 值 | 规则 |
|---|---|---|
| `n` | Host 名字 | 可打印 ASCII（0x20–0x7E），1–32 字节，首尾不是空格；默认取机器的 hostname |
| `h` | Host PeerId | 32 个小写十六进制字符，字节序与 Session wire 相同（high 在前，big-endian）；非零 |
| `s` | SSID | 1–32 字节可打印 ASCII |
| `p` | WPA2-PSK 口令 | 8–63 字节可打印 ASCII |
| `c` | 信道 | 2.4 GHz，1–13 |
| `a` | Host 地址 | 点分十进制 IPv4 |
| `e` | Edge 地址 | 与 `a` 在同一个 /24，主机号不同 |
| `o` | 配对口 | 1–65535 |
| `k` | 一次性码 | 16 个 base32 字符 |
| `x` | 剩余秒数 | 生成这段文本时窗口还剩的秒数，1–120 |

写法：

- **转义**：值里的 `\`、`;`、`,`、`:`、`"` 前面加 `\`，和 Wi-Fi 二维码（`WIFI:`）一样；
  其它字符不转义。
- **数字**：十进制，不带前导零。
- **地址**：四段十进制。首段不是 0、127 或 224 以上，末段不是 0 或 255。
- **字段**：顺序任意，每个已知字段恰好出现一次。不认识的字段跳过，留给后续版本，例如 Host
  公钥指纹 `f=`。
- **结尾**：文本以一个空字段结尾，即最后是未转义的 `;;`。
- **不合规**：整段拒绝，Edge 不做部分配置。

**Host 名字为什么只用 ASCII**：“选主机”是 Edge 本地的界面，没连上 Host 时也要画得出名字，
而 Edge 本地字体只有 ASCII 和少量提示用字。这条在 Host 开窗口时检查，问题暴露在 Host 旁边、
有人能改的地方。

**重访条件**：Edge 本地界面带上常用汉字字库时，放开为 UTF-8。

**Edge 读 QR**：

- 相机帧只在内存里处理，不落盘；读到文本或超时就停相机；
- 从读到起 `x` 秒后视为过期，不再发请求。

## 5. 配对交换

在 Host 的配对口上走 TCP：一问一答，然后关连接。整数和 PeerId 的字节序与 Session wire 相同
（big-endian）。

### 5.1 请求（Edge → Host，72 字节）

| 字节 | 内容 |
|---|---|
| 0..3 | `MPR1` |
| 4 | op；v0 只有 1 = pair |
| 5..20 | 一次性码，即 QR 的 `k` |
| 21..36 | Edge PeerId，非零 |
| 37..68 | Edge 名字，UTF-8，1–32 字节，不足补 0：名字里不含 0，名字结束后只有 0 |
| 69..71 | 0 |

Edge 名字的规则：

- 必须是严格 UTF-8：不允许 overlong、代理码点，码点不超过 U+10FFFF；
- 不含 C0/C1 控制字符和 DEL；
- 由 Host 显示，所以可以用中文。

### 5.2 答复（Host → Edge，24 字节）

| 字节 | 内容 |
|---|---|
| 0..3 | `MPR1` |
| 4 | 2 = paired，3 = refused |
| 5 | paired 时为 0；refused 时为下表的原因 |
| 6..21 | paired：Host PeerId；refused：全 0 |
| 22..23 | 0 |

| 原因 | 名称 | Host 什么时候回 |
|---|---|---|
| 1 | CODE | 码不是本窗口的，或已经用过 |
| 2 | EXPIRED | 连接在窗口内被接受，但请求到达时窗口已关 |
| 3 | BUSY | 正有另一次配对交换在进行。另一副 Edge 正在 Session 里不算忙：配对在 Session 外面，可以同时进行 |
| 4 | VERSION | 魔数和长度对，但 op 不是本 Host 认识的 |

### 5.3 Host

- **不回话、直接关连接**：
  - 接受连接后 5 s 内收不满 72 字节；
  - 前 4 字节不是 `MPR1`；
  - op 认识但其余字段不合规：码不是 base32、PeerId 为零、名字不合规、补零位不为零；
  - 连接的来源地址不是本窗口给出的 `e`。
- **op 不认识**：回 VERSION。
- **码不对**：比较一次性码用常数时间（`meiso_pairing_code_equal`）。码不对回 CODE 并计数，
  连续 3 次关窗口。
- **码对**：
  1. 存下这副 Edge：PeerId、名字，地址为 `e`；同一 PeerId 的旧记录被替换；
  2. 回 paired；
  3. 关窗口。
- **处理一次交换期间**：再来的连接回 BUSY，然后关闭。

### 5.4 Edge

- **设地址前**：先对 `e` 做 ARP 探测。有人回应就不用这个地址、不发请求，回到“选主机”。
- **失败**：发完请求后 5 s 内收不满 24 字节，或答复不合规，算失败。
- **paired**：答复里的 Host PeerId 必须等于 QR 的 `h`，才存下这台 Host；不相等就不存，
  算失败。
- **refused**：不存，按原因提示用户。

## 6. 配对以后

**Edge**：

- 开机只启用上次那台 Host 的网络，任何时刻只启用一个网络；
- 断开后不自动换到别的 Host；
- 换 Host 只经本地“选主机”：先确认扫得到它，再结束当前 Session，然后切网络、建新 Session。

**Host**：

- 按连接的来源地址查已配对 Edge 表，得到期望的 PeerId，再照 Session Core v0 握手；
- 查不到就不开 Session；
- 握手里 PeerId 对不上，照 Session Core 失败，不因为地址对就放行。

## 7. v0 的码能证明什么

**能证明**：

- 配对的人在 Host 旁边，看得见 Host 当时显示的码；
- 窗口是 Host 那边的人主动打开的。

空口上的门是 Wi-Fi 口令（WPA2-PSK）。

**不防**：

- **抢先配对**：拿到 QR 照片的人可以在窗口内抢先配对。窗口短、码只能用一次，只能缓解；
- **冒充 PeerId**：同一 Wi-Fi 上的设备可以冒充别人的 PeerId，因为 Session Core v0 本身不认证
  （[Link 安全](link.md#安全)）。

QR 文本里含 Wi-Fi 口令：Host 不把它写进日志，Edge 不保存 QR 图像。

**后续版本**：

- QR 带 Host 公钥指纹（`f=`），请求带 Edge 公钥；
- Session 用经过审查的标准协议认证、加密。

## 8. 实现与验证

**`Meiso::Pairing`**：随 Core 组件安装，只依赖 Session Core 的 `MeisoPeerId`。提供：

- `meiso_pairing_request_encode/decode`、`meiso_pairing_reply_encode/decode`：定长报文。
  - 解码先核长度、魔数、op 和补零，再核各字段。
  - 不认识的 op 也解码失败。Host 回 VERSION 前，自己看第 4 字节是否为
    `MEISO_PAIRING_OP_PAIR`。
- `meiso_pairing_qr_encode/decode`：QR 文本。编码时字段不合规或超出 300 字符就失败，不截断。
- `meiso_pairing_code_valid/equal`、`meiso_pairing_host_name_valid`、`meiso_pairing_edge_name_valid`。

这个库不含 socket、存储、时钟、随机源和 QR 图像识别，这些由各端实现。

**测试**：`tests/test_pairing.c` 覆盖两条报文和 QR 文本的 golden、往返，以及各类拒收。

**未验证**：

- 两端接入；
- Edge 板上读 QR 的距离和耗时；
- 换 Host 一圈，中间不改任何配置文件。
