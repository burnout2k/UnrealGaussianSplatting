#include "GaussianSplatWorldSubsystem.h"

#include "GaussianSplatComponent.h"

void UGaussianSplatWorldSubsystem::RegisterComponent(UGaussianSplatComponent* Component)
{
    // 这个 subsystem 只维护“当前 World 里需要走 billboard 后处理路径的组件”。
    // 因此即使外部误调用注册，这里也会再守一层，只接受合法的 billboard 组件。
    if (!IsValid(Component))
    {
        return;
    }

    RegisteredComponents.Add(Component);
}

void UGaussianSplatWorldSubsystem::UnregisterComponent(UGaussianSplatComponent* Component)
{
    // 反注册比注册更宽松一些：只要指针非空，就尝试从集合里移除。
    // 这样即使组件正在销毁或状态不完整，也不影响清理。
    if (!Component)
    {
        return;
    }

    RegisteredComponents.Remove(Component);
}

void UGaussianSplatWorldSubsystem::GetRegisteredComponents(TArray<UGaussianSplatComponent*>& OutComponents)
{
    // RegisteredComponents 存的是弱引用。
    // 这里一边遍历一边顺手清掉已经失效的组件，避免注册表越来越脏。
    for (TSet<TWeakObjectPtr<UGaussianSplatComponent>>::TIterator It(RegisteredComponents); It; ++It)
    {
        UGaussianSplatComponent* Component = It->Get();
        if (!IsValid(Component))
        {
            It.RemoveCurrent();
            continue;
        }

        OutComponents.Add(Component);
    }
}
