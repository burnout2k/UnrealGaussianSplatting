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

    // Fix 5's identity check (Step 1 specification item 1), callable from an editor
    // Python script. Dump a PLY's paged bake order and the same capture's legacy
    // order, then diff the two files: they must be identical, because a prefix of
    // a cell in a different order is a different set of splats, and no picture
    // comparison can tell that apart from a bug.
    UFUNCTION(BlueprintCallable, Category = "Gaussian Splat|Fix 5")
    static bool DumpPagedBakeOrder(
        const FString& PlyPath,
        const FString& OutCsvPath,
        float CellSize,
        int32 MinCellOccupancy,
        FString& OutError);

    UFUNCTION(BlueprintCallable, Category = "Gaussian Splat|Fix 5")
    static bool DumpLegacyBakeOrder(UGaussianSplatAsset* Asset, const FString& OutCsvPath, FString& OutError);
};
