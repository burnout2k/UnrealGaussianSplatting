#pragma once

#include "CoreMinimal.h"

class FRDGBuilder;
class FSceneView;
struct FScreenPassRenderTarget;
struct FScreenPassTexture;

struct FGaussianSplatRenderPoint
{
    FVector4f PositionWS = FVector4f(0, 0, 0, 1);
    FVector4f Cov3D0 = FVector4f(0.0004f, 0, 0, 0);
    FVector4f Cov3D1 = FVector4f(0.0004f, 0, 0.0004f, 0);
    FVector4f Color = FVector4f(1, 1, 1, 1);
    FVector4f WorldToLocalRow0 = FVector4f(1, 0, 0, 0);
    FVector4f WorldToLocalRow1 = FVector4f(0, 1, 0, 0);
    FVector4f WorldToLocalRow2 = FVector4f(0, 0, 1, 0);
    FVector4f SHCoefficients[15];

    FGaussianSplatRenderPoint()
    {
        for (FVector4f& Coefficient : SHCoefficients)
        {
            Coefficient = FVector4f(0, 0, 0, 0);
        }
    }
};

namespace GaussianSplatPasses
{
    FScreenPassTexture AddPostProcessPass(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        const FScreenPassTexture& SceneColor,
        const FScreenPassRenderTarget& Output,
        const TArray<FGaussianSplatRenderPoint>& Points);
}
