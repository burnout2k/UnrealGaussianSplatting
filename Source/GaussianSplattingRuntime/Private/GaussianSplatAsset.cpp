#include "GaussianSplatAsset.h"

#include "Algo/Sort.h"
#include "Async/ParallelFor.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/CustomVersion.h"
#include "GaussianSplatBoundsUtils.h"
#include "GaussianSplatComponent.h"
#include "Render/GaussianSplatRenderResources.h"

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatAsset, Log, All);

const FGuid FGaussianSplatCustomVersion::GUID(0x9C4E7A31, 0x5B2D4F60, 0xA1E38C7D, 0x2F60B914);
static FCustomVersionRegistration GRegisterGaussianSplatCustomVersion(
    FGaussianSplatCustomVersion::GUID,
    FGaussianSplatCustomVersion::LatestVersion,
    TEXT("GaussianSplatVer"));

void UGaussianSplatAsset::Serialize(FArchive& Ar)
{
    // The large data arrays are intentionally not UPROPERTY fields. Serialize
    // them once here so multi-million-point assets stay compact and do not
    // become editable array widgets in the Details panel.
    Ar.UsingCustomVersion(FGaussianSplatCustomVersion::GUID);

    Super::Serialize(Ar);
    Ar << Positions;
    Ar << Covariances;
    Ar << ColorsOpacity;
    Ar << SHCoefficients;

    // Assets written before cells existed have nothing here; reading anyway
    // would run off the end of the file. They load with Cells empty and get
    // them built in RefreshDerivedData instead.
    if (Ar.CustomVer(FGaussianSplatCustomVersion::GUID) >= FGaussianSplatCustomVersion::SpatialCells)
    {
        Ar << Cells;
    }
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

    const FName Changed = PropertyChangedEvent.GetPropertyName();
    if (Changed == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, CellSize) ||
        Changed == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, MinCellOccupancy))
    {
        Cells.Reset();
    }

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

namespace
{
    // H3DGS's merge weight: opacity * sqrt(det Sigma). sqrt(det Sigma) is the
    // product of the three axis scales, so this ranks a splat by roughly how
    // much visible volume it accounts for -- exactly the order in which you want
    // to drop splats. Computable from the stored covariance alone: no camera
    // poses, no source imagery, no retraining.
    //
    // The determinant is accumulated in double because the scales span
    // 0.0000-134 m on real captures, and its cube underflows float badly at the
    // small end.
    FORCEINLINE float SplatImportance(const FGaussianCovariance3f& C, float Opacity)
    {
        const double Det =
              static_cast<double>(C.XX) * (static_cast<double>(C.YY) * C.ZZ - static_cast<double>(C.YZ) * C.YZ)
            - static_cast<double>(C.XY) * (static_cast<double>(C.XY) * C.ZZ - static_cast<double>(C.XZ) * C.YZ)
            + static_cast<double>(C.XZ) * (static_cast<double>(C.XY) * C.YZ - static_cast<double>(C.XZ) * C.YY);

        return static_cast<float>(Opacity * FMath::Sqrt(FMath::Max(Det, 0.0)));
    }

    // Move Source[Order[d]] into slot d, for an array of Stride elements per
    // splat. Done one array at a time so the peak extra allocation is the size
    // of the largest single array rather than all of them at once.
    template <typename T>
    void ApplyPermutation(TArray<T>& Data, const TArray<int32>& Order, int32 Stride = 1)
    {
        if (Data.IsEmpty())
        {
            return;
        }

        TArray<T> Reordered;
        Reordered.SetNumUninitialized(Order.Num() * Stride);
        for (int32 Dest = 0; Dest < Order.Num(); ++Dest)
        {
            const int32 Source = Order[Dest];
            for (int32 Element = 0; Element < Stride; ++Element)
            {
                Reordered[Dest * Stride + Element] = Data[Source * Stride + Element];
            }
        }
        Data = MoveTemp(Reordered);
    }
}

bool UGaussianSplatAsset::BuildCells()
{
    Cells.Reset();

    const int32 SourceCount = Positions.Num();
    if (SourceCount == 0)
    {
        return false;
    }

    const double StartTime = FPlatformTime::Seconds();
    const float Pitch = FMath::Max(1.0f, CellSize);
    const float InvPitch = 1.0f / Pitch;

    // Pass 1: bin. Sparse by necessity -- a handful of reconstruction floaters
    // stretch a 2.4 km capture's AABB to ~14 km, so a dense 64 m grid over that
    // would be ~9.7M slots holding ~2,900 occupied ones.
    TMap<FIntVector, int32> SlotByCoord;
    SlotByCoord.Reserve(16384);
    TArray<FIntVector> SlotCoord;
    TArray<int32> SlotCount;
    TArray<int32> SlotBySplat;
    SlotBySplat.SetNumUninitialized(SourceCount);

    for (int32 Index = 0; Index < SourceCount; ++Index)
    {
        const FVector3f P = Positions[Index];
        const FIntVector Coord(
            FMath::FloorToInt(P.X * InvPitch),
            FMath::FloorToInt(P.Y * InvPitch),
            FMath::FloorToInt(P.Z * InvPitch));

        int32 Slot = INDEX_NONE;
        if (const int32* Existing = SlotByCoord.Find(Coord))
        {
            Slot = *Existing;
        }
        else
        {
            Slot = SlotCoord.Num();
            SlotByCoord.Add(Coord, Slot);
            SlotCoord.Add(Coord);
            SlotCount.Add(0);
        }

        SlotBySplat[Index] = Slot;
        ++SlotCount[Slot];
    }

    // Pass 2: keep the cells that hold a surface. Ordered by coordinate rather
    // than by discovery so a rebuild of the same capture is reproducible, and so
    // neighbouring cells land near each other in memory.
    const int32 MinOccupancy = FMath::Max(0, MinCellOccupancy);
    TArray<int32> KeptSlots;
    KeptSlots.Reserve(SlotCoord.Num());
    for (int32 Slot = 0; Slot < SlotCoord.Num(); ++Slot)
    {
        if (SlotCount[Slot] >= MinOccupancy && SlotCount[Slot] > 0)
        {
            KeptSlots.Add(Slot);
        }
    }
    Algo::Sort(KeptSlots, [&SlotCoord](int32 A, int32 B)
    {
        const FIntVector& CA = SlotCoord[A];
        const FIntVector& CB = SlotCoord[B];
        if (CA.X != CB.X) { return CA.X < CB.X; }
        if (CA.Y != CB.Y) { return CA.Y < CB.Y; }
        return CA.Z < CB.Z;
    });

    TArray<int32> CellBySlot;
    CellBySlot.Init(INDEX_NONE, SlotCoord.Num());
    Cells.Reserve(KeptSlots.Num());
    int32 Running = 0;
    for (int32 CellIndex = 0; CellIndex < KeptSlots.Num(); ++CellIndex)
    {
        const int32 Slot = KeptSlots[CellIndex];
        CellBySlot[Slot] = CellIndex;

        FGaussianSplatCell Cell;
        Cell.FirstIndex = Running;
        Cell.Count = SlotCount[Slot];
        Cells.Add(Cell);
        Running += Cell.Count;
    }
    const int32 KeptCount = Running;

    // Pass 3: destination -> source, cell-contiguous. Splats in rejected cells
    // are NOT deleted, only parked past the last cell: nothing here is
    // destructive, so MinCellOccupancy stays a tunable rather than a one-way
    // door. They are simply never selected, and once residency is driven by
    // cells they never reach the GPU either.
    TArray<int32> Order;
    Order.SetNumUninitialized(SourceCount);
    TArray<int32> Cursor;
    Cursor.SetNumUninitialized(Cells.Num());
    for (int32 CellIndex = 0; CellIndex < Cells.Num(); ++CellIndex)
    {
        Cursor[CellIndex] = Cells[CellIndex].FirstIndex;
    }
    int32 ParkedCursor = KeptCount;
    for (int32 Index = 0; Index < SourceCount; ++Index)
    {
        const int32 CellIndex = CellBySlot[SlotBySplat[Index]];
        Order[CellIndex != INDEX_NONE ? Cursor[CellIndex]++ : ParkedCursor++] = Index;
    }

    // Pass 4: importance order inside each cell, so a keep-count of N means the
    // N most significant splats rather than an arbitrary N.
    TArray<float> Importance;
    Importance.SetNumUninitialized(SourceCount);
    ParallelFor(SourceCount, [this, &Importance](int32 Index)
    {
        const FGaussianCovariance3f& C = Covariances.IsValidIndex(Index)
            ? Covariances[Index]
            : GaussianSplatBoundsUtils::MakeIsotropic(0.02f);
        const float Opacity = ColorsOpacity.IsValidIndex(Index) ? ColorsOpacity[Index].W : 1.0f;
        Importance[Index] = SplatImportance(C, Opacity);
    });

    ParallelFor(Cells.Num(), [this, &Order, &Importance](int32 CellIndex)
    {
        const FGaussianSplatCell& Cell = Cells[CellIndex];
        TArrayView<int32> Slice(Order.GetData() + Cell.FirstIndex, Cell.Count);
        Algo::Sort(Slice, [&Importance](int32 A, int32 B)
        {
            // Tie-break on index so the order is stable across rebuilds.
            return Importance[A] != Importance[B] ? Importance[A] > Importance[B] : A < B;
        });
    });
    Importance.Empty();

    // Pass 5: move the data. One array at a time keeps peak memory to the size
    // of the largest array rather than a full second copy of everything.
    ApplyPermutation(Positions, Order);
    ApplyPermutation(Covariances, Order);
    ApplyPermutation(ColorsOpacity, Order);
    if (!SHCoefficients.IsEmpty())
    {
        ApplyPermutation(SHCoefficients, Order, 45);
    }

    // Pass 6: shrink-wrap. The grid slot is 64 m across; a cell holding six
    // floaters occupies a metre of it. Storing what the splats actually span
    // makes the frustum test and the distance estimate honest, and later gives
    // quantization a tight local frame.
    ParallelFor(Cells.Num(), [this](int32 CellIndex)
    {
        FGaussianSplatCell& Cell = Cells[CellIndex];
        FVector3f Min(TNumericLimits<float>::Max());
        FVector3f Max(TNumericLimits<float>::Lowest());
        for (int32 Index = Cell.FirstIndex; Index < Cell.FirstIndex + Cell.Count; ++Index)
        {
            const FVector3f P = Positions[Index];
            Min = FVector3f::Min(Min, P);
            Max = FVector3f::Max(Max, P);
        }
        Cell.BoundsMin = Min;
        Cell.BoundsMax = Max;
    });

    const int32 DroppedCells = SlotCoord.Num() - Cells.Num();
    const int32 DroppedSplats = SourceCount - KeptCount;
    UE_LOG(
        LogGaussianSplatAsset,
        Display,
        TEXT("Built %d cells at %.0f units from %d splats in %.1f s; %d cells under ")
        TEXT("%d splats excluded (%d splats, %.3f%% -- kept in the asset, parked ")
        TEXT("after the last cell), %d splats in cells"),
        Cells.Num(),
        Pitch,
        SourceCount,
        FPlatformTime::Seconds() - StartTime,
        DroppedCells,
        MinOccupancy,
        DroppedSplats,
        SourceCount > 0 ? 100.0f * DroppedSplats / SourceCount : 0.0f,
        KeptCount);

    return true;
}

void UGaussianSplatAsset::RefreshDerivedData()
{
    // Cells are derived data. An asset saved before cells existed, or one whose
    // cell settings just changed, arrives with none and gets them built here.
    if (Cells.IsEmpty() && !Positions.IsEmpty())
    {
        BuildCells();
#if WITH_EDITOR
        // Building is not free on a large capture, so make sure a save persists
        // it rather than repeating the work on every load.
        if (!HasAnyFlags(RF_ClassDefaultObject))
        {
            MarkPackageDirty();
            UE_LOG(
                LogGaussianSplatAsset,
                Display,
                TEXT("Save this asset to persist the cells; otherwise they are rebuilt every load."));
        }
#endif
    }

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
        Cells,
        EffectivePointCount);

    UE_LOG(
        LogGaussianSplatAsset,
        Display,
        TEXT("Prepared %u of %d Gaussian splats for GPU upload across %d cells ")
        TEXT("(MaxGpuPointCount=%d, ceiling=%d)"),
        RenderResources->GetPointCount(),
        Positions.Num(),
        RenderResources->GetCells().Num(),
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
