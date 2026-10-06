#include "Render/GaussianSplatPagePool.h"

#include "GaussianSplatPagedAsset.h"
#include "HAL/IConsoleManager.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "GaussianSplatFormat.h"
#include "Render/GaussianSplatDeviceMemory.h"

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

    // Then the resident tail pages, which the allocator placed contiguously, so
    // this normally appends exactly one more run.
    const int32 ResidentPages = Residency.ResidentTailPages.IsValidIndex(CellIndex)
        ? Residency.ResidentTailPages[CellIndex]
        : 0;
    const int32 FirstPage = Residency.FirstTailPageSlotIndex.IsValidIndex(CellIndex)
        ? Residency.FirstTailPageSlotIndex[CellIndex]
        : INDEX_NONE;
    if (ResidentPages <= 0 || FirstPage == INDEX_NONE)
    {
        return Covered;
    }

    const int64 TailCount = static_cast<int64>(Cell.Count) - Cell.FloorCount;
    const int64 ResidentTail = FMath::Min<int64>(
        static_cast<int64>(ResidentPages) * FGaussianSplatPagePool::SlotsPerPage, TailCount);
    const uint32 TailTake = static_cast<uint32>(FMath::Min<int64>(Take - Covered, ResidentTail));
    if (TailTake > 0)
    {
        Append(static_cast<uint32>(static_cast<int64>(FirstPage) * FGaussianSplatPagePool::SlotsPerPage), TailTake);
        Covered += TailTake;
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
    const int64 Needed = static_cast<int64>(PageCount) * SlotsPerPage;
    if (NextUnusedSlot + Needed <= TotalSlots)
    {
        const int32 FirstPage = static_cast<int32>(NextUnusedSlot / SlotsPerPage);
        NextUnusedSlot += Needed;
        return FirstPage;
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
    for (int32 CellIndex = 0; CellIndex < Residency.FirstTailPageSlotIndex.Num(); ++CellIndex)
    {
        const int32 FirstPage = Residency.FirstTailPageSlotIndex[CellIndex];
        const int32 Pages = Residency.ResidentTailPages.IsValidIndex(CellIndex) ? Residency.ResidentTailPages[CellIndex] : 0;
        for (int32 Page = 0; Page < Pages; ++Page)
        {
            if (FirstPage != INDEX_NONE)
            {
                FreePages.Add(FirstPage + Page);
            }
        }
    }
    Residency.ResidentTailPages.Reset();
    Residency.FirstTailPageSlotIndex.Reset();
    Residency.ResidentSplats = 0;
}

void FGaussianSplatPagePool::BuildAssetBuffers(
    const UGaussianSplatPagedAsset& Asset,
    FGaussianSplatPoolResidency& Residency)
{
    // Cell bounds: origin.xyz then extent.xyz per cell, which is what the shader
    // decodes a 16-bit position against. Built from the same CellExtent the
    // importer packed with, so a one-splat-wide cell cannot divide by zero here
    // and not there.
    TArray<FVector4f> Bounds;
    Bounds.Reserve(FMath::Max(2, Asset.Cells.Num() * 2));
    for (const FGaussianSplatPagedCell& Cell : Asset.Cells)
    {
        const FVector3f Extent = GaussianSplatFormat::CellExtent(Cell.BoundsMin, Cell.BoundsMax);
        Bounds.Add(FVector4f(Cell.BoundsMin.X, Cell.BoundsMin.Y, Cell.BoundsMin.Z, 0.0f));
        Bounds.Add(FVector4f(Extent.X, Extent.Y, Extent.Z, 0.0f));
    }
    if (Bounds.IsEmpty())
    {
        Bounds.Add(FVector4f(ForceInitToZero));
        Bounds.Add(FVector4f(ForceInitToZero));
    }

    TArray<float> Palette = Asset.SHPalette;
    if (Palette.IsEmpty())
    {
        Palette.Add(0.0f);   // a single dummy element: the shader must not sample it, but the SRV must exist
    }

    FGaussianSplatPoolResidency* Target = &Residency;
    ENQUEUE_RENDER_COMMAND(GaussianSplatPoolAssetBuffers)(
        [Target, Bounds = MoveTemp(Bounds), Palette = MoveTemp(Palette)](FRHICommandListImmediate& RHICmdList)
        {
            const auto Make = [&RHICmdList](const TCHAR* Name, uint32 Stride, const void* Data, uint32 Count,
                                            TRefCountPtr<FRDGPooledBuffer>& OutBuffer)
            {
                OutBuffer = AllocatePooledBuffer(FRDGBufferDesc::CreateStructuredDesc(Stride, Count), Name);
                void* Dest = RHICmdList.LockBuffer(OutBuffer->GetRHI(), 0, Stride * Count, RLM_WriteOnly);
                FMemory::Memcpy(Dest, Data, Stride * Count);
                RHICmdList.UnlockBuffer(OutBuffer->GetRHI());
            };

            Make(TEXT("GaussianSplat.PagedCellBounds"), sizeof(FVector4f), Bounds.GetData(),
                 static_cast<uint32>(Bounds.Num()), Target->CellBounds);
            Make(TEXT("GaussianSplat.PagedSHPalette"), sizeof(float), Palette.GetData(),
                 static_cast<uint32>(Palette.Num()), Target->SHPalette);
        });
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

const FGaussianSplatPoolResidency* FGaussianSplatPagePool::FindResidency(const UGaussianSplatPagedAsset* Asset) const
{
    return Residencies.Find(Asset);
}

const FGaussianSplatPoolResidency* FGaussianSplatPagePool::RegisterAsset(const UGaussianSplatPagedAsset* Asset)
{
    if (Asset == nullptr || Asset->Cells.IsEmpty())
    {
        return nullptr;
    }
    if (const FGaussianSplatPoolResidency* Existing = Residencies.Find(Asset))
    {
        return Existing;
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

    // How much of the tail fits. When it all does, every cell is whole; when it
    // does not, every cell keeps the SAME FRACTION of its own pages, which is the
    // rule today's residency already uses -- thinning evenly everywhere rather
    // than dropping whole districts.
    const int64 TailPagesNeeded = Asset->TailPages;
    const int64 TailPagesFree = GetFreeSlots() / SlotsPerPage;
    const double Ratio = (TailPagesNeeded <= TailPagesFree || TailPagesNeeded == 0)
        ? 1.0
        : static_cast<double>(TailPagesFree) / static_cast<double>(TailPagesNeeded);

    Residency.ResidentTailPages.Init(0, Asset->Cells.Num());
    Residency.FirstTailPageSlotIndex.Init(INDEX_NONE, Asset->Cells.Num());

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

        Residency.FirstTailPageSlotIndex[CellIndex] = FirstPage;
        Residency.ResidentTailPages[CellIndex] = Wanted;

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

    FGaussianSplatPoolResidency& Stored = Residencies.Add(Asset, MoveTemp(Residency));
    BuildAssetBuffers(*Asset, Stored);
    return &Stored;
}

void FGaussianSplatPagePool::UnregisterAsset(const UGaussianSplatPagedAsset* Asset)
{
    if (FGaussianSplatPoolResidency* Residency = Residencies.Find(Asset))
    {
        FreePagesOf(*Residency);
        // Floor ranges are not reclaimed: they came from the untouched space and
        // giving them back would need a range allocator with coalescing, which
        // Step 1a has no use for (a level switch invalidates the table instead).
        Residencies.Remove(Asset);
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
}
