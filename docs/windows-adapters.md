# Windows 网络适配

这是实现/API 接入说明；Session 与媒体语义分别沿用 [Link](link.md)、
[Runtime](runtime.md) 和 [Display Ingress](../profiles/display_ingress/README.md)。适配器不增加 wire、
Profile、发现协议或重连状态机。公开接口见
[TCP](../include/meiso/adapters/windows/tcp.h) 与
[DIFR UDP](../include/meiso/adapters/windows/display_ingress_media_udp.h)。

## 接入与所有权

应用负责 `WSAStartup(2.2)`、创建并连接 AF_INET/AF_INET6 socket、事件循环及最终
`closesocket`/`WSACleanup`。传入的 `connected_socket` 是保留完整位宽的 `uintptr_t`，
对应 Winsock `SOCKET`，不是 CRT 文件描述符。公开头不引入 Windows 系统头。

初始化先检查配置、arena、绑定和已连接的 socket 类型，最后用 `FIONBIO=1` 设置非阻塞。
Windows 没有与 POSIX `F_GETFL` 等价的非阻塞位查询，因此这一步是明确的初始化副作用。
应用在绑定期间不得改回阻塞模式，也不得并发消费同一 socket 或驱动同一 Session/media 对象。
适配器不创建线程、不分配堆内存、不关闭或 shutdown 借用的 socket。

TCP 使用一份 RX 帧和一份 TX 帧空间。每次 readable/writable 至多调用一次 recv/send，
长度限制为配置预算、当前帧余量与 `INT_MAX` 的最小值。分段发送保留确切的未发送部分；
Session 中途换帧而旧前缀已发出时进入 transport lost，不拼接两帧。EOF、硬错误和显式 abort
使 Session 失效，重新连接必须创建新 Session。配置的帧容量不得小于 Session 本地帧上限。

UDP 沿用现有 DIFR 封包与有界媒体对象：一个包对应一个 datagram，最多 1200 B。
Host 只发、Edge 只收。prepare 借用一个稳定的包；只有完整发送成功才完成该包。
`WSAEWOULDBLOCK`/`WSAEINTR` 保留原借用及游标，短发送与其他硬错误终结该媒体对象。
超长 UDP 包的 `WSAEMSGSIZE` 被记为整包截断并丢弃，截断前缀不交给媒体解码器；
长度恰为 1200 B 和空 datagram 仍交由完整 wire 校验判定。此处理依据
[Microsoft recv 说明](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-recv)。

绑定必须匹配新鲜的 ACTIVE media transport。每个新 Session/media 对象使用新的 UDP
四元组，不支持 tuple 复用或 resume。该适配器不认证/加密字节；诊断值是 Winsock 错误码。
本次不提供 CameraMedia TCP、相机设备、OpenXR 或视频解码适配。

## 构建与安装

按 [SDK Windows 构建](sdk-guide.md#windows-原生默认构建)构建后，应用可在构建树链接：

```cmake
target_link_libraries(app PRIVATE
    Meiso::WindowsTcpAdapter
    Meiso::DisplayIngressMediaWindowsUdpAdapter)
```

安装包使用 `find_package(MeisoGlass 0.1 CONFIG REQUIRED COMPONENTS Windows)`，
链接相同的 target；Winsock 系统库由 target 传递。按组件安装时需同时安装 Core、Runtime、
Profiles、Windows，或直接安装全部默认组件。

测试覆盖 Windows TCP 上的真实 Session/Runtime Catalog、Desired/Actual、Stream、断线后
重新协商；UDP 的 IPv4/IPv6 分片重组、初始化失败、角色限制、截断包、空包、非阻塞、
中断、短发送、硬错误及终结后的借用所有权。安装门迁移 prefix 后用 C11/C++17 编译并运行。
这些是原生回环与包测试，不构成头显显示、真实摄像头或跨机性能证据。
