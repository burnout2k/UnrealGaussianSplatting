#include "GaussianSplatAsset.h"

#include "Render/GaussianSplatRenderResources.h"

void UGaussianSplatAsset::Serialize(FArchive& Ar)
{
    Super::Serialize(Ar);
    Ar << Positions;
    Ar << Rotations;
    Ar << Scales;
    Ar << ColorsOpacity;
    Ar << SHCoefficients;
}

void UGaussianSplatAsset::PostLoad()
{
    Super::PostLoad();
    RebuildBounds();
    BuildRenderResources();
}

void UGaussianSplatAsset::BeginDestroy()
{
    ReleaseRenderResources();
    Super::BeginDestroy();
}

void UGaussianSplatAsset::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
    Super::GetResourceSizeEx(CumulativeResourceSize);
    CumulativeResourceSize.AddDedicatedSystemMemoryBytes(Positions.GetAllocatedSize());
    CumulativeResourceSize.AddDedicatedSystemMemoryBytes(Rotations.GetAllocatedSize());
    CumulativeResourceSize.AddDedicatedSystemMemoryBytes(Scales.GetAllocatedSize());
    CumulativeResourceSize.AddDedicatedSystemMemoryBytes(ColorsOpacity.GetAllocatedSize());
    CumulativeResourceSize.AddDedicatedSystemMemoryBytes(SHCoefficients.GetAllocatedSize());
}

int32 UGaussianSplatAsset::GetPointCount() const
{
    return Positions.Num();
}

void UGaussianSplatAsset::RebuildBounds()
{
    if (Positions.IsEmpty())
    {
        Bounds = FBoxSphereBounds(EForceInit::ForceInitToZero);
        return;
    }

    FBox Box(EForceInit::ForceInit);
    for (int32 Index = 0; Index < Positions.Num(); ++Index)
    {
        const FVector3f P = Positions[Index];
        const FVector3f S = Scales.IsValidIndex(Index)
            ? FVector3f(
                FMath::Clamp(Scales[Index].X, 0.001f, 10.0f),
                FMath::Clamp(Scales[Index].Y, 0.001f, 10.0f),
                FMath::Clamp(Scales[Index].Z, 0.001f, 10.0f))
            : FVector3f(0.02f, 0.02f, 0.02f);

        Box += FVector(P - S);
        Box += FVector(P + S);
    }

    Bounds = FBoxSphereBounds(Box);
}

const FGaussianSplatRenderResources* UGaussianSplatAsset::GetRenderResources() const
{
    return RenderResources.Get();
}

void UGaussianSplatAsset::BuildRenderResources()
{
    ReleaseRenderResources();

    RenderResources = MakeUnique<FGaussianSplatRenderResources>();
    RenderResources->BuildFromAssetData(Positions, Rotations, Scales, ColorsOpacity, SHCoefficients);
    BeginInitResource(RenderResources.Get());
}

void UGaussianSplatAsset::ReleaseRenderResources()
{
    if (RenderResources)
    {
        BeginReleaseResource(RenderResources.Get());
        FlushRenderingCommands();
        RenderResources.Reset();
    }
}
