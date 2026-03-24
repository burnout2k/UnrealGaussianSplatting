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
    TArray<FGaussianSplatRenderPoint> LocalPoints;
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
    TArray<FGaussianSplatRenderPoint> LocalPoints;
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
    TArray<FGaussianSplatRenderPoint> NewPoints;
    NewPoints.Reserve(65536);

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

        if (Asset->Positions.IsEmpty())
        {
            UE_LOG(LogGaussianSplatViewExtension, Log, TEXT("BuildPointSnapshot: Skip %s because Asset has no positions."), *Component->GetPathName());
            continue;
        }

        ++AcceptedComponents;

        const float Density = FMath::Clamp(Component->DensityScale, 0.001f, 1.0f);
        const int32 Stride = FMath::Max(1, FMath::RoundToInt(1.0f / Density));
        const int32 LocalMax = FMath::Max(1, Component->MaxRenderPoints);
        const FTransform LocalToWorld = Component->GetComponentTransform();
        const FMatrix ComponentToWorldNoScale = FRotationMatrix::Make(LocalToWorld.GetRotation());
        const FMatrix WorldToComponentNoScale = ComponentToWorldNoScale.GetTransposed();

        int32 AddedForComponent = 0;
        for (int32 Index = 0; Index < Asset->Positions.Num(); Index += Stride)
        {
            if (AddedForComponent >= LocalMax)
            {
                break;
            }

            const FVector WorldPos = LocalToWorld.TransformPosition(FVector(Asset->Positions[Index]));
            const FVector3f Scale = Asset->Scales.IsValidIndex(Index)
                ? Asset->Scales[Index]
                : FVector3f(0.02f, 0.02f, 0.02f);
            const FQuat AssetRotation = Asset->Rotations.IsValidIndex(Index)
                ? FQuat(Asset->Rotations[Index])
                : FQuat::Identity;

            const float AxisScale = FMath::Clamp(Component->PointSize, 0.1f, 32.0f) * 0.35f;
            const FVector Axis0World = LocalToWorld.TransformVector(AssetRotation.RotateVector(FVector(Scale.X * AxisScale, 0.0f, 0.0f)));
            const FVector Axis1World = LocalToWorld.TransformVector(AssetRotation.RotateVector(FVector(0.0f, Scale.Y * AxisScale, 0.0f)));
            const FVector Axis2World = LocalToWorld.TransformVector(AssetRotation.RotateVector(FVector(0.0f, 0.0f, Scale.Z * AxisScale)));

            const float Cov00 = static_cast<float>(Axis0World.X * Axis0World.X + Axis1World.X * Axis1World.X + Axis2World.X * Axis2World.X);
            const float Cov01 = static_cast<float>(Axis0World.X * Axis0World.Y + Axis1World.X * Axis1World.Y + Axis2World.X * Axis2World.Y);
            const float Cov02 = static_cast<float>(Axis0World.X * Axis0World.Z + Axis1World.X * Axis1World.Z + Axis2World.X * Axis2World.Z);
            const float Cov11 = static_cast<float>(Axis0World.Y * Axis0World.Y + Axis1World.Y * Axis1World.Y + Axis2World.Y * Axis2World.Y);
            const float Cov12 = static_cast<float>(Axis0World.Y * Axis0World.Z + Axis1World.Y * Axis1World.Z + Axis2World.Y * Axis2World.Z);
            const float Cov22 = static_cast<float>(Axis0World.Z * Axis0World.Z + Axis1World.Z * Axis1World.Z + Axis2World.Z * Axis2World.Z);

            FVector4f Color(1.0f, 1.0f, 1.0f, 1.0f);
            if (Asset->ColorsOpacity.IsValidIndex(Index))
            {
                const FVector4f C = Asset->ColorsOpacity[Index];
                Color = FVector4f(C.X, C.Y, C.Z, FMath::Clamp(C.W * Component->OpacityScale, 0.0f, 1.0f));
            }

            FGaussianSplatRenderPoint Point;
            Point.PositionWS = FVector4f(
                static_cast<float>(WorldPos.X),
                static_cast<float>(WorldPos.Y),
                static_cast<float>(WorldPos.Z),
                1.0f);
            Point.Cov3D0 = FVector4f(Cov00, Cov01, Cov02, 0.0f);
            Point.Cov3D1 = FVector4f(Cov11, Cov12, Cov22, 0.0f);
            Point.Color = Color;
            Point.WorldToLocalRow0 = FVector4f(
                static_cast<float>(WorldToComponentNoScale.M[0][0]),
                static_cast<float>(WorldToComponentNoScale.M[0][1]),
                static_cast<float>(WorldToComponentNoScale.M[0][2]),
                0.0f);
            Point.WorldToLocalRow1 = FVector4f(
                static_cast<float>(WorldToComponentNoScale.M[1][0]),
                static_cast<float>(WorldToComponentNoScale.M[1][1]),
                static_cast<float>(WorldToComponentNoScale.M[1][2]),
                0.0f);
            Point.WorldToLocalRow2 = FVector4f(
                static_cast<float>(WorldToComponentNoScale.M[2][0]),
                static_cast<float>(WorldToComponentNoScale.M[2][1]),
                static_cast<float>(WorldToComponentNoScale.M[2][2]),
                0.0f);

            const int32 SHBase = Index * 45;
            if (Asset->SHCoefficients.Num() >= SHBase + 45)
            {
                auto ReadCoeff = [&](int32 CoeffOffset) -> FVector
                {
                    return FVector(
                        Asset->SHCoefficients[SHBase + CoeffOffset + 0],
                        Asset->SHCoefficients[SHBase + CoeffOffset + 1],
                        Asset->SHCoefficients[SHBase + CoeffOffset + 2]);
                };

                for (int32 SHIndex = 0; SHIndex < 15; ++SHIndex)
                {
                    const FVector SHLocal = ReadCoeff(SHIndex * 3);
                    Point.SHCoefficients[SHIndex] = FVector4f(
                        static_cast<float>(SHLocal.X),
                        static_cast<float>(SHLocal.Y),
                        static_cast<float>(SHLocal.Z),
                        0.0f);
                }
            }

            NewPoints.Add(Point);

            ++AddedForComponent;
        }

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
