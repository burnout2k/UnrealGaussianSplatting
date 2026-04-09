#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "GaussianSplatAssetFactory.generated.h"

// 编辑器导入工厂：
// 负责把磁盘上的 PLY 解析成 UGaussianSplatAsset。
// 当前插件的大部分“文件格式理解”都在这里，而不是运行时 Parser。
UCLASS()
class GAUSSIANSPLATTINGEDITOR_API UGaussianSplatAssetFactory : public UFactory
{
    GENERATED_BODY()

public:
    UGaussianSplatAssetFactory();

    // UE 在用户导入文件时会调用这个入口。
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
