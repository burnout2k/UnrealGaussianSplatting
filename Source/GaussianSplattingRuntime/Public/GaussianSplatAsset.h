#pragma once

#include "CoreMinimal.h"
#include "GaussianSplatAsset.generated.h"

class FGaussianSplatRenderResources;

UCLASS(BlueprintType)
class GAUSSIANSPLATTINGRUNTIME_API UGaussianSplatAsset : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    TArray<FVector3f> Positions;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    TArray<FQuat4f> Rotations;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    TArray<FVector3f> Scales;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    TArray<FVector4f> ColorsOpacity;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    TArray<float> SHCoefficients;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    FBoxSphereBounds Bounds;

    virtual void Serialize(FArchive& Ar) override;
    virtual void PostLoad() override;
    virtual void BeginDestroy() override;
    virtual void GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize) override;

    int32 GetPointCount() const;
    void RebuildBounds();
    const FGaussianSplatRenderResources* GetRenderResources() const;

private:
    void BuildRenderResources();
    void ReleaseRenderResources();

    TUniquePtr<FGaussianSplatRenderResources> RenderResources;
};
