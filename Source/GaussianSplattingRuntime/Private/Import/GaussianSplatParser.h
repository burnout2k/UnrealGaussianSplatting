#pragma once

#include "CoreMinimal.h"

class UGaussianSplatAsset;

namespace GaussianSplatParser
{
    // 运行时解析入口。
    // 当前实现还是占位版本，未来如果需要运行时从文件加载高斯，可以把编辑器导入逻辑沉到这里。
    bool ParseFromFile(const FString& FilePath, UGaussianSplatAsset& OutAsset, FString& OutError);
}
