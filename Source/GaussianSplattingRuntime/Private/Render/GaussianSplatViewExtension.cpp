#include "Render/GaussianSplatViewExtension.h"

#include "GaussianSplatAsset.h"
#include "GaussianSplatComponent.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "Render/GaussianSplatPasses.h"
#include "ScreenPass.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatViewExtension, Log, All);

FGaussianSplatViewExtension::FGaussianSplatViewExtension(const FAutoRegister& AutoRegister)
    : FSceneViewExtensionBase(AutoRegister)
{
    UE_LOG(LogGaussianSplatViewExtension, Log, TEXT("FGaussianSplatViewExtension constructed."));
}

bool FGaussianSplatViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
    UE_LOG(LogGaussianSplatViewExtension, VeryVerbose, TEXT("IsActiveThisFrame_Internal called."));
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
    UE_LOG(LogGaussianSplatViewExtension, Log, TEXT("BeginRenderViewFamily: Views=%d"), InViewFamily.Views.Num());
    BuildPointSnapshot_GameThread();
}

void FGaussianSplatViewExtension::PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
    TArray<FGaussianSplatRenderBatch> LocalPoints;
    {
        FReadScopeLock Lock(CachedPointsLock);
        LocalPoints = CachedPoints;
    }

    UE_LOG(
        LogGaussianSplatViewExtension,
        Log,
        TEXT("PreRenderViewFamily_RenderThread: Views=%d CachedPoints=%d RenderTarget=%s"),
        InViewFamily.Views.Num(),
        LocalPoints.Num(),
        InViewFamily.RenderTarget ? TEXT("Yes") : TEXT("No"));
}

void FGaussianSplatViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
    if (PassId == EPostProcessingPass::MotionBlur)
    {
        UE_LOG(LogGaussianSplatViewExtension, Log, TEXT("SubscribeToPostProcessingPass: Registering MotionBlur callback."));
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

    UE_LOG(
        LogGaussianSplatViewExtension,
        Log,
        TEXT("PostProcessPass_RenderThread: CachedPoints=%d"),
        LocalPoints.Num());

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

void FGaussianSplatViewExtension::BuildPointSnapshot_GameThread()
{
    TArray<FGaussianSplatRenderBatch> NewPoints;
    NewPoints.Reserve(64);

    int32 TotalComponents = 0;
    int32 RegisteredVisibleComponents = 0;
    int32 BillboardComponents = 0;
    int32 ComponentsWithAssets = 0;
    int32 AcceptedComponents = 0;

    for (TObjectIterator<UGaussianSplatComponent> It; It; ++It)
    {
        UGaussianSplatComponent* Component = *It;
        ++TotalComponents;

        if (!IsValid(Component))
        {
            UE_LOG(LogGaussianSplatViewExtension, Warning, TEXT("BuildPointSnapshot: Skipping invalid component."));
            continue;
        }

        if (Component->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
        {
            UE_LOG(LogGaussianSplatViewExtension, Log, TEXT("BuildPointSnapshot: Skip %s because it is a CDO/archetype object."), *Component->GetPathName());
            continue;
        }

        UWorld* ComponentWorld = Component->GetWorld();
        if (!IsValid(ComponentWorld))
        {
            UE_LOG(LogGaussianSplatViewExtension, Log, TEXT("BuildPointSnapshot: Skip %s because World is null."), *Component->GetPathName());
            continue;
        }

        if (ComponentWorld->WorldType == EWorldType::Inactive || ComponentWorld->WorldType == EWorldType::None)
        {
            UE_LOG(
                LogGaussianSplatViewExtension,
                Log,
                TEXT("BuildPointSnapshot: Skip %s because WorldType=%d."),
                *Component->GetPathName(),
                static_cast<int32>(ComponentWorld->WorldType));
            continue;
        }

        if (!Component->IsRegistered())
        {
            UE_LOG(LogGaussianSplatViewExtension, Log, TEXT("BuildPointSnapshot: Skip %s because it is not registered."), *Component->GetPathName());
            continue;
        }

        if (!Component->IsVisible())
        {
            UE_LOG(LogGaussianSplatViewExtension, Log, TEXT("BuildPointSnapshot: Skip %s because IsVisible() is false."), *Component->GetPathName());
            continue;
        }

        ++RegisteredVisibleComponents;

        if (Component->PreviewRenderMode != EGaussianPreviewRenderMode::Billboards)
        {
            UE_LOG(
                LogGaussianSplatViewExtension,
                Log,
                TEXT("BuildPointSnapshot: Skip %s because PreviewRenderMode=%d."),
                *Component->GetPathName(),
                static_cast<int32>(Component->PreviewRenderMode));
            continue;
        }

        ++BillboardComponents;

        const UGaussianSplatAsset* Asset = Component->Asset;
        if (!IsValid(Asset))
        {
            UE_LOG(LogGaussianSplatViewExtension, Log, TEXT("BuildPointSnapshot: Skip %s because Asset is null."), *Component->GetPathName());
            continue;
        }

        ++ComponentsWithAssets;

        if (Asset->Positions.IsEmpty() || Asset->GetRenderResources() == nullptr || Asset->GetRenderResources()->GetPointCount() == 0)
        {
            UE_LOG(LogGaussianSplatViewExtension, Log, TEXT("BuildPointSnapshot: Skip %s because Asset render resources are unavailable."), *Component->GetPathName());
            continue;
        }

        ++AcceptedComponents;

        const float Density = FMath::Clamp(Component->DensityScale, 0.001f, 1.0f);
        const int32 Stride = FMath::Max(1, FMath::RoundToInt(1.0f / Density));
        const int32 LocalMax = FMath::Max(1, Component->MaxRenderPoints);
        const FTransform LocalToWorld = Component->GetComponentTransform();
        const FMatrix ComponentToWorldNoScale = FRotationMatrix::Make(LocalToWorld.GetRotation());
        const FMatrix WorldToComponentNoScale = ComponentToWorldNoScale.GetTransposed();

        FGaussianSplatRenderBatch Batch;
        Batch.Resources = Asset->GetRenderResources();
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

        const int32 AddedForComponent = FMath::Min(LocalMax, FMath::DivideAndRoundUp(Asset->Positions.Num(), Stride));

        UE_LOG(
            LogGaussianSplatViewExtension,
            Log,
            TEXT("BuildPointSnapshot: Accepted %s AssetPoints=%d Density=%.3f Stride=%d LocalMax=%d Added=%d Visible=%d Registered=%d"),
            *Component->GetPathName(),
            Asset->Positions.Num(),
            Density,
            Stride,
            LocalMax,
            AddedForComponent,
            Component->IsVisible() ? 1 : 0,
            Component->IsRegistered() ? 1 : 0);

    }

    UE_LOG(
        LogGaussianSplatViewExtension,
        Log,
        TEXT("BuildPointSnapshot summary: Total=%d RegisteredVisible=%d Billboard=%d WithAsset=%d Accepted=%d CachedPoints=%d"),
        TotalComponents,
        RegisteredVisibleComponents,
        BillboardComponents,
        ComponentsWithAssets,
        AcceptedComponents,
        NewPoints.Num());

    {
        FWriteScopeLock Lock(CachedPointsLock);
        CachedPoints = MoveTemp(NewPoints);
    }
}
