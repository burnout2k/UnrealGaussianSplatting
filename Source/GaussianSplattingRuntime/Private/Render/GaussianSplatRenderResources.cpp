#include "Render/GaussianSplatRenderResources.h"

#include "GaussianSplatBoundsUtils.h"
#include "RHICommandList.h"

void FGaussianSplatRenderResources::BuildFromAssetData(
    const TArray<FVector3f>& InPositions,
    const TArray<FGaussianCovariance3f>& InCovariances,
    const TArray<FVector4f>& InColorsOpacity,
    const TArray<float>& InSHCoefficients,
    const TArray<FGaussianSplatCell>& InCells,
    int32 MaxPointCount)
{
    const int32 SourcePointCount = InPositions.Num();
    const int32 Budget = FMath::Max(0, MaxPointCount);

    // Which splats become resident.
    //
    // Uniformly striding the whole asset (the original scheme) is spatially even
    // but blind to importance -- it keeps every Nth splat in file order, so the
    // road under the bumper is thinned exactly as hard as the forest 2 km away,
    // and which splats survive is an accident of file layout.
    //
    // With cells there is a better answer at identical cost: take a PREFIX of
    // each cell. Cells are importance-ordered, so a prefix is the most
    // significant splats of that cell, and taking the same fraction everywhere
    // keeps coverage spatially even. Same count, same VRAM, better choice.
    //
    // Splats parked past the last cell by MinCellOccupancy are simply never
    // reached here, so excluded floaters cost no VRAM at all.
    Cells.Reset();
    TArray<int32> SourceOfDest;

    if (InCells.IsEmpty())
    {
        // No cells (an asset that predates them, or one whose splats were all
        // filtered out). Fall back to the original even sample.
        const int32 Count = FMath::Min(SourcePointCount, Budget);
        SourceOfDest.SetNumUninitialized(Count);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            SourceOfDest[Index] = Count > 1
                ? static_cast<int32>((static_cast<int64>(Index) * (SourcePointCount - 1)) / (Count - 1))
                : 0;
        }
    }
    else
    {
        int64 TotalInCells = 0;
        for (const FGaussianSplatCell& Cell : InCells)
        {
            TotalInCells += Cell.Count;
        }

        const double Ratio = (TotalInCells <= Budget || TotalInCells == 0)
            ? 1.0
            : static_cast<double>(Budget) / static_cast<double>(TotalInCells);

        SourceOfDest.Reserve(FMath::Min<int64>(TotalInCells, Budget) + InCells.Num());
        Cells.Reserve(InCells.Num());
        for (const FGaussianSplatCell& Source : InCells)
        {
            // At least one splat per cell, so a cell never vanishes entirely
            // from residency just because it is small.
            const int32 Take = Ratio >= 1.0
                ? Source.Count
                : FMath::Clamp(FMath::RoundToInt(Source.Count * Ratio), 1, Source.Count);

            FGaussianSplatCell Resident;
            Resident.BoundsMin = Source.BoundsMin;
            Resident.BoundsMax = Source.BoundsMax;
            Resident.FirstIndex = SourceOfDest.Num();
            Resident.Count = Take;
            Cells.Add(Resident);

            for (int32 Offset = 0; Offset < Take; ++Offset)
            {
                SourceOfDest.Add(Source.FirstIndex + Offset);
            }
        }
    }

    PointCount = static_cast<uint32>(SourceOfDest.Num());

    PositionColorData.Empty(PointCount);
    Covariance0Data.Empty(PointCount);
    Covariance1Data.Empty(PointCount);

    // Fit the RGB quantization range to what this capture actually contains.
    //
    // A fixed [0,1] would be wrong: 3DGS colour is 0.5 + 0.28209 * f_dc and is
    // unbounded. Measured on the 85.8M Vuores capture, 2.4-7.6% of splats have a
    // channel outside [0,1], peaking at 1.73 -- all of which a naive encoding
    // would clamp to white. Fitting costs one pass over the resident subset and
    // removes the question entirely.
    float ColorMin = TNumericLimits<float>::Max();
    float ColorMax = TNumericLimits<float>::Lowest();
    for (int32 Index = 0; Index < SourceOfDest.Num(); ++Index)
    {
        const int32 SourceIndex = SourceOfDest[Index];
        if (!InColorsOpacity.IsValidIndex(SourceIndex))
        {
            continue;
        }

        const FVector4f& Color = InColorsOpacity[SourceIndex];
        ColorMin = FMath::Min3(ColorMin, FMath::Min(Color.X, Color.Y), Color.Z);
        ColorMax = FMath::Max3(ColorMax, FMath::Max(Color.X, Color.Y), Color.Z);
    }
    if (ColorMin > ColorMax)
    {
        ColorMin = 0.0f;
        ColorMax = 1.0f;
    }
    // A degenerate range (every splat one colour) would divide by zero.
    ColorEncoding = FVector2f(ColorMin, FMath::Max(ColorMax - ColorMin, KINDA_SMALL_NUMBER));

    // A capture exported at SH degree 0 has no f_rest_* data, so every one of the
    // 15 float4 per splat would be zero: 240 of the 304 bytes per splat on the GPU,
    // read every frame and multiplied by nothing. Skip the buffer entirely and let
    // the shader branch past SH evaluation.
    bHasSH = !InSHCoefficients.IsEmpty();
    SHData.Empty(bHasSH ? PointCount * 15 : 1);

    for (uint32 Index = 0; Index < PointCount; ++Index)
    {
        const int32 SourceIndex = SourceOfDest[Index];
        const FVector3f Position = InPositions[SourceIndex];
        const FGaussianCovariance3f Covariance = InCovariances.IsValidIndex(SourceIndex)
            ? InCovariances[SourceIndex]
            : GaussianSplatBoundsUtils::MakeIsotropic(0.02f);
        const FVector4f Color = InColorsOpacity.IsValidIndex(SourceIndex)
            ? InColorsOpacity[SourceIndex]
            : FVector4f(1.0f, 1.0f, 1.0f, 1.0f);

        // Colour into the w lane position never used. RGB over the fitted
        // range, opacity straight from its sigmoid [0,1].
        const FVector3f Normalised = (FVector3f(Color.X, Color.Y, Color.Z) - FVector3f(ColorEncoding.X))
            / ColorEncoding.Y;
        const uint32 PackedColor =
              static_cast<uint32>(FMath::RoundToInt(FMath::Clamp(Normalised.X, 0.0f, 1.0f) * 255.0f))
            | static_cast<uint32>(FMath::RoundToInt(FMath::Clamp(Normalised.Y, 0.0f, 1.0f) * 255.0f)) << 8
            | static_cast<uint32>(FMath::RoundToInt(FMath::Clamp(Normalised.Z, 0.0f, 1.0f) * 255.0f)) << 16
            | static_cast<uint32>(FMath::RoundToInt(FMath::Clamp(Color.W, 0.0f, 1.0f) * 255.0f)) << 24;

        PositionColorData.Add(FVector4f(
            Position.X, Position.Y, Position.Z, *reinterpret_cast<const float*>(&PackedColor)));
        Covariance0Data.Add(FVector4f(Covariance.XX, Covariance.XY, Covariance.XZ, Covariance.YY));
        Covariance1Data.Add(FVector2f(Covariance.YZ, Covariance.ZZ));

        if (!bHasSH)
        {
            continue;
        }

        const int32 SHBase = SourceIndex * 45;
        for (int32 CoeffIndex = 0; CoeffIndex < 15; ++CoeffIndex)
        {
            const int32 CoeffBase = SHBase + CoeffIndex * 3;
            SHData.Add(FVector4f(
                InSHCoefficients.IsValidIndex(CoeffBase + 0) ? InSHCoefficients[CoeffBase + 0] : 0.0f,
                InSHCoefficients.IsValidIndex(CoeffBase + 1) ? InSHCoefficients[CoeffBase + 1] : 0.0f,
                InSHCoefficients.IsValidIndex(CoeffBase + 2) ? InSHCoefficients[CoeffBase + 2] : 0.0f,
                0.0f));
        }
    }

    // One dummy element keeps the SRV valid to bind. Binding a null SRV is not
    // allowed, and the shader never reads this when HasSH is 0.
    if (!bHasSH)
    {
        SHData.Add(FVector4f(0.0f, 0.0f, 0.0f, 0.0f));
    }
}

template<typename ElementType>
void FGaussianSplatRenderResources::InitStructuredBuffer(
    FRHICommandListBase& RHICmdList,
    const TCHAR* DebugName,
    TResourceArray<ElementType, VERTEXBUFFER_ALIGNMENT>& ResourceArray,
    FBufferRHIRef& OutBuffer,
    FShaderResourceViewRHIRef& OutSRV)
{
    if (ResourceArray.IsEmpty())
    {
        return;
    }

    FRHIResourceCreateInfo CreateInfo(DebugName, &ResourceArray);
    OutBuffer = RHICmdList.CreateStructuredBuffer(
        sizeof(ElementType),
        ResourceArray.GetResourceDataSize(),
        BUF_Static | BUF_ShaderResource,
        ERHIAccess::SRVMask,
        CreateInfo);
    OutSRV = RHICmdList.CreateShaderResourceView(OutBuffer);
}

void FGaussianSplatRenderResources::InitRHI(FRHICommandListBase& RHICmdList)
{
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetPositionColor"), PositionColorData, PositionColorBuffer, PositionColorSRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetCovariance0"), Covariance0Data, Covariance0Buffer, Covariance0SRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetCovariance1"), Covariance1Data, Covariance1Buffer, Covariance1SRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetSH"), SHData, SHBuffer, SHSRV);
}

void FGaussianSplatRenderResources::ReleaseRHI()
{
    PositionColorSRV.SafeRelease();
    Covariance0SRV.SafeRelease();
    Covariance1SRV.SafeRelease();
    SHSRV.SafeRelease();

    PositionColorBuffer.SafeRelease();
    Covariance0Buffer.SafeRelease();
    Covariance1Buffer.SafeRelease();
    SHBuffer.SafeRelease();
}
