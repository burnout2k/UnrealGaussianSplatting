#pragma once

// A splat capture stored as PACKED PAGES, ready to copy straight to the GPU.
//
// The difference from UGaussianSplatAsset is where the work happens. That class
// keeps the capture as source arrays (positions, rotations, log scales, colours,
// SH) and quantizes them into GPU records every time it loads, which means the
// whole capture must be in RAM in its widest form and the records are rebuilt on
// every load. This class quantizes ONCE, at import, and stores the result: the
// load path is a read, and the upload path is a copy.
//
// That is what makes streaming possible. A page -- 4,096 splats, 64 KiB of
// PackedA, 16 KiB of PackedB, 16 KiB of SH index -- is the unit that moves in
// and out of the VRAM pool, and nothing has to be unpacked or repacked to move
// it.
//
// Layout of one asset:
//
//   inline (ordinary serialized fields, always in RAM)
//     header      format version, page size, cell size, colour range, s_ref/s_p99
//     cell table  per cell: shrink-wrapped bounds, total count, floor count,
//                 first tail page, tail page count
//     SH palette  built over EVERY splat, so an index never depends on which
//                 splats happen to be resident
//
//   payload (FByteBulkData, NOT inlined into the .uasset at cook time)
//     floor block every cell's most important R_c splats, packed, in cell order.
//                 Always resident: a cell that is visible at all draws at least
//                 these, so a miss can never empty a cell.
//     tail pages  the rest of each cell, cut into pages of 4,096. Contiguous in
//                 cell order, so page k of a cell is at a computable offset and
//                 there is no second page table.
//
// Each block is stored SoA: all PackedA, then all PackedB, then all SH indices.
// A page is therefore three contiguous runs, which is exactly what one scatter
// dispatch wants.

#include "CoreMinimal.h"
#include "GaussianSplatAsset.h"
#include "Serialization/BulkData.h"
#include "GaussianSplatPagedAsset.generated.h"

// Splats per tail page. 4,096 was measured against 16,384 in Fix 5 Step 0: the
// larger page wastes less of the range table but blows up drone captures, whose
// cells are small (Vuores went from 712 MiB to 6.4 GB of resident pages).
#define GAUSSIAN_SPLAT_PAGE_SPLATS 4096

// Bytes per splat in each of the three parallel streams.
#define GAUSSIAN_SPLAT_PACKED_A_STRIDE 16
#define GAUSSIAN_SPLAT_PACKED_B_STRIDE 4
#define GAUSSIAN_SPLAT_SH_INDEX_STRIDE 4

USTRUCT()
struct FGaussianSplatPagedCell
{
    GENERATED_BODY()

    // Shrink-wrapped to the splats actually inside, not to the grid slot. The
    // frustum test and the distance estimate use it, and PackedA's 16-bit
    // positions are fractions of it, which is what buys ~1 mm precision.
    UPROPERTY() FVector3f BoundsMin = FVector3f::ZeroVector;
    UPROPERTY() FVector3f BoundsMax = FVector3f::ZeroVector;

    // Every splat the cell holds, floor and tail together. Selection works on
    // this number, never on what is resident, so a view's take does not change
    // when a page is evicted.
    UPROPERTY() int32 Count = 0;

    // The always-resident prefix, R_c. Baked at import from the floor fraction
    // in the header; the selection's own floor CVar is clamped to it, because an
    // asset cannot serve splats below a floor it never baked.
    UPROPERTY() int32 FloorCount = 0;

    // Where this cell's tail starts, in pages, within the payload's tail block.
    // Pages are contiguous in cell order, so page k is at
    // (FirstTailPage + k) * GAUSSIAN_SPLAT_PAGE_SPLATS slots and no separate
    // page table exists.
    UPROPERTY() int32 FirstTailPage = 0;
    UPROPERTY() int32 TailPageCount = 0;

    friend FArchive& operator<<(FArchive& Ar, FGaussianSplatPagedCell& V)
    {
        Ar << V.BoundsMin;
        Ar << V.BoundsMax;
        Ar << V.Count;
        Ar << V.FloorCount;
        Ar << V.FirstTailPage;
        Ar << V.TailPageCount;
        return Ar;
    }
};

// Versions the PAYLOAD's byte layout only. The inline fields are versioned by
// FGaussianSplatCustomVersion, which is UE's own mechanism and already registered
// for this plugin -- duplicating it with a magic number inside the payload would
// give two sources of truth for the same question.
enum class EGaussianSplatPagedFormat : int32
{
    Initial = 1,
    Latest = Initial
};

UCLASS(BlueprintType)
class GAUSSIANSPLATTINGRUNTIME_API UGaussianSplatPagedAsset : public UObject
{
    GENERATED_BODY()

public:
    // ------------------------------------------------------------- the header

    UPROPERTY(VisibleAnywhere, Category = "Gaussian Splat|Format")
    int32 FormatVersion = static_cast<int32>(EGaussianSplatPagedFormat::Latest);

    // Splats per tail page, stored so an asset baked with a different page size
    // is readable rather than silently misread.
    UPROPERTY(VisibleAnywhere, Category = "Gaussian Splat|Format")
    int32 PageSplats = GAUSSIAN_SPLAT_PAGE_SPLATS;

    // Grid pitch in asset units. 16 m for street captures, 64 m for drone ones:
    // the cell is the unit the LOD selection reasons about, so it wants to be
    // about the distance over which detail should change.
    UPROPERTY(VisibleAnywhere, Category = "Gaussian Splat|Cells")
    float CellSize = 16.0f;

    // Cells holding fewer splats than this were dropped at import. Nothing
    // spread that thinly across a whole cell is a surface; it is reconstruction
    // noise. Unlike the legacy class, dropped splats are not parked past the
    // last cell -- they are not in the payload at all, so they cost no bytes.
    UPROPERTY(VisibleAnywhere, Category = "Gaussian Splat|Cells")
    int32 MinCellOccupancy = 100;

    // The baked floor: R_c = max(1, round(FloorFraction * Count)) per cell.
    UPROPERTY(VisibleAnywhere, Category = "Gaussian Splat|Cells")
    float FloorFraction = 0.02f;

    // (min, range) that RGB was quantized over, fitted to the whole capture.
    // Opacity is a sigmoid, always in [0,1], and is not part of this.
    UPROPERTY(VisibleAnywhere, Category = "Gaussian Splat|Format")
    FVector2f ColorEncoding = FVector2f(0.0f, 1.0f);

    // The median and 99th-percentile largest axis, exp(max log scale), over
    // every splat in a cell. LodMode 1's full-detail distance and cell margin.
    // Fix 5 Step 2: this capture was baked WITHOUT its spherical harmonics on
    // purpose (review M1). Stored so a reimport reproduces the bake rather than
    // silently picking the SH back up -- the same reason every other bake setting
    // is a UPROPERTY here and not a console variable.
    UPROPERTY(VisibleAnywhere, Category = "Gaussian Splat|Bake")
    bool bStrippedSH = false;

    UPROPERTY(VisibleAnywhere, Category = "Gaussian Splat|Format")
    float SizeRef = 0.0f;

    UPROPERTY(VisibleAnywhere, Category = "Gaussian Splat|Format")
    float SizeP99 = 0.0f;

    UPROPERTY(VisibleAnywhere, Category = "Gaussian Splat")
    FBoxSphereBounds Bounds = FBoxSphereBounds(ForceInit);

    // Where this came from, for reimport and for telling two bakes apart.
    UPROPERTY(VisibleAnywhere, Category = "Gaussian Splat|Source")
    FString SourcePath;

    UPROPERTY(VisibleAnywhere, Category = "Gaussian Splat|Source")
    int64 SourceSplatCount = 0;

    UPROPERTY(VisibleAnywhere, Category = "Gaussian Splat|Source")
    uint64 SourceHash = 0;

    // --------------------------------------------------------- the big tables

    // Serialized explicitly, not as UPROPERTY arrays: the details panel would
    // otherwise try to build a row per cell, and the transaction buffer would
    // copy the lot on every undo.
    TArray<FGaussianSplatPagedCell> Cells;

    // The cell table in the shape the LOD selection already takes, built once at
    // load. Bounds and count are the real ones; FirstIndex is meaningless here
    // because a paged cell is not contiguous in any one buffer -- the range
    // emitter supplies the pool slots instead. This exists so the selection code
    // needs no second version of itself.
    TArray<FGaussianSplatCell> SelectionCells;

    // Every distinct SH coefficient set, 45 floats each, no padding. Empty for
    // a capture exported at SH degree 0. Inline rather than in the payload
    // because it stays resident for the asset's whole life anyway.
    TArray<float> SHPalette;

    // How many splats live in each block. Derived at import; kept so the reader
    // does not have to walk the cell table to find the tail block.
    UPROPERTY(VisibleAnywhere, Category = "Gaussian Splat|Format")
    int64 FloorSplats = 0;

    UPROPERTY(VisibleAnywhere, Category = "Gaussian Splat|Format")
    int64 TailPages = 0;

    UPROPERTY(VisibleAnywhere, Category = "Gaussian Splat|Format")
    int64 TotalSplats = 0;

    // ------------------------------------------------------------ the payload

    // Not inlined at cook time: without BULKDATA_Force_NOT_InlinePayload a cook
    // writes the whole payload into the .uasset, which would put a district's
    // gigabytes back on the synchronous load path.
    FByteBulkData Payload;

    // ------------------------------------------------------------- the reader

    // True once the payload is in RAM and the asset can hand out pages.
    bool IsPayloadResident() const { return PayloadBytes.Num() > 0; }

    // The payload, in RAM, as one buffer. Valid after PostLoad.
    // Constructed explicitly, NOT with MakeArrayView: that deduces TArrayView<uint8>,
    // whose SizeType defaults to int32 (ArrayView.h:829), so a payload past 2 GiB
    // trips the count assertion at ArrayView.h:229 before the conversion to the
    // 64-bit view declared here ever happens. Lublin is 5.22 GB and crashed the
    // editor on 2026-10-07 the first time its viewport drew; every earlier asset
    // (Uno 516 MB, San Juan 1.29 GB, Vuores 1.68 GB) was under the line.
    TConstArrayView64<uint8> GetPayload() const
    {
        return TConstArrayView64<uint8>(PayloadBytes.GetData(), PayloadBytes.Num());
    }

    bool HasSH() const { return !SHPalette.IsEmpty(); }

    int64 GetRecordStride() const
    {
        return GAUSSIAN_SPLAT_PACKED_A_STRIDE + GAUSSIAN_SPLAT_PACKED_B_STRIDE
            + (HasSH() ? GAUSSIAN_SPLAT_SH_INDEX_STRIDE : 0);
    }

    int64 GetTailSplats() const { return TailPages * PageSplats; }

    // Byte offsets of the six SoA runs inside the payload. The floor block comes
    // first and whole, then the tail block, each in A / B / SH-index order.
    int64 GetFloorOffsetA() const { return 0; }
    int64 GetFloorOffsetB() const { return FloorSplats * GAUSSIAN_SPLAT_PACKED_A_STRIDE; }
    int64 GetFloorOffsetSH() const { return GetFloorOffsetB() + FloorSplats * GAUSSIAN_SPLAT_PACKED_B_STRIDE; }
    int64 GetTailOffsetA() const { return FloorSplats * GetRecordStride(); }
    int64 GetTailOffsetB() const { return GetTailOffsetA() + GetTailSplats() * GAUSSIAN_SPLAT_PACKED_A_STRIDE; }
    int64 GetTailOffsetSH() const { return GetTailOffsetB() + GetTailSplats() * GAUSSIAN_SPLAT_PACKED_B_STRIDE; }
    int64 GetPayloadBytes() const { return (FloorSplats + GetTailSplats()) * GetRecordStride(); }

    // Where cell c's floor block starts, in slots. A prefix sum over FloorCount,
    // computed at load rather than stored: it is derivable, and a stored copy
    // would be a second thing to keep right.
    // Fix 5 Step 2 (review M4): the gate visits only the cells inside the floor
    // radius, so it needs to walk a coordinate box rather than the whole table.
    // The coordinate is NOT serialized -- it is rebuilt at load from BoundsMin
    // and CellSize, which is exactly how the importer derived it, so every asset
    // baked before Step 2 gains it without a re-import and the payload format is
    // untouched.
    FIntVector GetCellCoord(int32 CellIndex) const
    {
        return CellCoords.IsValidIndex(CellIndex) ? CellCoords[CellIndex] : FIntVector::ZeroValue;
    }

    // INDEX_NONE when the grid slot holds no cell -- most of a city's box is
    // empty, and the occupancy threshold drops sparse slots at bake time.
    //
    // A DENSE array when the coordinate box is small enough to hold one, which it
    // is for every scene we have except Perry Road (whose 64,214 floaters stretch
    // its bounds to 4.5 km for a 324 m street, so its box would cost 69 MiB and it
    // keeps the map). The gate walks the whole box including its empty slots, and
    // at a dense pose that was a TMap hash per slot -- the largest single cost in
    // a gate tick that has a 0.5 ms budget and was measured at up to 1.55 ms.
    int32 FindCellByCoord(const FIntVector& Coord) const
    {
        if (CellGrid.Num() > 0)
        {
            const FIntVector Local = Coord - CoordMin;
            if (Local.X < 0 || Local.Y < 0 || Local.Z < 0
                || Local.X >= GridSize.X || Local.Y >= GridSize.Y || Local.Z >= GridSize.Z)
            {
                return INDEX_NONE;
            }
            return CellGrid[(Local.X * GridSize.Y + Local.Y) * GridSize.Z + Local.Z];
        }
        const int32* Found = CellByCoord.Find(Coord);
        return Found != nullptr ? *Found : INDEX_NONE;
    }

    // The inclusive coordinate box the cells span, for clamping a gate's walk.
    FIntVector GetCoordMin() const { return CoordMin; }
    FIntVector GetCoordMax() const { return CoordMax; }

    int64 GetFloorFirstSlot(int32 CellIndex) const
    {
        return FloorFirstSlots.IsValidIndex(CellIndex) ? FloorFirstSlots[CellIndex] : 0;
    }

    // How many splats a cell's page k actually holds. Every page but a cell's
    // last is full; the last is partial, and the slots past the end hold a
    // repeat of the last splat so nothing in the pool is ever uninitialised.
    int32 GetPageSplatCount(int32 CellIndex, int32 PageInCell) const;

    // ------------------------------------------------------------- UObject

    virtual void Serialize(FArchive& Ar) override;
    virtual void PostLoad() override;
    virtual void BeginDestroy() override;
    virtual void GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize) override;

    // Reads the whole payload into RAM, once, straight into our own buffer.
    // Returns false and logs if the read failed; the asset then draws nothing
    // rather than drawing rubbish.
    bool LoadPayloadToMemory();

    void ReleasePayloadFromMemory();

    // Takes freshly baked bytes: writes them to the bulk data AND keeps them
    // resident, because an importer already holds what a load would have to read
    // back from a file that has not been saved yet.
    void SetPayloadFromMemory(TArray64<uint8>&& Bytes);

    // Recomputes everything derived from the tables (the floor prefix sums and
    // the bounds). Called after load and after an import.
    void RefreshDerivedData();

    int64 GetPointCount() const { return TotalSplats; }

private:
    // The payload in RAM. int64-sized throughout: a district is past 2 GB.
    TArray64<uint8> PayloadBytes;

    TArray<int64> FloorFirstSlots;

    // Derived, never serialized: see GetCellCoord.
    TArray<FIntVector> CellCoords;
    TMap<FIntVector, int32> CellByCoord;    // the fallback, for an absurdly sparse box
    TArray<int32> CellGrid;                 // dense, INDEX_NONE where empty; empty when not built
    FIntVector GridSize = FIntVector::ZeroValue;
    FIntVector CoordMin = FIntVector::ZeroValue;
    FIntVector CoordMax = FIntVector::ZeroValue;
};
