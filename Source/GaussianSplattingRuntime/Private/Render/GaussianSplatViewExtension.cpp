#include "Render/GaussianSplatViewExtension.h"

#include "GaussianSplatPagedAsset.h"
#include "Render/GaussianSplatPagePool.h"

#include "GaussianSplatAsset.h"
#include "GaussianSplatComponent.h"
#include "Algo/Reverse.h"
#include "GaussianSplatWorldSubsystem.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "Render/GaussianSplatPasses.h"
#include "Render/GaussianSplatRenderResources.h"
#include "ScreenPass.h"

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatViewExtension, Log, All);

// Fix 5 Step 3 gate G3b, oracle 1. Reverses the order the view's splat components are
// snapshotted in, which is the order they are blended in.
//
// It is a test of the thing Step 3 fixes, not a feature. The blend is an under-blend,
// so a later batch composites BEHIND an earlier one: with a sort per batch the two
// orders give different pictures wherever two districts interleave in depth, and that
// difference IS critic C10. Once one sort covers them both, the order a component was
// registered in stops meaning anything and the two pictures must agree within the
// sort-tie noise floor.
static TAutoConsoleVariable<int32> CVarReverseBatchOrder(
    TEXT("r.GaussianSplat.ReverseBatchOrder"),
    0,
    TEXT("Gate G3b: 1 = snapshot the view's splat components in reverse registration order. With a sort per batch ")
    TEXT("this changes the picture where districts overlap (that is the bug); with one sort it must not."),
    ECVF_RenderThreadSafe);

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
    BuildPointSnapshot_GameThread(ViewFamilyWorld, InViewFamily);
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
    // The batches snapshotted for THIS family (review M2). Absent means no splat
    // component was registered when it was built, or this family never went through
    // BeginRenderViewFamily -- either way there is nothing of ours to draw.
    const FGaussianSplatFamilyData* const FamilyData =
        View.Family != nullptr ? View.Family->GetExtentionData<FGaussianSplatFamilyData>() : nullptr;
    if (FamilyData == nullptr || FamilyData->Batches.IsEmpty())
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }
    const TArray<FGaussianSplatRenderBatch>& LocalPoints = FamilyData->Batches;

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

    FRDGTextureRef SceneDepthTexture = nullptr;
    if (Inputs.SceneTextures.SceneTextures)
    {
        SceneDepthTexture = Inputs.SceneTextures.SceneTextures->GetParameters()->SceneDepthTexture;
    }
    else if (Inputs.SceneTextures.MobileSceneTextures)
    {
        SceneDepthTexture = Inputs.SceneTextures.MobileSceneTextures->GetParameters()->SceneDepthTexture;
    }

    return GaussianSplatPasses::AddPostProcessPass(
        GraphBuilder,
        View,
        SceneColor,
        Output,
        SceneDepthTexture,
        LocalPoints);
}

void FGaussianSplatViewExtension::BuildPointSnapshot_GameThread(
    const UWorld* TargetWorld,
    FSceneViewFamily& InViewFamily)
{
    TArray<FGaussianSplatRenderBatch> NewPoints;
    NewPoints.Reserve(64);

    // Created even when the set turns out to be empty: a family that went through
    // here and found nothing is not the same as one that never ran, and leaving the
    // slot absent would make those two indistinguishable on the render thread.
    FGaussianSplatFamilyData* const FamilyData = InViewFamily.GetOrCreateExtentionData<FGaussianSplatFamilyData>();

    if (TargetWorld == nullptr)
    {
        FamilyData->Batches = MoveTemp(NewPoints);
        return;
    }

    UGaussianSplatWorldSubsystem* WorldSubsystem = TargetWorld->GetSubsystem<UGaussianSplatWorldSubsystem>();
    if (WorldSubsystem == nullptr)
    {
        FamilyData->Batches = MoveTemp(NewPoints);
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

        // Fix 5: a paged component draws from the pool. Registering is a map lookup
        // after the first call, and the first call is what uploads the asset.
        const UGaussianSplatPagedAsset* Paged = nullptr;
        const FGaussianSplatPoolResidency* Residency = nullptr;
        if (FGaussianSplatPagePool::ArePagedAssetsEnabled() && IsValid(Component->PagedAsset))
        {
            Paged = Component->PagedAsset;
            Residency = FGaussianSplatPagePool::Get().RegisterAsset(Paged);
            if (Residency == nullptr)
            {
                continue;   // it did not fit, and RegisterAsset has already said why
            }
        }

        const UGaussianSplatAsset* Asset = Component->Asset;
        const FGaussianSplatRenderResources* RenderResources = nullptr;
        if (Paged == nullptr)
        {
            if (!IsValid(Asset))
            {
                continue;
            }

            RenderResources = Asset->GetRenderResources();
            if (Asset->Positions.IsEmpty() || RenderResources == nullptr || RenderResources->GetPointCount() == 0)
            {
                continue;
            }
        }

        const int32 GpuPointCount = Paged != nullptr
            ? static_cast<int32>(FMath::Min<int64>(Paged->TotalSplats, MAX_int32))
            : static_cast<int32>(RenderResources->GetPointCount());
        const float Density = FMath::Clamp(Component->DensityScale, 0.001f, 1.0f);
        const int32 DensityStride = FMath::Max(1, FMath::RoundToInt(1.0f / Density));
        const int32 LocalMax = FMath::Max(1, Component->MaxRenderPoints);
        const int32 LimitStride = FMath::Max(1, FMath::DivideAndRoundUp(GpuPointCount, LocalMax));
        const int32 Stride = FMath::Max(DensityStride, LimitStride);
        const FTransform LocalToWorld = Component->GetComponentTransform();

        const FMatrix ComponentToWorldNoScale = FRotationMatrix::Make(LocalToWorld.GetRotation());
        const FMatrix WorldToComponentNoScale = ComponentToWorldNoScale.GetTransposed();

        FGaussianSplatRenderBatch Batch;
        Batch.Resources = RenderResources;
        Batch.PagedAsset = Paged;
        Batch.PagedResidency = Residency;
        Batch.RenderMode = Component->PreviewRenderMode == EGaussianPreviewRenderMode::Points
            ? EGaussianSplatRenderMode::Points
            : EGaussianSplatRenderMode::Billboards;
        // Double, absolute (review M7). The per-view translated float matrix is
        // built in AddPostProcessPass, once, from this.
        Batch.LocalToWorld = LocalToWorld.ToMatrixWithScale();
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
        Batch.AssetPointCount = static_cast<uint32>(GpuPointCount);
        Batch.Stride = static_cast<uint32>(Stride);
        Batch.MaxRenderPoints = static_cast<uint32>(LocalMax);
        NewPoints.Add(Batch);
    }

    if (CVarReverseBatchOrder.GetValueOnGameThread() != 0)
    {
        Algo::Reverse(NewPoints);
    }
    FamilyData->Batches = MoveTemp(NewPoints);
}
