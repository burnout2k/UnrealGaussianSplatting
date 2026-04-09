#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GaussianSplatFunctionLibrary.generated.h"

class UGaussianSplatAsset;
class UGaussianSplatComponent;

// BlueprintFunctionLibrary 只暴露很薄的一层蓝图 API。
// 目的不是承载逻辑，而是让蓝图可以方便地驱动 Component。
UCLASS()
class GAUSSIANSPLATTINGRUNTIME_API UGaussianSplatFunctionLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // 蓝图里直接替换 Component 当前使用的 Gaussian Asset。
    UFUNCTION(BlueprintCallable, Category = "Gaussian Splat")
    static void SetGaussianAsset(UGaussianSplatComponent* Component, UGaussianSplatAsset* Asset);
};
