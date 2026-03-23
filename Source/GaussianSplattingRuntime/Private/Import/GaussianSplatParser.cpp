#include "Import/GaussianSplatParser.h"

#include "GaussianSplatAsset.h"
#include "Misc/FileHelper.h"

namespace GaussianSplatParser
{
    bool ParseFromFile(const FString& FilePath, UGaussianSplatAsset& OutAsset, FString& OutError)
    {
        TArray<uint8> RawData;
        if (!FFileHelper::LoadFileToArray(RawData, *FilePath))
        {
            OutError = TEXT("Failed to read source file.");
            return false;
        }

        OutAsset.Positions.Reset();
        OutAsset.Rotations.Reset();
        OutAsset.Scales.Reset();
        OutAsset.ColorsOpacity.Reset();
        OutAsset.SHCoefficients.Reset();
        OutAsset.RebuildBounds();
        return true;
    }
}
