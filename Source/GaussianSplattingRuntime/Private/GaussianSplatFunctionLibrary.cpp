#include "GaussianSplatFunctionLibrary.h"

#include "GaussianSplatComponent.h"

void UGaussianSplatFunctionLibrary::SetGaussianAsset(UGaussianSplatComponent* Component, UGaussianSplatAsset* Asset)
{
    // 保持蓝图层接口尽量薄，把真正的状态刷新逻辑留给 Component::SetGaussianAsset。
    if (Component)
    {
        Component->SetGaussianAsset(Asset);
    }
}
