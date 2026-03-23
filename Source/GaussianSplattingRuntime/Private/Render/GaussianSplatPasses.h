#pragma once

#include "CoreMinimal.h"

class FRDGBuilder;
class FSceneViewFamily;

struct FGaussianSplatRenderPoint
{
    FVector4f PositionRadiusWS = FVector4f(0, 0, 0, 0.02f);
    FVector4f Color = FVector4f(1, 1, 1, 1);
};

namespace GaussianSplatPasses
{
    void AddPreRenderPasses(FRDGBuilder& GraphBuilder, const FSceneViewFamily& ViewFamily, const TArray<FGaussianSplatRenderPoint>& Points);
}
