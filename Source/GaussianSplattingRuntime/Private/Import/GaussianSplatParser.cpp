#include "Import/GaussianSplatParser.h"

#include "GaussianSplatAsset.h"
#include "Misc/FileHelper.h"

namespace GaussianSplatParser
{
    bool ParseFromFile(const FString& FilePath, UGaussianSplatAsset& OutAsset, FString& OutError)
    {
        // 这个解析器目前只是一个最薄的占位实现：
        // 能读取源文件并清空 Asset，但真正完整的 PLY 解析逻辑已经放在编辑器导入工厂里。
        // 如果以后要支持运行时加载文件，可以把 Factory 里的解析逻辑下沉到这里复用。
        TArray<uint8> RawData;
        if (!FFileHelper::LoadFileToArray(RawData, *FilePath))
        {
            OutError = TEXT("Failed to read source file.");
            return false;
        }

        // 当前仅做“重置数据”的占位行为。
        OutAsset.Positions.Reset();
        OutAsset.Rotations.Reset();
        OutAsset.Scales.Reset();
        OutAsset.ColorsOpacity.Reset();
        OutAsset.SHCoefficients.Reset();
        OutAsset.RebuildBounds();
        return true;
    }
}
