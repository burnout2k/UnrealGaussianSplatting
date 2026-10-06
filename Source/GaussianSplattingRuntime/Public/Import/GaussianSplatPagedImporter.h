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
}
