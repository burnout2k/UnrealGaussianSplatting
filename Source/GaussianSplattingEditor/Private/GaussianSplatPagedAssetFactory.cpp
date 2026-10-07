#include "GaussianSplatPagedAssetFactory.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "GaussianSplatPagedAsset.h"
#include "Import/GaussianSplatPagedImporter.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatPagedFactory, Log, All);

namespace
{
    GaussianSplatPagedImporter::FOptions MakeOptions(const UGaussianSplatPagedAssetFactory& Factory)
    {
        GaussianSplatPagedImporter::FOptions Options;
        Options.CellSize = Factory.CellSize;
        Options.MinCellOccupancy = Factory.MinCellOccupancy;
        Options.FloorFraction = Factory.FloorFraction;
        Options.bStripSH = Factory.bStripSH;
        return Options;
    }
}

UGaussianSplatPagedAssetFactory::UGaussianSplatPagedAssetFactory()
{
    bEditorImport = true;
    bCreateNew = false;
    SupportedClass = UGaussianSplatPagedAsset::StaticClass();
    Formats.Add(TEXT("ply;Gaussian Splat PLY (paged)"));

    // Below the legacy factory, which also claims .ply: a double-click on a PLY
    // should still make the asset the rest of the plugin understands until Step 5
    // flips the default.
    ImportPriority = DefaultImportPriority - 1;
}

UObject* UGaussianSplatPagedAssetFactory::FactoryCreateFile(
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

    UGaussianSplatPagedAsset* Asset = NewObject<UGaussianSplatPagedAsset>(InParent, InClass, InName, Flags);

    FString Error;
    if (!GaussianSplatPagedImporter::ImportFromFile(Filename, MakeOptions(*this), *Asset, Error))
    {
        UE_LOG(LogGaussianSplatPagedFactory, Error, TEXT("Failed to import %s: %s"), *Filename, *Error);
        bOutOperationCanceled = true;
        return nullptr;
    }

    FAssetRegistryModule::AssetCreated(Asset);
    if (Asset->GetPackage())
    {
        Asset->GetPackage()->MarkPackageDirty();
    }

    UE_LOG(
        LogGaussianSplatPagedFactory,
        Display,
        TEXT("Imported %lld splats in %d cells from %s"),
        Asset->TotalSplats,
        Asset->Cells.Num(),
        *FPaths::GetCleanFilename(Filename));
    return Asset;
}

bool UGaussianSplatPagedAssetFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
    const UGaussianSplatPagedAsset* Asset = Cast<UGaussianSplatPagedAsset>(Obj);
    if (Asset == nullptr || Asset->SourcePath.IsEmpty())
    {
        return false;
    }
    OutFilenames.Add(Asset->SourcePath);
    return true;
}

void UGaussianSplatPagedAssetFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
    if (UGaussianSplatPagedAsset* Asset = Cast<UGaussianSplatPagedAsset>(Obj))
    {
        if (NewReimportPaths.Num() > 0)
        {
            Asset->SourcePath = NewReimportPaths[0];
        }
    }
}

EReimportResult::Type UGaussianSplatPagedAssetFactory::Reimport(UObject* Obj)
{
    UGaussianSplatPagedAsset* Asset = Cast<UGaussianSplatPagedAsset>(Obj);
    if (Asset == nullptr || Asset->SourcePath.IsEmpty())
    {
        return EReimportResult::Failed;
    }

    // The asset's own bake settings win over the factory's: re-importing must
    // reproduce the bake, not quietly adopt whatever the factory was last set to.
    GaussianSplatPagedImporter::FOptions Options;
    Options.CellSize = Asset->CellSize;
    Options.MinCellOccupancy = Asset->MinCellOccupancy;
    Options.FloorFraction = Asset->FloorFraction;
    Options.bStripSH = Asset->bStrippedSH;

    FString Error;
    if (!GaussianSplatPagedImporter::ImportFromFile(Asset->SourcePath, Options, *Asset, Error))
    {
        UE_LOG(
            LogGaussianSplatPagedFactory,
            Error,
            TEXT("Failed to re-import %s from %s: %s"),
            *Asset->GetName(),
            *Asset->SourcePath,
            *Error);
        return EReimportResult::Failed;
    }

    Asset->MarkPackageDirty();
    return EReimportResult::Succeeded;
}

int32 UGaussianSplatPagedAssetFactory::GetPriority() const
{
    return ImportPriority;
}
