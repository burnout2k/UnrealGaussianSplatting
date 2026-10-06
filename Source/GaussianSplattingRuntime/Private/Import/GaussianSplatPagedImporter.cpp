#include "Import/GaussianSplatPagedImporter.h"

#include "Algo/Sort.h"
#include "Async/ParallelFor.h"
#include "GaussianSplatFormat.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatPagedAsset.h"
#include "Hash/CityHash.h"
#include "Import/GaussianSplatPlyReader.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/BulkData.h"

#include <algorithm>

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatPagedImport, Log, All);

using namespace GaussianSplatPly;

namespace
{
    constexpr int32 SHFloatsPerSet = 45;

    // Everything pass 1 learns, kept across the sort and read again by pass 2.
    // Deliberately narrow: at Lublin's 259M splats each byte here is a quarter of
    // a gigabyte, so nothing is stored that pass 2 can decode again from the row.
    struct FPass1
    {
        TArray64<FVector3f> Position;    // 12 B: needed for the cell key AND the shrink-wrapped bounds
        TArray64<float> Importance;      //  4 B: the bake order
        TArray64<float> LargestLog;      //  4 B: s_ref / s_p99
        TArray64<int32> SlotOfSplat;     //  4 B: which grid slot the splat fell in
        TArray64<int32> SHIndexOfSplat;  //  4 B, only when the capture has SH

        float ColorMin = TNumericLimits<float>::Max();
        float ColorMax = TNumericLimits<float>::Lowest();
        TArray<float> SHPalette;
    };

    struct FPlyLayout
    {
        const uint8* Rows = nullptr;
        int64 RowStride = 0;
        int64 VertexCount = 0;
        int32 X = INDEX_NONE, Y = INDEX_NONE, Z = INDEX_NONE;
        int32 Dc0 = INDEX_NONE, Dc1 = INDEX_NONE, Dc2 = INDEX_NONE, Opacity = INDEX_NONE;
        int32 S0 = INDEX_NONE, S1 = INDEX_NONE, S2 = INDEX_NONE;
        int32 R0 = INDEX_NONE, R1 = INDEX_NONE, R2 = INDEX_NONE, R3 = INDEX_NONE;
        TArray<int32> RestIndices;
        bool bHasSH = false;
        const FPlyHeader* Header = nullptr;
    };

    // One row's properties as floats, in property order -- the same TArray<float>
    // the legacy parser builds, so every Build* helper reads it identically.
    void ReadRow(const FPlyLayout& Layout, int64 RowIndex, TArray<float>& OutValues)
    {
        const uint8* Cursor = Layout.Rows + RowIndex * Layout.RowStride;
        OutValues.Reset(Layout.Header->VertexProperties.Num());
        for (const FPlyProperty& Property : Layout.Header->VertexProperties)
        {
            OutValues.Add(ReadScalarAsFloat(Cursor, Property.Type));
            Cursor += GetTypeSize(Property.Type);
        }
    }

    bool BuildLayout(const TArray64<uint8>& Raw, const FPlyHeader& Header, FPlyLayout& Out, FString& OutError)
    {
        if (!Header.bBinaryLittleEndian)
        {
            OutError = TEXT("The paged importer reads binary little-endian PLY only: pass 2 gathers rows in sorted "
                            "order, which needs a fixed row stride to seek by. Convert the file first.");
            return false;
        }

        if (!ResolveCommonPropertyIndices(
                Header, Out.X, Out.Y, Out.Z, Out.Dc0, Out.Dc1, Out.Dc2, Out.Opacity,
                Out.S0, Out.S1, Out.S2, Out.R0, Out.R1, Out.R2, Out.R3, Out.RestIndices))
        {
            OutError = TEXT("The PLY has no x/y/z vertex properties.");
            return false;
        }

        if (Out.S0 == INDEX_NONE || Out.R0 == INDEX_NONE)
        {
            OutError = TEXT("The PLY has no scale_* or rot_* properties. The quantized GPU format needs both.");
            return false;
        }

        Out.RowStride = 0;
        for (const FPlyProperty& Property : Header.VertexProperties)
        {
            Out.RowStride += GetTypeSize(Property.Type);
        }

        Out.Header = &Header;
        Out.Rows = Raw.GetData() + Header.HeaderByteSize;
        Out.VertexCount = Header.VertexCount;
        Out.bHasSH = CaptureHasSH(Out.RestIndices);

        const int64 Needed = Header.HeaderByteSize + Out.VertexCount * Out.RowStride;
        if (Raw.Num() < Needed)
        {
            OutError = FString::Printf(
                TEXT("The PLY is %lld bytes but its header describes %lld (%lld vertices of %lld bytes). Truncated?"),
                Raw.Num(), Needed, Out.VertexCount, Out.RowStride);
            return false;
        }
        return true;
    }

    // Pass 1. Everything that is per-splat, per-cell or whole-asset is learned
    // here; pass 2 only gathers and packs (Step 1 specification item 2).
    void RunPass1(const FPlyLayout& Layout, float InvCellPitch, FPass1& Out)
    {
        const int64 Count = Layout.VertexCount;
        Out.Position.SetNumUninitialized(Count);
        Out.Importance.SetNumUninitialized(Count);
        Out.LargestLog.SetNumUninitialized(Count);

        // Grid coordinates first, in parallel; the coordinate -> slot map below is
        // a hash insert per splat and has to be serial.
        TArray64<FIntVector> Coord;
        Coord.SetNumUninitialized(Count);

        constexpr int64 ChunkSplats = 1 << 16;
        const int32 ChunkCount = static_cast<int32>(FMath::DivideAndRoundUp<int64>(Count, ChunkSplats));
        TArray<float> ChunkColorMin, ChunkColorMax;
        ChunkColorMin.Init(TNumericLimits<float>::Max(), ChunkCount);
        ChunkColorMax.Init(TNumericLimits<float>::Lowest(), ChunkCount);

        ParallelFor(ChunkCount, [&](int32 Chunk)
        {
            TArray<float> Values;
            const int64 First = static_cast<int64>(Chunk) * ChunkSplats;
            const int64 Last = FMath::Min(First + ChunkSplats, Count);
            float LocalMin = TNumericLimits<float>::Max();
            float LocalMax = TNumericLimits<float>::Lowest();

            for (int64 Index = First; Index < Last; ++Index)
            {
                ReadRow(Layout, Index, Values);
                const FImportedGaussian G = BuildImportedGaussian(
                    Values, Layout.X, Layout.Y, Layout.Z, Layout.S0, Layout.S1, Layout.S2,
                    Layout.R0, Layout.R1, Layout.R2, Layout.R3);
                const FLinearColor Color = BuildColor(Values, Layout.Dc0, Layout.Dc1, Layout.Dc2, Layout.Opacity);

                Out.Position[Index] = G.Position;
                Out.Importance[Index] = GaussianSplatFormat::SplatImportance(G.LogScale, Color.A);
                Out.LargestLog[Index] = FMath::Max3(G.LogScale.X, G.LogScale.Y, G.LogScale.Z);
                Coord[Index] = GaussianSplatFormat::CellCoord(G.Position, InvCellPitch);

                LocalMin = FMath::Min3(LocalMin, FMath::Min(Color.R, Color.G), Color.B);
                LocalMax = FMath::Max3(LocalMax, FMath::Max(Color.R, Color.G), Color.B);
            }
            ChunkColorMin[Chunk] = LocalMin;
            ChunkColorMax[Chunk] = LocalMax;
        });

        for (int32 Chunk = 0; Chunk < ChunkCount; ++Chunk)
        {
            Out.ColorMin = FMath::Min(Out.ColorMin, ChunkColorMin[Chunk]);
            Out.ColorMax = FMath::Max(Out.ColorMax, ChunkColorMax[Chunk]);
        }

        // Coordinate -> slot. Sparse by necessity: a handful of reconstruction
        // floaters stretch a 2.4 km capture's box to ~14 km, so a dense grid over
        // that would be millions of empty slots.
        Out.SlotOfSplat.SetNumUninitialized(Count);
        TMap<FIntVector, int32> SlotByCoord;
        SlotByCoord.Reserve(16384);
        for (int64 Index = 0; Index < Count; ++Index)
        {
            const FIntVector& C = Coord[Index];
            if (const int32* Existing = SlotByCoord.Find(C))
            {
                Out.SlotOfSplat[Index] = *Existing;
            }
            else
            {
                const int32 Slot = SlotByCoord.Num();
                SlotByCoord.Add(C, Slot);
                Out.SlotOfSplat[Index] = Slot;
            }
        }

        // The slot order the cell table will use, by coordinate rather than by
        // discovery, so a rebuild of the same capture is reproducible.
        TArray<FIntVector> SlotCoord;
        SlotCoord.SetNumUninitialized(SlotByCoord.Num());
        for (const TPair<FIntVector, int32>& Pair : SlotByCoord)
        {
            SlotCoord[Pair.Value] = Pair.Key;
        }
        Coord.Empty();

        // Remap slot ids into coordinate order now, so nothing downstream has to
        // carry both numbering schemes.
        TArray<int32> SlotOrder;
        SlotOrder.SetNumUninitialized(SlotCoord.Num());
        for (int32 Slot = 0; Slot < SlotCoord.Num(); ++Slot)
        {
            SlotOrder[Slot] = Slot;
        }
        Algo::Sort(SlotOrder, [&SlotCoord](int32 A, int32 B)
        {
            return GaussianSplatFormat::CellCoordLess(SlotCoord[A], SlotCoord[B]);
        });
        TArray<int32> RankOfSlot;
        RankOfSlot.SetNumUninitialized(SlotCoord.Num());
        for (int32 Rank = 0; Rank < SlotOrder.Num(); ++Rank)
        {
            RankOfSlot[SlotOrder[Rank]] = Rank;
        }
        ParallelFor(ChunkCount, [&](int32 Chunk)
        {
            const int64 First = static_cast<int64>(Chunk) * ChunkSplats;
            const int64 Last = FMath::Min(First + ChunkSplats, Count);
            for (int64 Index = First; Index < Last; ++Index)
            {
                Out.SlotOfSplat[Index] = RankOfSlot[Out.SlotOfSplat[Index]];
            }
        });
    }

    // The SH palette, over EVERY splat rather than the resident subset, so an
    // index never changes when a page is evicted. Sets are keyed by a 64-bit hash
    // of their bytes and a hit is only taken after a byte compare, so a collision
    // costs a duplicate entry, never a wrong colour.
    void BuildSHPalette(const FPlyLayout& Layout, FPass1& Out)
    {
        const int64 Count = Layout.VertexCount;
        Out.SHIndexOfSplat.SetNumUninitialized(Count);

        // The RHI takes a buffer size as uint32, and TResourceArray would hit a
        // Fatal past 4 GiB.
        constexpr int64 MaxSets = MAX_uint32 / (SHFloatsPerSet * sizeof(float));

        TMap<uint64, int32> EntryOfHash;
        TArray<float> Values;
        float Coeffs[SHFloatsPerSet] = {};

        for (int64 Index = 0; Index < Count; ++Index)
        {
            ReadRow(Layout, Index, Values);
            FMemory::Memzero(Coeffs, sizeof(Coeffs));
            BuildReorderedSH(Values, Layout.RestIndices, Coeffs);

            const uint64 Hash = CityHash64(reinterpret_cast<const char*>(Coeffs), sizeof(Coeffs));
            const int32* Existing = EntryOfHash.Find(Hash);
            if (Existing && FMemory::Memcmp(&Out.SHPalette[*Existing * SHFloatsPerSet], Coeffs, sizeof(Coeffs)) == 0)
            {
                Out.SHIndexOfSplat[Index] = *Existing;
                continue;
            }

            const int32 Entry = Out.SHPalette.Num() / SHFloatsPerSet;
            if (Entry >= MaxSets)
            {
                UE_LOG(
                    LogGaussianSplatPagedImport,
                    Warning,
                    TEXT("The SH palette passed one 4 GiB buffer (more than %lld distinct sets). Importing WITHOUT "
                         "spherical harmonics: every splat keeps its base colour."),
                    MaxSets);
                Out.SHPalette.Reset();
                Out.SHIndexOfSplat.Empty();
                return;
            }
            if (!Existing)
            {
                EntryOfHash.Add(Hash, Entry);
            }
            Out.SHPalette.Append(Coeffs, SHFloatsPerSet);
            Out.SHIndexOfSplat[Index] = Entry;
        }
    }

    // The cell table and the bake order: which cells survive, and in what order
    // each cell's splats are written. This is the half that has to match
    // UGaussianSplatAsset::BuildCells exactly, or a paged asset and the legacy one
    // it was built from draw different splats at the same budget.
    struct FBake
    {
        TArray<FGaussianSplatPagedCell> Cells;
        TArray64<int32> Order;          // destination slot -> original PLY index
        TArray<int64> CellFirst;        // destination slot of each cell's first splat
    };

    void BuildBakeOrder(const FPass1& Pass1, int64 SplatCount, int32 SlotCount, int32 MinOccupancy, FBake& Out)
    {
        TArray<int32> SlotSplatCount;
        SlotSplatCount.Init(0, SlotCount);
        for (int64 Index = 0; Index < SplatCount; ++Index)
        {
            ++SlotSplatCount[Pass1.SlotOfSplat[Index]];
        }

        // Slots are already in coordinate order, so keeping them in order keeps
        // the cell table in coordinate order too.
        TArray<int32> CellOfSlot;
        CellOfSlot.Init(INDEX_NONE, SlotCount);
        int64 Running = 0;
        for (int32 Slot = 0; Slot < SlotCount; ++Slot)
        {
            if (SlotSplatCount[Slot] < MinOccupancy || SlotSplatCount[Slot] <= 0)
            {
                continue;
            }
            CellOfSlot[Slot] = Out.Cells.Num();

            FGaussianSplatPagedCell Cell;
            Cell.Count = SlotSplatCount[Slot];
            Out.Cells.Add(Cell);
            Out.CellFirst.Add(Running);
            Running += Cell.Count;
        }

        // Destination -> source, cell-contiguous. Splats in dropped cells are not
        // written at all, so unlike the legacy class they cost no bytes.
        Out.Order.SetNumUninitialized(Running);
        TArray<int64> Cursor = Out.CellFirst;
        for (int64 Index = 0; Index < SplatCount; ++Index)
        {
            const int32 CellIndex = CellOfSlot[Pass1.SlotOfSplat[Index]];
            if (CellIndex != INDEX_NONE)
            {
                Out.Order[Cursor[CellIndex]++] = static_cast<int32>(Index);
            }
        }

        // Importance order inside each cell, so a keep-count of N means the N most
        // significant splats rather than an arbitrary N. Ties go to the lower
        // original index, which is what makes the order reproducible.
        ParallelFor(Out.Cells.Num(), [&](int32 CellIndex)
        {
            TArrayView<int32> Slice(Out.Order.GetData() + Out.CellFirst[CellIndex], Out.Cells[CellIndex].Count);
            Algo::Sort(Slice, [&Pass1](int32 A, int32 B)
            {
                return GaussianSplatFormat::ImportanceLess(Pass1.Importance[A], A, Pass1.Importance[B], B);
            });
        });

        // Shrink-wrap each cell to the splats actually inside it, not to the grid
        // slot: a cell holding six floaters then presents a one-metre target to the
        // frustum test instead of a 64 m one, and PackedA's 16 bits span a metre.
        ParallelFor(Out.Cells.Num(), [&](int32 CellIndex)
        {
            FGaussianSplatPagedCell& Cell = Out.Cells[CellIndex];
            FVector3f Min(TNumericLimits<float>::Max());
            FVector3f Max(TNumericLimits<float>::Lowest());
            const int64 First = Out.CellFirst[CellIndex];
            for (int64 Offset = 0; Offset < Cell.Count; ++Offset)
            {
                const FVector3f& P = Pass1.Position[Out.Order[First + Offset]];
                Min = FVector3f::Min(Min, P);
                Max = FVector3f::Max(Max, P);
            }
            Cell.BoundsMin = Min;
            Cell.BoundsMax = Max;
        });
    }

    // The floor/tail split. Pages are contiguous in cell order, so a cell's page k
    // is at (FirstTailPage + k) * PageSplats and there is no page table.
    void AssignPages(TArray<FGaussianSplatPagedCell>& Cells, float FloorFraction, int64& OutFloorSplats, int64& OutTailPages)
    {
        OutFloorSplats = 0;
        OutTailPages = 0;
        for (FGaussianSplatPagedCell& Cell : Cells)
        {
            Cell.FloorCount = FMath::Clamp(FMath::RoundToInt(Cell.Count * FloorFraction), 1, Cell.Count);
            const int64 TailCount = Cell.Count - Cell.FloorCount;
            Cell.FirstTailPage = static_cast<int32>(OutTailPages);
            Cell.TailPageCount = static_cast<int32>(FMath::DivideAndRoundUp<int64>(TailCount, GAUSSIAN_SPLAT_PAGE_SPLATS));
            OutFloorSplats += Cell.FloorCount;
            OutTailPages += Cell.TailPageCount;
        }
    }
}

namespace GaussianSplatPagedImporter
{
    bool ImportFromFile(
        const FString& FilePath,
        const FOptions& Options,
        UGaussianSplatPagedAsset& OutAsset,
        FString& OutError)
    {
        const double StartTime = FPlatformTime::Seconds();

        // The whole file in RAM. A streaming reader would buy nothing: pass 2
        // gathers rows in sorted order, which is random access, so it wants the
        // file resident or mapped anyway (review 2 M5).
        TArray64<uint8> Raw;
        if (!FFileHelper::LoadFileToArray(Raw, *FilePath))
        {
            OutError = FString::Printf(TEXT("Could not read %s."), *FilePath);
            return false;
        }

        FPlyHeader Header;
        if (!ParseHeader(Raw, Header) || Header.VertexCount <= 0)
        {
            OutError = TEXT("Not a PLY, or it has no vertices.");
            return false;
        }

        FPlyLayout Layout;
        if (!BuildLayout(Raw, Header, Layout, OutError))
        {
            return false;
        }

        const float Pitch = FMath::Max(1.0f, Options.CellSize);
        FPass1 Pass1;
        RunPass1(Layout, 1.0f / Pitch, Pass1);

        int32 SlotCount = 0;
        for (int64 Index = 0; Index < Layout.VertexCount; ++Index)
        {
            SlotCount = FMath::Max(SlotCount, Pass1.SlotOfSplat[Index] + 1);
        }

        if (Layout.bHasSH)
        {
            BuildSHPalette(Layout, Pass1);
        }

        FBake Bake;
        BuildBakeOrder(Pass1, Layout.VertexCount, SlotCount, FMath::Max(0, Options.MinCellOccupancy), Bake);
        if (Bake.Cells.IsEmpty())
        {
            OutError = FString::Printf(
                TEXT("Every cell held fewer than %d splats at a %.1f unit pitch, so nothing would be drawn. "
                     "Lower MinCellOccupancy or raise CellSize."),
                Options.MinCellOccupancy, Pitch);
            return false;
        }

        int64 FloorSplats = 0;
        int64 TailPages = 0;
        AssignPages(Bake.Cells, Options.FloorFraction, FloorSplats, TailPages);

        // s_ref and s_p99 over every splat IN A CELL, matching the legacy path so
        // LodScreenK keeps its meaning across the two.
        float SizeRef = 0.0f;
        float SizeP99 = 0.0f;
        if (!Bake.Order.IsEmpty())
        {
            TArray64<float> LargestLog;
            LargestLog.SetNumUninitialized(Bake.Order.Num());
            for (int64 Slot = 0; Slot < Bake.Order.Num(); ++Slot)
            {
                LargestLog[Slot] = Pass1.LargestLog[Bake.Order[Slot]];
            }
            const int64 N = LargestLog.Num();
            const int64 MedianIndex = N / 2;
            const int64 P99Index = (N - 1) * 99 / 100;
            std::nth_element(LargestLog.GetData(), LargestLog.GetData() + MedianIndex, LargestLog.GetData() + N);
            SizeRef = static_cast<float>(FMath::Exp(static_cast<double>(LargestLog[MedianIndex])));
            std::nth_element(LargestLog.GetData(), LargestLog.GetData() + P99Index, LargestLog.GetData() + N);
            SizeP99 = static_cast<float>(FMath::Exp(static_cast<double>(LargestLog[P99Index])));
        }

        const bool bHasSH = Layout.bHasSH && !Pass1.SHPalette.IsEmpty();
        const FVector2f ColorEncoding = GaussianSplatFormat::MakeColorEncoding(Pass1.ColorMin, Pass1.ColorMax);
        const int64 TailSplats = TailPages * GAUSSIAN_SPLAT_PAGE_SPLATS;
        const int64 RecordStride = GAUSSIAN_SPLAT_PACKED_A_STRIDE + GAUSSIAN_SPLAT_PACKED_B_STRIDE
            + (bHasSH ? GAUSSIAN_SPLAT_SH_INDEX_STRIDE : 0);
        const int64 PayloadBytes = (FloorSplats + TailSplats) * RecordStride;

        // Pass 2: gather and pack. Nothing is decided here -- every value it needs
        // was settled in pass 1 -- so it parallelises over cells.
        TArray64<uint8> Payload;
        Payload.SetNumUninitialized(PayloadBytes);

        const int64 FloorA = 0;
        const int64 FloorB = FloorSplats * GAUSSIAN_SPLAT_PACKED_A_STRIDE;
        const int64 FloorSH = FloorB + FloorSplats * GAUSSIAN_SPLAT_PACKED_B_STRIDE;
        const int64 TailA = FloorSplats * RecordStride;
        const int64 TailB = TailA + TailSplats * GAUSSIAN_SPLAT_PACKED_A_STRIDE;
        const int64 TailSH = TailB + TailSplats * GAUSSIAN_SPLAT_PACKED_B_STRIDE;

        TArray<int64> FloorFirstSlot;
        FloorFirstSlot.SetNumUninitialized(Bake.Cells.Num());
        int64 FloorRunning = 0;
        for (int32 CellIndex = 0; CellIndex < Bake.Cells.Num(); ++CellIndex)
        {
            FloorFirstSlot[CellIndex] = FloorRunning;
            FloorRunning += Bake.Cells[CellIndex].FloorCount;
        }

        uint8* const Base = Payload.GetData();
        ParallelFor(Bake.Cells.Num(), [&](int32 CellIndex)
        {
            const FGaussianSplatPagedCell& Cell = Bake.Cells[CellIndex];
            const FVector3f Origin = Cell.BoundsMin;
            const FVector3f Extent = GaussianSplatFormat::CellExtent(Cell.BoundsMin, Cell.BoundsMax);
            const int64 First = Bake.CellFirst[CellIndex];
            TArray<float> Values;

            // Writes one baked splat into the block that owns DestSlot.
            const auto Write = [&](int64 DestSlot, int64 SourceSlot, bool bFloor)
            {
                const int32 PlyIndex = Bake.Order[SourceSlot];
                ReadRow(Layout, PlyIndex, Values);
                const FImportedGaussian G = BuildImportedGaussian(
                    Values, Layout.X, Layout.Y, Layout.Z, Layout.S0, Layout.S1, Layout.S2,
                    Layout.R0, Layout.R1, Layout.R2, Layout.R3);
                const FLinearColor Color = BuildColor(Values, Layout.Dc0, Layout.Dc1, Layout.Dc2, Layout.Opacity);

                const FUintVector4 A = GaussianSplatFormat::PackSplatA(G.Position, Origin, Extent, G.LogScale, G.Rotation);
                const uint32 B = GaussianSplatFormat::PackSplatB(
                    FVector4f(Color.R, Color.G, Color.B, Color.A), ColorEncoding);

                const int64 OffA = (bFloor ? FloorA : TailA) + DestSlot * GAUSSIAN_SPLAT_PACKED_A_STRIDE;
                const int64 OffB = (bFloor ? FloorB : TailB) + DestSlot * GAUSSIAN_SPLAT_PACKED_B_STRIDE;
                FMemory::Memcpy(Base + OffA, &A, sizeof(A));
                FMemory::Memcpy(Base + OffB, &B, sizeof(B));
                if (bHasSH)
                {
                    const uint32 SH = static_cast<uint32>(Pass1.SHIndexOfSplat[PlyIndex]);
                    const int64 OffSH = (bFloor ? FloorSH : TailSH) + DestSlot * GAUSSIAN_SPLAT_SH_INDEX_STRIDE;
                    FMemory::Memcpy(Base + OffSH, &SH, sizeof(SH));
                }
            };

            for (int32 Offset = 0; Offset < Cell.FloorCount; ++Offset)
            {
                Write(FloorFirstSlot[CellIndex] + Offset, First + Offset, /*bFloor*/ true);
            }

            const int64 TailCount = Cell.Count - Cell.FloorCount;
            const int64 TailBase = static_cast<int64>(Cell.FirstTailPage) * GAUSSIAN_SPLAT_PAGE_SPLATS;
            for (int64 Offset = 0; Offset < TailCount; ++Offset)
            {
                Write(TailBase + Offset, First + Cell.FloorCount + Offset, /*bFloor*/ false);
            }

            // A cell's last page is partial. Repeat its last splat through the
            // unused slots: a page is uploaded whole, and a slot the shader can
            // reach must never hold whatever was in the pool before.
            const int64 PaddedTo = static_cast<int64>(Cell.TailPageCount) * GAUSSIAN_SPLAT_PAGE_SPLATS;
            for (int64 Offset = TailCount; Offset < PaddedTo; ++Offset)
            {
                const int64 Source = TailCount > 0 ? First + Cell.Count - 1 : First;
                Write(TailBase + Offset, Source, /*bFloor*/ false);
            }
        });

        // Fill the asset.
        OutAsset.FormatVersion = static_cast<int32>(EGaussianSplatPagedFormat::Latest);
        OutAsset.PageSplats = GAUSSIAN_SPLAT_PAGE_SPLATS;
        OutAsset.CellSize = Pitch;
        OutAsset.MinCellOccupancy = Options.MinCellOccupancy;
        OutAsset.FloorFraction = Options.FloorFraction;
        OutAsset.ColorEncoding = ColorEncoding;
        OutAsset.SizeRef = SizeRef;
        OutAsset.SizeP99 = SizeP99;
        OutAsset.SourcePath = FilePath;
        OutAsset.SourceSplatCount = Layout.VertexCount;
        OutAsset.SourceHash = CityHash64(reinterpret_cast<const char*>(Raw.GetData()), FMath::Min<int64>(Raw.Num(), 1 << 20));
        OutAsset.Cells = MoveTemp(Bake.Cells);
        OutAsset.SHPalette = bHasSH ? MoveTemp(Pass1.SHPalette) : TArray<float>();
        OutAsset.FloorSplats = FloorSplats;
        OutAsset.TailPages = TailPages;
        OutAsset.TotalSplats = Bake.Order.Num();

        OutAsset.SetPayloadFromMemory(MoveTemp(Payload));
        OutAsset.RefreshDerivedData();

        UE_LOG(
            LogGaussianSplatPagedImport,
            Display,
            TEXT("Imported %s in %.1f s: %lld of %lld splats in %d cells at %.1f units (%lld floor, %lld pages, "
                 "%.1f MiB payload, %s, s_ref %.6f s_p99 %.6f)"),
            *FPaths::GetCleanFilename(FilePath),
            FPlatformTime::Seconds() - StartTime,
            OutAsset.TotalSplats,
            Layout.VertexCount,
            OutAsset.Cells.Num(),
            Pitch,
            FloorSplats,
            TailPages,
            PayloadBytes / (1024.0 * 1024.0),
            bHasSH ? TEXT("with SH") : TEXT("base colour only"),
            SizeRef,
            SizeP99);

        // Cells that hold only a page or two make the range table longer for no
        // gain; cells that hold hundreds make the LOD distance coarse.
        const double PagesPerCell = OutAsset.Cells.Num() > 0
            ? static_cast<double>(TailPages) / OutAsset.Cells.Num()
            : 0.0;
        if (PagesPerCell < 8.0)
        {
            UE_LOG(
                LogGaussianSplatPagedImport,
                Warning,
                TEXT("Cells hold %.1f tail pages on average, under the 8 this format is sized for. A larger CellSize "
                     "than %.1f would cut the range-table work."),
                PagesPerCell,
                Pitch);
        }
        return true;
    }

    bool DumpBakeOrder(
        const FString& FilePath,
        const FOptions& Options,
        const FString& OutCsvPath,
        FString& OutError)
    {
        TArray64<uint8> Raw;
        if (!FFileHelper::LoadFileToArray(Raw, *FilePath))
        {
            OutError = FString::Printf(TEXT("Could not read %s."), *FilePath);
            return false;
        }

        FPlyHeader Header;
        FPlyLayout Layout;
        if (!ParseHeader(Raw, Header) || !BuildLayout(Raw, Header, Layout, OutError))
        {
            return false;
        }

        FPass1 Pass1;
        RunPass1(Layout, 1.0f / FMath::Max(1.0f, Options.CellSize), Pass1);

        int32 SlotCount = 0;
        for (int64 Index = 0; Index < Layout.VertexCount; ++Index)
        {
            SlotCount = FMath::Max(SlotCount, Pass1.SlotOfSplat[Index] + 1);
        }

        FBake Bake;
        BuildBakeOrder(Pass1, Layout.VertexCount, SlotCount, FMath::Max(0, Options.MinCellOccupancy), Bake);

        // Positions, so this is comparable with a legacy asset, which permutes its
        // arrays and keeps no record of where a splat came from. %.6f is well
        // inside float precision at these magnitudes, so equal splats print equal.
        FString Text = TEXT("cell,rank,x,y,z\n");
        Text.Reserve(Bake.Order.Num() * 48);
        for (int32 CellIndex = 0; CellIndex < Bake.Cells.Num(); ++CellIndex)
        {
            const int64 First = Bake.CellFirst[CellIndex];
            for (int32 Rank = 0; Rank < Bake.Cells[CellIndex].Count; ++Rank)
            {
                const FVector3f& P = Pass1.Position[Bake.Order[First + Rank]];
                Text.Appendf(TEXT("%d,%d,%.6f,%.6f,%.6f\n"), CellIndex, Rank, P.X, P.Y, P.Z);
            }
        }

        if (!FFileHelper::SaveStringToFile(Text, *OutCsvPath))
        {
            OutError = FString::Printf(TEXT("Could not write %s."), *OutCsvPath);
            return false;
        }

        UE_LOG(
            LogGaussianSplatPagedImport,
            Display,
            TEXT("Bake order for %s: %lld splats in %d cells -> %s"),
            *FPaths::GetCleanFilename(FilePath),
            Bake.Order.Num(),
            Bake.Cells.Num(),
            *OutCsvPath);
        return true;
    }

    bool DumpLegacyBakeOrder(const UGaussianSplatAsset& Asset, const FString& OutCsvPath, FString& OutError)
    {
        if (Asset.Cells.IsEmpty())
        {
            OutError = TEXT("The legacy asset has no cells: import it and let BuildCells run first.");
            return false;
        }

        FString Text = TEXT("cell,rank,x,y,z\n");
        int64 Rows = 0;
        for (int32 CellIndex = 0; CellIndex < Asset.Cells.Num(); ++CellIndex)
        {
            const FGaussianSplatCell& Cell = Asset.Cells[CellIndex];
            for (int32 Rank = 0; Rank < Cell.Count; ++Rank)
            {
                if (!Asset.Positions.IsValidIndex(Cell.FirstIndex + Rank))
                {
                    continue;
                }
                const FVector3f& P = Asset.Positions[Cell.FirstIndex + Rank];
                Text.Appendf(TEXT("%d,%d,%.6f,%.6f,%.6f\n"), CellIndex, Rank, P.X, P.Y, P.Z);
                ++Rows;
            }
        }

        if (!FFileHelper::SaveStringToFile(Text, *OutCsvPath))
        {
            OutError = FString::Printf(TEXT("Could not write %s."), *OutCsvPath);
            return false;
        }

        UE_LOG(
            LogGaussianSplatPagedImport,
            Display,
            TEXT("Legacy bake order for %s: %lld splats in %d cells -> %s"),
            *Asset.GetName(),
            Rows,
            Asset.Cells.Num(),
            *OutCsvPath);
        return true;
    }
}
