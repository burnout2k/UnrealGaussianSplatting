# UnrealGaussianSplatting 阶段总结

## 项目目标

目标是把当前 UE5.5.4 插件，从“能导入资产、点云模式可显示”的早期原型，推进成接近 Unity 参考项目的一整套 3D Gaussian Splatting 系统。

当前总结基于截至 2026-03-24 的实现状态。

## 四阶段路线

### 第一阶段：单个 Splat 的数学正确性

目标：

- 让单个 splat 的几何形状和屏幕投影正确
- 从“球形近似 / 圆形 billboard”推进到真正的椭圆高斯

已完成内容：

- `ViewExtension` 主链打通
- `billboard` 模式不再依赖 `SceneProxy`
- 挂点从无效/错误位置转到可见且合理的后处理主链
- 单 splat 从半径近似改为 `cov3d -> cov2d -> conic`
- 去掉 TAA jitter 导致的静态抖动
- 修复近距离观察时整片消失的问题

主要文件：

- `Source/GaussianSplattingRuntime/Private/Render/GaussianSplatViewExtension.cpp`
- `Source/GaussianSplattingRuntime/Private/Render/GaussianSplatPasses.cpp`
- `Source/GaussianSplattingRuntime/Private/Render/GaussianSplatShaders.h`
- `Shaders/Private/GaussianSplatCommon.ush`
- `Shaders/Private/GaussianSplatRaster.usf`

阶段状态：

- 基本完成

### 第二阶段：渲染范式正确性

目标：

- 从错误的 full-screen brute force 路线切到更接近 Unity 的逐 splat 绘制路线
- 建立独立 splat RT 和 composite 结构

已完成内容：

- 从全屏遍历全部 splat，切换为 instanced quad raster
- 新增独立 `SplatTexture`
- 新增独立 composite pass
- 修复中间 RT 精度问题，避免“等高线 / banding”
- 修复 raster 逻辑，避免“方形面片感”，恢复正确椭圆外观

主要文件：

- `Source/GaussianSplattingRuntime/Private/Render/GaussianSplatPasses.cpp`
- `Source/GaussianSplattingRuntime/Private/Render/GaussianSplatShaders.cpp`
- `Shaders/Private/GaussianSplatRaster.usf`
- `Shaders/Private/GaussianSplatComposite.usf`

阶段状态：

- 核心完成

### 第三阶段：排序、裁剪、规模化

目标：

- 从“能显示”推进到“全量、高效、结构完整”
- 建立 GPU cull / sort / indirect draw 主链

已完成内容：

- 移除 20000 点上限，支持全量 splat 渲染
- 从 CPU 排序推进到 GPU bitonic sort
- 资产静态数据改为 GPU 常驻 buffer
- 批次快照从 per-point CPU 组包，改为 per-batch 资产引用
- 新增 GPU 可见性判断
- 从“哨兵值 + 尾部截断”进一步收成真正的 visible list
- raster pass 改为 `DrawPrimitiveIndirect`

主要文件：

- `Source/GaussianSplattingRuntime/Private/Render/GaussianSplatRenderResources.h`
- `Source/GaussianSplattingRuntime/Private/Render/GaussianSplatRenderResources.cpp`
- `Source/GaussianSplattingRuntime/Public/GaussianSplatAsset.h`
- `Source/GaussianSplattingRuntime/Private/GaussianSplatAsset.cpp`
- `Source/GaussianSplattingRuntime/Private/Render/GaussianSplatPasses.h`
- `Source/GaussianSplattingRuntime/Private/Render/GaussianSplatPasses.cpp`
- `Source/GaussianSplattingRuntime/Private/Render/GaussianSplatViewExtension.h`
- `Source/GaussianSplattingRuntime/Private/Render/GaussianSplatViewExtension.cpp`
- `Source/GaussianSplattingRuntime/Private/Render/GaussianSplatShaders.h`
- `Shaders/Private/GaussianSplatCullSort.usf`

阶段状态：

- 基本完成

未完全收掉的点：

- cull 仍是 per-splat 粗粒度可见性判断，还没有 chunk / 层级化 cull
- blend / color 与参考实现仍有少量观感差距

### 第四阶段：完整系统能力

目标：

- 从“渲染主链完成”推进到“完整 3DGS 系统”
- 包括工具链、编辑、调试、导出、选择、cutout 等

当前已做内容：

- SH 着色已接到三阶
- 导入器已支持 `f_rest_0..44`
- 为后续 GPU 工具链打下了资源层基础

尚未系统推进的内容：

- cutout / selection / deletion 的完整运行时链路
- 调试视图
- 编辑器工具
- 导出 / 验证流程
- 更系统的 GPU-only view cache / compacted data flow

阶段状态：

- 刚开始

## 当前总体进度判断

保守估计：

- 第一阶段：90% 以上
- 第二阶段：80% 以上
- 第三阶段：80% 左右
- 第四阶段：20% 左右

一句话概括：

当前插件已经不再是“点云预览 + 高斯原型”，而是“具备完整 3DGS runtime 雏形的 UE 实现”，但还没有达到 Unity 参考项目那种完整系统级成熟度。

## 当前渲染架构

### 主链

当前高斯模式的正式主链是：

1. `UGaussianSplatAsset`
   - 持有 GPU 常驻 buffer

2. `FGaussianSplatViewExtension::BuildPointSnapshot_GameThread()`
   - 每帧只收集组件批次描述
   - 不再重组静态 splat 数据

3. `MotionBlur` 后处理挂点
   - 使用 `SubscribeToPostProcessingPass()`
   - 在 tonemap 之前合成高斯结果

4. `GaussianSplatPasses::AddPostProcessPass()`
   - GPU cull
   - GPU visible list
   - GPU sort
   - indirect draw
   - raster 到 `SplatTexture`
   - composite 回 UE 主链

### 为什么挂在 MotionBlur

当前选择 `MotionBlur` 的原因：

- 在主后处理链里，输出能真正进入最终视口
- 位于 `Tonemap` 之前，颜色空间正确
- 比 `Tonemap` 挂点更适合线性 HDR 的高斯合成

当前判断：

- 这是现阶段合理的稳定挂点
- 不一定是最终永远不变的终点
- 未来如果要更深参与时域效果或更底层 renderer，可再评估下沉或前移

## 和 Unity 参考项目的主要差距

### 已基本对齐的部分

- 单 splat 数学链
- instanced quad raster
- 独立 splat RT + composite
- 全量渲染
- SH 着色主链
- GPU sort
- GPU 可见性列表

### 仍有差距的部分

1. 颜色与混合细节

- 当前视觉已接近正确，但和 viewer / Unity 仍有细微差距
- 尤其亮部和白色区域还不算完全一致

2. 裁剪层级

- 当前是 per-splat 粗裁剪
- 还没有 chunk 级、层级化、cache 化的完整 cull 系统

3. 工具链

- 还没有完整 cutout / selection / editing / export / debug 体系

4. GPU-only 视图缓存

- 当前已经做到资产数据 GPU 常驻
- 但还没有 Unity 那种更完整的 `_SplatViewData` 风格运行时缓存体系

## 关键踩坑记录

### 1. ViewExtension 创建了但不运行

现象：

- `ViewExtension` 构造成功
- 但 `BeginRenderViewFamily()` / post-process callback 没有真正参与

原因：

- 创建时机过早，`GEngine` 尚未完全就绪

修复：

- 延迟到 `PostEngineInit` 再创建 extension

### 2. Shader 模块与目录映射问题

现象：

- global shader 注册 / 编译异常

原因：

- 模块加载时机和插件 shader 目录映射不正确

修复：

- 修正模块加载阶段
- 修正插件名与 shader 目录映射
- `.usf` 文件补上 `#include "/Engine/Public/Platform.ush"`

### 3. 挂到错误的 pass，看起来“pass 在跑但画面没变化”

现象：

- 日志显示 pass 被调用
- 但红屏测试看不到效果

原因：

- 挂点不在最终主画面有效链路上

结论：

- `SSRInput` 不适合作为当前 3DGS 主链挂点

### 4. 挂到 Tonemap 后导致颜色空间错误

现象：

- 白色偏亮
- 整体颜色不对

原因：

- 场景已经 tonemap，而高斯颜色仍按线性 HDR 语义叠加

修复：

- 改挂到 `MotionBlur`

### 5. pre-exposure 处理走错方向

现象：

- 改到 pre-tonemap 后一度完全看不到

原因：

- 曝光缩放方向判断错误

结论：

- 当前 `MotionBlur` 挂点下不应保留错误的重复曝光缩放

### 6. 中间 RT 精度不够导致“等高线 / banding”

现象：

- 椭球连成一片，看起来像等高线

原因：

- 中间 `SplatTexture` 精度不足

修复：

- 改为 `PF_FloatRGBA`

### 7. 早期 billboard 路径干扰判断

现象：

- `DrawSprite` / `DrawPoint` 与真正 `ViewExtension` 路径混在一起

修复：

- billboard 模式下禁用 `SceneProxy`
- 只保留 `ViewExtension` 主链

### 8. 近距离观察时整片消失

现象：

- 把模型拉近后完全看不到

原因：

- 早期裁剪逻辑对中心点 / 包围范围判断过于激进

修复：

- 改为基于屏幕包围盒的可见性判断

### 9. “方形面片感”

现象：

- 模型看起来像由方形面片组成，而不是高斯椭圆

原因：

- raster 阶段没有按 Unity 参考实现的椭圆 basis 路径来组织 quad

修复：

- 顶点阶段分解协方差得到屏幕空间 `axis1/axis2`
- 片元阶段按标准化 `quadPos` 评估高斯权重

### 10. GPU 常驻批次化后出现“闪烁彩色乱码”

现象：

- 切到按 batch 绘制后，画面变成闪烁彩色乱码

原因：

- 第一批 raster pass 错误使用了 `ELoad`
- 从未初始化 RT 开始叠加

修复：

- 第一批 `EClear`
- 后续批次 `ELoad`

## Git 相关记录

已处理事项：

- 增加插件根目录 `.gitignore`
- 停止追踪以下内容：
  - `Binaries/`
  - `Intermediate/`
  - `Docs/`
  - `Saved/`
  - 常见 IDE 临时文件

最近一次远端同步包含：

- 仓库清理
- GPU 常驻资源层相关改动

## 明天第四阶段建议起点

建议从以下顺序开始：

1. 补工具链基础
   - selection / deletion / cutout 正式运行时链路

2. 做调试能力
   - splat 数量
   - cull 后数量
   - sort / draw 开销
   - debug view

3. 收颜色剩余差距
   - 进一步对齐 viewer / Unity 参考实现

4. 视情况继续推进更完整的 GPU view cache

## 当前结论

今天结束时，第三阶段已经基本收完。

当前系统具备：

- 数学正确的椭圆高斯
- 正式的 instanced raster 路线
- 全量 splat 渲染
- GPU 常驻资产数据
- GPU cull
- GPU visible list
- GPU sort
- indirect draw
- 稳定的 pre-tonemap 合成主链

剩下的工作，已经更像“完整系统建设”和“体验打磨”，而不再是“主渲染链能不能成立”的问题。
