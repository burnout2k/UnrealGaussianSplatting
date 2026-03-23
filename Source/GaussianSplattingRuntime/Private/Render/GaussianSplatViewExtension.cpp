#include "Render/GaussianSplatViewExtension.h"

#include "GaussianSplatAsset.h"
#include "GaussianSplatComponent.h"
#include "Render/GaussianSplatPasses.h"
#include "UObject/UObjectIterator.h"

FGaussianSplatViewExtension::FGaussianSplatViewExtension(const FAutoRegister& AutoRegister)
    : FSceneViewExtensionBase(AutoRegister)
{
}

bool FGaussianSplatViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
    return true;
}

void FGaussianSplatViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
}

void FGaussianSplatViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
}

void FGaussianSplatViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
    BuildPointSnapshot_GameThread();
}

void FGaussianSplatViewExtension::PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
    TArray<FGaussianSplatRenderPoint> LocalPoints;
    {
        FReadScopeLock Lock(CachedPointsLock);
        LocalPoints = CachedPoints;
    }

    GaussianSplatPasses::AddPreRenderPasses(GraphBuilder, InViewFamily, LocalPoints);
}

void FGaussianSplatViewExtension::BuildPointSnapshot_GameThread()
{
    constexpr int32 MaxTotalPoints = 20000;

    TArray<FGaussianSplatRenderPoint> NewPoints;
    NewPoints.Reserve(MaxTotalPoints);

    for (TObjectIterator<UGaussianSplatComponent> It; It; ++It)
    {
        UGaussianSplatComponent* Component = *It;
        if (!IsValid(Component) || !Component->IsRegistered() || !Component->IsVisible())
        {
            continue;
        }

        if (Component->PreviewRenderMode != EGaussianPreviewRenderMode::Billboards)
        {
            continue;
        }

        const UGaussianSplatAsset* Asset = Component->Asset;
        if (!IsValid(Asset) || Asset->Positions.IsEmpty())
        {
            continue;
        }

        const float Density = FMath::Clamp(Component->DensityScale, 0.001f, 1.0f);
        const int32 Stride = FMath::Max(1, FMath::RoundToInt(1.0f / Density));
        const int32 LocalMax = FMath::Max(1, Component->MaxRenderPoints);
        const FTransform LocalToWorld = Component->GetComponentTransform();

        int32 AddedForComponent = 0;
        for (int32 Index = 0; Index < Asset->Positions.Num(); Index += Stride)
        {
            if (AddedForComponent >= LocalMax || NewPoints.Num() >= MaxTotalPoints)
            {
                break;
            }

            const FVector WorldPos = LocalToWorld.TransformPosition(FVector(Asset->Positions[Index]));
            const FVector3f Scale = Asset->Scales.IsValidIndex(Index)
                ? Asset->Scales[Index]
                : FVector3f(0.02f, 0.02f, 0.02f);

            const float Radius = FMath::Clamp(
                FMath::Max3(Scale.X, Scale.Y, Scale.Z) * FMath::Clamp(Component->PointSize, 0.1f, 32.0f) * 0.35f,
                0.002f,
                20.0f);

            FVector4f Color(1.0f, 1.0f, 1.0f, 1.0f);
            if (Asset->ColorsOpacity.IsValidIndex(Index))
            {
                const FVector4f C = Asset->ColorsOpacity[Index];
                Color = FVector4f(C.X, C.Y, C.Z, FMath::Clamp(C.W * Component->OpacityScale, 0.0f, 1.0f));
            }

            FGaussianSplatRenderPoint Point;
            Point.PositionRadiusWS = FVector4f(
                static_cast<float>(WorldPos.X),
                static_cast<float>(WorldPos.Y),
                static_cast<float>(WorldPos.Z),
                Radius);
            Point.Color = Color;
            NewPoints.Add(Point);

            ++AddedForComponent;
        }

        if (NewPoints.Num() >= MaxTotalPoints)
        {
            break;
        }
    }

    {
        FWriteScopeLock Lock(CachedPointsLock);
        CachedPoints = MoveTemp(NewPoints);
    }
}
