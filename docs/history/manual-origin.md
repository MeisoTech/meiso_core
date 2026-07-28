# 手写方向来源

> 状态：非规范的手写历史。不是规范，也不是待实现接口。

本页保护提交 `70c364376a07f7c4a7ec713b8979bf37295e9cb1` 中独有的手写方向。当前含义已经收敛到 [系统设计](../system.md)、[Composition 契约](../composition.md)、[链路契约](../link.md)、[SDK 设计与开发指南](../sdk-guide.md)和[系统进展与待决策](../system-report.md)。

旧 `SDK_DESIGN_OVERVIEW.md` 与更早提交中的生成稿逐字节相同，不属于这次独有手写输入，因此不在这里复制。完整历史仍由 Git commit 保存。

## 概念映射

| 手写输入 | 当前权威 | 保留的含义 |
|---|---|---|
| `Edge Core <-> Host Core` | [system.md](../system.md) | `Edge` 与 `Host` 是唯一 peer boundary |
| `Host Runtime` 与 CLI | [system.md](../system.md) | 它们是 `Host` 包装，不是第三角色 |
| 单侧持有 truth | [system.md](../system.md)、[link.md](../link.md) | 每个同步值只有一个 owner |
| object `a` | [link.md](../link.md) | 无 ACK、允许丢失的 observation |
| object `b` | [link.md](../link.md) | 有 ACK，最新版本取代旧版本 |
| object `c` | [link.md](../link.md) | 有 ACK、有序、不可静默丢失 |
| object `d` | [link.md](../link.md) | 可分块，但必须完整完成或明确失败 |
| 多 object 共用 packet | [link.md](../link.md) | 链路容量和所有 buffer 有界 |
| `c > b > a > d` | [link.md](../link.md) | Object plane 拥塞时的保留与准入偏好 |
| discovery、reconnect、heartbeat | [link.md](../link.md) | `Link` 管理 session 生命周期 |
| heartbeat 与时间估计 | [link.md](../link.md) | liveness 交换可复用时钟观测 |
| sensor 配置与 lease | [sdk-guide.md](../sdk-guide.md) | `Host` 拥有期望配置，`Edge` 拥有 sample |
| HUD、Render Profile | [composition.md](../composition.md) | per-layer source 可并存，Edge 拥有最终合成 |
| 网卡由 Meiso 持有 | [link.md](../link.md) | 应用不拥有 adapter、socket、port 或独立 connection；UDP 是内部 reference carrier |
| 早期三层网络结构 | [link.md](../link.md) | 已细分为 Object/Media、LinkSession 与可替换 Carrier |
| object handle 中的 ACK | [link.md](../link.md) | acknowledged form 提供可观察 delivery 状态 |
| 过大的 `b` object | [link.md](../link.md) | 大型 payload 以 `d` 交付，`b` 通过 content hash 引用 |
| object 是消息形式 | [link.md](../link.md) | 不要求 heap object 或长期生命周期 |
| 早期 wire 草稿 | [link.md](../link.md) | wire 有界、可版本化、可检查长度与传输错误；字节布局延后 |
| data-link 暂不细写 | [link.md](../link.md) | 未测量真实硬件前不发明 raw carrier、MTU、queue 和 retry 参数 |

## 原始手写文本

以下内容保留原始措辞。语言选型尚无 BSP/工具链证据；“不需要 TCP/UDP”等措辞现在只表示 carrier 不暴露给应用，并不否定 UDP/IP reference carrier；早期 byte table 含重复字段和类型错误。它们都不是当前 contract。

### `docs/spec/index.md` manual section

```text
## Language Neutrality

本 spec 是语言中立 contract。示例使用 payload shape 或伪代码表达

实现分层原则：

| Layer | Preferred Implementation |
|---|---|
| Edge HAL / MCU firmware | C （非本篇内容）|
| Edge embedded runtime core | C |
| Host core runtime / transport / daemon | Rust or C++ |
| Host Full runtime SDK| Rust |
| CLI | Rust |

## 基本结构
Edge Core ↔ Host Core 底层双端通信协议，所有协议都会通过这里
Host Core ↔ Host Runtime Host层对Core的进一步封装
Host Core/Runtime ↔ CLI/Dashboard 本质是对Host的进一步封装

## 网络基本Core
1. meisoObject 是为了同步而存在的，一个meisoObject一定由单侧持有（truth），另一侧被同步
2. meisoObject分为4类
    a. 不带ACK的Object：如传感器数据，日志，电量。允许丢失
    b. 带ACK但是Last抢占的Object：如功能开关。需保证最后状态不丢失
    c. 带ACK的queue：如眼镜触摸屏或按键交互事件。需要有明确顺序，且所有不可丢失
    d. 带ACK的bulk object：如图片。允许分块发送，且必须完整送达
3. 每一次发送网络包允许携带X byte数据，多个Object的同步数据可以打包发送。同步内容过多时允许小buffer，再多了丢弃。保留顺序：c>b>a>d
4. 对于应用层来说，只有上述4种object可以使用，对于底层连接建立来说，还需拟定一个过程，利用链路层发现设备，建立连接，以及在重启重连的时候恢复状态。心跳包可以和NTP同步包共用，这样时间同步就在网络底层里了，还省一个心跳包
5. 主流接口返回各种object handle，object handle里有是否收到回包ack等内容

## 网络设计参考
比如传感器数据：
传感器本身是一个b object，由Host持有，Host修改b的参数或者开关，同步到Edge
Edge创建传感器的Data object，是一个a object，由Edge持有，并持续同步给Host直到本地的时钟超过传感器租约
开关传感器可以简化为：修改传感器租约到期时间，传一个过去的时间等于关闭
a object的创建也可以不带ACK，收到任意包直接创建
唯一需要注意的是，如果设备重启关机，需要完善的释放逻辑
核心设计点在于：网卡全部由meiso持有，不需要端口，不需要建立连接（每个应用不需要），允许合并 会话层，表示层和应用层和传输层 四层的数据
TCP UDP也不需要了
同时点到点传输不需要路由，网络层也扬了
3层网络结构
物理层
链路层
应用层
这样和linux也不沾边了

## 其余系统位置
开关系统功能： Host修改对应b object，Edge接收到包后响应ACK。接口返回object handle，查询object handle的状态就可以知道本次开关是否成功了送达了
系统电量，传感器数据，State：Edge持有对应的a object，持续传回给Host，Host无需回应Edge （a object 无ACK）
HUD，Render Profile：上层系统，基本上是由Host持有各种object然后传给Edge处理
功能开关，各种Flag：b object

## Q&A
Q:如果一个a object 或 b object塞不进一个网络包如何？
A:b object塞不进应该用bulk object。另外，这里叫object并不代表一定需要创建一个管理生命周期的object，也指代一种消息格式，特别是a object这种没有ACK的，我edge完全可以不存，传感器数据直接转成a object的数据丢出去。
```

### `docs/spec/wire-protocol.md`

```text
# Wire 协议规范

## Basic Wire   Host Core ↔ Edge Core
把一个包发到另一端的手段，有可能会丢包
这里已经是抽象底层了，不考虑LoRA or WiFi or BLE 或者实际上是有线

基本包结构
| Offset | Size | Field | Type | Rule |
|---:|---:|---|---|---|
| 0 | 2 | `magic` | chat[2] | fixed `"MS"` |
| 2 | 1 | `version_kind` | uint8 | V0.1 = `1`|
| 3 | 1 | `seq` | uint8 | sequence |
| 4 | 1 | `seq` | uint8 | Xor Check |
| 5 | 2 | `payload_len` | uint8 | payload len byte |
payload should less than 65536Byte


## Meiso Net Queue
```

### `docs/spec/data-link.md`

```text
# 这一部分和嵌入式具体硬件强相关，暂时不做书写
```
