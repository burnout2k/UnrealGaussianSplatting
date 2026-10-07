#pragma once

#include "CoreMinimal.h"

class UGaussianSplatAsset;
class UGaussianSplatPagedAsset;

namespace GaussianSplatPagedImporter
{
    struct FOptions
    {
        // 16 m for street captures, 64 m for drone ones. The cell is what the LOD
        // selection reasons about, so it wants to be about the distance over which
        // detail should change -- not about how many splats fit in it.
        float CellSize = 16.0f;

        // Cells thinner than this are reconstruction noise, not surfaces. Unlike
        // the legacy importer, their splats are not written at all.
        int32 MinCellOccupancy = 100;

        // The always-resident prefix of each cell, R_c = max(1, round(f * count)).
        // 0.02 matches the selection's LodMinFraction default, which the paged
        // path then clamps to this baked value.
        float FloorFraction = 0.02f;

        // Fix 5 Step 2 (review M1, m3): bake the capture WITHOUT its spherical
        // harmonics even though the PLY carries them. This exists for one
        // measurement -- Uno against Uno-nosh at the same spot, identical geometry
        // and identical selection, the SH read the only difference -- which is what
        // replaces the cross-scene residual the Step 2 plan used to price SH3.
        // It also drops the palette, so the asset is ~380 MiB smaller.
        bool bStripSH = false;

        // Log a line per cell-size candidate instead of importing. Used to pick a
        // cell size for a new capture.
        bool bSurveyOnly = false;
    };

    // Bakes a PLY into packed pages. Binary little-endian only: pass 2 gathers
    // rows in sorted order, which needs a fixed row stride to seek by. Convert an
    // ASCII capture first (splat-transform does it).
    GAUSSIANSPLATTINGRUNTIME_API bool ImportFromFile(
        const FString& FilePath,
        const FOptions& Options,
        UGaussianSplatPagedAsset& OutAsset,
        FString& OutError);

    // The per-cell bake order, for the identity check of Step 1 specification item 1.
    //
    // Each row is "cell,rank,x,y,z" -- POSITIONS, not PLY indices, because the
    // legacy asset permutes its arrays at build time and throws the original
    // indices away, so positions are the only thing the two bakes can be compared
    // on. If the two files are identical then every cell holds the same splats in
    // the same order, and G1a's "within noise" means what it says; if they are
    // not, a prefix of a cell is a DIFFERENT set of splats and no picture
    // comparison can tell that apart from a bug.
    GAUSSIANSPLATTINGRUNTIME_API bool DumpBakeOrder(
        const FString& FilePath,
        const FOptions& Options,
        const FString& OutCsvPath,
        FString& OutError);

    // The same dump taken from a legacy asset that has already been imported and
    // had its cells built. Diff the two files.
    GAUSSIANSPLATTINGRUNTIME_API bool DumpLegacyBakeOrder(
        const UGaussianSplatAsset& Asset,
        const FString& OutCsvPath,
        FString& OutError);

    // The identity check the order dump does NOT make: the same splats in the same
    // order can still be PACKED differently, and a splat whose 16-bit position or
    // fp16 scale lands one step away can fail the cull that its twin passes.
    //
    // Re-packs every splat of the legacy asset through the shared format functions,
    // using the legacy asset's own cell bounds and colour range, and compares the
    // bytes with the paged asset's payload. Reports the first MaxReports
    // mismatches with the cell, the rank and both values, so the answer is a splat
    // rather than a suspicion.
    GAUSSIANSPLATTINGRUNTIME_API bool ComparePackedRecords(
        const UGaussianSplatAsset& Legacy,
        const UGaussianSplatPagedAsset& Paged,
        int32 MaxReports,
        FString& OutReport);
}
