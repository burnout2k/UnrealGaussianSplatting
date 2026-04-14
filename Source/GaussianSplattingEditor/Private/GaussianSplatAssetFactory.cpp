#include "GaussianSplatAssetFactory.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "GaussianSplatAsset.h"
#include "Import/GaussianSplatParser.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatFactory, Log, All);

UGaussianSplatAssetFactory::UGaussianSplatAssetFactory()
{
    bEditorImport = true;
    bCreateNew = false;
    SupportedClass = UGaussianSplatAsset::StaticClass();
    Formats.Add(TEXT("ply;Gaussian Splat PLY"));
}

UObject* UGaussianSplatAssetFactory::FactoryCreateFile(
    UClass* InClass,
    UObject* InParent,
    FName InName,
    EObjectFlags Flags,
    const FString& Filename,
    const TCHAR* Parms,
    FFeedbackContext* Warn,
    bool& bOutOperationCanceled)
{
    bOutOperationCanceled = false;

    UGaussianSplatAsset* Asset = NewObject<UGaussianSplatAsset>(InParent, InClass, InName, Flags);

    FString Error;
    if (!GaussianSplatParser::ParseFromFile(Filename, *Asset, Error))
    {
        UE_LOG(LogGaussianSplatFactory, Error, TEXT("Failed to import %s: %s"), *Filename, *Error);
        bOutOperationCanceled = true;
        return nullptr;
    }

    FAssetRegistryModule::AssetCreated(Asset);
    if (Asset->GetPackage())
    {
        Asset->GetPackage()->MarkPackageDirty();
    }

    UE_LOG(LogGaussianSplatFactory, Display, TEXT("Imported %d points from %s"), Asset->GetPointCount(), *FPaths::GetCleanFilename(Filename));
    return Asset;
}
