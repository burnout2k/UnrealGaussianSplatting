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

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    TObjectPtr<UGaussianSplatComponent> SplatComponent;
};
