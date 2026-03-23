#pragma once

#include "CoreMinimal.h"

class UGaussianSplatAsset;

namespace GaussianSplatParser
{
    bool ParseFromFile(const FString& FilePath, UGaussianSplatAsset& OutAsset, FString& OutError);
}
