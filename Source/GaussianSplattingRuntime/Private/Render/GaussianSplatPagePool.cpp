#include "Render/GaussianSplatPagePool.h"

#include "GaussianSplatPagedAsset.h"
#include "HAL/IConsoleManager.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "GaussianSplatFormat.h"
#include "Render/GaussianSplatDeviceMemory.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Async/ParallelFor.h"

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatPool, Log, All);

using namespace GaussianSplatDeviceMemory;

namespace
{
    TAutoConsoleVariable<int32> CVarPagedAssets(
        TEXT("r.GaussianSplat.PagedAssets"),
        0,
        TEXT("0: draw from each asset's own buffers, as before (default).\n")
        TEXT("1: draw paged assets from the process-wide pool."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarPoolMB(
        TEXT("r.GaussianSplat.PoolMB"),
        0,
        TEXT("Size of the splat page pool, MiB. 0 derives it from this card (see the log line at creation), which is\n")
        TEXT("what a machine with a different amount of VRAM needs. Read ONCE: the pool is never re-created."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarPoolReserveMBEditor(
        TEXT("r.GaussianSplat.PoolReserveMBEditor"),
        1000,
        TEXT("PoolReserveMB's value in the editor, where there is one viewport rather than a six-camera rig and the\n")
        TEXT("package's reserve would leave nothing. PIE uses this too: PIE does not spawn the rig either."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarPoolReserveMB(
        TEXT("r.GaussianSplat.PoolReserveMB"),
        3000,
        TEXT("What the derivation leaves for everything that arrives AFTER the pool: the rig's render targets and view\n")
        TEXT("state (~1.9 GB for six 800x450 cameras), the RDG transient heaps (512 MiB measured), sort scratch at the\n")
        TEXT("draw budget (~0.4 GB at 20.9M) and texture headroom. The pool is created during level load, where UE's\n")
        TEXT("usage is about 2 GB against the ~3.9 GB it settles at, so without this the pool would be ~2 GB too big."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarPoolCapMode(
        TEXT("r.GaussianSplat.PoolCapMode"),
        0,
        TEXT("What the pool is capped against.\n")
        TEXT("0 (default): UE's 70%% texture-eviction line, so textures allocated after the pool stay in VRAM.\n")
        TEXT("   Past that line every LATER texture allocation is placed in host memory, silently and with no log\n")
        TEXT("   line, which a scenario spawning vehicles or streaming new mips would walk straight into.\n")
        TEXT("1: the physical budget. For splat-only maps, where nothing allocates a texture after the pool."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarStream(
        TEXT("r.GaussianSplat.Stream"),
        0,
        TEXT("0 (default): every registered paged asset is uploaded whole when it registers, as Step 1a does.\n")
        TEXT("1: only the pinned floor is uploaded at registration and the settle gate moves tail pages in and out\n")
        TEXT("   every tick, so an asset may be far larger than the pool. Needs r.GaussianSplat.PagedAssets 1."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarStreamProtectedTicks(
        TEXT("r.GaussianSplat.Stream.ProtectedTicks"),
        30,
        TEXT("A page that left the required set within this many gate ticks is evicted only when no older candidate\n")
        TEXT("exists. LRU already gives hysteresis except at 100%% pool occupancy, which is exactly the regime a\n")
        TEXT("capped pool creates, so without this a pose oscillating at a cell boundary would re-upload the same\n")
        TEXT("pages every tick -- visible as upload time, never as a wrong picture (review M5)."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarStreamMaxUploadMBPerCommand(
        TEXT("r.GaussianSplat.Stream.MaxUploadMBPerCommand"),
        256,
        TEXT("No single render command carries more than this, MiB. A large required fill -- a teleport is ~1 GB --\n")
        TEXT("is split into several commands in the same tick, so render-thread work stays bounded per command and\n")
        TEXT("nothing comes near g.TimeoutForBlockOnRenderFence (30 s in CARLA's ini), which is fatal in a package."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarStreamParallelCopy(
        TEXT("r.GaussianSplat.Stream.ParallelCopy"),
        1,
        TEXT("1 (default): fill the staging buffer with a ParallelFor over pages. Two thirds of the measured\n")
        TEXT("0.1 ms/MiB is one render thread copying into write-combined memory at ~11 GB/s, so this is the\n")
        TEXT("difference between ~90 ms and ~25 ms on a 1 GB teleport fill. 0 keeps the serial copy, for comparison."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarStreamMaxUploadMB(
        TEXT("r.GaussianSplat.Stream.MaxUploadMBPerTick"),
        32,
        TEXT("Prefetch byte cap per gate tick, MiB. REQUIRED pages ignore it -- a camera frame must not draw a hole --\n")
        TEXT("so this bounds only the look-ahead. 32 MiB measured +9.6 ms on the 10 Hz cycle when saturated every\n")
        TEXT("tick; steady driving needs 4.7-6.9 MiB and the worst 1/30 s is 12 MiB, so it rarely binds."),
        ECVF_RenderThreadSafe);

    // UE's texture-eviction line, r.Vulkan.EvictionLimitPercentage.
    constexpr double EvictionLimitFraction = 0.70;

    // One structured buffer cannot pass 4 GiB, and PackedA is the widest stream at
    // 16 B per slot, so it binds first: 4 GiB / 16 B = 268,435,456 slots, which is
    // 6 GiB across all three buffers. Nothing in the RHI or RDG checks this -- the
    // size is a uint32 and the multiply wraps -- so the plugin must.
    constexpr int64 MaxSlotsPerBuffer = (4ll << 30) / 16;
    constexpr int64 MaxPoolBytes = MaxSlotsPerBuffer * FGaussianSplatPagePool::BytesPerSlot;
}

int32 FGaussianSplatPoolResidency::GetResidentSplats(int32 CellIndex) const
{
    if (Asset == nullptr || !Asset->Cells.IsValidIndex(CellIndex))
    {
        return 0;
    }

    const FGaussianSplatPagedCell& Cell = Asset->Cells[CellIndex];
    const int64 Pages = ResidentTailPages.IsValidIndex(CellIndex) ? ResidentTailPages[CellIndex] : 0;
    const int64 TailCount = static_cast<int64>(Cell.Count) - Cell.FloorCount;
    const int64 ResidentTail = FMath::Min(Pages * FGaussianSplatPagePool::SlotsPerPage, TailCount);
    return static_cast<int32>(Cell.FloorCount + ResidentTail);
}

int32 FGaussianSplatPoolResidency::GetTailPageSlot(
    const UGaussianSplatPagedAsset& InAsset,
    int32 CellIndex,
    int32 PageInCell) const
{
    if (!InAsset.Cells.IsValidIndex(CellIndex))
    {
        return INDEX_NONE;
    }
    const FGaussianSplatPagedCell& Cell = InAsset.Cells[CellIndex];
    if (PageInCell < 0 || PageInCell >= Cell.TailPageCount)
    {
        return INDEX_NONE;
    }
    const int32 Global = Cell.FirstTailPage + PageInCell;
    return TailPageSlot.IsValidIndex(Global) ? TailPageSlot[Global] : INDEX_NONE;
}

uint32 BuildPoolRuns(
    const UGaussianSplatPagedAsset& Asset,
    const FGaussianSplatPoolResidency& Residency,
    int32 CellIndex,
    uint32 Take,
    TArray<FGaussianSplatPoolRun>& OutRuns)
{
    OutRuns.Reset();
    if (!Asset.Cells.IsValidIndex(CellIndex) || Take == 0)
    {
        return 0;
    }

    const FGaussianSplatPagedCell& Cell = Asset.Cells[CellIndex];
    const auto Append = [&OutRuns](uint32 FirstSlot, uint32 Count)
    {
        if (Count == 0)
        {
            return;
        }
        // Adjacent in slot space AND adjacent in dispatch order, which they are by
        // construction here, so the two become one entry.
        if (!OutRuns.IsEmpty() && OutRuns.Last().FirstSlot + OutRuns.Last().Count == FirstSlot)
        {
            OutRuns.Last().Count += Count;
            return;
        }
        OutRuns.Add({FirstSlot, Count});
    };

    uint32 Covered = 0;

    // The floor block, which is always resident.
    const uint32 FloorTake = FMath::Min<uint32>(Take, Cell.FloorCount);
    if (FloorTake > 0)
    {
        Append(static_cast<uint32>(Residency.FloorFirstSlot + Asset.GetFloorFirstSlot(CellIndex)), FloorTake);
        Covered += FloorTake;
    }

    // Then the resident tail pages, one run each, merged wherever two land in
    // adjacent slots. Step 1a allocated a cell's pages contiguously and so got
    // ONE entry per cell (measured: entries == cell count at G1d); streaming
    // reuses freed slots, so adjacency is likely but no longer guaranteed, and
    // this is where D6's entry count can grow. The stats line reports it.
    const int32 ResidentPages = Residency.ResidentTailPages.IsValidIndex(CellIndex)
        ? Residency.ResidentTailPages[CellIndex]
        : 0;
    if (ResidentPages <= 0 || Covered >= Take)
    {
        return Covered;
    }

    const int64 TailCount = static_cast<int64>(Cell.Count) - Cell.FloorCount;
    for (int32 PageInCell = 0; PageInCell < ResidentPages && Covered < Take; ++PageInCell)
    {
        const int32 Slot = Residency.GetTailPageSlot(Asset, CellIndex, PageInCell);
        if (Slot == INDEX_NONE)
        {
            // Residency is a prefix by invariant, so a hole means the page table
            // and the count disagree. Stopping is correct: drawing past a hole
            // would read whatever the slot held before.
            break;
        }

        // The last page of a cell is partial, so it carries only what is left.
        const int64 PageSplats = FMath::Min<int64>(
            FGaussianSplatPagePool::SlotsPerPage,
            TailCount - static_cast<int64>(PageInCell) * FGaussianSplatPagePool::SlotsPerPage);
        if (PageSplats <= 0)
        {
            break;
        }

        const uint32 PageTake = static_cast<uint32>(FMath::Min<int64>(Take - Covered, PageSplats));
        Append(static_cast<uint32>(static_cast<int64>(Slot) * FGaussianSplatPagePool::SlotsPerPage), PageTake);
        Covered += PageTake;
    }
    return Covered;
}

FGaussianSplatPagePool& FGaussianSplatPagePool::Get()
{
    // One per process, for the life of the process: worlds come and go, the pool
    // does not (plan D3, review 1 M1).
    static FGaussianSplatPagePool Pool;
    return Pool;
}

bool FGaussianSplatPagePool::ArePagedAssetsEnabled()
{
    return CVarPagedAssets.GetValueOnAnyThread() != 0;
}

int64 FGaussianSplatPagePool::DeriveSizeBytes() const
{
    const int64 OverrideMB = CVarPoolMB.GetValueOnAnyThread();
    const int32 ReserveMB = GIsEditor
        ? CVarPoolReserveMBEditor.GetValueOnAnyThread()
        : CVarPoolReserveMB.GetValueOnAnyThread();
    const int64 ReserveBytes = static_cast<int64>(FMath::Max(0, ReserveMB)) << 20;
    const int32 CapMode = CVarPoolCapMode.GetValueOnAnyThread();

    if (OverrideMB > 0)
    {
        const int64 Bytes = FMath::Min(OverrideMB << 20, MaxPoolBytes);
        UE_LOG(
            LogGaussianSplatPool,
            Display,
            TEXT("Pool size %lld MiB, set by r.GaussianSplat.PoolMB (cap %lld MiB)."),
            Bytes >> 20,
            MaxPoolBytes >> 20);
        return Bytes;
    }

    FDeviceMemory Memory;
    if (!QueryDeviceMemory(Memory) || Memory.Size == 0)
    {
        // No budget query on this RHI. 512 MiB is small enough to be safe
        // anywhere and large enough to draw a street scene's floor blocks.
        UE_LOG(
            LogGaussianSplatPool,
            Warning,
            TEXT("No memory budget query on this RHI: the pool falls back to 512 MiB. Set r.GaussianSplat.PoolMB."));
        return 512ll << 20;
    }

    const int64 EvictionLine = static_cast<int64>(Memory.Size * EvictionLimitFraction);
    const int64 Ceiling = CapMode == 0 ? EvictionLine : static_cast<int64>(Memory.Budget);
    const int64 Headroom = Ceiling - static_cast<int64>(Memory.Usage) - ReserveBytes;
    const int64 Bytes = FMath::Clamp<int64>(Headroom, 0, MaxPoolBytes);

    UE_LOG(
        LogGaussianSplatPool,
        Display,
        TEXT("Pool size derived: %lld MiB = min(cap %lld, %s %.0f - usage now %.0f - reserve %lld) | card %.0f MiB, ")
        TEXT("budget %.0f MiB, eviction line %.0f MiB (70%%), cap mode %d"),
        Bytes >> 20,
        MaxPoolBytes >> 20,
        CapMode == 0 ? TEXT("eviction line") : TEXT("budget"),
        ToMiB(Ceiling),
        ToMiB(Memory.Usage),
        ReserveBytes >> 20,
        ToMiB(Memory.Size),
        ToMiB(Memory.Budget),
        ToMiB(EvictionLine),
        CapMode);

    if (Bytes <= 0)
    {
        UE_LOG(
            LogGaussianSplatPool,
            Error,
            TEXT("The derivation leaves nothing for the pool: UE already uses %.0f MiB of a %.0f MiB ceiling and the ")
            TEXT("reserve is %lld MiB. Lower r.GaussianSplat.PoolReserveMB or set r.GaussianSplat.PoolMB."),
            ToMiB(Memory.Usage),
            ToMiB(Ceiling),
            ReserveBytes >> 20);
    }
    return Bytes;
}

void FGaussianSplatPagePool::EnsureAllocated()
{
    if (IsAllocated())
    {
        return;
    }

    // What the derivation depends on. If none of it has changed since a refusal,
    // the answer has not changed either, and repeating it once a frame helps
    // nobody. Touching any of these three asks the question again.
    const int32 Settings =
        CVarPoolMB.GetValueOnAnyThread() * 7919
        + CVarPoolReserveMB.GetValueOnAnyThread() * 104729
        + CVarPoolReserveMBEditor.GetValueOnAnyThread() * 1299709
        + CVarPoolCapMode.GetValueOnAnyThread();
    if (bSizingFailed && Settings == FailedSettings)
    {
        return;
    }

    const int64 Bytes = DeriveSizeBytes();
    const int64 Pages = Bytes / (SlotsPerPage * BytesPerSlot);
    if (Pages <= 0)
    {
        bSizingFailed = true;
        FailedSettings = Settings;
        return;
    }
    const int64 Slots = Pages * SlotsPerPage;

    // Registering it puts the pool in the global resource list, which is what makes
    // ReleaseRHI run BEFORE the RHI shuts down. Without this the pooled buffers outlive
    // the RHI and take the process down on exit (Step 0's V6 note).
    if (!IsInitialized())
    {
        BeginInitResource(this);
    }

    FGaussianSplatPagePool* Self = this;
    ENQUEUE_RENDER_COMMAND(GaussianSplatPoolAllocate)(
        [Self, Slots](FRHICommandListImmediate& RHICmdList)
        {
            FDeviceMemory Before;
            const bool bBefore = QueryDeviceMemory(Before);

            Self->PackedA = AllocatePooledBuffer(
                FRDGBufferDesc::CreateStructuredDesc(16, static_cast<uint32>(Slots)), TEXT("GaussianSplat.PoolA"));
            Self->PackedB = AllocatePooledBuffer(
                FRDGBufferDesc::CreateStructuredDesc(4, static_cast<uint32>(Slots)), TEXT("GaussianSplat.PoolB"));
            // Allocated even for a capture with no SH: the pool exists before any
            // asset registers, so it cannot know whether one will need it, and a
            // second pool size per capture is not worth a sixth of the memory.
            Self->SHIndex = AllocatePooledBuffer(
                FRDGBufferDesc::CreateStructuredDesc(4, static_cast<uint32>(Slots)), TEXT("GaussianSplat.PoolSH"));

            FDeviceMemory After;
            const bool bAfter = QueryDeviceMemory(After);
            const double Wanted = ToMiB(Slots * BytesPerSlot);
            const double Delta = (bBefore && bAfter) ? ToMiB(After.Usage) - ToMiB(Before.Usage) : 0.0;
            Self->bInVideoMemory = !(bBefore && bAfter) || Delta >= 0.9 * Wanted;

            UE_LOG(
                LogGaussianSplatPool,
                Display,
                TEXT("Pool allocated: %.0f MiB, %lld slots in %lld pages | device-local usage %.0f -> %.0f MiB (%+.0f) ")
                TEXT("| placement %s"),
                Wanted,
                Slots,
                Slots / SlotsPerPage,
                bBefore ? ToMiB(Before.Usage) : 0.0,
                bAfter ? ToMiB(After.Usage) : 0.0,
                Delta,
                Self->bInVideoMemory ? TEXT("VRAM") : TEXT("NOT VRAM"));

            if (!Self->bInVideoMemory)
            {
                UE_LOG(
                    LogGaussianSplatPool,
                    Error,
                    TEXT("The pool did not land in device-local memory. Synchronous-mode streaming is refused: every ")
                    TEXT("page read would cross PCIe. Lower r.GaussianSplat.PoolMB or raise PoolReserveMB."));
            }
        });

    // The bookkeeping is the game thread's, and does not depend on the RHI work
    // above having finished -- nothing may be handed out until a later tick.
    TotalSlots = Slots;
    NextUnusedSlot = 0;
    FreePages.Reset();
}

int64 FGaussianSplatPagePool::AllocateFloorRange(int64 Slots)
{
    if (Slots <= 0 || NextUnusedSlot + Slots > TotalSlots)
    {
        return INDEX_NONE;
    }
    const int64 First = NextUnusedSlot;
    NextUnusedSlot += Slots;
    return First;
}

int32 FGaussianSplatPagePool::AllocatePages(int32 PageCount)
{
    if (PageCount <= 0)
    {
        return INDEX_NONE;
    }

    // Contiguous from the untouched space whenever it is there: a cell whose
    // pages are adjacent costs ONE range entry instead of one per page, which is
    // what keeps the lookup out of the expensive regime (plan D6).
    //
    // ALIGNED, because a page is addressed by its INDEX: page p lives at slot
    // p * SlotsPerPage, so the page region has to begin on a page boundary. The
    // floor blocks before it are variable-size and leave NextUnusedSlot wherever
    // they end. Truncating here instead of rounding up put the first page BELOW
    // the floor block's end and the upload overwrote it: Uno's floor is 418,347
    // slots, 418,347 / 4,096 = 102.13, so page 102 started 555 slots inside the
    // floor and corrupted 555 splats -- about ten of which were visible in two of
    // six cameras (2026-10-07).
    const int64 Needed = static_cast<int64>(PageCount) * SlotsPerPage;
    const int64 Aligned = FMath::DivideAndRoundUp<int64>(NextUnusedSlot, SlotsPerPage) * SlotsPerPage;
    if (Aligned + Needed <= TotalSlots)
    {
        NextUnusedSlot = Aligned + Needed;
        return static_cast<int32>(Aligned / SlotsPerPage);
    }

    // Otherwise the free list, which only has pages once something unregistered.
    if (FreePages.Num() >= PageCount)
    {
        FreePages.Sort();
        for (int32 Start = 0; Start + PageCount <= FreePages.Num(); ++Start)
        {
            if (FreePages[Start + PageCount - 1] - FreePages[Start] == PageCount - 1)
            {
                const int32 FirstPage = FreePages[Start];
                FreePages.RemoveAt(Start, PageCount, EAllowShrinking::No);
                return FirstPage;
            }
        }
    }
    return INDEX_NONE;
}

void FGaussianSplatPagePool::FreePagesOf(FGaussianSplatPoolResidency& Residency)
{
    // The page table is the one source of truth for what this asset holds, so an
    // unregister returns exactly those slots -- whether they were handed out in
    // one contiguous run at registration or one at a time by the gate.
    for (const int32 Slot : Residency.TailPageSlot)
    {
        if (Slot != INDEX_NONE)
        {
            FreePages.Add(Slot);
        }
    }
    Residency.TailPageSlot.Reset();
    Residency.ResidentTailPages.Reset();
    Residency.ResidentPageCount = 0;
    Residency.LastNeededTick.Reset();
    Residency.EvictedTick.Reset();
    Residency.ResidentSplats = 0;
}

bool FGaussianSplatPagePool::IsStreamingEnabled()
{
    return ArePagedAssetsEnabled() && CVarStream.GetValueOnAnyThread() != 0;
}

int32 FGaussianSplatPagePool::AcquirePage()
{
    if (FreePages.Num() > 0)
    {
        return FreePages.Pop(EAllowShrinking::No);
    }

    // The untouched frontier, page-aligned for the same reason AllocatePages is:
    // a page is addressed by its INDEX, so page p must begin at slot p * SlotsPerPage.
    // Getting this wrong cost an afternoon in Step 1a (it silently overwrote 555
    // floor splats), so the alignment is computed, never assumed.
    const int64 Aligned = FMath::DivideAndRoundUp<int64>(NextUnusedSlot, SlotsPerPage) * SlotsPerPage;
    if (Aligned + SlotsPerPage > TotalSlots)
    {
        return INDEX_NONE;
    }
    NextUnusedSlot = Aligned + SlotsPerPage;
    return static_cast<int32>(Aligned / SlotsPerPage);
}

void FGaussianSplatPagePool::ReleasePage(int32 PageIndex)
{
    if (PageIndex != INDEX_NONE)
    {
        FreePages.Add(PageIndex);
    }
}

int32 FGaussianSplatPagePool::GetFreePageCount() const
{
    const int64 Frontier = (TotalSlots - NextUnusedSlot) / SlotsPerPage;
    return static_cast<int32>(FMath::Min<int64>(FreePages.Num() + FMath::Max<int64>(0, Frontier), MAX_int32));
}

void FGaussianSplatPagePool::GetRegisteredAssets(TArray<const UGaussianSplatPagedAsset*>& Out) const
{
    Out.Reset(Residencies.Num());
    for (const TPair<const UGaussianSplatPagedAsset*, TUniquePtr<FGaussianSplatPoolResidency>>& Pair : Residencies)
    {
        Out.Add(Pair.Key);
    }
}

namespace
{
    // One structured buffer filled from Data, on the render thread.
    void MakePooledBuffer(
        FRHICommandListImmediate& RHICmdList,
        const TCHAR* Name,
        uint32 Stride,
        const void* Data,
        uint32 Count,
        TRefCountPtr<FRDGPooledBuffer>& OutBuffer)
    {
        OutBuffer = AllocatePooledBuffer(FRDGBufferDesc::CreateStructuredDesc(Stride, Count), Name);
        void* Dest = RHICmdList.LockBuffer(OutBuffer->GetRHI(), 0, Stride * Count, RLM_WriteOnly);
        FMemory::Memcpy(Dest, Data, Stride * Count);
        RHICmdList.UnlockBuffer(OutBuffer->GetRHI());
    }
}

void FGaussianSplatPagePool::RebuildSharedCellBounds()
{
    // Fix 5 Step 3 (D7): every registered asset's cells in ONE buffer, so a single
    // draw across districts can decode any splat's position from a global cell
    // index. Rebuilt whole rather than patched: this runs when an asset registers
    // or unregisters -- at level load -- and the whole thing is tens of KB
    // (Lublin's ~10K cells are 320 KB), so the simple version is the right one.
    TArray<FVector4f> Bounds;
    int32 NextBase = 0;
    for (TPair<const UGaussianSplatPagedAsset*, TUniquePtr<FGaussianSplatPoolResidency>>& Pair : Residencies)
    {
        const UGaussianSplatPagedAsset* Asset = Pair.Key;
        Pair.Value->CellBoundsBase = NextBase;
        if (Asset == nullptr)
        {
            continue;
        }
        Bounds.Reserve(Bounds.Num() + Asset->Cells.Num() * 2);
        for (const FGaussianSplatPagedCell& Cell : Asset->Cells)
        {
            // Built from the same CellExtent the importer packed with, so a
            // one-splat-wide cell cannot divide by zero here and not there.
            const FVector3f Extent = GaussianSplatFormat::CellExtent(Cell.BoundsMin, Cell.BoundsMax);
            Bounds.Add(FVector4f(Cell.BoundsMin.X, Cell.BoundsMin.Y, Cell.BoundsMin.Z, 0.0f));
            Bounds.Add(FVector4f(Extent.X, Extent.Y, Extent.Z, 0.0f));
        }
        NextBase += Asset->Cells.Num();
    }
    if (Bounds.IsEmpty())
    {
        Bounds.Add(FVector4f(ForceInitToZero));
        Bounds.Add(FVector4f(ForceInitToZero));
    }

    // Diagnostic for the eight-asset GPU hang (2026-10-09): seven assets draw, eight hang, and sixteen
    // IDENTICAL ones draw, which no count threshold explains. This table is the one structure that spans
    // assets, so print what it actually contains -- per asset, and the total -- so a working run and a
    // hanging one can be diffed line for line. Cheap and once per registration.
    {
        int32 Printed = 0;
        for (const TPair<const UGaussianSplatPagedAsset*, TUniquePtr<FGaussianSplatPoolResidency>>& Pair : Residencies)
        {
            if (Pair.Key == nullptr || !Pair.Value.IsValid())
            {
                continue;
            }
            UE_LOG(LogGaussianSplatPool, Display,
                   TEXT("CELLBOUNDS asset %2d %-24s cells %6d base %7d floorSlots %10lld tailPages %7d"),
                   Printed, *Pair.Key->GetName(), Pair.Key->Cells.Num(), Pair.Value->CellBoundsBase,
                   static_cast<long long>(Pair.Value->FloorSlots), Pair.Key->TailPages);
            ++Printed;
        }
        UE_LOG(LogGaussianSplatPool, Display,
               TEXT("CELLBOUNDS total %d assets, %d entries (%d bytes), NextBase %d"),
               Printed, Bounds.Num(), Bounds.Num() * static_cast<int32>(sizeof(FVector4f)), NextBase);
    }

    FGaussianSplatPagePool* Self = this;
    ENQUEUE_RENDER_COMMAND(GaussianSplatPoolCellBounds)(
        [Self, Bounds = MoveTemp(Bounds)](FRHICommandListImmediate& RHICmdList)
        {
            MakePooledBuffer(RHICmdList, TEXT("GaussianSplat.PagedCellBounds"), sizeof(FVector4f),
                             Bounds.GetData(), static_cast<uint32>(Bounds.Num()), Self->SharedCellBounds);
        });
}

void FGaussianSplatPagePool::GetSHPaletteSRVs(FRHIShaderResourceView* Out[]) const
{
    FRHIShaderResourceView* const Dummy = DummyPalette.IsValid() ? DummyPalette->GetSRV() : nullptr;
    for (int32 Slot = 0; Slot < MaxSHPalettes; ++Slot)
    {
        Out[Slot] = Dummy;
    }
    for (int32 Slot = 0; Slot < MaxSHPalettes; ++Slot)
    {
        if (PaletteBuffers[Slot].IsValid())
        {
            Out[Slot] = PaletteBuffers[Slot]->GetSRV();
        }
    }
}

void FGaussianSplatPagePool::BuildAssetBuffers(
    const UGaussianSplatPagedAsset& Asset,
    FGaussianSplatPoolResidency& Residency)
{
    // An SH0 asset takes no palette slot at all: HasSH gates the read, so it never
    // samples one (review M5).
    if (!Asset.SHPalette.IsEmpty())
    {
        for (int32 Slot = 0; Slot < MaxSHPalettes; ++Slot)
        {
            if (PaletteSlotOwner[Slot] == nullptr || PaletteSlotOwner[Slot] == &Asset)
            {
                PaletteSlotOwner[Slot] = &Asset;
                Residency.PaletteSlot = Slot;
                break;
            }
        }
        if (Residency.PaletteSlot == INDEX_NONE)
        {
            // Refused loudly rather than drawn with another asset's colours. Raising
            // the limit is a constant here and a case in the vertex shader's switch.
            UE_LOG(
                LogGaussianSplatPool,
                Warning,
                TEXT("SH PALETTE SLOTS FULL: %s has spherical harmonics but all %d slots are taken, so it will draw ")
                TEXT("WITHOUT them. One world may hold %d assets with SH; SH0 assets take no slot."),
                *Asset.GetName(),
                MaxSHPalettes,
                MaxSHPalettes);
        }
    }

    // By VALUE, and only the slot. Capturing &Residency here was a use-after-free:
    // Residencies is a TMap, every later RegisterAsset Adds to it, and an Add
    // reallocates -- so by the time the render thread drained this command the
    // pointer could name freed memory. One asset never showed it; the five-district
    // map crashed on every load, always on the fifth (2026-10-09). The pool is
    // stable, so the buffer goes there and the slot is what crosses the threads.
    FGaussianSplatPagePool* Self = this;
    const int32 Slot = Residency.PaletteSlot;
    TArray<float> Palette = (Slot != INDEX_NONE) ? Asset.SHPalette : TArray<float>();
    ENQUEUE_RENDER_COMMAND(GaussianSplatPoolAssetBuffers)(
        [Self, Slot, Palette = MoveTemp(Palette)](FRHICommandListImmediate& RHICmdList)
        {
            // Unconditionally, and FIRST: an all-SH0 world builds no palette at all,
            // and every slot of the shader's SRV array still has to be bound.
            if (!Self->DummyPalette.IsValid())
            {
                const float Zero = 0.0f;
                MakePooledBuffer(RHICmdList, TEXT("GaussianSplat.PagedSHPaletteDummy"), sizeof(float),
                                 &Zero, 1, Self->DummyPalette);
            }
            if (Slot != INDEX_NONE && Palette.Num() > 0)
            {
                MakePooledBuffer(RHICmdList, TEXT("GaussianSplat.PagedSHPalette"), sizeof(float),
                                 Palette.GetData(), static_cast<uint32>(Palette.Num()),
                                 Self->PaletteBuffers[Slot]);
            }
        });

    RebuildSharedCellBounds();
}

void FGaussianSplatPagePool::UploadRange(
    int64 FirstSlot,
    const uint8* SourceA,
    const uint8* SourceB,
    const uint8* SourceSH,
    int64 Slots)
{
    if (Slots <= 0 || !IsAllocated())
    {
        return;
    }

    // TArray counts are int32. One call never carries more than a cell's pages or one
    // asset's floor block, so this cannot trip today -- but it would wrap silently rather
    // than fail if it ever did, which is not a way to find out.
    if (Slots * 16 > MAX_int32)
    {
        UE_LOG(
            LogGaussianSplatPool,
            Error,
            TEXT("An upload of %lld slots is past what one call can carry. Nothing was uploaded."),
            Slots);
        return;
    }

    // Step 1a uploads once, at load, so a plain locked write is enough and honest.
    // Step 2's per-tick path is the dynamic upload buffer plus a scatter dispatch
    // (plan D5); this is not that, and is not on the frame loop.
    TArray<uint8> CopyA(SourceA, static_cast<int32>(Slots * 16));
    TArray<uint8> CopyB(SourceB, static_cast<int32>(Slots * 4));
    TArray<uint8> CopySH;
    if (SourceSH != nullptr)
    {
        CopySH.Append(SourceSH, static_cast<int32>(Slots * 4));
    }

    FGaussianSplatPagePool* Self = this;
    ENQUEUE_RENDER_COMMAND(GaussianSplatPoolUpload)(
        [Self, FirstSlot, Slots, A = MoveTemp(CopyA), B = MoveTemp(CopyB), SH = MoveTemp(CopySH)]
        (FRHICommandListImmediate& RHICmdList)
        {
            const auto Write = [&RHICmdList](const TRefCountPtr<FRDGPooledBuffer>& Buffer,
                                             const TArray<uint8>& Data, int64 Offset)
            {
                if (!Buffer.IsValid() || Data.Num() == 0)
                {
                    return;
                }
                void* Dest = RHICmdList.LockBuffer(Buffer->GetRHI(), Offset, Data.Num(), RLM_WriteOnly);
                FMemory::Memcpy(Dest, Data.GetData(), Data.Num());
                RHICmdList.UnlockBuffer(Buffer->GetRHI());
            };

            Write(Self->PackedA, A, FirstSlot * 16);
            Write(Self->PackedB, B, FirstSlot * 4);
            Write(Self->SHIndex, SH, FirstSlot * 4);
        });
}

// D5's scatter shader. Three destination buffers, because a page lands in three
// of them, which is exactly why a copy-based upload costs three passes per page.
class FGaussianSplatPageScatterCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatPageScatterCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatPageScatterCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // Raw, not RDG: the staging buffer is BUF_Dynamic host memory written
        // before the graph opens, so RDG has nothing to track and no barrier to
        // insert (D5; the Step 0 probe validated this shape).
        SHADER_PARAMETER_SRV(StructuredBuffer<uint4>, SrcPages)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, DestSlots)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, DstPackedA)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DstPackedB)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DstSHIndex)
        SHADER_PARAMETER(uint32, PageCount)
        SHADER_PARAMETER(uint32, HasSH)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

IMPLEMENT_GLOBAL_SHADER(
    FGaussianSplatPageScatterCS,
    "/GaussianSplatting/Private/GaussianSplatPageScatter.usf",
    "MainCS",
    SF_Compute);

void FGaussianSplatPagePool::UploadPageBatch(
    const UGaussianSplatPagedAsset& Asset,
    const TArray<int32>& GlobalPages,
    const TArray<int32>& DestPageSlots)
{
    if (GlobalPages.Num() == 0 || GlobalPages.Num() != DestPageSlots.Num() || !IsAllocated())
    {
        return;
    }

    const bool bHasSH = !Asset.SHPalette.IsEmpty();
    const int64 PageBytesA = SlotsPerPage * GAUSSIAN_SPLAT_PACKED_A_STRIDE;
    const int64 PageBytesSmall = SlotsPerPage * GAUSSIAN_SPLAT_PACKED_B_STRIDE;
    const int64 PageBytes = PageBytesA + PageBytesSmall + (bHasSH ? PageBytesSmall : 0);
    const int64 MaxCommandBytes = static_cast<int64>(
        FMath::Max(1, CVarStreamMaxUploadMBPerCommand.GetValueOnAnyThread())) * 1024 * 1024;
    const int32 PagesPerCommand = FMath::Max(1, static_cast<int32>(MaxCommandBytes / PageBytes));
    const bool bParallel = CVarStreamParallelCopy.GetValueOnAnyThread() != 0;

    const TConstArrayView64<uint8> Payload = Asset.GetPayload();
    const uint8* const SourceA = Payload.GetData() + Asset.GetTailOffsetA();
    const uint8* const SourceB = Payload.GetData() + Asset.GetTailOffsetB();
    const uint8* const SourceSH = bHasSH ? Payload.GetData() + Asset.GetTailOffsetSH() : nullptr;

    // Split so no single render command carries more than the cap. A teleport's
    // ~1 GB fill becomes several bounded commands in the same tick, rather than
    // one that could sit on the render thread long enough to matter.
    //
    // This path had never actually run before Step 4: no single-district fill was
    // large enough to exceed the cap, so every upload was one command. A districts
    // teleport is the first that can split, and an untested path should say so the
    // first time it runs rather than be assumed to work (review, Step 4).
    const int32 CommandCount = FMath::DivideAndRoundUp(GlobalPages.Num(), PagesPerCommand);
    if (CommandCount > 1)
    {
        static bool bLoggedSplit = false;
        UE_LOG(
            LogGaussianSplatPool,
            Display,
            TEXT("Upload SPLIT%s: %d pages (%.0f MiB) for %s across %d commands of at most %d pages (%d MiB cap)."),
            bLoggedSplit ? TEXT("") : TEXT(" -- first time this path has run"),
            GlobalPages.Num(),
            GlobalPages.Num() * PageBytes / 1048576.0,
            *Asset.GetName(),
            CommandCount,
            PagesPerCommand,
            FMath::Max(1, CVarStreamMaxUploadMBPerCommand.GetValueOnAnyThread()));
        bLoggedSplit = true;
    }

    for (int32 First = 0; First < GlobalPages.Num(); First += PagesPerCommand)
    {
        const int32 Count = FMath::Min(PagesPerCommand, GlobalPages.Num() - First);
        TArray<int32> Pages(GlobalPages.GetData() + First, Count);
        TArray<uint32> Slots;
        Slots.SetNumUninitialized(Count);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Slots[Index] = static_cast<uint32>(DestPageSlots[First + Index]);
        }

        FGaussianSplatPagePool* Self = this;
        const int64 Bytes = static_cast<int64>(Count) * PageBytes;
        ENQUEUE_RENDER_COMMAND(GaussianSplatPageScatter)(
            [Self, Pages = MoveTemp(Pages), Slots = MoveTemp(Slots), Bytes, PageBytes, PageBytesA,
             PageBytesSmall, bHasSH, bParallel, SourceA, SourceB, SourceSH]
            (FRHICommandListImmediate& RHICmdList)
            {
                // BUF_Dynamic: each lock hands back a FRESH host-visible block that
                // the RHI thread swaps in, so there is no copy at unlock and no
                // barrier before it. A static buffer's unlock records a copy with
                // nothing ordering it against the previous tick's scatter still
                // reading -- rare, non-deterministic corruption (D5).
                FRHIResourceCreateInfo CreateInfo(TEXT("GaussianSplat.PageUpload"));
                FBufferRHIRef Staging = RHICmdList.CreateStructuredBuffer(
                    16, Bytes, BUF_Dynamic | BUF_ShaderResource, CreateInfo);
                if (!Staging.IsValid())
                {
                    return;
                }

                uint8* const Mapped = static_cast<uint8*>(RHICmdList.LockBuffer(Staging, 0, Bytes, RLM_WriteOnly));
                if (Mapped == nullptr)
                {
                    return;
                }

                // One page's three parts, contiguous in staging, which is what lets
                // the fill be a ParallelFor over pages (review m2).
                const auto FillPage = [&](int32 Index)
                {
                    const int64 Page = Pages[Index];
                    const int64 SourceFirstSlot = Page * SlotsPerPage;
                    uint8* const Dest = Mapped + static_cast<int64>(Index) * PageBytes;
                    FMemory::Memcpy(Dest, SourceA + SourceFirstSlot * GAUSSIAN_SPLAT_PACKED_A_STRIDE, PageBytesA);
                    FMemory::Memcpy(Dest + PageBytesA, SourceB + SourceFirstSlot * GAUSSIAN_SPLAT_PACKED_B_STRIDE, PageBytesSmall);
                    if (bHasSH)
                    {
                        FMemory::Memcpy(
                            Dest + PageBytesA + PageBytesSmall,
                            SourceSH + SourceFirstSlot * GAUSSIAN_SPLAT_SH_INDEX_STRIDE,
                            PageBytesSmall);
                    }
                };
                if (bParallel)
                {
                    ParallelFor(Pages.Num(), FillPage);
                }
                else
                {
                    for (int32 Index = 0; Index < Pages.Num(); ++Index) { FillPage(Index); }
                }
                RHICmdList.UnlockBuffer(Staging);

                FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("GaussianSplat.PageScatter"));
                FRDGBufferRef SlotBuffer = CreateStructuredBuffer(
                    GraphBuilder, TEXT("GaussianSplat.PageDestSlots"),
                    sizeof(uint32), Slots.Num(), Slots.GetData(), Slots.Num() * sizeof(uint32));

                FGaussianSplatPageScatterCS::FParameters* Parameters =
                    GraphBuilder.AllocParameters<FGaussianSplatPageScatterCS::FParameters>();
                Parameters->SrcPages = RHICmdList.CreateShaderResourceView(Staging);
                Parameters->DestSlots = GraphBuilder.CreateSRV(SlotBuffer);
                Parameters->DstPackedA = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(Self->PackedA));
                Parameters->DstPackedB = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(Self->PackedB));
                Parameters->DstSHIndex = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(Self->SHIndex));
                Parameters->PageCount = static_cast<uint32>(Pages.Num());
                Parameters->HasSH = bHasSH ? 1u : 0u;

                const uint32 Uint4sPerPage = static_cast<uint32>(PageBytes / 16);
                TShaderMapRef<FGaussianSplatPageScatterCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
                FComputeShaderUtils::AddPass(
                    GraphBuilder,
                    RDG_EVENT_NAME("GaussianSplat.PageScatter %d pages", Pages.Num()),
                    Shader,
                    Parameters,
                    FComputeShaderUtils::GetGroupCount(Pages.Num() * Uint4sPerPage, 256));
                GraphBuilder.Execute();
            });
    }
}

const FGaussianSplatPoolResidency* FGaussianSplatPagePool::FindResidency(const UGaussianSplatPagedAsset* Asset) const
{
    const TUniquePtr<FGaussianSplatPoolResidency>* const Found = Residencies.Find(Asset);
    return Found ? Found->Get() : nullptr;
}

const FGaussianSplatPoolResidency* FGaussianSplatPagePool::RegisterAsset(const UGaussianSplatPagedAsset* Asset)
{
    if (Asset == nullptr || Asset->Cells.IsEmpty())
    {
        return nullptr;
    }
    if (const TUniquePtr<FGaussianSplatPoolResidency>* const Existing = Residencies.Find(Asset))
    {
        return Existing->Get();
    }

    EnsureAllocated();
    if (!IsAllocated() || !Asset->IsPayloadResident())
    {
        UE_LOG(
            LogGaussianSplatPool,
            Error,
            TEXT("%s cannot register: %s."),
            *Asset->GetName(),
            IsAllocated() ? TEXT("its payload is not in RAM") : TEXT("the pool has no slots"));
        return nullptr;
    }

    const TConstArrayView64<uint8> Payload = Asset->GetPayload();
    const bool bHasSH = Asset->HasSH();

    FGaussianSplatPoolResidency Residency;
    Residency.Asset = Asset;

    // The floor block first and whole. It is the asset's guarantee that a visible
    // cell always has something to draw, so an asset that cannot fit its floor
    // does not register at all rather than drawing holes.
    Residency.FloorSlots = Asset->FloorSplats;
    Residency.FloorFirstSlot = AllocateFloorRange(Residency.FloorSlots);
    if (Residency.FloorFirstSlot == INDEX_NONE)
    {
        UE_LOG(
            LogGaussianSplatPool,
            Error,
            TEXT("%s does not fit: its floor blocks alone need %.0f MiB and the pool has %.0f MiB free. Raise ")
            TEXT("r.GaussianSplat.PoolMB, or lower the asset's floor fraction and re-import."),
            *Asset->GetName(),
            ToMiB(Residency.FloorSlots * BytesPerSlot),
            ToMiB(GetFreeSlots() * BytesPerSlot));
        return nullptr;
    }

    UploadRange(
        Residency.FloorFirstSlot,
        Payload.GetData() + Asset->GetFloorOffsetA(),
        Payload.GetData() + Asset->GetFloorOffsetB(),
        bHasSH ? Payload.GetData() + Asset->GetFloorOffsetSH() : nullptr,
        Residency.FloorSlots);
    Residency.ResidentSplats = Residency.FloorSlots;

    // The per-page tables, sized once. int32 per entry over Lublin's ~1.45M pages
    // is ~17 MiB of RAM for all three, which is the price of being able to move a
    // single page rather than a cell's whole run.
    const int32 TotalTailPages = static_cast<int32>(FMath::Min<int64>(Asset->TailPages, MAX_int32));
    Residency.TailPageSlot.Init(INDEX_NONE, TotalTailPages);
    Residency.ResidentTailPages.Init(0, Asset->Cells.Num());

    if (IsStreamingEnabled())
    {
        // Streaming: the floor is all that registration uploads. Every tail page
        // is the gate's to move, which is what lets an asset be larger than the
        // pool -- Lublin is 5,928 MiB against a pool this card derives at ~2,158.
        Residency.LastNeededTick.Init(MIN_int32, TotalTailPages);
        Residency.EvictedTick.Init(MIN_int32, TotalTailPages);
        Residency.bComplete = Asset->TailPages == 0;

        UE_LOG(
            LogGaussianSplatPool,
            Display,
            TEXT("%s registered for STREAMING: floor %lld splats resident (%.0f MiB), %lld tail pages left to the ")
            TEXT("gate (%.0f MiB if all were resident), pool %.0f of %.0f MiB used."),
            *Asset->GetName(),
            Residency.FloorSlots,
            ToMiB(Residency.FloorSlots * BytesPerSlot),
            Asset->TailPages,
            ToMiB(Asset->TailPages * SlotsPerPage * BytesPerSlot),
            ToMiB((TotalSlots - GetFreeSlots()) * BytesPerSlot),
            ToMiB(TotalSlots * BytesPerSlot));

        // BuildAssetBuffers too, and not only on the whole-asset path below: the
        // cell bounds are what a 16-bit position decodes against and the palette
        // is what SH reads, so a streaming asset without them draws garbage with
        // no error. Returning early past it was a bug, found before it ever ran.
        FGaussianSplatPoolResidency& StoredStreaming =
            *Residencies.Add(Asset, MakeUnique<FGaussianSplatPoolResidency>(MoveTemp(Residency)));
        BuildAssetBuffers(*Asset, StoredStreaming);
        return &StoredStreaming;
    }

    // How much of the tail fits. When it all does, every cell is whole; when it
    // does not, every cell keeps the SAME FRACTION of its own pages, which is the
    // rule today's residency already uses -- thinning evenly everywhere rather
    // than dropping whole districts.
    const int64 TailPagesNeeded = Asset->TailPages;
    const int64 TailPagesFree = GetFreeSlots() / SlotsPerPage;
    const double Ratio = (TailPagesNeeded <= TailPagesFree || TailPagesNeeded == 0)
        ? 1.0
        : static_cast<double>(TailPagesFree) / static_cast<double>(TailPagesNeeded);

    for (int32 CellIndex = 0; CellIndex < Asset->Cells.Num(); ++CellIndex)
    {
        const FGaussianSplatPagedCell& Cell = Asset->Cells[CellIndex];
        if (Cell.TailPageCount <= 0)
        {
            continue;
        }

        const int32 Wanted = Ratio >= 1.0
            ? Cell.TailPageCount
            : FMath::Clamp(FMath::FloorToInt32(Cell.TailPageCount * Ratio), 0, Cell.TailPageCount);
        if (Wanted <= 0)
        {
            continue;
        }

        const int32 FirstPage = AllocatePages(Wanted);
        if (FirstPage == INDEX_NONE)
        {
            continue;   // the pool filled up mid-way; the rest of this cell is a miss, not an error
        }

        Residency.ResidentTailPages[CellIndex] = Wanted;
        Residency.ResidentPageCount += Wanted;
        for (int32 PageInCell = 0; PageInCell < Wanted; ++PageInCell)
        {
            const int32 Global = Cell.FirstTailPage + PageInCell;
            if (Residency.TailPageSlot.IsValidIndex(Global))
            {
                Residency.TailPageSlot[Global] = FirstPage + PageInCell;
            }
        }

        const int64 SourceFirstSlot = static_cast<int64>(Cell.FirstTailPage) * SlotsPerPage;
        const int64 Slots = static_cast<int64>(Wanted) * SlotsPerPage;
        UploadRange(
            static_cast<int64>(FirstPage) * SlotsPerPage,
            Payload.GetData() + Asset->GetTailOffsetA() + SourceFirstSlot * 16,
            Payload.GetData() + Asset->GetTailOffsetB() + SourceFirstSlot * 4,
            bHasSH ? Payload.GetData() + Asset->GetTailOffsetSH() + SourceFirstSlot * 4 : nullptr,
            Slots);
        Residency.ResidentSplats += Residency.GetResidentSplats(CellIndex) - Cell.FloorCount;
    }

    Residency.bComplete = Ratio >= 1.0;
    if (!Residency.bComplete)
    {
        // Said once, not per cell and not per frame: the interesting number is
        // how much of the asset is drawable, not that it did not all fit.
        UE_LOG(
            LogGaussianSplatPool,
            Warning,
            TEXT("%s does not fit the pool: %lld of %lld splats are resident (%.0f%%), every cell keeping the same ")
            TEXT("fraction of its pages. Raise r.GaussianSplat.PoolMB for the whole asset."),
            *Asset->GetName(),
            Residency.ResidentSplats,
            Asset->TotalSplats,
            100.0 * Residency.ResidentSplats / FMath::Max<int64>(1, Asset->TotalSplats));
    }
    else
    {
        UE_LOG(
            LogGaussianSplatPool,
            Display,
            TEXT("%s registered: %lld splats resident (%.0f MiB), pool %.0f of %.0f MiB used."),
            *Asset->GetName(),
            Residency.ResidentSplats,
            ToMiB(Residency.ResidentSplats * BytesPerSlot),
            ToMiB((TotalSlots - GetFreeSlots()) * BytesPerSlot),
            ToMiB(TotalSlots * BytesPerSlot));
    }

    FGaussianSplatPoolResidency& Stored =
        *Residencies.Add(Asset, MakeUnique<FGaussianSplatPoolResidency>(MoveTemp(Residency)));
    BuildAssetBuffers(*Asset, Stored);
    return &Stored;
}

void FGaussianSplatPagePool::ApplyStreamPlan(FGaussianSplatStreamPlan&& Plan, int32 TickNumber)
{
    StreamTick = TickNumber;
    LastStreamStats = FGaussianSplatStreamStats();
    LastStreamStats.OverflowMultiplierUnused = Plan.OverflowMultiplier;
    if (!IsAllocated())
    {
        return;
    }

    const double StartTime = FPlatformTime::Seconds();
    const int32 ProtectedTicks = FMath::Max(0, CVarStreamProtectedTicks.GetValueOnAnyThread());

    // ---- Evictions first, so a tick that swaps pages needs no spare capacity.
    for (const FGaussianSplatStreamPage& Page : Plan.Evict)
    {
        const TUniquePtr<FGaussianSplatPoolResidency>* const ResidencySlot = Residencies.Find(Page.Asset);
        FGaussianSplatPoolResidency* Residency = ResidencySlot ? ResidencySlot->Get() : nullptr;
        if (Residency == nullptr || !Residency->TailPageSlot.IsValidIndex(Page.GlobalPage))
        {
            continue;
        }
        const int32 Slot = Residency->TailPageSlot[Page.GlobalPage];
        if (Slot == INDEX_NONE)
        {
            continue;
        }

        // Residency is a PREFIX of a cell's pages and the drawn set depends on
        // that (BuildPoolRuns stops at the first hole). Evicting from the middle
        // would silently shorten the cell instead, so it is refused here rather
        // than trusted to the gate.
        const FGaussianSplatPagedCell& Cell = Page.Asset->Cells[Page.CellIndex];
        const int32 PageInCell = Page.GlobalPage - Cell.FirstTailPage;
        const int32 Resident = Residency->ResidentTailPages.IsValidIndex(Page.CellIndex)
            ? Residency->ResidentTailPages[Page.CellIndex]
            : 0;
        if (PageInCell != Resident - 1)
        {
            UE_LOG(
                LogGaussianSplatPool,
                Warning,
                TEXT("%s cell %d: asked to evict page %d of %d resident, which is not the last. Refused -- residency ")
                TEXT("must stay a prefix."),
                *Page.Asset->GetName(), Page.CellIndex, PageInCell, Resident);
            continue;
        }

        Residency->TailPageSlot[Page.GlobalPage] = INDEX_NONE;
        Residency->ResidentTailPages[Page.CellIndex] = Resident - 1;
        if (Residency->EvictedTick.IsValidIndex(Page.GlobalPage))
        {
            Residency->EvictedTick[Page.GlobalPage] = StreamTick;
        }
        Residency->ResidentSplats -= FMath::Min<int64>(
            SlotsPerPage,
            static_cast<int64>(Cell.Count) - Cell.FloorCount - static_cast<int64>(PageInCell) * SlotsPerPage);
        Residency->bComplete = false;
        --Residency->ResidentPageCount;
        ReleasePage(Slot);
        ++LastStreamStats.PagesEvicted;
    }

    // ---- Then uploads. The required prefix is uncapped (D4): the pages are in
    // RAM, so this costs memcpy and PCIe, never a wait on IO or the GPU.
    const int64 PrefetchCapBytes = static_cast<int64>(FMath::Max(0, CVarStreamMaxUploadMB.GetValueOnAnyThread()))
        * 1024 * 1024;
    int64 PrefetchBytes = 0;

    // Batched per asset and flushed at the end: one staging buffer and one scatter
    // dispatch per asset rather than a lock per page (D5). HasSH is uniform inside
    // a dispatch, which is the other reason the batch key is the asset.
    TMap<const UGaussianSplatPagedAsset*, TPair<TArray<int32>, TArray<int32>>> Batches;

    for (int32 Index = 0; Index < Plan.Upload.Num(); ++Index)
    {
        const FGaussianSplatStreamPage& Page = Plan.Upload[Index];
        const bool bRequired = Index < Plan.RequiredUploads;

        const TUniquePtr<FGaussianSplatPoolResidency>* const ResidencySlot = Residencies.Find(Page.Asset);
        FGaussianSplatPoolResidency* Residency = ResidencySlot ? ResidencySlot->Get() : nullptr;
        if (Residency == nullptr || !Residency->TailPageSlot.IsValidIndex(Page.GlobalPage)
            || Residency->TailPageSlot[Page.GlobalPage] != INDEX_NONE)
        {
            continue;
        }

        const FGaussianSplatPagedCell& Cell = Page.Asset->Cells[Page.CellIndex];
        const int32 PageInCell = Page.GlobalPage - Cell.FirstTailPage;
        const int32 Resident = Residency->ResidentTailPages.IsValidIndex(Page.CellIndex)
            ? Residency->ResidentTailPages[Page.CellIndex]
            : 0;
        if (PageInCell != Resident)
        {
            continue;   // out of prefix order; the gate emits pages in order, so this is a no-op guard
        }

        const int64 PageSlots = FMath::Min<int64>(
            SlotsPerPage,
            static_cast<int64>(Cell.Count) - Cell.FloorCount - static_cast<int64>(PageInCell) * SlotsPerPage);
        if (PageSlots <= 0)
        {
            continue;
        }

        // A whole page is uploaded even when its last slots are padding: the page
        // is the residency unit, and the padding repeats the cell's last splat so
        // nothing the shader can reach is uninitialised.
        const int64 Bytes = SlotsPerPage * BytesPerSlot;
        if (!bRequired && PrefetchBytes + Bytes > PrefetchCapBytes)
        {
            continue;   // prefetch stops at the cap; required pages never do
        }

        const int32 Slot = AcquirePage();
        if (Slot == INDEX_NONE)
        {
            if (bRequired)
            {
                ++LastStreamStats.PagesRequiredNotUploaded;
            }
            continue;
        }

        TPair<TArray<int32>, TArray<int32>>& Batch = Batches.FindOrAdd(Page.Asset);
        Batch.Key.Add(Page.GlobalPage);
        Batch.Value.Add(Slot);

        Residency->TailPageSlot[Page.GlobalPage] = Slot;
        Residency->ResidentTailPages[Page.CellIndex] = Resident + 1;
        Residency->ResidentSplats += PageSlots;
        ++Residency->ResidentPageCount;

        // Thrash: this page was evicted within the protected age and is already
        // wanted again. It costs upload time and shows up nowhere else.
        if (Residency->EvictedTick.IsValidIndex(Page.GlobalPage)
            && Residency->EvictedTick[Page.GlobalPage] != MIN_int32
            && StreamTick - Residency->EvictedTick[Page.GlobalPage] <= ProtectedTicks)
        {
            ++LastStreamStats.ThrashCount;
        }
        if (Residency->LastNeededTick.IsValidIndex(Page.GlobalPage))
        {
            Residency->LastNeededTick[Page.GlobalPage] = StreamTick;
        }

        ++LastStreamStats.PagesUploaded;
        LastStreamStats.UploadBytes += Bytes;
        if (!bRequired)
        {
            PrefetchBytes += Bytes;
        }
    }

    // The copies themselves, now that the page table says where everything goes.
    // Doing it here rather than inside the loop means one command per asset (split
    // only by the per-command cap), not one per page.
    for (const TPair<const UGaussianSplatPagedAsset*, TPair<TArray<int32>, TArray<int32>>>& Batch : Batches)
    {
        UploadPageBatch(*Batch.Key, Batch.Value.Key, Batch.Value.Value);
    }

    // Game-thread time only: the scatter itself is render-thread work, and the
    // frame's own timing is what reports that. Calling this "upload ms" without
    // saying so would hide the part that actually lands on the GPU.
    LastStreamStats.UploadMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
}

void FGaussianSplatPagePool::UnregisterAsset(const UGaussianSplatPagedAsset* Asset)
{
    const TUniquePtr<FGaussianSplatPoolResidency>* const ResidencySlot = Residencies.Find(Asset);
    if (FGaussianSplatPoolResidency* Residency = ResidencySlot ? ResidencySlot->Get() : nullptr)
    {
        FreePagesOf(*Residency);
        // The palette slot IS reclaimed, unlike the floor range below: slots are a
        // hard limit of 8, so leaking one would refuse a later asset for no reason.
        const int32 Slot = Residency->PaletteSlot;
        if (Slot >= 0 && Slot < MaxSHPalettes && PaletteSlotOwner[Slot] == Asset)
        {
            PaletteSlotOwner[Slot] = nullptr;
            PaletteBuffers[Slot].SafeRelease();
        }
        // Floor ranges are not reclaimed: they came from the untouched space and
        // giving them back would need a range allocator with coalescing, which
        // Step 1a has no use for (a level switch invalidates the table instead).
        Residencies.Remove(Asset);
        // The remaining assets' bases shift, so the buffer and every base are redone
        // together -- a base that outlived its buffer would decode positions against
        // another asset's cells.
        RebuildSharedCellBounds();
    }
}

void FGaussianSplatPagePool::ReleaseRHI()
{
    Residencies.Reset();
    FreePages.Reset();
    TotalSlots = 0;
    NextUnusedSlot = 0;
    bInVideoMemory = false;
    PackedA.SafeRelease();
    PackedB.SafeRelease();
    SHIndex.SafeRelease();
    SharedCellBounds.SafeRelease();
    DummyPalette.SafeRelease();
    for (int32 Slot = 0; Slot < MaxSHPalettes; ++Slot)
    {
        PaletteSlotOwner[Slot] = nullptr;
        PaletteBuffers[Slot].SafeRelease();
    }
}
