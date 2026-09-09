# Meiso 文档导航

先选择这次要做的事，不需要通读全部文档。

| 我想做什么 | 从这里开始 |
|---|---|
| 运行眼镜或写一个视频/消息应用 | [Host/Edge 快速开始](host-edge-quickstart.md) |
| 下载、构建一套匹配的版本 | [版本配对与发布](releases.md) |
| 理解代码、链接库或换平台 | [SDK 指南](sdk-guide.md)，需要进程/缓冲细节时读[实现地图](system-runtime-map.md) |
| 改通信、资源或显示语义 | 下方对应的产品规范 |
| 查协议字段、旧集成或板测记录 | [工程参考索引](reference.md) |

## 产品规范

仅以下五份文档拥有通用产品语义；按问题选读。

- [System](system.md)：角色、职责、依赖和系统不变量。
- [Resource](resource.md)：State / Stream / Action / Blob 的所有权和生命周期。
- [Runtime](runtime.md)：能力、endpoint、desired/actual、路由和资源可见性。
- [Link](link.md)：身份、Session、时间、交付与失效。
- [Composition](composition.md)：场景、图层、激活与最终呈现责任。

Profile/wire 只能具体化规范留开的参数。实现指南、测试和实验报告不能创建另一套产品契约。

## 当前使用与历史

当前应用的支持范围、构建和设备要求集中在[快速开始](host-edge-quickstart.md)，不在每个入口
复制一份进度表。旧实验只证明其记录的源码、平台和负载；[历史归档](archive/README.md)用于追溯。
