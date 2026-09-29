# 旧 v12 解码器拒绝验证

此消费工程必须链接未修改的 `d2882982709b372a975ca91d6c2c62bb5b24bb1f` SDK；
CMake 检查安装包 provenance，源码检查旧头的容量常量。不会使用本分支的新解码器模拟旧端。

在当前源码树外建立旧修订 worktree，先构建并安装 `Profiles` 组件的依赖：

```sh
git worktree add --detach ../core-pre-xr d2882982709b372a975ca91d6c2c62bb5b24bb1f
cmake -S ../core-pre-xr -B ../build-pre-xr -DMEISO_BUILD_APPLICATIONS=OFF -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build ../build-pre-xr
cmake --install ../build-pre-xr --prefix ../sdk-pre-xr
cmake -S tests/legacy-v12-consumer -B ../test-pre-xr -DCMAKE_PREFIX_PATH="$PWD/../sdk-pre-xr"
cmake --build ../test-pre-xr
ctest --test-dir ../test-pre-xr --output-on-failure
```

Windows 在 x64 Native Tools 终端选择 `NMake Makefiles`，将 shell 路径换成对应的绝对路径。

每类先验证旧值能被接收，再注入新值：tracking=3、view_layout=3（含全零旧几何的完整新布局）、
显示 evidence mask=4、模型 world_epoch、模型 evidence=3、视频 evidence=3/72 B，全被旧端拒绝。
当前分支的 `meiso_display_ingress_edge_extensions_test` 则验证新字节、非法组合和真实 Runtime gate。
这不替代 K230D 固件回归或 VR 提交实测。
