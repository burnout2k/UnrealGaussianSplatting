#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GaussianSplatActor.generated.h"

class UGaussianSplatComponent;

// 一个最薄的 Actor 封装：
// 让关卡里可以像放普通 Actor 一样放置 GaussianSplatComponent，
// 对不熟悉 UE 组件体系的人来说会更容易上手。
UCLASS()
class GAUSSIANSPLATTINGRUNTIME_API AGaussianSplatActor : public AActor
{
    GENERATED_BODY()

public:
    AGaussianSplatActor();

    // 运行时所有真正的渲染逻辑都在这个 Component 上。
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    TObjectPtr<UGaussianSplatComponent> SplatComponent;
};
