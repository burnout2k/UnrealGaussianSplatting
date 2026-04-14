#include "GaussianSplatFunctionLibrary.h"

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
