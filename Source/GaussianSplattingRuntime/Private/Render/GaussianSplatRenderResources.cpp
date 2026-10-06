#include "Render/GaussianSplatRenderResources.h"

#include "GaussianSplatBoundsUtils.h"
#include "GaussianSplatFormat.h"
#include "Hash/CityHash.h"
#include "Math/Float16.h"
#include "RHICommandList.h"

#include <algorithm>

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatResources, Log, All);

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

    // Splat sizes for LodMode 1's full-detail distance (s_ref) and cell margin (s_p99). Element N/2 and
    // (N-1)*99/100 of the sorted largest log scales, so S/lod/sref_f3.py reproduces them exactly.
    SizeRef = 0.0f;
    SizeP99 = 0.0f;
    if (!InLogScales.IsEmpty())
    {
        TArray<float> LargestLog;
        const auto AddLargest = [&InLogScales, &LargestLog](int32 Index)
        {
            const FVector3f& LogScale = InLogScales[Index];
            LargestLog.Add(FMath::Max3(LogScale.X, LogScale.Y, LogScale.Z));
        };
        if (InCells.IsEmpty())
        {
            LargestLog.Reserve(InLogScales.Num());
            for (int32 Index = 0; Index < InLogScales.Num(); ++Index)
            {
                AddLargest(Index);
            }
        }
        else
        {
            int64 InCellCount = 0;
            for (const FGaussianSplatCell& Cell : InCells)
            {
                InCellCount += FMath::Max(0, Cell.Count);
            }
            LargestLog.Reserve(InCellCount);
            for (const FGaussianSplatCell& Cell : InCells)
            {
                for (int32 Offset = 0; Offset < Cell.Count; ++Offset)
                {
                    if (InLogScales.IsValidIndex(Cell.FirstIndex + Offset))
                    {
                        AddLargest(Cell.FirstIndex + Offset);
                    }
                }
            }
        }
        if (!LargestLog.IsEmpty())
        {
            const int64 Count = LargestLog.Num();
            const int64 MedianIndex = Count / 2;
            const int64 P99Index = (Count - 1) * 99 / 100;
            std::nth_element(LargestLog.GetData(), LargestLog.GetData() + MedianIndex, LargestLog.GetData() + Count);
            SizeRef = static_cast<float>(FMath::Exp(static_cast<double>(LargestLog[MedianIndex])));
            std::nth_element(LargestLog.GetData(), LargestLog.GetData() + P99Index, LargestLog.GetData() + Count);
            SizeP99 = static_cast<float>(FMath::Exp(static_cast<double>(LargestLog[P99Index])));
            UE_LOG(
                LogGaussianSplatResources,
                Display,
                TEXT("Splat sizes for LodMode 1: s_ref %.6f, s_p99 %.6f asset units (largest axis over %lld splats in cells)"),
                SizeRef,
                SizeP99,
                Count);
        }
    }

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
    ColorEncoding = GaussianSplatFormat::MakeColorEncoding(ColorMin, ColorMax);

    // A capture exported at SH degree 0 has no f_rest_* data, so every
    // coefficient would be zero: read every frame and multiplied by nothing.
    // Skip the buffers entirely and let the shader branch past them.
    //
    // Otherwise SH is deduplicated into a palette (see the header). Sets are
    // keyed by a 64-bit hash of their bytes, and a hit is only taken after a
    // byte compare, so a hash collision costs a duplicate entry, never a wrong
    // one.
    bHasSH = !InSHCoefficients.IsEmpty();
    SHIndexData.Empty(bHasSH ? PointCount : 1);
    SHPaletteData.Reset();
    TMap<uint64, int32> SHEntryOfHash;
    constexpr int32 SHFloatsPerSet = 45;
    // The RHI takes a buffer's size as uint32; a palette past 4 GiB would hit
    // a Fatal in TResourceArray::GetResourceDataSize and take the editor down.
    constexpr int64 MaxSHPaletteSets = MAX_uint32 / (SHFloatsPerSet * sizeof(float));

    for (const FGaussianSplatCell& Cell : Cells)
    {
        const FVector3f Origin = Cell.BoundsMin;
        const FVector3f Extent = GaussianSplatFormat::CellExtent(Origin, Cell.BoundsMax);

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

            PackedAData.Add(GaussianSplatFormat::PackSplatA(Position, Origin, Extent, LogScale, Rotation));
            PackedBData.Add(GaussianSplatFormat::PackSplatB(Color, ColorEncoding));

            if (!bHasSH)
            {
                continue;
            }

            const int32 SHBase = SourceIndex * SHFloatsPerSet;
            float Coeffs[SHFloatsPerSet];
            for (int32 CoeffIndex = 0; CoeffIndex < SHFloatsPerSet; ++CoeffIndex)
            {
                Coeffs[CoeffIndex] = InSHCoefficients.IsValidIndex(SHBase + CoeffIndex)
                    ? InSHCoefficients[SHBase + CoeffIndex]
                    : 0.0f;
            }

            const uint64 Hash = CityHash64(reinterpret_cast<const char*>(Coeffs), sizeof(Coeffs));
            const int32* Existing = SHEntryOfHash.Find(Hash);
            if (Existing
                && FMemory::Memcmp(&SHPaletteData[*Existing * SHFloatsPerSet], Coeffs, sizeof(Coeffs)) == 0)
            {
                SHIndexData.Add(static_cast<uint32>(*Existing));
                continue;
            }

            const int32 Entry = SHPaletteData.Num() / SHFloatsPerSet;
            if (Entry >= MaxSHPaletteSets)
            {
                // Dropping SH keeps every splat and only loses view-dependent
                // colour; the alternative is the Fatal.
                UE_LOG(
                    LogGaussianSplatResources,
                    Warning,
                    TEXT("SH palette exceeds one 4 GiB GPU buffer (more than %lld distinct ")
                    TEXT("coefficient sets); uploading %u splats WITHOUT spherical harmonics. ")
                    TEXT("Lower MaxGpuPointCount to keep them."),
                    MaxSHPaletteSets,
                    PointCount);
                bHasSH = false;
                continue;
            }
            if (!Existing)
            {
                SHEntryOfHash.Add(Hash, Entry);
            }
            SHPaletteData.Append(Coeffs, SHFloatsPerSet);
            SHIndexData.Add(static_cast<uint32>(Entry));
        }
    }

    if (bHasSH && !SHIndexData.IsEmpty())
    {
        UE_LOG(
            LogGaussianSplatResources,
            Display,
            TEXT("SH palette: %d distinct coefficient sets for %d splats, %.0f MiB"),
            SHPaletteData.Num() / SHFloatsPerSet,
            SHIndexData.Num(),
            (SHPaletteData.Num() * sizeof(float) + SHIndexData.Num() * sizeof(uint32)) / (1024.0 * 1024.0));
    }
    else
    {
        // One dummy element keeps each SRV valid to bind; a null SRV is not
        // allowed, and the shader never reads these when HasSH is 0.
        bHasSH = false;
        SHIndexData.Empty(1);
        SHPaletteData.Empty(1);
        SHIndexData.Add(0u);
        SHPaletteData.Add(0.0f);
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
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.SHIndex"), SHIndexData, SHIndexBuffer, SHIndexSRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.SHPalette"), SHPaletteData, SHPaletteBuffer, SHPaletteSRV);
}

void FGaussianSplatRenderResources::ReleaseRHI()
{
    PackedASRV.SafeRelease();
    PackedBSRV.SafeRelease();
    CellBoundsSRV.SafeRelease();
    SHIndexSRV.SafeRelease();
    SHPaletteSRV.SafeRelease();

    PackedABuffer.SafeRelease();
    PackedBBuffer.SafeRelease();
    CellBoundsBuffer.SafeRelease();
    SHIndexBuffer.SafeRelease();
    SHPaletteBuffer.SafeRelease();
}
