#include "GaussianSplatFunctionLibrary.h"

#include "Import/GaussianSplatPagedImporter.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatComponent.h"
#include "Import/GaussianSplatParser.h"

void UGaussianSplatFunctionLibrary::SetGaussianAsset(UGaussianSplatComponent* Component, UGaussianSplatAsset* Asset)
{
    if (Component)
    {
        Component->SetGaussianAsset(Asset);
    }
}

UGaussianSplatAsset* UGaussianSplatFunctionLibrary::LoadGaussianAssetFromFile(const FString& FilePath, FString& OutError)
{
    UGaussianSplatAsset* Asset = NewObject<UGaussianSplatAsset>(GetTransientPackage());
    if (!Asset)
    {
        OutError = TEXT("Failed to create transient Gaussian asset.");
        return nullptr;
    }

    if (!GaussianSplatParser::ParseFromFile(FilePath, *Asset, OutError))
    {
        return nullptr;
    }

    return Asset;
}

bool UGaussianSplatFunctionLibrary::DumpPagedBakeOrder(
    const FString& PlyPath,
    const FString& OutCsvPath,
    float CellSize,
    int32 MinCellOccupancy,
    FString& OutError)
{
    GaussianSplatPagedImporter::FOptions Options;
    Options.CellSize = CellSize;
    Options.MinCellOccupancy = MinCellOccupancy;
    return GaussianSplatPagedImporter::DumpBakeOrder(PlyPath, Options, OutCsvPath, OutError);
}

bool UGaussianSplatFunctionLibrary::DumpLegacyBakeOrder(
    UGaussianSplatAsset* Asset,
    const FString& OutCsvPath,
    FString& OutError)
{
    if (Asset == nullptr)
    {
        OutError = TEXT("No asset.");
        return false;
    }
    return GaussianSplatPagedImporter::DumpLegacyBakeOrder(*Asset, OutCsvPath, OutError);
}
