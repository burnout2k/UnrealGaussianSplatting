#include "GaussianSplatAsset.h"

#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "GaussianSplatBoundsUtils.h"
#include "GaussianSplatComponent.h"
#include "Render/GaussianSplatRenderResources.h"

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatAsset, Log, All);

void UGaussianSplatAsset::Serialize(FArchive& Ar)
{
    // The large data arrays are intentionally not UPROPERTY fields. Serialize
    // them once here so multi-million-point assets stay compact and do not
    // become editable array widgets in the Details panel.
    Super::Serialize(Ar);
    Ar << Positions;
    Ar << Covariances;
    Ar << ColorsOpacity;
    Ar << SHCoefficients;
}

void UGaussianSplatAsset::PostLoad()
{
    Super::PostLoad();

    // 从磁盘恢复后立即重建派生数据，避免编辑器打开工程时 Asset 只有 CPU 数组没有 GPU Buffer。
    RefreshDerivedData();
}

void UGaussianSplatAsset::BeginDestroy()
{
    // RenderResources 属于渲染线程可见资源，销毁时要先把它安全地下线。
    ReleaseRenderResources();
    Super::BeginDestroy();
}

void UGaussianSplatAsset::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
    Super::GetResourceSizeEx(CumulativeResourceSize);
    CumulativeResourceSize.AddDedicatedSystemMemoryBytes(Positions.GetAllocatedSize());
    CumulativeResourceSize.AddDedicatedSystemMemoryBytes(Covariances.GetAllocatedSize());
    CumulativeResourceSize.AddDedicatedSystemMemoryBytes(ColorsOpacity.GetAllocatedSize());
    CumulativeResourceSize.AddDedicatedSystemMemoryBytes(SHCoefficients.GetAllocatedSize());
}

int32 UGaussianSplatAsset::GetPointCount() const
{
    return Positions.Num();
}

#if WITH_EDITOR
void UGaussianSplatAsset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    RefreshDerivedData();
}
#endif

void UGaussianSplatAsset::RebuildBounds()
{
    // Bounds 既是渲染裁剪依据，也是编辑器视口能否正确聚焦到对象的依据。
    if (Positions.IsEmpty())
    {
        Bounds = FBoxSphereBounds(EForceInit::ForceInitToZero);
        return;
    }

    FBox Box(EForceInit::ForceInit);
    for (int32 Index = 0; Index < Positions.Num(); ++Index)
    {
        const FVector3f P = Positions[Index];
        const FGaussianCovariance3f Covariance = Covariances.IsValidIndex(Index)
            ? Covariances[Index]
            : GaussianSplatBoundsUtils::MakeIsotropic(0.02f);
        const FVector Extent = GaussianSplatBoundsUtils::ComputeExtent(Covariance);

        Box += FVector(P) - Extent;
        Box += FVector(P) + Extent;
    }

    Bounds = FBoxSphereBounds(Box);
}

void UGaussianSplatAsset::RefreshDerivedData()
{
    // CPU 数据有变化时，先更新包围体，再重建 GPU 资源。
    RebuildBounds();
    BuildRenderResources();

#if WITH_EDITOR
    // 编辑器里如果已有组件引用这个 Asset，需要主动通知它们刷新渲染状态。
    for (TObjectIterator<UGaussianSplatComponent> It; It; ++It)
    {
        UGaussianSplatComponent* Component = *It;
        if (!IsValid(Component) || Component->Asset != this)
        {
            continue;
        }

        Component->UpdateBounds();
        Component->MarkRenderTransformDirty();
        Component->MarkRenderStateDirty();
    }
#endif
}

const FGaussianSplatRenderResources* UGaussianSplatAsset::GetRenderResources() const
{
    return RenderResources.Get();
}

// A safety ceiling on the GPU upload. MaxGpuPointCount is saved into the asset,
// so a value the card cannot fit turns into a crash loop: the editor reloads the
// map on startup, tries the same upload, and dies before it can be edited. Vulkan
// aborts the process on allocation failure rather than degrading, so this has to
// be prevented rather than handled.
//
// Budget is per-splat cost times count, plus the depth sort's key/order ping-pong
// which pads to a power of two -- at 64M that pair alone is ~1 GB.
static TAutoConsoleVariable<int32> CVarMaxGpuPointCountCeiling(
    TEXT("r.GaussianSplat.MaxGpuPointCountCeiling"),
    32000000,
    TEXT("Hard ceiling on how many splats any asset may upload, whatever its ")
    TEXT("MaxGpuPointCount says. Guards against a saved value that exhausts VRAM ")
    TEXT("and crash-loops the editor. Raise it if you have headroom to spare; the ")
    TEXT("default suits roughly a 10 GB card."),
    ECVF_RenderThreadSafe);

void UGaussianSplatAsset::BuildRenderResources()
{
    // 先释放旧资源，避免新旧 Buffer 同时悬挂。
    ReleaseRenderResources();

    const int32 Ceiling = FMath::Max(1000, CVarMaxGpuPointCountCeiling.GetValueOnAnyThread());
    const int32 RequestedPointCount = FMath::Max(1000, MaxGpuPointCount);
    const int32 EffectivePointCount = FMath::Min(RequestedPointCount, Ceiling);
    if (EffectivePointCount < RequestedPointCount)
    {
        UE_LOG(
            LogGaussianSplatAsset,
            Warning,
            TEXT("MaxGpuPointCount %d exceeds the r.GaussianSplat.MaxGpuPointCountCeiling of %d; ")
            TEXT("uploading %d instead. Raise the ceiling if this GPU has the memory."),
            RequestedPointCount,
            Ceiling,
            EffectivePointCount);
    }

    RenderResources = MakeUnique<FGaussianSplatRenderResources>();
    RenderResources->BuildFromAssetData(
        Positions,
        Covariances,
        ColorsOpacity,
        SHCoefficients,
        EffectivePointCount);

    UE_LOG(
        LogGaussianSplatAsset,
        Display,
        TEXT("Prepared %u of %d Gaussian splats for GPU upload (MaxGpuPointCount=%d, ceiling=%d)"),
        RenderResources->GetPointCount(),
        Positions.Num(),
        MaxGpuPointCount,
        Ceiling);

    // 把 FRenderResource 注册到渲染线程初始化队列。
    BeginInitResource(RenderResources.Get());
}

void UGaussianSplatAsset::ReleaseRenderResources()
{
    if (RenderResources)
    {
        // BeginReleaseResource 只是在渲染线程排一个释放命令。
        // 紧跟 Flush 的代价较高，但能保证 Asset 销毁时不会留下悬空引用。
        BeginReleaseResource(RenderResources.Get());
        FlushRenderingCommands();
        RenderResources.Reset();
    }
}
