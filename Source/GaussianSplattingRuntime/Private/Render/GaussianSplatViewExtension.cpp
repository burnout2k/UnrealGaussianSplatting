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
    // 当前实现始终启用。
    // 如果后面要继续优化，可以在这里根据 world 类型、是否存在 Gaussian 组件等条件动态关闭。
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
    // 这是游戏线程阶段，适合读取 UObject / Component。
    // 这里先根据当前 ViewFamily 找到对应的 World，再把本帧需要渲染的 billboard
    // Gaussian 组件压成快照，供后面的渲染线程消费。
    const UWorld* ViewFamilyWorld = InViewFamily.Scene ? InViewFamily.Scene->GetWorld() : nullptr;
    BuildPointSnapshot_GameThread(ViewFamilyWorld);
}

void FGaussianSplatViewExtension::PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
}

void FGaussianSplatViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
    // 这里选择把 Gaussian 绘制挂在 MotionBlur 之后。
    // UE 走到这个后处理阶段时，会回调 PostProcessPass_RenderThread，
    // 然后由我们往 RDG 里继续追加 Gaussian 的渲染与合成 pass。
    if (PassId == EPostProcessingPass::MotionBlur)
    {
        InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FGaussianSplatViewExtension::PostProcessPass_RenderThread));
    }
}

FScreenPassTexture FGaussianSplatViewExtension::PostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
{
    // CachedPoints 是游戏线程在 BeginRenderViewFamily() 里构造的快照。
    // 这里先复制一份本地数组，避免长时间持锁进入后续 RDG 构图逻辑。
    TArray<FGaussianSplatRenderBatch> LocalPoints;
    {
        FReadScopeLock Lock(CachedPointsLock);
        LocalPoints = CachedPoints;
    }

    // 没有 Gaussian 要画时，直接返回原始 SceneColor。
    if (LocalPoints.IsEmpty())
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    // 取当前后处理链路传下来的 SceneColor。
    const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
    if (!SceneColor.IsValid())
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    // 如果上游没有指定输出 RT，就基于 SceneColor 创建一个默认输出。
    FScreenPassRenderTarget Output = Inputs.OverrideOutput;
    if (!Output.IsValid())
    {
        Output = FScreenPassRenderTarget::CreateFromInput(GraphBuilder, SceneColor, View.GetOverwriteLoadAction(), TEXT("GaussianSplat.PostProcessOutput"));
    }

    // 真正的 Cull / Sort / Raster / Composite 组合都在 AddPostProcessPass() 里完成。
    return GaussianSplatPasses::AddPostProcessPass(GraphBuilder, View, SceneColor, Output, LocalPoints);
}

void FGaussianSplatViewExtension::BuildPointSnapshot_GameThread(const UWorld* TargetWorld)
{
    // NewPoints 是本帧快照。构造完成后一次性替换 CachedPoints，减少锁占用时间。
    TArray<FGaussianSplatRenderBatch> NewPoints;
    NewPoints.Reserve(64);

    // 没有目标 World，说明这一帧没有合法的场景上下文，直接清空快照。
    if (TargetWorld == nullptr)
    {
        FWriteScopeLock Lock(CachedPointsLock);
        CachedPoints = MoveTemp(NewPoints);
        return;
    }

    // 每个 World 自己维护一份 Gaussian billboard 组件注册表。
    // 这里不再全局扫 TObjectIterator，而是直接从当前 World 的 subsystem 取组件列表。
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
        if (!IsValid(Component))
        {
            continue;
        }

        // 这里的注册表已经只缓存 billboard 组件，但可见性仍然是逐帧状态，
        // 所以应当在快照构建阶段判断。
        if (!Component->IsVisible())
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

        // DensityScale 通过固定步长抽样实现。
        // 例如 0.25 大致对应每 4 个点里取 1 个。
        const int32 Stride = FMath::Max(1, FMath::RoundToInt(1.0f / Density));
        const int32 LocalMax = FMath::Max(1, Component->MaxRenderPoints);
        const FTransform LocalToWorld = Component->GetComponentTransform();

        // 这里只需要旋转部分的逆矩阵，把世界方向变回组件局部空间，
        // 供 SH 方向光照相关计算使用。
        const FMatrix ComponentToWorldNoScale = FRotationMatrix::Make(LocalToWorld.GetRotation());
        const FMatrix WorldToComponentNoScale = ComponentToWorldNoScale.GetTransposed();

        // 把组件和资源状态压成一个渲染线程可安全读取的 batch。
        FGaussianSplatRenderBatch Batch;
        Batch.Resources = RenderResources;
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
