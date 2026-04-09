#include "GaussianSplatAsset.h"

#include "EngineUtils.h"
#include "GaussianSplatComponent.h"
#include "Render/GaussianSplatRenderResources.h"

void UGaussianSplatAsset::Serialize(FArchive& Ar)
{
    // UObject 默认不会自动序列化这些裸 TArray 成员，所以这里显式写入。
    Super::Serialize(Ar);
    Ar << Positions;
    Ar << Rotations;
    Ar << Scales;
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
    CumulativeResourceSize.AddDedicatedSystemMemoryBytes(Rotations.GetAllocatedSize());
    CumulativeResourceSize.AddDedicatedSystemMemoryBytes(Scales.GetAllocatedSize());
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

        // 用 scale 扩一圈包围盒，把每个高斯近似为局部轴对齐盒。
        // 这样做不够精确，但足够便宜，也适合当作粗包围体。
        const FVector3f S = Scales.IsValidIndex(Index)
            ? FVector3f(
                FMath::Clamp(Scales[Index].X, 0.001f, 10.0f),
                FMath::Clamp(Scales[Index].Y, 0.001f, 10.0f),
                FMath::Clamp(Scales[Index].Z, 0.001f, 10.0f))
            : FVector3f(0.02f, 0.02f, 0.02f);

        FQuat Rotation = FQuat::Identity;
        if (Rotations.IsValidIndex(Index))
        {
            Rotation = FQuat(Rotations[Index]);
            Rotation.Normalize();
        }

        const FVector AxisX = Rotation.RotateVector(FVector(S.X, 0.0f, 0.0f));
        const FVector AxisY = Rotation.RotateVector(FVector(0.0f, S.Y, 0.0f));
        const FVector AxisZ = Rotation.RotateVector(FVector(0.0f, 0.0f, S.Z));
        const FVector Extent(
            FMath::Abs(AxisX.X) + FMath::Abs(AxisY.X) + FMath::Abs(AxisZ.X),
            FMath::Abs(AxisX.Y) + FMath::Abs(AxisY.Y) + FMath::Abs(AxisZ.Y),
            FMath::Abs(AxisX.Z) + FMath::Abs(AxisY.Z) + FMath::Abs(AxisZ.Z));

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

void UGaussianSplatAsset::BuildRenderResources()
{
    // 先释放旧资源，避免新旧 Buffer 同时悬挂。
    ReleaseRenderResources();

    RenderResources = MakeUnique<FGaussianSplatRenderResources>();
    RenderResources->BuildFromAssetData(Positions, Rotations, Scales, ColorsOpacity, SHCoefficients);

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
