#pragma once

#include "Render/GaussianSplatPasses.h"
#include "SceneViewExtension.h"

class FGaussianSplatViewExtension final : public FSceneViewExtensionBase
{
public:
    FGaussianSplatViewExtension(const FAutoRegister& AutoRegister);

    virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;
    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
    virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
    virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;

private:
    void BuildPointSnapshot_GameThread();

    FRWLock CachedPointsLock;
    TArray<FGaussianSplatRenderPoint> CachedPoints;
};
