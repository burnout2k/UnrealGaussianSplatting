#include "GaussianSplatActor.h"

#include "GaussianSplatComponent.h"

AGaussianSplatActor::AGaussianSplatActor()
{
    PrimaryActorTick.bCanEverTick = false;

    SplatComponent = CreateDefaultSubobject<UGaussianSplatComponent>(TEXT("GaussianSplatComponent"));
    RootComponent = SplatComponent;

    // Default new actors to a meter-to-centimeter friendly scale.
    SetActorScale3D(FVector(DefaultUniformScale));
}
