# Edge local compositor v0

> 状态：`K230D CPU OFFSCREEN COMPOSITOR BOARD-EVIDENCED / PHYSICAL PRESENT NOT IMPLEMENTED`。
> 本页是当前实现摘要；通用语义见[Composition](composition.md)，exact wire/Profile见
> [Composition Profile](../profiles/composition/README.md)。

## 责任

Edge local compositor 位于Runtime–SDK/application integration层，不进入Core。它：

- 消费Host-owned Mesh Blob、SceneDesired和PresentationPlan；
- 在Edge内部维护bounded BlobStore、SceneStore、prepared/active world和C2 coordinator；
- 只在plan/scene/token、production gate和资源admission通过后提交renderer job；
- 发布Edge-owned Scene/Presentation Actual、RenderResult和RenderPreviewMedia；
- 不解析Session/TCP，不制造prepared token，不把render complete写成scanout。

## 当前管线

```text
Host Mesh Blob + Scene/Presentation desired
  -> MRT0 v4 / COMPOSE_/_PROFILE:v1 exact 6/18 graph
  -> BlobStore publish -> explicit Scene visibility
  -> SceneStore prepared world -> C2 prepare/token echo -> ACTIVE world
  -> caller-owned CPU raster job -> inactive target -> atomic complete
  -> RenderResult metadata/thumbnail + full RGBA RenderPreviewMedia
```

CPU renderer固定两个320×240 RGBA8888 + depth32 target set和两job，覆盖canonical Mesh、
TRS/camera、clip/cull、24.8 top-left raster、strict-less depth、perspective color、SHA-256和64×48
thumbnail。这是 bounded reference CPU backend，不是 GPU、DRM 或平台 compositor。

## 已验层级

| 层 | 结果 | 不证明 |
|---|---|---|
| Host source/golden | C11/C++17、sanitizer、source-host literal SHA、world swap/retire | 跨target pixel exact、GPU、性能 |
| AF_INET integration | Blob/State/Actual/Stream、Scene/C2 ACTIVE、media transition/failure | 板端driver与物理输出 |
| K230D combined | CPU backend实际生成RenderResult与307200B RGBA CMFB，并与CameraMedia/telemetry共存 | display plane、fence、vblank、面板像素 |
| 历史standalone K1a ELF | cross-build byte-exact | 该独立ELF仍未板上执行 |

因此“K230D compositor”若指**Edge-native CPU offscreen合成与回传**，已验证；若指
**驱动面板的physical compositor/present backend**，尚未实现/验证。

## 与 Looking Glass 的接缝

Looking Glass 可作为Host compositor/HostRuntime，通过一个adapter调用公开`EdgeHandle`；它的
local object、C++ layout、IPC、lane batch与buffer/fence handle不进Meiso wire。K230D仍是Edge truth和
final presentation authority。Host merge是Looking Glass仓内决策，不通过复制其renderer到本仓解决。

## 下一独立单元

1. 选择明确panel/DRM/DSI backend与物理验收面；
2. 冻结target buffer/fence/cache ownership与安全fallback；
3. 把render complete、commit accepted、vblank latched、physical verified分层留证；
4. 独立于performance/thermal/soak、security和recovery工作单元。

逐切片开发记录已移到[历史方案](archive/edge-local-compositor-development-history-v0.md)。
