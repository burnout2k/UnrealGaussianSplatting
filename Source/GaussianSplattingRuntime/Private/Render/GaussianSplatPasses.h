#pragma once

#include "CoreMinimal.h"

class FRDGBuilder;
class FSceneView;
class FGaussianSplatRenderResources;
struct FScreenPassRenderTarget;
struct FScreenPassTexture;

// 每个 Batch 对应一个 UGaussianSplatComponent 在当前帧被快照出来的渲染参数。
// ViewExtension 不直接在渲染线程持有 UObject，
// 这样渲染线程只依赖快照，不需要跨线程直接访问组件对象。而是把渲染所需的最小只读数据抽成这个 POD 结构。
struct FGaussianSplatRenderBatch
{
    const FGaussianSplatRenderResources* Resources = nullptr;
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
        const TArray<FGaussianSplatRenderBatch>& Batches);
}
