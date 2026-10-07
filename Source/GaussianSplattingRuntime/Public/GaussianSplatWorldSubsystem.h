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
    // Fix 5 Step 2: the settle gate's tick function is registered and torn down
    // with the world, so it cannot outlive the level it ticks on.
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    void RegisterComponent(UGaussianSplatComponent* Component);
    void UnregisterComponent(UGaussianSplatComponent* Component);
    void GetRegisteredComponents(TArray<UGaussianSplatComponent*>& OutComponents);

private:
    TSet<TWeakObjectPtr<UGaussianSplatComponent>> RegisteredComponents;
};
