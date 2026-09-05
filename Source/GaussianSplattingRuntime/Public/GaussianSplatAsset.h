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
    void RefreshDerivedData();
    const FGaussianSplatRenderResources* GetRenderResources() const;

private:
    void BuildRenderResources();
    void ReleaseRenderResources();

    TUniquePtr<FGaussianSplatRenderResources> RenderResources;
};
