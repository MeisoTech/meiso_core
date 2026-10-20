# Meiso 文档导航

先选择这次要做的事，不需要通读全部文档。

| 我想做什么 | 从这里开始 |
|---|---|
| 运行眼镜或写一个视频/消息应用 | [Host/Edge 快速开始](host-edge-quickstart.md) |
| 下载、构建一套匹配的版本 | [版本配对与发布](releases.md) |
| 理解代码、链接库或换平台 | [SDK 指南](sdk-guide.md)，需要进程/缓冲细节时读[实现地图](system-runtime-map.md) |
| 没有开发板，或验证跨设备Host/Edge | [桌面开发工具](https://github.com/MeisoTech/meiso_edge/blob/main/docs/desktop-development.md) |
| 改通信、资源或显示语义 | 下方对应的产品规范 |
| 查协议字段、旧集成或板测记录 | [工程参考索引](reference.md) |

## 产品规范

以下五份文档定义通用产品语义；当前选择的版本化契约列在其后。

- [System](system.md)：角色、职责、依赖和系统不变量。
- [Resource](resource.md)：State / Stream / Action / Blob 的所有权和生命周期。
- [Runtime](runtime.md)：能力、endpoint、desired/actual、路由和资源可见性。
- [Link](link.md)：身份、Session、时间、交付与失效。
- [Composition](composition.md)：图层、激活与最终呈现责任；当前 Display Ingress 选择其中的 direct-latest 语义。
- [Display Ingress planes-v12](../profiles/display_ingress/planes-v12.md)：当前空间平面、串流/静态图、跟踪、输入和呈现证据的两端 Profile 契约。
- [Pairing v0](pairing-v0.md)：Edge 与 Host 第一次配对：QR 文本、配对口上的两条报文和两端各记什么。

这里的 Composition 通用规范不等于默认关闭的旧 Composition 应用栈；
旧栈的构建和当前 CameraMedia 依赖见[SDK 组件边界](sdk-guide.md#3-cmake-package-与组件)。

Profile/wire 只能具体化规范留开的参数。实现指南、测试和实验报告不能创建另一套产品契约。

## 当前使用与历史

当前应用的支持范围、构建和设备要求集中在[快速开始](host-edge-quickstart.md)，不在每个入口
复制一份进度表。旧实验只证明其记录的源码、平台和负载；[历史归档](archive/README.md)用于追溯。
