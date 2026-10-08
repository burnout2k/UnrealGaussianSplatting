#include "GaussianSplatStreamGate.h"

#include "GaussianSplatComponent.h"
#include "GaussianSplatPagePool.h"
#include "GaussianSplatPagedAsset.h"
#include "GaussianSplatPasses.h"
#include "GaussianSplatWorldSubsystem.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatStream, Log, All);

namespace
{
    TAutoConsoleVariable<float> CVarLookAheadSec(
        TEXT("r.GaussianSplat.Stream.LookAheadSec"),
        2.0f,
        TEXT("How far ahead of the ego's current velocity to prefetch, seconds. Those pages are WANTED, not\n")
        TEXT("required: they sit outside the fit, rank below required pages and stop at the byte cap, so they can\n")
        TEXT("never push a required page out or delay a camera frame. 0 turns prefetch off."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarGroupMarginCm(
        TEXT("r.GaussianSplat.Stream.GroupMarginCm"),
        200.0f,
        TEXT("Added to a group's radius, centimetres. The required set uses dist(centre, cell) - radius, which bounds\n")
        TEXT("every camera x with |x - centre| + margin <= radius; the offline data measured the exact rig up to\n")
        TEXT("3.6%% above the 360-degree rule because the cameras sit up to 2 m from the car origin, and this is the\n")
        TEXT("fix. Lowering it does not save work -- it breaks the bound."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarMinLookAheadSpeedCms(
        TEXT("r.GaussianSplat.Stream.MinLookAheadSpeedCms"),
        800.0f,
        TEXT("Below this speed (cm/s; 800 = 8 m/s = ~29 km/h) the look-ahead uses the ego's FACING direction at this\n")
        TEXT("floor speed instead of its velocity. A car stopped at a light has no velocity but is about to drive\n")
        TEXT("forward, and prefetching from velocity alone drops the road ahead exactly when re-uploading it is\n")
        TEXT("least affordable. 0 restores pure velocity extrapolation."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarGateLog(
        TEXT("r.GaussianSplat.Stream.LogGate"),
        0,
        TEXT("1: one line per gate tick with the group set, the required and wanted page counts and the timings."),
        ECVF_RenderThreadSafe);

    // A cell the look-ahead wants some tail of. Per cell for the same reason the
    // required set is: expanding to pages inside the walk was the gate's largest
    // single cost.
    struct FWantedCell
    {
        const UGaussianSplatPagedAsset* Asset = nullptr;
        int32 CellIndex = 0;
        int32 FirstTailPage = 0;
        int32 Pages = 0;
        double Distance = 0.0;
    };

    // One cell the gate decided it needs some tail of. Recorded rather than
    // expanded straight to pages, because the overflow bisection has to re-solve
    // every cell's take at a smaller multiplier and must not walk the grid again.
    struct FRequiredCell
    {
        const UGaussianSplatPagedAsset* Asset = nullptr;
        int32 CellIndex = 0;
        double Distance = 0.0;      // group centre to the cell's world box, minus the radius
        double HalfFull = 1.0;
        float MinFraction = 0.02f;

        // How many tail pages this cell needs at a given global multiplier. The
        // multiplier scales the full distance, exactly as a view's budget bisection
        // does, so the gate thins the same way the selection would.
        int32 PagesAt(double Multiplier) const
        {
            const FGaussianSplatPagedCell& Cell = Asset->Cells[CellIndex];
            const uint32 Take = GaussianSplatLod::TakeAt(Cell.Count, Distance, HalfFull * Multiplier, MinFraction);
            const int64 Tail = static_cast<int64>(Take) - Cell.FloorCount;
            return Tail <= 0
                ? 0
                : FMath::Min(
                    Cell.TailPageCount,
                    static_cast<int32>(FMath::DivideAndRoundUp<int64>(Tail, GAUSSIAN_SPLAT_PAGE_SPLATS)));
        }
    };

    // One page of a cell, named the way the pool names it.
    struct FWantedPage
    {
        const UGaussianSplatPagedAsset* Asset = nullptr;
        int32 CellIndex = 0;
        int32 PageInCell = 0;
        int32 GlobalPage = 0;
        double Distance = 0.0;      // from the nearest group centre, for the ordering
    };

    // The ego a view belongs to: the attach root, so six sensors on one car form
    // one group rather than six.
    const AActor* GroupRootOf(const AActor* ViewActor)
    {
        if (ViewActor == nullptr)
        {
            return nullptr;
        }
        const AActor* Root = ViewActor;
        while (const AActor* Parent = Root->GetAttachParentActor())
        {
            Root = Parent;
        }
        return Root;
    }
}

// The tick function itself. TG_LastDemotable is the point of the whole exercise:
// it runs after every actor, sensor and camera has its final pose for this tick
// and before CARLA captures in OnWorldPostActorTick.
struct FGaussianSplatGateTickFunction final : public FTickFunction
{
    TWeakObjectPtr<UWorld> TargetWorld;

    virtual void ExecuteTick(
        float DeltaTime,
        ELevelTick TickType,
        ENamedThreads::Type CurrentThread,
        const FGraphEventRef& CompletionGraphEvent) override
    {
        if (UWorld* World = TargetWorld.Get())
        {
            FGaussianSplatStreamGate::Get().RunGate(World);
        }
    }

    virtual FString DiagnosticMessage() override
    {
        return TEXT("FGaussianSplatGateTickFunction");
    }

    virtual FName DiagnosticContext(bool bDetailed) override
    {
        return FName(TEXT("GaussianSplatStreamGate"));
    }
};

namespace
{
    // One tick function per world. A map rather than a member because worlds come
    // and go (PIE, level switches) and the gate itself is process-wide, like the pool.
    TMap<TWeakObjectPtr<UWorld>, TUniquePtr<FGaussianSplatGateTickFunction>> GTickFunctions;
}

namespace
{
    // Written once a tick by the gate (game thread), read per view by the selection
    // (render thread). Relaxed is enough: it is a hint that bounds a bisection, and
    // a tick of staleness costs at most one frame of slightly conservative LOD.
    std::atomic<float> GOverflowMultiplier{1.0f};
}

float FGaussianSplatStreamGate::GetOverflowMultiplier()
{
    return GOverflowMultiplier.load(std::memory_order_relaxed);
}

FGaussianSplatStreamGate& FGaussianSplatStreamGate::Get()
{
    static FGaussianSplatStreamGate Instance;
    return Instance;
}

void FGaussianSplatStreamGate::StampView(const AActor* ViewActor, const FVector& Origin, double FocalPx)
{
    // An editor viewport has NO ViewActor, and it is the only way to look at a
    // streamed scene by hand. D4 left the editor ungated and said its misses would
    // become wanted pages; that path was never built, so in the editor nothing
    // streamed at all and only the pinned floor drew. Treating an ownerless view
    // as its own group is the deviation, and it is a safe one: it cannot affect
    // the package, where CARLA's sensors always carry an owner, and grouping
    // several ownerless views together only ever makes the sphere LARGER, which
    // asks for more pages, never fewer.
    const bool bHasOwner = ViewActor != nullptr;

    FScopeLock Lock(&StampLock);
    for (FGaussianSplatGateView& Existing : StampedViews)
    {
        if (Existing.bHasOwner == bHasOwner && Existing.ViewActor.Get() == ViewActor)
        {
            Existing.Origin = Origin;
            Existing.FocalPx = FocalPx;
            Existing.Stamp = TickNumber;
            return;
        }
    }
    StampedViews.Add({ViewActor, bHasOwner, Origin, FocalPx, TickNumber});
}

void FGaussianSplatStreamGate::RegisterWorld(UWorld* World)
{
    if (World == nullptr || World->PersistentLevel == nullptr || GTickFunctions.Contains(World))
    {
        return;
    }

    TUniquePtr<FGaussianSplatGateTickFunction> Tick = MakeUnique<FGaussianSplatGateTickFunction>();
    Tick->TargetWorld = World;
    Tick->bCanEverTick = true;
    Tick->bStartWithTickEnabled = true;
    Tick->bHighPriority = false;
    Tick->TickGroup = TG_LastDemotable;
    Tick->RegisterTickFunction(World->PersistentLevel);
    GTickFunctions.Add(World, MoveTemp(Tick));

    UE_LOG(LogGaussianSplatStream, Display, TEXT("Streaming gate registered on %s (TG_LastDemotable)."), *World->GetName());
}

void FGaussianSplatStreamGate::UnregisterWorld(UWorld* World)
{
    if (TUniquePtr<FGaussianSplatGateTickFunction>* Found = GTickFunctions.Find(World))
    {
        (*Found)->UnRegisterTickFunction();
        GTickFunctions.Remove(World);
    }

    FScopeLock Lock(&StampLock);
    StampedViews.Reset();
}

void FGaussianSplatStreamGate::OnWorldInitialized(UWorld* World)
{
    Get().RegisterWorld(World);
}

void FGaussianSplatStreamGate::OnWorldTornDown(UWorld* World)
{
    Get().UnregisterWorld(World);
}

void FGaussianSplatStreamGate::BuildGroups(TArray<FGaussianSplatGateGroup>& OutGroups)
{
    OutGroups.Reset();

    TArray<FGaussianSplatGateView> Views;
    {
        FScopeLock Lock(&StampLock);
        // A view that did not draw splats since the previous gate tick is dropped:
        // a camera that stopped rendering stops holding pages. One tick of slack,
        // because the stamp is written on the render thread a frame behind.
        StampedViews.RemoveAll([this](const FGaussianSplatGateView& View)
        {
            // An ownerless view is never dropped for a dead actor -- it has none.
            const bool bGone = View.bHasOwner && !View.ViewActor.IsValid();
            return bGone || TickNumber - View.Stamp > 2;
        });
        Views = StampedViews;
    }

    const double MarginCm = FMath::Max(0.0f, CVarGroupMarginCm.GetValueOnAnyThread());

    for (const FGaussianSplatGateView& View : Views)
    {
        const AActor* Root = GroupRootOf(View.ViewActor.Get());
        FGaussianSplatGateGroup* Group = OutGroups.FindByPredicate(
            [Root](const FGaussianSplatGateGroup& G) { return G.Root == Root; });
        if (Group == nullptr)
        {
            Group = &OutGroups.AddDefaulted_GetRef();
            Group->Root = Root;
        }
        Group->Centre += View.Origin;
        Group->MaxFocalPx = FMath::Max(Group->MaxFocalPx, View.FocalPx);
        ++Group->ViewCount;
    }

    // The centre is the mean camera origin; the radius is the largest offset from
    // it plus the margin. Every camera x then satisfies |x - c| + margin <= R, which
    // is what makes dist(c, box) - R <= dist(x, box) and so the gate's take >= every
    // view's take, for every cell (plan D4).
    for (FGaussianSplatGateGroup& Group : OutGroups)
    {
        Group.Centre /= FMath::Max(1, Group.ViewCount);
        for (const FGaussianSplatGateView& View : Views)
        {
            if (GroupRootOf(View.ViewActor.Get()) == Group.Root)
            {
                Group.Radius = FMath::Max(Group.Radius, FVector::Dist(View.Origin, Group.Centre));
            }
        }
        Group.Radius += MarginCm;
        if (IsValid(Group.Root))
        {
            Group.Velocity = Group.Root->GetVelocity();
            Group.Forward = Group.Root->GetActorForwardVector();
        }
    }
}

void FGaussianSplatStreamGate::RunGate(UWorld* World)
{
    if (World == nullptr || !FGaussianSplatPagePool::IsStreamingEnabled())
    {
        // Streaming off: no clamp, or a stale one would quietly thin every view.
        GOverflowMultiplier.store(1.0f, std::memory_order_relaxed);
        return;
    }

    FGaussianSplatPagePool& Pool = FGaussianSplatPagePool::Get();
    if (!Pool.IsAllocated())
    {
        return;
    }

    const double StartTime = FPlatformTime::Seconds();
    ++TickNumber;

    FGaussianSplatGateStats Stats;
    TArray<FGaussianSplatGateGroup> Groups;
    BuildGroups(Groups);
    Stats.Groups = Groups.Num();
    for (const FGaussianSplatGateGroup& Group : Groups)
    {
        Stats.Views += Group.ViewCount;
    }

    // The components, not the pool's registrations: a component carries the
    // transform, the PointSize and the draw budget, and one asset may be placed
    // more than once.
    TArray<UGaussianSplatComponent*> Components;
    if (UGaussianSplatWorldSubsystem* Subsystem = World->GetSubsystem<UGaussianSplatWorldSubsystem>())
    {
        Subsystem->GetRegisteredComponents(Components);
    }

    const float LookAheadSec = FMath::Max(0.0f, CVarLookAheadSec.GetValueOnAnyThread());
    const double MinLookAheadSpeed = FMath::Max(0.0f, CVarMinLookAheadSpeedCms.GetValueOnAnyThread());
    const float MinFractionCVar = GaussianSplatLod::GetMinFraction();

    // Required pages this tick, and the pages merely wanted by look-ahead. Both
    // keyed the way the pool names a page, so the plan needs no translation.
    TArray<FRequiredCell> RequiredCells;
    TArray<FWantedCell> WantedCells;
    TMap<uint64, int32> WantedCellIndex;
    TArray<FWantedPage> Wanted;
    // Key -> index into RequiredCells. A TSet plus FindByPredicate was a LINEAR
    // scan inside the cell walk: at 690 cells that is up to 238,000 comparisons
    // per tick, and it was most of the 1.88 ms the first measured run cost.
    TMap<uint64, int32> RequiredCellIndex;

    const auto PageKey = [](const UGaussianSplatPagedAsset* Asset, int32 GlobalPage) -> uint64
    {
        return (static_cast<uint64>(GetTypeHash(Asset)) << 32) ^ static_cast<uint64>(GlobalPage);
    };

    for (UGaussianSplatComponent* Component : Components)
    {
        if (!IsValid(Component) || !Component->IsVisible() || !IsValid(Component->PagedAsset))
        {
            continue;
        }
        const UGaussianSplatPagedAsset* Asset = Component->PagedAsset;
        if (Asset->Cells.IsEmpty() || Pool.FindResidency(Asset) == nullptr)
        {
            continue;
        }

        const FTransform LocalToWorld = Component->GetComponentTransform();
        const FTransform WorldToLocal = LocalToWorld.Inverse();
        const double ActorScale = LocalToWorld.GetScale3D().GetAbsMax();
        const float PointSize = FMath::Clamp(Component->PointSize, 0.1f, 32.0f);
        // Fix 5 Step 4 (review M5): the SAME per-actor d_full ceiling the selection uses.
        // Capping only one of the two is worse than capping neither -- cap the selection
        // alone and this gate keeps streaming pages nothing will draw; cap the gate alone
        // and the selection asks for pages the gate deliberately did not fetch, which is
        // a miss. GaussianSplatLod exists so the two cannot drift.
        const float MaxFullDistanceM = FMath::Max(0.0f, Component->LodMaxFullDistance);

        // D9: the paged path clamps the LOD floor to the value the asset BAKED.
        // Asking for splats below the baked floor would make the picture depend on
        // a setting the asset cannot honour.
        const float BakedFloor = FMath::Clamp(Asset->FloorFraction, 0.0f, 1.0f);
        const float MinFraction = FMath::Max(MinFractionCVar, BakedFloor);
        const float CellSize = FMath::Max(1.0f, Asset->CellSize);

        for (const FGaussianSplatGateGroup& Group : Groups)
        {
            const double HalfFull = GaussianSplatLod::ComputeHalfFull(
                Group.MaxFocalPx, PointSize, Asset->SizeRef, ActorScale, MaxFullDistanceM);

            // Beyond this distance every cell is floor-only, so there is nothing to
            // visit: keep > R_c needs 4 (HalfFull/d)^2 > MinFraction (review M4).
            const double FloorRadius = MinFraction > 0.0f
                ? HalfFull * 2.0 / FMath::Sqrt(static_cast<double>(MinFraction))
                : TNumericLimits<double>::Max() * 0.5;

            // The current pose is REQUIRED; the look-ahead poses are WANTED. Both
            // are built from this tick's state only, so the required set is a
            // function of the current poses and nothing else (D9).
            // Probe 0 is the CURRENT pose and is the only one the required set
            // may use. The rest are look-ahead and feed the wanted set only.
            struct FProbe { FVector Centre; };
            TArray<FProbe, TInlineAllocator<4>> Probes;
            Probes.Add({Group.Centre});
            if (LookAheadSec > 0.0f)
            {
                // Prefetch along the direction of travel -- or, when the ego is
                // stopped or crawling, along where it FACES at a floor speed.
                //
                // Velocity alone is wrong exactly when it matters (Allan,
                // 2026-10-07): a car waiting at a red light has none, so the road
                // ahead stops being wanted, becomes an eviction candidate once the
                // protected age lapses (~1.5 s at 20 Hz, against a thirty-second
                // light), and is re-uploaded the moment the light changes. Facing
                // is a better guess than nothing, and it is still only a guess --
                // the real answer is the route, which CARLA does not give us (D4).
                //
                // Residency only, never the drawn set, so D9 is untouched.
                FVector Direction = Group.Velocity;
                double Speed = Direction.Size();
                if (Speed < MinLookAheadSpeed)
                {
                    Direction = Group.Forward;
                    Speed = MinLookAheadSpeed;
                }
                if (Direction.Normalize())
                {
                    const FVector Step = Direction * Speed;
                    Probes.Add({Group.Centre + Step * (LookAheadSec * 0.25f)});
                    Probes.Add({Group.Centre + Step * (LookAheadSec * 0.5f)});
                    Probes.Add({Group.Centre + Step * LookAheadSec});
                }
            }

            // ONE walk over the union of every probe's box, not one walk per probe.
            // The look-ahead probes sit within a second or two of travel of the
            // current pose -- ~28 m at 50 km/h against a floor radius of 50-77 m --
            // so their union is only ~30 % wider in one axis, while walking it four
            // times cost four times the grid lookups. The first measured run spent
            // 0.54-1.88 ms here against a 0.5 ms bar WITHOUT look-ahead running at
            // all; with velocity it would have been four times worse in CARLA.
            const double Reach = FloorRadius + Group.Radius;
            if (Reach >= TNumericLimits<double>::Max() * 0.25)
            {
                continue;   // a degenerate MinFraction; the whole-asset path would be the answer, not a walk
            }
            const double LocalReach = Reach / FMath::Max(ActorScale, UE_SMALL_NUMBER);

            FIntVector Lo(MAX_int32, MAX_int32, MAX_int32);
            FIntVector Hi(MIN_int32, MIN_int32, MIN_int32);
            for (const FProbe& Probe : Probes)
            {
                const FVector LocalCentre = WorldToLocal.TransformPosition(Probe.Centre);
                const FIntVector BoxMin(
                    FMath::FloorToInt((LocalCentre.X - LocalReach) / CellSize),
                    FMath::FloorToInt((LocalCentre.Y - LocalReach) / CellSize),
                    FMath::FloorToInt((LocalCentre.Z - LocalReach) / CellSize));
                const FIntVector BoxMax(
                    FMath::FloorToInt((LocalCentre.X + LocalReach) / CellSize),
                    FMath::FloorToInt((LocalCentre.Y + LocalReach) / CellSize),
                    FMath::FloorToInt((LocalCentre.Z + LocalReach) / CellSize));
                Lo = FIntVector(FMath::Min(Lo.X, BoxMin.X), FMath::Min(Lo.Y, BoxMin.Y), FMath::Min(Lo.Z, BoxMin.Z));
                Hi = FIntVector(FMath::Max(Hi.X, BoxMax.X), FMath::Max(Hi.Y, BoxMax.Y), FMath::Max(Hi.Z, BoxMax.Z));
            }
            Lo = FIntVector(
                FMath::Max(Lo.X, Asset->GetCoordMin().X),
                FMath::Max(Lo.Y, Asset->GetCoordMin().Y),
                FMath::Max(Lo.Z, Asset->GetCoordMin().Z));
            Hi = FIntVector(
                FMath::Min(Hi.X, Asset->GetCoordMax().X),
                FMath::Min(Hi.Y, Asset->GetCoordMax().Y),
                FMath::Min(Hi.Z, Asset->GetCoordMax().Z));

            for (int32 X = Lo.X; X <= Hi.X; ++X)
            for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
            for (int32 Z = Lo.Z; Z <= Hi.Z; ++Z)
            {
                const int32 CellIndex = Asset->FindCellByCoord(FIntVector(X, Y, Z));
                if (CellIndex == INDEX_NONE)
                {
                    continue;   // most of a city's coordinate box is empty
                }
                ++Stats.CellsVisited;

                const FGaussianSplatPagedCell& Cell = Asset->Cells[CellIndex];
                if (Cell.Count <= 0 || Cell.TailPageCount <= 0)
                {
                    continue;
                }

                // Transformed ONCE per cell now, rather than once per probe.
                const FBox WorldBox =
                    FBox(FVector(Cell.BoundsMin), FVector(Cell.BoundsMax)).TransformBy(LocalToWorld);
                const auto DistanceFrom = [&WorldBox, &Group](const FVector& Centre) -> double
                {
                    // From the group CENTRE minus its radius, which is what bounds
                    // every camera in the group. No frustum test: a camera may turn
                    // between now and the capture.
                    return FMath::Max(
                        0.0,
                        FMath::Sqrt(ComputeSquaredDistanceFromBoxToPoint(WorldBox.Min, WorldBox.Max, Centre))
                            - Group.Radius);
                };
                const auto PagesFor = [&Cell, HalfFull, MinFraction](double Distance) -> int32
                {
                    const uint32 Take = GaussianSplatLod::TakeAt(Cell.Count, Distance, HalfFull, MinFraction);
                    const int64 TailNeeded = static_cast<int64>(Take) - Cell.FloorCount;
                    return TailNeeded <= 0
                        ? 0
                        : FMath::Min(
                            Cell.TailPageCount,
                            static_cast<int32>(FMath::DivideAndRoundUp<int64>(TailNeeded, GAUSSIAN_SPLAT_PAGE_SPLATS)));
                };

                // REQUIRED, from the current pose only (D9: the drawn set is a
                // function of the current poses and nothing else).
                const double RequiredDistance = DistanceFrom(Probes[0].Centre);
                if (PagesFor(RequiredDistance) > 0)
                {
                    const uint64 CellKey = PageKey(Asset, CellIndex);
                    if (const int32* Found = RequiredCellIndex.Find(CellKey))
                    {
                        // A second group reaching the same cell keeps the CLOSER
                        // distance, which is the larger take -- the gate must bound
                        // every view, not average them.
                        FRequiredCell& Existing = RequiredCells[*Found];
                        if (RequiredDistance < Existing.Distance)
                        {
                            Existing.Distance = RequiredDistance;
                            Existing.HalfFull = FMath::Max(Existing.HalfFull, HalfFull);
                        }
                    }
                    else
                    {
                        RequiredCellIndex.Add(CellKey, RequiredCells.Num());
                        RequiredCells.Add({Asset, CellIndex, RequiredDistance, HalfFull, MinFraction});
                    }
                }

                // WANTED: prefetch, from the nearest look-ahead pose. Outside the
                // fit, below required in rank, and stopped by the byte cap, so it
                // can never push a required page out or delay a camera frame.
                double WantedDistance = TNumericLimits<double>::Max();
                for (int32 ProbeIndex = 1; ProbeIndex < Probes.Num(); ++ProbeIndex)
                {
                    WantedDistance = FMath::Min(WantedDistance, DistanceFrom(Probes[ProbeIndex].Centre));
                }
                if (Probes.Num() <= 1)
                {
                    continue;
                }

                // Per CELL, not per page. Enumerating wanted pages here cost a TSet
                // contains, a TSet insert and a TArray append EACH, about 7,000 times
                // a tick -- and the measurement says so: two runs visiting the same
                // 212 cells differed 2.2x in gate ms while their wanted-page counts
                // differed 1.7x. The cost tracked pages, not cells.
                const int32 WantedPages = PagesFor(WantedDistance);
                if (WantedPages > 0)
                {
                    const uint64 CellKey = PageKey(Asset, CellIndex);
                    if (int32* Found = WantedCellIndex.Find(CellKey))
                    {
                        FWantedCell& Existing = WantedCells[*Found];
                        Existing.Pages = FMath::Max(Existing.Pages, WantedPages);
                        Existing.Distance = FMath::Min(Existing.Distance, WantedDistance);
                    }
                    else
                    {
                        WantedCellIndex.Add(CellKey, WantedCells.Num());
                        WantedCells.Add({Asset, CellIndex, Cell.FirstTailPage, WantedPages, WantedDistance});
                    }
                }
            }
        }
    }

    // ---- The fit (plan D4). If the required set does not fit the pool, ONE global
    // multiplier is solved by bisection and every cell's tail is trimmed by it. The
    // fit is solved before anything is uploaded, so the gate always terminates, and
    // the multiplier is the upper bound of every view's own bisection this frame.
    const auto PagesAtMultiplier = [&RequiredCells](double Multiplier) -> int64
    {
        int64 Sum = 0;
        for (const FRequiredCell& Cell : RequiredCells)
        {
            Sum += Cell.PagesAt(Multiplier);
        }
        return Sum;
    };

    // Capacity is what is free PLUS what the gate may evict: a page that is
    // resident but neither required nor wanted is capacity, not a cost.
    int64 ResidentPages = 0;
    {
        TArray<const UGaussianSplatPagedAsset*> Assets;
        Pool.GetRegisteredAssets(Assets);
        for (const UGaussianSplatPagedAsset* Asset : Assets)
        {
            if (const FGaussianSplatPoolResidency* Residency = Pool.FindResidency(Asset))
            {
                // The running count, not a walk of TailPageSlot: that would be
                // 1.45M iterations per tick on Lublin against a 0.5 ms budget.
                ResidentPages += Residency->ResidentPageCount;
            }
        }
    }
    Stats.ResidentPages = static_cast<int32>(ResidentPages);

    const int64 Capacity = static_cast<int64>(Pool.GetFreePageCount()) + ResidentPages;
    double Multiplier = 1.0;
    if (PagesAtMultiplier(1.0) > Capacity)
    {
        constexpr double MinMultiplier = 1.0 / 1048576.0;
        if (PagesAtMultiplier(MinMultiplier) > Capacity)
        {
            Multiplier = MinMultiplier;   // even the floor-only set overflows; the clamp below is what is left
        }
        else
        {
            double Fits = MinMultiplier;
            double Over = 1.0;
            for (int32 Pass = 0; Pass < 20; ++Pass)
            {
                const double Mid = 0.5 * (Fits + Over);
                if (PagesAtMultiplier(Mid) <= Capacity) { Fits = Mid; } else { Over = Mid; }
            }
            Multiplier = Fits;
        }
    }
    Stats.OverflowMultiplier = static_cast<float>(Multiplier);
    GOverflowMultiplier.store(Stats.OverflowMultiplier, std::memory_order_relaxed);

    // ---- Expand the fitted cells to pages, in prefix order per cell: the pool
    // refuses an upload that would leave a hole, because BuildPoolRuns stops at
    // the first one.
    TArray<FWantedPage> Required;
    TSet<uint64> RequiredSeen;
    for (const FRequiredCell& Cell : RequiredCells)
    {
        const FGaussianSplatPagedCell& PagedCell = Cell.Asset->Cells[Cell.CellIndex];
        const int32 Pages = Cell.PagesAt(Multiplier);
        for (int32 PageInCell = 0; PageInCell < Pages; ++PageInCell)
        {
            const int32 GlobalPage = PagedCell.FirstTailPage + PageInCell;
            RequiredSeen.Add(PageKey(Cell.Asset, GlobalPage));
            Required.Add({Cell.Asset, Cell.CellIndex, PageInCell, GlobalPage, Cell.Distance});
        }
    }
    // Expand the wanted cells to pages ONCE, here, skipping anything already
    // required -- which is most of the saving, since the two sets overlap heavily
    // near the camera.
    for (const FWantedCell& WCell : WantedCells)
    {
        for (int32 PageInCell = 0; PageInCell < WCell.Pages; ++PageInCell)
        {
            const int32 GlobalPage = WCell.FirstTailPage + PageInCell;
            if (!RequiredSeen.Contains(PageKey(WCell.Asset, GlobalPage)))
            {
                Wanted.Add({WCell.Asset, WCell.CellIndex, PageInCell, GlobalPage, WCell.Distance});
            }
        }
    }

    Stats.RequiredPages = Required.Num();
    Stats.WantedPages = Wanted.Num();

    // ONLY the required set is protected from eviction. A wanted page is prefetch:
    // D4 says it ranks BELOW a required page, and that has to mean it yields its
    // slot when a required page needs one -- not that it holds the slot hostage.
    //
    // Protecting them was a real bug, and the first capped drive found it: at a
    // 600 MiB pool (6,400 pages) a 5,270-page required set should have fitted,
    // but required ∪ wanted came to ~10,200 pages, so almost nothing was
    // evictable, 57-91 required uploads failed every tick (`short`), and up to
    // 3.88M splats went missing -- while the overflow bisection engaged (m 0.83)
    // and could not help, because the fit was never the problem.
    TSet<uint64> WantedSet;
    for (const FWantedPage& Page : Wanted)
    {
        WantedSet.Add(PageKey(Page.Asset, Page.GlobalPage));
    }

    // ---- The plan. Uploads first so the order is prefix-per-cell; evictions are
    // chosen only for what the uploads actually need.
    FGaussianSplatStreamPlan Plan;
    Plan.OverflowMultiplier = Stats.OverflowMultiplier;

    const auto IsResident = [&Pool](const UGaussianSplatPagedAsset* Asset, int32 GlobalPage) -> bool
    {
        const FGaussianSplatPoolResidency* Residency = Pool.FindResidency(Asset);
        return Residency != nullptr && Residency->TailPageSlot.IsValidIndex(GlobalPage)
            && Residency->TailPageSlot[GlobalPage] != INDEX_NONE;
    };

    Required.Sort([](const FWantedPage& A, const FWantedPage& B)
    {
        if (A.Asset != B.Asset) { return A.Asset < B.Asset; }
        return A.GlobalPage < B.GlobalPage;
    });
    for (const FWantedPage& Page : Required)
    {
        if (!IsResident(Page.Asset, Page.GlobalPage))
        {
            Plan.Upload.Add({Page.Asset, Page.GlobalPage, Page.CellIndex});
        }
    }
    Plan.RequiredUploads = Plan.Upload.Num();

    // Then the wanted pages, nearest first: prefetch that will be needed soonest.
    Wanted.Sort([](const FWantedPage& A, const FWantedPage& B) { return A.Distance < B.Distance; });
    for (const FWantedPage& Page : Wanted)
    {
        if (!IsResident(Page.Asset, Page.GlobalPage))
        {
            Plan.Upload.Add({Page.Asset, Page.GlobalPage, Page.CellIndex});
        }
    }

    // ---- Eviction. Candidates are resident pages that are neither required nor
    // wanted. Order: a cell's LAST page first (residency must stay a prefix), then
    // least recently needed, and a page released within the protected age goes last
    // -- LRU is hysteresis except at 100%% occupancy, which is exactly the regime a
    // capped pool creates (review M5).
    const int32 NeedPages = Plan.RequiredUploads - Pool.GetFreePageCount();
    if (NeedPages > 0)
    {
        // Candidates are ranked per CELL, not per page. Ranking pages by their own
        // LastNeededTick splits a cell apart -- a deeper page was needed less
        // recently, so it sorts away from its neighbours -- and the plan then asks
        // to evict page 5 while 6, 7 and 8 are still resident. The pool refuses
        // that, correctly, because residency must stay a prefix: 12,432 refusals
        // in one capped drive, and the required uploads failed for want of the
        // slots those evictions would have freed.
        struct FCandidateCell
        {
            const UGaussianSplatPagedAsset* Asset = nullptr;
            int32 CellIndex = 0;
            int32 FirstTailPage = 0;
            int32 Shallowest = 0;        // the shallowest page in the evictable suffix
            int32 Deepest = 0;           // the deepest resident page
            int32 Rank = MIN_int32;      // the suffix's least-recently-needed page
            bool bWanted = false;        // any page in the suffix is prefetch
        };
        TArray<FCandidateCell> Candidates;

        TArray<const UGaussianSplatPagedAsset*> Assets;
        Pool.GetRegisteredAssets(Assets);
        for (const UGaussianSplatPagedAsset* Asset : Assets)
        {
            const FGaussianSplatPoolResidency* Residency = Pool.FindResidency(Asset);
            if (Residency == nullptr)
            {
                continue;
            }
            for (int32 CellIndex = 0; CellIndex < Asset->Cells.Num(); ++CellIndex)
            {
                const FGaussianSplatPagedCell& Cell = Asset->Cells[CellIndex];
                const int32 Resident = Residency->ResidentTailPages.IsValidIndex(CellIndex)
                    ? Residency->ResidentTailPages[CellIndex]
                    : 0;
                if (Resident <= 0)
                {
                    continue;
                }

                // Walk the tail from the end and stop at the first required page:
                // what is left is a suffix, which is the only shape that may go.
                int32 Shallowest = Resident;
                int32 Rank = MAX_int32;
                bool bWanted = false;
                for (int32 PageInCell = Resident - 1; PageInCell >= 0; --PageInCell)
                {
                    const int32 GlobalPage = Cell.FirstTailPage + PageInCell;
                    const uint64 Key = PageKey(Asset, GlobalPage);
                    if (RequiredSeen.Contains(Key))
                    {
                        break;
                    }
                    Shallowest = PageInCell;
                    bWanted = bWanted || WantedSet.Contains(Key);
                    const int32 Needed = Residency->LastNeededTick.IsValidIndex(GlobalPage)
                        ? Residency->LastNeededTick[GlobalPage]
                        : MIN_int32;
                    Rank = FMath::Min(Rank, Needed);
                }
                if (Shallowest >= Resident)
                {
                    continue;   // the whole tail is required
                }
                Candidates.Add({Asset, CellIndex, Cell.FirstTailPage, Shallowest, Resident - 1, Rank, bWanted});
            }
        }

        Candidates.Sort([](const FCandidateCell& A, const FCandidateCell& B)
        {
            // A cell whose suffix is pure prefetch goes last: a page nothing wants
            // is always the better victim, but both yield to a required page.
            if (A.bWanted != B.bWanted) { return !A.bWanted; }
            return A.Rank < B.Rank;                                   // least recently needed cell first
        });

        int32 Freed = 0;
        for (const FCandidateCell& Cell : Candidates)
        {
            // Deepest first, so each eviction is that cell's LAST resident page at
            // the moment the pool applies it, and residency stays a prefix.
            for (int32 PageInCell = Cell.Deepest; PageInCell >= Cell.Shallowest && Freed < NeedPages; --PageInCell)
            {
                Plan.Evict.Add({Cell.Asset, Cell.FirstTailPage + PageInCell, Cell.CellIndex});
                ++Freed;
            }
            if (Freed >= NeedPages)
            {
                break;
            }
        }
    }

    if (!Plan.IsEmpty())
    {
        Pool.ApplyStreamPlan(MoveTemp(Plan), TickNumber);
    }

    // The required set's hash, for the picture-free determinism check (review m7):
    // the same pose reached forwards and backwards must hash equal. Required is
    // already sorted, so the hash does not depend on the walk order.
    for (const FWantedPage& Page : Required)
    {
        Stats.RequiredHash = CityHash64WithSeed(
            reinterpret_cast<const char*>(&Page.GlobalPage), sizeof(Page.GlobalPage), Stats.RequiredHash);
    }

    LastStats = Stats;
    LastStats.GateMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

    if (CVarGateLog.GetValueOnAnyThread() != 0)
    {
        UE_LOG(
            LogGaussianSplatStream,
            Display,
            TEXT("gate tick %d: %d group(s), %d view(s), %d cells visited, required %d pages, wanted %d, %.3f ms, hash %llx"),
            TickNumber, Stats.Groups, Stats.Views, Stats.CellsVisited,
            Stats.RequiredPages, Stats.WantedPages, LastStats.GateMs, Stats.RequiredHash);
    }
}
