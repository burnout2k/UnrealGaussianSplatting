#include "./GaussianSplatSceneProxy.h"

#include "Algo/Sort.h"
#include "GaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "SceneManagement.h"

namespace
{
    // UE 用 TypeHash 区分不同 PrimitiveSceneProxy 类型。
    uint32 GGaussianSplatProxyTypeId = 0;
}

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatSceneProxy, Log, All);

FGaussianSplatSceneProxy::FGaussianSplatSceneProxy(const UGaussianSplatComponent* InComponent)
    : FPrimitiveSceneProxy(InComponent)
{
    // SceneProxy 是“游戏线程对象在渲染线程上的镜像”。
    // 构造时要把后续绘制需要的数据都拷一份出来，避免渲染线程回头读 UObject。
    if (!InComponent || !InComponent->Asset)
    {
        return;
    }

    const UGaussianSplatAsset* Asset = InComponent->Asset;
    PointCount = static_cast<uint32>(Asset->GetPointCount());
    PointSize = InComponent->PointSize;
    DensityScale = InComponent->DensityScale;
    OpacityScale = InComponent->OpacityScale;
    bDepthSort = InComponent->bDepthSort;
    bFrustumCull = InComponent->bFrustumCull;
    MaxRenderPoints = InComponent->MaxRenderPoints;
    PreviewRenderMode = static_cast<uint8>(InComponent->PreviewRenderMode);

    Positions = Asset->Positions;
    SplatRotations.SetNum(Positions.Num());
    SplatScales.SetNum(Positions.Num());
    Colors.SetNum(Positions.Num());

    for (int32 Index = 0; Index < Positions.Num(); ++Index)
    {
        // 把 Asset 里的可选数据整理成长度一致的渲染数组，并填默认值。
        FQuat4f Rotation = FQuat4f::Identity;
        if (Asset->Rotations.IsValidIndex(Index))
        {
            Rotation = Asset->Rotations[Index];
            Rotation.Normalize();
        }
        SplatRotations[Index] = Rotation;

        FVector3f Scale(0.02f, 0.02f, 0.02f);
        if (Asset->Scales.IsValidIndex(Index))
        {
            const FVector3f S = Asset->Scales[Index];
            Scale = FVector3f(
                FMath::Clamp(S.X, 0.001f, 10.0f),
                FMath::Clamp(S.Y, 0.001f, 10.0f),
                FMath::Clamp(S.Z, 0.001f, 10.0f));
        }
        SplatScales[Index] = Scale;

        FLinearColor Color = FLinearColor::White;
        if (Asset->ColorsOpacity.IsValidIndex(Index))
        {
            const FVector4f Packed = Asset->ColorsOpacity[Index];
            Color = FLinearColor(Packed.X, Packed.Y, Packed.Z, Packed.W);
        }

        Colors[Index] = Color;
    }
}

FGaussianSplatSceneProxy::~FGaussianSplatSceneProxy() = default;

SIZE_T FGaussianSplatSceneProxy::GetTypeHash() const
{
    return reinterpret_cast<SIZE_T>(&GGaussianSplatProxyTypeId);
}

// 这条路径只服务于 Points / Boxes 调试预览。
// 真正的 billboard 3DGS 并不会进这个函数。
void FGaussianSplatSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
    // 和 billboard 路径保持一致，密度控制同样通过固定步长抽样实现。
    const float ClampedDensity = FMath::Max(0.001f, DensityScale);
    const int32 SampleStride = FMath::Max(1, FMath::RoundToInt(1.0f / FMath::Min(ClampedDensity, 1.0f)));

    for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
    {
        if ((VisibilityMap & (1U << ViewIndex)) == 0)
        {
            continue;
        }

        const FSceneView* View = Views[ViewIndex];
        FPrimitiveDrawInterface* PDI = Collector.GetPDI(ViewIndex);
        const FVector ViewOrigin = View->ViewMatrices.GetViewOrigin();
        const FVector ViewDirection = View->GetViewDirection();

        // RenderList 是当前视图下要画的候选点列表，里面只存索引和深度，尽量少拷贝数据。
        TArray<FSortablePoint> RenderList;
        RenderList.Reserve(FMath::Min<int32>(Positions.Num(), MaxRenderPoints));

        for (int32 Index = 0; Index < Positions.Num(); Index += SampleStride)
        {
            if (RenderList.Num() >= MaxRenderPoints)
            {
                break;
            }

            const FVector WorldPos = GetLocalToWorld().TransformPosition(FVector(Positions[Index]));

            // 用视线方向上的投影当深度近似值，够满足调试预览排序。
            FSortablePoint Entry;
            Entry.Index = Index;
            Entry.Depth = FVector::DotProduct(WorldPos - ViewOrigin, ViewDirection);
            RenderList.Add(Entry);
        }

        if (bDepthSort)
        {
            // Far-to-near draw order gives a more stable alpha look for point sprites.
            Algo::Sort(RenderList, [](const FSortablePoint& A, const FSortablePoint& B)
            {
                return A.Depth > B.Depth;
            });
        }

        for (const FSortablePoint& Entry : RenderList)
        {
            const int32 Index = Entry.Index;
            const FVector WorldPos = GetLocalToWorld().TransformPosition(FVector(Positions[Index]));
            const FLinearColor Color = Colors.IsValidIndex(Index) ? Colors[Index] : FLinearColor::White;
            FLinearColor FinalColor = Color;
            FinalColor.A = FMath::Clamp(FinalColor.A * OpacityScale, 0.0f, 1.0f);
            if (FinalColor.A <= 0.001f)
            {
                continue;
            }

            if (PreviewRenderMode == static_cast<uint8>(EGaussianPreviewRenderMode::Boxes))
            {
                // 盒子模式把高斯椭球的三个主轴画成一个有朝向的包围盒，方便调试旋转和尺度。
                const FVector CenterLocal = FVector(Positions[Index]);
                const FQuat Rotation = SplatRotations.IsValidIndex(Index) ? FQuat(SplatRotations[Index]) : FQuat::Identity;
                const FVector3f Scale = SplatScales.IsValidIndex(Index) ? SplatScales[Index] : FVector3f(0.02f, 0.02f, 0.02f);
                const FVector AxisX = Rotation.RotateVector(FVector(Scale.X, 0.0f, 0.0f));
                const FVector AxisY = Rotation.RotateVector(FVector(0.0f, Scale.Y, 0.0f));
                const FVector AxisZ = Rotation.RotateVector(FVector(0.0f, 0.0f, Scale.Z));

                FVector Corners[8];
                Corners[0] = GetLocalToWorld().TransformPosition(CenterLocal - AxisX - AxisY - AxisZ);
                Corners[1] = GetLocalToWorld().TransformPosition(CenterLocal + AxisX - AxisY - AxisZ);
                Corners[2] = GetLocalToWorld().TransformPosition(CenterLocal + AxisX + AxisY - AxisZ);
                Corners[3] = GetLocalToWorld().TransformPosition(CenterLocal - AxisX + AxisY - AxisZ);
                Corners[4] = GetLocalToWorld().TransformPosition(CenterLocal - AxisX - AxisY + AxisZ);
                Corners[5] = GetLocalToWorld().TransformPosition(CenterLocal + AxisX - AxisY + AxisZ);
                Corners[6] = GetLocalToWorld().TransformPosition(CenterLocal + AxisX + AxisY + AxisZ);
                Corners[7] = GetLocalToWorld().TransformPosition(CenterLocal - AxisX + AxisY + AxisZ);

                constexpr int32 EdgeIndices[12][2] =
                {
                    {0, 1}, {1, 2}, {2, 3}, {3, 0},
                    {4, 5}, {5, 6}, {6, 7}, {7, 4},
                    {0, 4}, {1, 5}, {2, 6}, {3, 7}
                };

                for (int32 EdgeIndex = 0; EdgeIndex < UE_ARRAY_COUNT(EdgeIndices); ++EdgeIndex)
                {
                    PDI->DrawLine(
                        Corners[EdgeIndices[EdgeIndex][0]],
                        Corners[EdgeIndices[EdgeIndex][1]],
                        FinalColor,
                        SDPG_World,
                        0.25f);
                }
            }
            else
            {
                // 最简单的点模式，直接用 UE 的调试点绘制。
                PDI->DrawPoint(WorldPos, FinalColor, PointSize, SDPG_World);
            }
        }

    }
}

FPrimitiveViewRelevance FGaussianSplatSceneProxy::GetViewRelevance(const FSceneView* View) const
{
    // 告诉 UE：这是一个动态、半透明、参与主渲染通道的 Primitive。
    FPrimitiveViewRelevance Relevance;
    Relevance.bDrawRelevance = IsShown(View);
    Relevance.bDynamicRelevance = true;
    Relevance.bShadowRelevance = false;
    Relevance.bRenderInMainPass = true;
    Relevance.bUsesLightingChannels = false;
    Relevance.bOpaque = false;
    Relevance.bMasked = false;
    Relevance.bNormalTranslucency = true;
    Relevance.bSeparateTranslucency = true;
    return Relevance;
}

uint32 FGaussianSplatSceneProxy::GetMemoryFootprint() const
{
    return sizeof(*this) + Positions.GetAllocatedSize() + SplatScales.GetAllocatedSize() + Colors.GetAllocatedSize();
}
