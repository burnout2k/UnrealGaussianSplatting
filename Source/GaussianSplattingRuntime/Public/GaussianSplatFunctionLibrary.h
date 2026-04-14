#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GaussianSplatFunctionLibrary.generated.h"

class UGaussianSplatAsset;
class UGaussianSplatComponent;
class AGaussianSplatActor;
class APlayerController;

UCLASS()
class GAUSSIANSPLATTINGRUNTIME_API UGaussianSplatFunctionLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "Gaussian Splat")
    static void SetGaussianAsset(UGaussianSplatComponent* Component, UGaussianSplatAsset* Asset);

    UFUNCTION(BlueprintCallable, Category = "Gaussian Splat")
    static UGaussianSplatAsset* LoadGaussianAssetFromFile(const FString& FilePath, FString& OutError);
};
