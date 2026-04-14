#pragma once

#include "CoreMinimal.h"

class UGaussianSplatAsset;

namespace GaussianSplatParser
{
    GAUSSIANSPLATTINGRUNTIME_API bool ParseFromFile(const FString& FilePath, UGaussianSplatAsset& OutAsset, FString& OutError);
    GAUSSIANSPLATTINGRUNTIME_API bool ParseFromBytes(const TArray<uint8>& RawData, UGaussianSplatAsset& OutAsset, FString& OutError);
}
