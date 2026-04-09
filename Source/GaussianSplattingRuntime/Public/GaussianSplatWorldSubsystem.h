#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "GaussianSplatWorldSubsystem.generated.h"

class UGaussianSplatComponent;

UCLASS()
class GAUSSIANSPLATTINGRUNTIME_API UGaussianSplatWorldSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    void RegisterComponent(UGaussianSplatComponent* Component);
    void UnregisterComponent(UGaussianSplatComponent* Component);
    void GetRegisteredComponents(TArray<UGaussianSplatComponent*>& OutComponents);

private:
    TSet<TWeakObjectPtr<UGaussianSplatComponent>> RegisteredComponents;
};
