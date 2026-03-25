#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "GaussianSplatComponent.generated.h"

class UGaussianSplatAsset;
class UTexture2D;
struct FPropertyChangedEvent;

UENUM(BlueprintType)
enum class EGaussianPreviewRenderMode : uint8
{
    Points UMETA(DisplayName = "Points"),
    Billboards UMETA(DisplayName = "Gaussian Billboards"),
    Boxes UMETA(DisplayName = "Gaussian Boxes"),
};

UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class GAUSSIANSPLATTINGRUNTIME_API UGaussianSplatComponent : public UPrimitiveComponent
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    TObjectPtr<UGaussianSplatAsset> Asset;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat", meta = (ClampMin = "0.0"))
    float DensityScale = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat", meta = (ClampMin = "0.0"))
    float OpacityScale = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat|Preview", meta = (ClampMin = "0.1", ClampMax = "32.0"))
    float PointSize = 2.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat|Preview")
    bool bDepthSort = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat|Preview")
    bool bFrustumCull = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat|Preview", meta = (ClampMin = "1000", ClampMax = "2000000"))
    int32 MaxRenderPoints = 250000;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat|Preview")
    EGaussianPreviewRenderMode PreviewRenderMode = EGaussianPreviewRenderMode::Billboards;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat|Preview")
    TObjectPtr<UTexture2D> GaussianFalloffTexture;

    virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
    virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
    virtual void OnRegister() override;
    virtual void OnUnregister() override;
    virtual void SendRenderDynamicData_Concurrent() override;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

    UFUNCTION(BlueprintCallable, Category = "Gaussian Splat")
    void SetGaussianAsset(UGaussianSplatAsset* InAsset);

    UFUNCTION(BlueprintCallable, Category = "Gaussian Splat|Preview")
    void BuildDefaultGaussianFalloffTexture();

private:
    void EnsureGaussianFalloffTexture();
};
