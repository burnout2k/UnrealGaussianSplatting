# UnrealGaussianSplatting

Unreal Engine 插件，用于导入、加载和显示标准 3D Gaussian Splatting 数据。

当前版本已经具备一条可用主链路：

- 将标准 3DGS `.ply` 导入为 Unreal 资产
- 在运行时从磁盘加载 `.ply`
- 通过 `AGaussianSplatActor / UGaussianSplatComponent` 在场景中显示
- 提供 `Points` 与 `Gaussian Billboards` 两种显示路径
- 提供一个最小可用的独立编辑器工作面板

## 当前能力

### 编辑器侧

- 自定义资产类型：`Gaussian Splat Asset`
- 支持从标准 3DGS PLY 导入资产
- 支持 `ascii` 和 `binary_little_endian` 两种 PLY 编码
- Details 面板定制
- 独立窗口：`Window -> Gaussian Splat Editor`

### 运行时侧

- 运行时加载外部 `.ply`
- 蓝图中把 `UGaussianSplatAsset` 挂到 `UGaussianSplatComponent`
- `AGaussianSplatActor` 提供默认封装，便于直接放进场景使用

### 当前渲染能力

- `Points` 模式：用于快速验证导入与调试位置数据
- `Gaussian Billboards` 模式：当前主要显示路径
- 支持 SH 颜色、透明度、密度抽样和基础参数调节

## 项目结构

- `Source/GaussianSplattingRuntime`
  运行时模块。包含资产、组件、运行时加载接口与渲染逻辑。
- `Source/GaussianSplattingEditor`
  编辑器模块。包含导入工厂、Details 定制和独立编辑器面板。
- `Shaders`
  Gaussian billboards / points 的 shader 实现。
- `Docs`
  阶段总结与后续质量改造计划。

## 安装

将插件放到项目的 `Plugins/` 目录下，例如：

```text
<YourProject>/Plugins/UnrealGaussianSplatting
```

然后重新生成工程文件并编译项目，或直接在 Unreal Editor 中重新编译插件。

## 编辑器使用

### 1. 导入 Gaussian 资产

在 Content Browser 中导入 `.ply` 文件，插件会创建一个 `Gaussian Splat Asset`。

当前解析器会读取标准 3DGS 常见字段：

- `x/y/z`
- `f_dc_0..2`
- `f_rest_*`
- `opacity`
- `scale_0..2`
- `rot_0..3`

导入过程中会完成：

- COLMAP 坐标系到 UE 坐标系转换
- 颜色 / 透明度解码
- SH 系数重排
- Bounds 生成
- GPU 资源初始化

### 2. 在场景中显示

最直接的方式是把 `AGaussianSplatActor` 放进关卡，然后给它的 `SplatComponent` 指定一个 `Gaussian Splat Asset`。

组件的主要参数包括：

- `DensityScale`
- `OpacityScale`
- `PointSize`
- `MaxRenderPoints`
- `PreviewRenderMode`

### 3. 使用独立编辑器面板

打开：

```text
Window -> Gaussian Splat Editor
```

这个面板会读取当前选中的：

- `AGaussianSplatActor`
- `UGaussianSplatComponent`
- `UGaussianSplatAsset`

当前支持的基础操作：

- `Refresh Asset`
- `Rebuild Bounds`
- `Focus View On Actor`
- `Move Actor To Origin`
- `Move Actor To View`
- `Points`
- `Billboards`

这个面板目前定位是“最小可用工作台”，不是完整的交互式 Gaussian 编辑模式。

## 运行时使用

当前插件提供两个基础蓝图接口：

- `LoadGaussianAssetFromFile`
- `SetGaussianAsset`

典型使用流程：

1. 在蓝图中调用 `LoadGaussianAssetFromFile(FilePath, OutError)`
2. 得到一个运行时创建的 `UGaussianSplatAsset`
3. 调用 `SetGaussianAsset(Component, Asset)`，把它挂到 `UGaussianSplatComponent`

说明：

- 这里加载的是磁盘文件路径，不是 Content Browser 资源路径
- 运行时加载得到的是 transient 资产，不会自动保存为 `.uasset`

## 单位与缩放

当前 `AGaussianSplatActor` 默认以 `100x` 缩放创建。

这样做的原因是：

- Unreal 世界单位默认是厘米
- 很多 COLMAP / 3DGS 数据在实际使用时会显得过小
- 先通过 Actor 默认缩放补偿，避免直接修改资产原始点位数据

如果你已有旧实例，或者数据源本身尺度特殊，仍然可能需要根据实际情况手动调整 Actor 缩放。

## 当前已知限制

当前版本已经能导入、加载、显示，但仍存在这些限制：

- 交互式编辑工具仍未接入
  - 没有 cutout / 框选 / 删除部分 splat
  - 没有专门的 `EdMode`
  - 没有基于 `InteractiveTool` 的完整编辑工作流
- 当前主渲染路径仍是 billboard 化的 3DGS 接入
  - 与原版 3DGS rasterizer 仍有质量差距
- 更完整的 antialiasing / footprint / coverage 对齐仍未完成
- 运行时体验层能力仍较基础
  - 还没有更完整的加载器 Actor / streaming / LOD 方案
