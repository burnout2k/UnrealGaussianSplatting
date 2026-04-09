#pragma once

#include "PrimitiveSceneProxy.h"

class UGaussianSplatComponent;
class FTexture;

// SceneProxy 是 UPrimitiveComponent 在渲染线程上的轻量镜像。
// 这个类只服务于 Points / Boxes 等调试预览模式，不负责真正的 billboard 3DGS 渲染。
class FGaussianSplatSceneProxy final : public FPrimitiveSceneProxy
{
public:
    explicit FGaussianSplatSceneProxy(const UGaussianSplatComponent* InComponent);
    virtual ~FGaussianSplatSceneProxy() override;

    // UE 通过这些虚函数向 SceneProxy 询问“你要怎么画、画到哪些 View、占多少内存”。
    virtual SIZE_T GetTypeHash() const override;
    virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;
    virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
    virtual uint32 GetMemoryFootprint() const override;

private:
    // 调试渲染时只需要点索引和一个近似深度值来做排序。
    struct FSortablePoint
    {
        int32 Index = 0;
        float Depth = 0.0f;
    };

    // 以下成员都是从 Component / Asset 复制出来的渲染线程只读快照。
    uint32 PointCount = 0;
    float PointSize = 2.0f;
    float DensityScale = 1.0f;
    float OpacityScale = 1.0f;
    bool bDepthSort = true;
    bool bFrustumCull = true;
    int32 MaxRenderPoints = 250000;
    uint8 PreviewRenderMode = 1;
    TArray<FVector3f> Positions;
    TArray<FQuat4f> SplatRotations;
    TArray<FVector3f> SplatScales;
    TArray<FLinearColor> Colors;
};
