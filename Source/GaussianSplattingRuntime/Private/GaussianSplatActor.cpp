#include "GaussianSplatActor.h"

#include "GaussianSplatComponent.h"

AGaussianSplatActor::AGaussianSplatActor()
{
    // 这个 Actor 只负责承载静态渲染对象，不需要 Tick。
    PrimaryActorTick.bCanEverTick = false;

    // 创建默认组件并直接设为 Root，Actor 的 Transform 就等同于高斯整体的 Transform。
    SplatComponent = CreateDefaultSubobject<UGaussianSplatComponent>(TEXT("GaussianSplatComponent"));
    RootComponent = SplatComponent;
}
