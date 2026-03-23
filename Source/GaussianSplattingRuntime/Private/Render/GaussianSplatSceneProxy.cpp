#include "./GaussianSplatSceneProxy.h"

#include "Algo/Sort.h"
#include "GaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "SceneManagement.h"

namespace
{
    uint32 GGaussianSplatProxyTypeId = 0;
}

FGaussianSplatSceneProxy::FGaussianSplatSceneProxy(const UGaussianSplatComponent* InComponent)
    : FPrimitiveSceneProxy(InComponent)
{
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
    GaussianFalloffResource = InComponent->GaussianFalloffTexture ? InComponent->GaussianFalloffTexture->GetResource() : nullptr;

    Positions = Asset->Positions;
    SplatScales.SetNum(Positions.Num());
    Colors.SetNum(Positions.Num());

    for (int32 Index = 0; Index < Positions.Num(); ++Index)
    {
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

void FGaussianSplatSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
    if (Positions.IsEmpty())
    {
        return;
    }

    const float ClampedDensity = FMath::Max(0.001f, DensityScale);
    const int32 SampleStride = FMath::Max(1, FMath::RoundToInt(1.0f / FMath::Min(ClampedDensity, 1.0f)));

    for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
    {
        if ((VisibilityMap & (1U << ViewIndex)) == 0)
        {
            continue;
        }

        const FSceneView* View = Views[ViewIndex];
        if (!View)
        {
            continue;
        }

        FPrimitiveDrawInterface* PDI = Collector.GetPDI(ViewIndex);
        if (!PDI)
        {
            continue;
        }

        const FVector ViewOrigin = View->ViewMatrices.GetViewOrigin();
        const FVector ViewDirection = View->GetViewDirection();

        TArray<FSortablePoint> RenderList;
        RenderList.Reserve(FMath::Min<int32>(Positions.Num(), MaxRenderPoints));

        for (int32 Index = 0; Index < Positions.Num(); Index += SampleStride)
        {
            if (RenderList.Num() >= MaxRenderPoints)
            {
                break;
            }

            const FVector WorldPos = GetLocalToWorld().TransformPosition(FVector(Positions[Index]));

            if (bFrustumCull && !View->ViewFrustum.IntersectSphere(WorldPos, 1.0f))
            {
                continue;
            }

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

        const float TanHalfFovY = 1.0f / FMath::Max(0.001f, View->ViewMatrices.GetProjectionMatrix().M[1][1]);
        const float ViewHeight = FMath::Max(1.0f, static_cast<float>(View->UnconstrainedViewRect.Height()));
        const float BasePixelSize = FMath::Clamp(PointSize, 0.1f, 128.0f);

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

            if (PreviewRenderMode == static_cast<uint8>(EGaussianPreviewRenderMode::Billboards))
            {
                const float Distance = FMath::Max(1.0f, Entry.Depth);
                const float WorldPerPixel = (2.0f * Distance * TanHalfFovY) / ViewHeight;

                const FVector3f Scale = SplatScales.IsValidIndex(Index) ? SplatScales[Index] : FVector3f(0.02f, 0.02f, 0.02f);
                const float A = FMath::Max3(Scale.X, Scale.Y, Scale.Z);
                const float C = FMath::Min3(Scale.X, Scale.Y, Scale.Z);
                const float B = FMath::Max(0.001f, Scale.X + Scale.Y + Scale.Z - A - C);

                const float RadiusPixelsX = A / FMath::Max(0.00001f, WorldPerPixel);
                const float RadiusPixelsY = B / FMath::Max(0.00001f, WorldPerPixel);
                const float PixelSizeX = FMath::Clamp(BasePixelSize * RadiusPixelsX, 2.0f, 256.0f);
                const float PixelSizeY = FMath::Clamp(BasePixelSize * RadiusPixelsY, 2.0f, 256.0f);
                const float SizeWorldX = FMath::Max(0.001f, PixelSizeX * WorldPerPixel);
                const float SizeWorldY = FMath::Max(0.001f, PixelSizeY * WorldPerPixel);

                const float RelativeArea = FMath::Clamp((A * B) / (0.02f * 0.02f), 0.1f, 4.0f);
                FinalColor.A = FMath::Clamp(FinalColor.A / FMath::Sqrt(RelativeArea), 0.0f, 1.0f);

                if (GaussianFalloffResource)
                {
                    PDI->DrawSprite(
                        WorldPos,
                        SizeWorldX,
                        SizeWorldY,
                        GaussianFalloffResource,
                        FinalColor,
                        SDPG_World,
                        0.0f,
                        1.0f,
                        0.0f,
                        1.0f,
                        SE_BLEND_Translucent);
                }
                else
                {
                    PDI->DrawPoint(WorldPos, FinalColor, FMath::Max(1.0f, 0.5f * (RadiusPixelsX + RadiusPixelsY) * 0.5f), SDPG_World);
                }
            }
            else
            {
                PDI->DrawPoint(WorldPos, FinalColor, PointSize, SDPG_World);
            }
        }
    }
}

FPrimitiveViewRelevance FGaussianSplatSceneProxy::GetViewRelevance(const FSceneView* View) const
{
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
    return sizeof(*this) + GetAllocatedSize();
}

uint32 FGaussianSplatSceneProxy::GetAllocatedSize() const
{
    return Positions.GetAllocatedSize() + SplatScales.GetAllocatedSize() + Colors.GetAllocatedSize();
}
