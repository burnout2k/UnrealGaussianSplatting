#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GaussianSplatActor.generated.h"

class UGaussianSplatComponent;

UCLASS()
class GAUSSIANSPLATTINGRUNTIME_API AGaussianSplatActor : public AActor
{
    GENERATED_BODY()

public:
    AGaussianSplatActor();

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Gaussian Splat")
    float DefaultUniformScale = 100.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    TObjectPtr<UGaussianSplatComponent> SplatComponent;
};
