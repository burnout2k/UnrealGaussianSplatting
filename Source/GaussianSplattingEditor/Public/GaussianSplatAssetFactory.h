#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "GaussianSplatAssetFactory.generated.h"

UCLASS()
class GAUSSIANSPLATTINGEDITOR_API UGaussianSplatAssetFactory : public UFactory
{
    GENERATED_BODY()

public:
    UGaussianSplatAssetFactory();

    virtual UObject* FactoryCreateFile(
        UClass* InClass,
        UObject* InParent,
        FName InName,
        EObjectFlags Flags,
        const FString& Filename,
        const TCHAR* Parms,
        FFeedbackContext* Warn,
        bool& bOutOperationCanceled) override;
};
