#pragma once

#include "CoreMinimal.h"

class FRDGBuilder;
class FSceneView;
class FGaussianSplatRenderResources;
struct FScreenPassRenderTarget;
struct FScreenPassTexture;

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
    FScreenPassTexture AddPostProcessPass(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        const FScreenPassTexture& SceneColor,
        const FScreenPassRenderTarget& Output,
        const TArray<FGaussianSplatRenderBatch>& Batches);
}
