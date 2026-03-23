#include "GaussianSplatFunctionLibrary.h"

#include "GaussianSplatComponent.h"

void UGaussianSplatFunctionLibrary::SetGaussianAsset(UGaussianSplatComponent* Component, UGaussianSplatAsset* Asset)
{
    if (Component)
    {
        Component->SetGaussianAsset(Asset);
    }
}
