#include "GaussianSplatComponent.h"

#include "GaussianSplatAsset.h"
#include "GaussianSplatWorldSubsystem.h"

void UGaussianSplatComponent::SyncWorldSubsystemRegistration()
{
    UWorld* World = GetWorld();
    if (World == nullptr)
    {
        return;
    }

    UGaussianSplatWorldSubsystem* WorldSubsystem = World->GetSubsystem<UGaussianSplatWorldSubsystem>();
    if (WorldSubsystem == nullptr)
    {
        return;
    }

    if (IsRegistered() && PreviewRenderMode != EGaussianPreviewRenderMode::Boxes)
    {
        WorldSubsystem->RegisterComponent(this);
        return;
    }

    WorldSubsystem->UnregisterComponent(this);
}

FPrimitiveSceneProxy* UGaussianSplatComponent::CreateSceneProxy()
{
    return nullptr;
}

FBoxSphereBounds UGaussianSplatComponent::CalcBounds(const FTransform& LocalToWorld) const
{
    if (!Asset)
    {
        return FBoxSphereBounds(EForceInit::ForceInitToZero);
    }

    FBoxSphereBounds PrimitiveBounds = Asset->Bounds.TransformBy(LocalToWorld);
    if (PreviewRenderMode == EGaussianPreviewRenderMode::Points)
    {
        PrimitiveBounds = PrimitiveBounds.ExpandBy(FMath::Max(1.0f, PointSize));
    }

    return PrimitiveBounds;
}

void UGaussianSplatComponent::OnRegister()
{
    Super::OnRegister();

    SyncWorldSubsystemRegistration();
    MarkRenderStateDirty();
}

void UGaussianSplatComponent::OnUnregister()
{
    SyncWorldSubsystemRegistration();
    Super::OnUnregister();
}

void UGaussianSplatComponent::SendRenderDynamicData_Concurrent()
{
    Super::SendRenderDynamicData_Concurrent();
}

void UGaussianSplatComponent::SetGaussianAsset(UGaussianSplatAsset* InAsset)
{
    Asset = InAsset;
    MarkRenderStateDirty();
}

void UGaussianSplatComponent::SetPreviewRenderMode(EGaussianPreviewRenderMode InPreviewRenderMode)
{
    if (PreviewRenderMode == InPreviewRenderMode)
    {
        return;
    }

    PreviewRenderMode = InPreviewRenderMode;
    SyncWorldSubsystemRegistration();
    MarkRenderStateDirty();
}

#if WITH_EDITOR
void UGaussianSplatComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    static const FName PreviewRenderModeName = GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, PreviewRenderMode);
    if (PropertyChangedEvent.GetPropertyName() == PreviewRenderModeName)
    {
        SyncWorldSubsystemRegistration();
    }

    MarkRenderStateDirty();
}
#endif
