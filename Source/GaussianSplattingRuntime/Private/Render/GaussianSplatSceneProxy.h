#pragma once

#include "PrimitiveSceneProxy.h"

class UGaussianSplatComponent;
class FTexture;

class FGaussianSplatSceneProxy final : public FPrimitiveSceneProxy
{
public:
    explicit FGaussianSplatSceneProxy(const UGaussianSplatComponent* InComponent);
    virtual ~FGaussianSplatSceneProxy() override;

    virtual SIZE_T GetTypeHash() const override;
    virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;
    virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
    virtual uint32 GetMemoryFootprint() const override;

private:
    struct FSortablePoint
    {
        int32 Index = 0;
        float Depth = 0.0f;
    };

    uint32 GetAllocatedSize() const;

    uint32 PointCount = 0;
    float PointSize = 2.0f;
    float DensityScale = 1.0f;
    float OpacityScale = 1.0f;
    bool bDepthSort = true;
    bool bFrustumCull = true;
    int32 MaxRenderPoints = 250000;
    uint8 PreviewRenderMode = 1;
    const FTexture* GaussianFalloffResource = nullptr;
    TArray<FVector3f> Positions;
    TArray<FQuat4f> SplatRotations;
    TArray<FVector3f> SplatScales;
    TArray<FLinearColor> Colors;
};
