#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"

class FRDGBuilder;
class FSceneView;
class FGaussianSplatRenderResources;
struct FScreenPassRenderTarget;
struct FScreenPassTexture;

enum class EGaussianSplatRenderMode : uint32
{
    Points = 0,
    Billboards = 1,
};

// 每个 Batch 对应一个 UGaussianSplatComponent 在当前帧被快照出来的渲染参数。
// ViewExtension 不直接在渲染线程持有 UObject，
// 这样渲染线程只依赖快照，不需要跨线程直接访问组件对象。而是把渲染所需的最小只读数据抽成这个 POD 结构。
struct FGaussianSplatRenderBatch
{
    const FGaussianSplatRenderResources* Resources = nullptr;

    // Fix 5: set together, and only for a paged asset, which draws from the
    // process-wide pool and so has no FGaussianSplatRenderResources of its own.
    // Both stay null while r.GaussianSplat.PagedAssets is 0.
    const class UGaussianSplatPagedAsset* PagedAsset = nullptr;
    const struct FGaussianSplatPoolResidency* PagedResidency = nullptr;
    EGaussianSplatRenderMode RenderMode = EGaussianSplatRenderMode::Billboards;
    FMatrix44f LocalToWorld = FMatrix44f::Identity;
    FVector4f WorldToLocalRow0 = FVector4f(1, 0, 0, 0);
    FVector4f WorldToLocalRow1 = FVector4f(0, 1, 0, 0);
    FVector4f WorldToLocalRow2 = FVector4f(0, 0, 1, 0);
    float PointSize = 1.0f;
    float OpacityScale = 1.0f;
    uint32 AssetPointCount = 0;
    uint32 Stride = 1;
    uint32 MaxRenderPoints = TNumericLimits<uint32>::Max();
};

// Fix 5 Step 2: the settle gate has to bound every view's selection, so it must
// read the SAME knobs and compute HalfFull the SAME way the selection does. These
// exist so there is ONE source for that rather than a second copy in the gate --
// the lesson Step 1a learned when two packers held two copies of the bake order.
namespace GaussianSplatLod
{
    // MinFraction, already clamped to the paged asset's baked floor by the caller
    // (D9: an asset cannot serve splats below a floor it never baked).
    float GetMinFraction();

    // F' = d_full / 2 at m = 1, including the debug override and the minimum
    // full distance. FocalPx is the view's; the rest are the component's.
    double ComputeHalfFull(double FocalPx, float PointSize, float SizeRef, double ActorScale);

    // The selection's own take for one cell at a given full distance. Distance is
    // from the view origin (or, in the gate, from the group centre minus its
    // radius) to the cell's world box.
    uint32 TakeAt(int32 CellCount, double Distance, double FullDistance, float MinFraction);
}

namespace GaussianSplatPasses
{
    // 往当前后处理图里插入一整套 Gaussian 渲染流程：
    // 1. Compute Pass 做可见性筛选、深度 key 初始化和 indirect args 生成。
    // 2. Compute Pass 做 bitonic sort。
    // 3. Raster Pass 把所有 billboards 画到独立的 SplatTexture。
    // 4. Fullscreen Pass 把 SplatTexture 合成回 SceneColor。
    FScreenPassTexture AddPostProcessPass(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        const FScreenPassTexture& SceneColor,
        const FScreenPassRenderTarget& Output,
        FRDGTextureRef SceneDepthTexture,
        const TArray<FGaussianSplatRenderBatch>& Batches);
}
