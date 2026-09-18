# 版本配对与发布

Meiso Glass 与 Looking Glass 是独立仓库。**需要匹配的是经过验证的依赖组合，不是两个仓的数字版本必须相等。**
当前采用固定源码清单，不引入 submodule 或新的仓库管理器；借鉴的是
[Zephyr 多仓 manifest](https://docs.zephyrproject.org/latest/develop/west/manifest.html) 固定项目 revision 的方式。

## 使用一个配套版本

初次技术预发布标识为 `v0.1.0-alpha.1`，两仓使用同名 annotated tag 标记这一套交付。
它标识首次技术预览，不表示正式产品完成。当前源码许可见 [LICENSE](../LICENSE) 与
[NOTICE](../NOTICE)；使用某个历史 tag 时核对该 tag 内的许可材料，版本标识本身不授予或撤销许可。

```sh
git clone --branch v0.1.0-alpha.1 https://github.com/MeisoTech/meiso_glass.git meiso
git clone --branch v0.1.0-alpha.1 https://github.com/MeisoTech/looking_glass.git looking_glass
```

仓库保持 Private，需要已有读取权限。然后按[快速开始](host-edge-quickstart.md)先安装 Meiso SDK，
再构建 Looking Glass Host 和 K230D Edge。

Looking Glass 中的 `sdc/integrations/meiso/meiso.lock.json` 是它所依赖 Meiso 源码的唯一配对清单：
记录仓库、完整 Git commit 与 CMake package 版本。tag 用于查找，完整 commit 用于锁定。
两端源码包来自这套 tag/清单；不能各取一个最新 main 当作已验证组合。

## 构建时怎样发现错配

Meiso 的安装配置导出 `MeisoGlass_VERSION` 和 `MeisoGlass_SOURCE_REVISION`。
Git checkout 使用实际 commit；工作树修改标为 `-dirty`；`git archive` 通过 export-subst 保留 commit；
无法确定来源的普通目录标为 `unknown`。

Looking Glass 的 Meiso 集成在 CMake configure 阶段校验清单。错误 revision、脏来源、旧的无来源包
会明确失败；外部应用使用安装的 MeisoHost 包时同样检查。不会静默下载或替换开发者的源码。
多仓联调可明确设置 `SDC_MEISO_ALLOW_UNVERIFIED_SDK=ON`，日志会标记组合未验证；该选项不能用于发布验收。

这项检查防止构建时混用 SDK，不能给任意人工拼接的板端二进制补上可信来源。部署仍应保存两端
源码版本、文件校验和及匹配的固件版本；网络端使用既有 Session/Profile/wire 协商，不另造发布号协议。

## 维护一个新版本

1. 先提交 Meiso 变更并完成相关测试，从干净 checkout 或 `git archive COMMIT` 配置、构建和安装。
2. Looking Glass 更新清单到该完整 commit，检查匹配可构建、错配会被拒绝、仓外客户端可消费。
3. 行为变化执行对应双端验收。仅发布元数据/文档变化沿用注明版本的板测，不声称重新上板。
4. 提交 Looking Glass 的清单与接线变更，为两仓已验证提交创建同一套新的 annotated tag；已发布 tag 不移动。
5. 发布说明链接对方的精确 tag/commit，记录 SDK 组合、适用设备/BSP、已测范围和剩余问题；只发布审核过的制品。

`0.x` 阶段只承诺记录下来的源码组合；C/C++ 二进制 ABI 尚未稳定。将来需要支持多个依赖版本时，
在真实兼容测试基础上扩大范围，不以版本号相似代替验证。主仓与工作树是同一个 Git 历史，工作树没有独立发行物。
