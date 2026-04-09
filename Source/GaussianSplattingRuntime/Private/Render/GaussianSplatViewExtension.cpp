#include "Render/GaussianSplatViewExtension.h"

#include "GaussianSplatAsset.h"
#include "GaussianSplatComponent.h"
#include "GaussianSplatWorldSubsystem.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "Render/GaussianSplatPasses.h"
#include "Render/GaussianSplatRenderResources.h"
#include "ScreenPass.h"

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatViewExtension, Log, All);

FGaussianSplatViewExtension::FGaussianSplatViewExtension(const FAutoRegister& AutoRegister)
    : FSceneViewExtensionBase(AutoRegister)
{
}

bool FGaussianSplatViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
    return true;
}

void FGaussianSplatViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
}

void FGaussianSplatViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
}

void FGaussianSplatViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
    const UWorld* ViewFamilyWorld = InViewFamily.Scene ? InViewFamily.Scene->GetWorld() : nullptr;
    BuildPointSnapshot_GameThread(ViewFamilyWorld);
}

void FGaussianSplatViewExtension::PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
}

void FGaussianSplatViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
    if (PassId == EPostProcessingPass::Tonemap)
    {
        InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FGaussianSplatViewExtension::PostProcessPass_RenderThread));
    }
}

FScreenPassTexture FGaussianSplatViewExtension::PostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
{
    TArray<FGaussianSplatRenderBatch> LocalPoints;
    {
        FReadScopeLock Lock(CachedPointsLock);
        LocalPoints = CachedPoints;
    }

    if (LocalPoints.IsEmpty())
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
    if (!SceneColor.IsValid())
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    FScreenPassRenderTarget Output = Inputs.OverrideOutput;
    if (!Output.IsValid())
    {
        Output = FScreenPassRenderTarget::CreateFromInput(GraphBuilder, SceneColor, View.GetOverwriteLoadAction(), TEXT("GaussianSplat.PostProcessOutput"));
    }

    return GaussianSplatPasses::AddPostProcessPass(GraphBuilder, View, SceneColor, Output, LocalPoints);
}

void FGaussianSplatViewExtension::BuildPointSnapshot_GameThread(const UWorld* TargetWorld)
{
    TArray<FGaussianSplatRenderBatch> NewPoints;
    NewPoints.Reserve(64);

    if (TargetWorld == nullptr)
    {
        FWriteScopeLock Lock(CachedPointsLock);
        CachedPoints = MoveTemp(NewPoints);
        return;
    }

    UGaussianSplatWorldSubsystem* WorldSubsystem = TargetWorld->GetSubsystem<UGaussianSplatWorldSubsystem>();
    if (WorldSubsystem == nullptr)
    {
        FWriteScopeLock Lock(CachedPointsLock);
        CachedPoints = MoveTemp(NewPoints);
        return;
    }

    TArray<UGaussianSplatComponent*> RegisteredComponents;
    WorldSubsystem->GetRegisteredComponents(RegisteredComponents);

    for (UGaussianSplatComponent* Component : RegisteredComponents)
    {
        if (!IsValid(Component) || !Component->IsVisible())
        {
            continue;
        }

        const UGaussianSplatAsset* Asset = Component->Asset;
        if (!IsValid(Asset))
        {
            continue;
        }

        const FGaussianSplatRenderResources* RenderResources = Asset->GetRenderResources();
        if (Asset->Positions.IsEmpty() || RenderResources == nullptr || RenderResources->GetPointCount() == 0)
        {
            continue;
        }

        const float Density = FMath::Clamp(Component->DensityScale, 0.001f, 1.0f);
        const int32 Stride = FMath::Max(1, FMath::RoundToInt(1.0f / Density));
        const int32 LocalMax = FMath::Max(1, Component->MaxRenderPoints);
        const FTransform LocalToWorld = Component->GetComponentTransform();

        const FMatrix ComponentToWorldNoScale = FRotationMatrix::Make(LocalToWorld.GetRotation());
        const FMatrix WorldToComponentNoScale = ComponentToWorldNoScale.GetTransposed();

        FGaussianSplatRenderBatch Batch;
        Batch.Resources = RenderResources;
        Batch.RenderMode = Component->PreviewRenderMode == EGaussianPreviewRenderMode::Points
            ? EGaussianSplatRenderMode::Points
            : EGaussianSplatRenderMode::Billboards;
        Batch.LocalToWorld = FMatrix44f(LocalToWorld.ToMatrixWithScale());
        Batch.WorldToLocalRow0 = FVector4f(
            static_cast<float>(WorldToComponentNoScale.M[0][0]),
            static_cast<float>(WorldToComponentNoScale.M[0][1]),
            static_cast<float>(WorldToComponentNoScale.M[0][2]),
            0.0f);
        Batch.WorldToLocalRow1 = FVector4f(
            static_cast<float>(WorldToComponentNoScale.M[1][0]),
            static_cast<float>(WorldToComponentNoScale.M[1][1]),
            static_cast<float>(WorldToComponentNoScale.M[1][2]),
            0.0f);
        Batch.WorldToLocalRow2 = FVector4f(
            static_cast<float>(WorldToComponentNoScale.M[2][0]),
            static_cast<float>(WorldToComponentNoScale.M[2][1]),
            static_cast<float>(WorldToComponentNoScale.M[2][2]),
            0.0f);
        Batch.PointSize = FMath::Clamp(Component->PointSize, 0.1f, 32.0f);
        Batch.OpacityScale = FMath::Clamp(Component->OpacityScale, 0.0f, 8.0f);
        Batch.AssetPointCount = static_cast<uint32>(Asset->GetPointCount());
        Batch.Stride = static_cast<uint32>(Stride);
        Batch.MaxRenderPoints = static_cast<uint32>(LocalMax);
        NewPoints.Add(Batch);
    }

    {
        FWriteScopeLock Lock(CachedPointsLock);
        CachedPoints = MoveTemp(NewPoints);
    }
}
