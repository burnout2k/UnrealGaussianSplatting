#pragma once

#include "CoreMinimal.h"
#include "GaussianSplatAsset.generated.h"

class FGaussianSplatRenderResources;

USTRUCT(BlueprintType)
struct FGaussianCovariance3f
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    float XX = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    float XY = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    float XZ = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    float YY = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    float YZ = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    float ZZ = 0.0f;

    FGaussianCovariance3f() = default;

    FGaussianCovariance3f(float InXX, float InXY, float InXZ, float InYY, float InYZ, float InZZ)
        : XX(InXX)
        , XY(InXY)
        , XZ(InXZ)
        , YY(InYY)
        , YZ(InYZ)
        , ZZ(InZZ)
    {
    }

    friend FArchive& operator<<(FArchive& Ar, FGaussianCovariance3f& Value)
    {
        Ar << Value.XX;
        Ar << Value.XY;
        Ar << Value.XZ;
        Ar << Value.YY;
        Ar << Value.YZ;
        Ar << Value.ZZ;
        return Ar;
    }
};

// One spatial cell of the capture. Splats are reordered at build time so a
// cell's splats are contiguous AND sorted by descending visual importance, which
// makes "level of detail" a COUNT: drawing a cell's first N splats draws the N
// that matter most, with no second copy of the data anywhere.
USTRUCT()
struct FGaussianSplatCell
{
    GENERATED_BODY()

    // Shrink-wrapped to the splats actually inside, NOT the grid slot. A cell
    // holding six floaters then presents a one-metre target to the frustum test
    // instead of a 64 m one, and its camera distance is honest.
    UPROPERTY() FVector3f BoundsMin = FVector3f::ZeroVector;
    UPROPERTY() FVector3f BoundsMax = FVector3f::ZeroVector;

    UPROPERTY() int32 FirstIndex = 0;
    UPROPERTY() int32 Count = 0;

    friend FArchive& operator<<(FArchive& Ar, FGaussianSplatCell& V)
    {
        Ar << V.BoundsMin;
        Ar << V.BoundsMax;
        Ar << V.FirstIndex;
        Ar << V.Count;
        return Ar;
    }
};

// Serialization version for this plugin's assets. Without it, adding Cells to
// Serialize() would make every existing .uasset unreadable -- the loader would
// run off the end of the file. Old assets report BeforeCustomVersionWasAdded,
// skip the Cells read, and get their cells built on load instead.
struct GAUSSIANSPLATTINGRUNTIME_API FGaussianSplatCustomVersion
{
    enum Type
    {
        BeforeCustomVersionWasAdded = 0,
        SpatialCells,

        VersionPlusOne,
        LatestVersion = VersionPlusOne - 1
    };

    static const FGuid GUID;
};

UCLASS(BlueprintType)
class GAUSSIANSPLATTINGRUNTIME_API UGaussianSplatAsset : public UObject
{
    GENERATED_BODY()

public:
    // These arrays are serialized explicitly in Serialize(). Keeping them out of
    // UPROPERTY avoids writing every splat twice and prevents the Details panel
    // from trying to build millions of editable array rows.
    TArray<FVector3f> Positions;

    TArray<FGaussianCovariance3f> Covariances;

    TArray<FVector4f> ColorsOpacity;

    TArray<float> SHCoefficients;

    // The complete source data remains in the asset. Only an evenly sampled
    // subset is uploaded to the GPU so large captures fit alongside CARLA on
    // memory-constrained GPUs.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat", meta = (ClampMin = "1000"))
    int32 MaxGpuPointCount = 1000000;

    // Spatial cells, sorted by (cell, descending importance). Empty means the
    // asset predates cells; they are built on load and persist once resaved.
    TArray<FGaussianSplatCell> Cells;

    // Grid pitch in asset units (metres for a COLMAP/3DGS capture). Measured on
    // an 85.8M-splat, 2.4 km capture: 256 m puts 4M splats in one cell, which is
    // far too coarse to LOD as a unit; 64 m gives ~2,900 cells with the largest
    // at 537 K. Smaller also means finer position quantization later.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat|Cells", meta = (ClampMin = "1.0"))
    float CellSize = 64.0f;

    // Cells holding fewer splats than this are discarded at build time. Nothing
    // spread that thinly across a whole cell is a surface -- it is reconstruction
    // noise, the floaters seen hanging in the air and buried under the ground.
    // On the 85.8M capture at 64 m this drops 1,891 of 2,890 cells while losing
    // 15,073 splats (0.018%), so per-frame cell work falls by two thirds and the
    // floaters go with it. Set to 0 to keep everything, then rebuild.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat|Cells", meta = (ClampMin = "0"))
    int32 MinCellOccupancy = 100;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    FBoxSphereBounds Bounds;

    virtual void Serialize(FArchive& Ar) override;
    virtual void PostLoad() override;
    virtual void BeginDestroy() override;
    virtual void GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize) override;
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

    int32 GetPointCount() const;
    void RebuildBounds();

    // Bins splats into CellSize cells, drops cells under MinCellOccupancy, and
    // reorders every per-splat array so each cell is contiguous and internally
    // sorted by descending importance. Returns true if the arrays moved.
    bool BuildCells();

    void RefreshDerivedData();
    const FGaussianSplatRenderResources* GetRenderResources() const;

private:
    void BuildRenderResources();
    void ReleaseRenderResources();

    TUniquePtr<FGaussianSplatRenderResources> RenderResources;
};
