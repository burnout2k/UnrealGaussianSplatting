#include "Render/GaussianSplatRenderResources.h"

#include "GaussianSplatBoundsUtils.h"
#include "Math/Float16.h"
#include "RHICommandList.h"

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatResources, Log, All);

namespace
{
    // Smallest-three: the largest component of a unit quaternion is recoverable
    // from the other three, so 2 bits name it and 10 bits each carry the rest
    // over [-1/sqrt(2), 1/sqrt(2)] -- the widest any non-largest component can
    // be. 32 bits total, ~0.1 degrees of error.
    FORCEINLINE uint32 PackUnitQuaternion(const FQuat4f& Q)
    {
        float C[4] = { Q.X, Q.Y, Q.Z, Q.W };

        int32 Largest = 0;
        for (int32 I = 1; I < 4; ++I)
        {
            if (FMath::Abs(C[I]) > FMath::Abs(C[Largest]))
            {
                Largest = I;
            }
        }

        // q and -q are the same rotation, so force the named component positive
        // and the decoder can take the positive square root unconditionally.
        if (C[Largest] < 0.0f)
        {
            for (int32 I = 0; I < 4; ++I)
            {
                C[I] = -C[I];
            }
        }

        constexpr float Range = 0.70710678f;   // 1/sqrt(2)
        uint32 Packed = static_cast<uint32>(Largest);
        int32 Shift = 2;
        for (int32 I = 0; I < 4; ++I)
        {
            if (I == Largest)
            {
                continue;
            }

            const float Normalised = FMath::Clamp(C[I] / Range * 0.5f + 0.5f, 0.0f, 1.0f);
            Packed |= static_cast<uint32>(FMath::RoundToInt(Normalised * 1023.0f)) << Shift;
            Shift += 10;
        }
        return Packed;
    }

    FORCEINLINE uint32 PackHalf2(float A, float B)
    {
        return static_cast<uint32>(FFloat16(A).Encoded)
            | (static_cast<uint32>(FFloat16(B).Encoded) << 16);
    }

    FORCEINLINE uint32 QuantizeUnit16(float Normalised)
    {
        return static_cast<uint32>(FMath::RoundToInt(FMath::Clamp(Normalised, 0.0f, 1.0f) * 65535.0f));
    }
}

void FGaussianSplatRenderResources::BuildFromAssetData(
    const TArray<FVector3f>& InPositions,
    const TArray<FGaussianCovariance3f>& InCovariances,
    const TArray<FQuat4f>& InRotations,
    const TArray<FVector3f>& InLogScales,
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

    PackedAData.Empty(PointCount);
    PackedBData.Empty(PointCount);
    CellBoundsData.Empty(FMath::Max(2, Cells.Num() * 2));

    // Quantization needs rotation and scale; the covariance cannot be recovered
    // into them without an eigendecomposition. Assets imported before the parser
    // kept them simply cannot be uploaded.
    if (PointCount > 0 && (InRotations.IsEmpty() || InLogScales.IsEmpty()))
    {
        UE_LOG(
            LogGaussianSplatResources,
            Error,
            TEXT("This asset stores a baked covariance and predates rotation/scale, which the ")
            TEXT("quantized GPU format needs. Re-import the source PLY. Nothing was uploaded."));
        PointCount = 0;
        Cells.Reset();
    }

    // Positions are 16-bit fractions of their cell, so there has to be a cell.
    // An asset whose splats were all filtered out of cells gets one synthetic
    // cell over everything -- 16 bits across a 2.4 km capture is ~40 mm rather
    // than the ~1 mm a 64 m cell gives, which is precisely why cells exist. This
    // is a fallback, not a design.
    if (PointCount > 0 && Cells.IsEmpty())
    {
        FVector3f Min(TNumericLimits<float>::Max());
        FVector3f Max(TNumericLimits<float>::Lowest());
        for (int32 Index = 0; Index < SourceOfDest.Num(); ++Index)
        {
            const FVector3f P = InPositions[SourceOfDest[Index]];
            Min = FVector3f::Min(Min, P);
            Max = FVector3f::Max(Max, P);
        }

        FGaussianSplatCell Whole;
        Whole.BoundsMin = Min;
        Whole.BoundsMax = Max;
        Whole.FirstIndex = 0;
        Whole.Count = SourceOfDest.Num();
        Cells.Add(Whole);
    }

    // Fit the RGB quantization range to what this capture actually contains.
    //
    // A fixed [0,1] would be wrong: 3DGS colour is 0.5 + 0.28209 * f_dc and is
    // unbounded. Measured on the 85.8M Vuores capture, 2.4-7.6% of splats have a
    // channel outside [0,1], peaking at 2.259 -- all of which a naive encoding
    // would clamp to white.
    //
    // Fitted over EVERY splat, not just the resident subset: otherwise the range
    // shifts whenever MaxGpuPointCount changes (measured [-0.037, 1.840] at 1M
    // resident against [-0.037, 2.259] at 60M), which makes the encoding depend
    // on an unrelated setting and makes two point counts incomparable.
    float ColorMin = TNumericLimits<float>::Max();
    float ColorMax = TNumericLimits<float>::Lowest();
    for (int32 Index = 0; Index < InColorsOpacity.Num(); ++Index)
    {
        const FVector4f& Color = InColorsOpacity[Index];
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
    // 15 float4 per splat would be zero: read every frame and multiplied by
    // nothing. Skip the buffer entirely and let the shader branch past it.
    bHasSH = !InSHCoefficients.IsEmpty();
    SHData.Empty(bHasSH ? PointCount * 15 : 1);

    for (const FGaussianSplatCell& Cell : Cells)
    {
        const FVector3f Origin = Cell.BoundsMin;
        // A cell one splat wide has zero extent on some axis; the reciprocal
        // would be infinite and the quantized value NaN.
        const FVector3f Extent(
            FMath::Max(Cell.BoundsMax.X - Origin.X, UE_KINDA_SMALL_NUMBER),
            FMath::Max(Cell.BoundsMax.Y - Origin.Y, UE_KINDA_SMALL_NUMBER),
            FMath::Max(Cell.BoundsMax.Z - Origin.Z, UE_KINDA_SMALL_NUMBER));

        CellBoundsData.Add(FVector4f(Origin.X, Origin.Y, Origin.Z, 0.0f));
        CellBoundsData.Add(FVector4f(Extent.X, Extent.Y, Extent.Z, 0.0f));

        for (int32 Offset = 0; Offset < Cell.Count; ++Offset)
        {
            const int32 SourceIndex = SourceOfDest[Cell.FirstIndex + Offset];
            const FVector3f Position = InPositions[SourceIndex];
            const FQuat4f Rotation = InRotations[SourceIndex];
            const FVector3f LogScale = InLogScales[SourceIndex];
            const FVector4f Color = InColorsOpacity.IsValidIndex(SourceIndex)
                ? InColorsOpacity[SourceIndex]
                : FVector4f(1.0f, 1.0f, 1.0f, 1.0f);

            const FVector3f Local = (Position - Origin) / Extent;
            const uint32 QX = QuantizeUnit16(Local.X);
            const uint32 QY = QuantizeUnit16(Local.Y);
            const uint32 QZ = QuantizeUnit16(Local.Z);

            PackedAData.Add(FUintVector4(
                QX | (QY << 16),
                QZ | (static_cast<uint32>(FFloat16(LogScale.X).Encoded) << 16),
                PackHalf2(LogScale.Y, LogScale.Z),
                PackUnitQuaternion(Rotation)));

            const FVector3f Normalised =
                (FVector3f(Color.X, Color.Y, Color.Z) - FVector3f(ColorEncoding.X)) / ColorEncoding.Y;
            PackedBData.Add(
                  static_cast<uint32>(FMath::RoundToInt(FMath::Clamp(Normalised.X, 0.0f, 1.0f) * 255.0f))
                | static_cast<uint32>(FMath::RoundToInt(FMath::Clamp(Normalised.Y, 0.0f, 1.0f) * 255.0f)) << 8
                | static_cast<uint32>(FMath::RoundToInt(FMath::Clamp(Normalised.Z, 0.0f, 1.0f) * 255.0f)) << 16
                | static_cast<uint32>(FMath::RoundToInt(FMath::Clamp(Color.W, 0.0f, 1.0f) * 255.0f)) << 24);

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
    }

    // One dummy element keeps each SRV valid to bind; a null SRV is not allowed,
    // and the shader never reads these when the counts are zero.
    if (!bHasSH)
    {
        SHData.Add(FVector4f(0.0f, 0.0f, 0.0f, 0.0f));
    }
    if (CellBoundsData.IsEmpty())
    {
        CellBoundsData.Add(FVector4f::Zero());
        CellBoundsData.Add(FVector4f(1.0f, 1.0f, 1.0f, 0.0f));
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
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.PackedA"), PackedAData, PackedABuffer, PackedASRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.PackedB"), PackedBData, PackedBBuffer, PackedBSRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.CellBounds"), CellBoundsData, CellBoundsBuffer, CellBoundsSRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetSH"), SHData, SHBuffer, SHSRV);
}

void FGaussianSplatRenderResources::ReleaseRHI()
{
    PackedASRV.SafeRelease();
    PackedBSRV.SafeRelease();
    CellBoundsSRV.SafeRelease();
    SHSRV.SafeRelease();

    PackedABuffer.SafeRelease();
    PackedBBuffer.SafeRelease();
    CellBoundsBuffer.SafeRelease();
    SHBuffer.SafeRelease();
}
