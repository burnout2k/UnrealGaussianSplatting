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

    // Fix 7 step 1: the d_full multiple at which the gate also prices the required set, so a
    // controller can ask "would one step up fit?" before taking it. Over the cells already
    // visited only -- one extra pass of PagesAt per tick, no second walk.
    TAutoConsoleVariable<float> CVarProbeStep(
        TEXT("r.GaussianSplat.Stream.ProbeStep"),
        2.0f,
        TEXT("d_full multiple the gate prices the required set at, beside the current one, as a lower bound\n")
        TEXT("on the demand one detail step up. 1 disables the extra pass."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarGateLog(
        TEXT("r.GaussianSplat.Stream.LogGate"),
        0,
        TEXT("1: one line per gate tick with the group set, the required and wanted page counts and the timings."),
        ECVF_RenderThreadSafe);

    // Fix 7: the detail controller (fix7-controller-plan.md v3, fix7-prototype-brief.md). One
    // multiplier on d_full, raised while four limits allow and lowered when one binds. OFF by
    // default, and off means the multiplier is 1.0 and the controller does not run, so the gate
    // and the views compute exactly what they did before it existed (G7d).
    TAutoConsoleVariable<int32> CVarCtlEnable(
        TEXT("r.GaussianSplat.Ctl.Enable"),
        0,
        TEXT("1: the gate raises d_full by Ctl.Step while every limit is clear and lowers it on the windowed drop\n")
        TEXT("rules, between Ctl.Min and Ctl.Max. 0 (default): the multiplier is 1.0 and none of it runs."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarCtlMin(
        TEXT("r.GaussianSplat.Ctl.Min"),
        1.0f,
        TEXT("Lower clamp on the detail multiplier. 1.0 = never below shipped detail."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarCtlMax(
        TEXT("r.GaussianSplat.Ctl.Max"),
        4.0f,
        TEXT("Upper clamp on the detail multiplier."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarCtlStep(
        TEXT("r.GaussianSplat.Ctl.Step"),
        1.25f,
        TEXT("Multiplicative step, up or down. While Ctl.Enable is on it also stands in for Stream.ProbeStep, so\n")
        TEXT("the profile line's `next` prices exactly the step the controller would take."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarCtlDwellTicks(
        TEXT("r.GaussianSplat.Ctl.DwellTicks"),
        30,
        TEXT("Consecutive gate ticks with every limit clear before one step up."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarCtlLockoutTicks(
        TEXT("r.GaussianSplat.Ctl.LockoutTicks"),
        60,
        TEXT("Gate ticks after a step down during which no step up is taken."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarCtlCycleTargetMs(
        TEXT("r.GaussianSplat.Ctl.CycleTargetMs"),
        85.0f,
        TEXT("Raise only while the window's longest cycle is under this, milliseconds."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarCtlCycleBarMs(
        TEXT("r.GaussianSplat.Ctl.CycleBarMs"),
        100.0f,
        TEXT("The cycle bar, milliseconds: a step down once Ctl.DropCount of the window's cycles exceed it."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarCtlDropCount(
        TEXT("r.GaussianSplat.Ctl.DropCount"),
        3,
        TEXT("Cycles over Ctl.CycleBarMs in the window that make a step down. More than one, so that a single\n")
        TEXT("hitch never does (review M6a)."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarCtlWindowTicks(
        TEXT("r.GaussianSplat.Ctl.WindowTicks"),
        30,
        TEXT("Gate ticks the drop rules, the cycle maximum and the budget limb are judged over."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarCtlDemandCeiling(
        TEXT("r.GaussianSplat.Ctl.DemandCeiling"),
        0.90f,
        TEXT("Raise only while the pre-fit demand one step up (the profile line's `next`) over the capacity is\n")
        TEXT("under this."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarCtlPinnedDropCount(
        TEXT("r.GaussianSplat.Ctl.PinnedDropCount"),
        3,
        TEXT("Step down once the gate's overflow multiplier m was below 1 on this many ticks of the window."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarCtlMaxCells(
        TEXT("r.GaussianSplat.Ctl.MaxCells"),
        2000,
        TEXT("Raise only while the gate visits fewer cells than this. The gate-CPU limb, in cells rather than\n")
        TEXT("ms because cells are deterministic and are what the ms are made of (review M7)."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarCtlUseCycleTime(
        TEXT("r.GaussianSplat.Ctl.UseCycleTime"),
        1,
        TEXT("1 (default): the cycle-time limb votes. 0: it is off. It is the only non-deterministic limb, so a\n")
        TEXT("same-route bracket with the controller on needs it 0 to pair tick for tick."),
        ECVF_RenderThreadSafe);

    // Fix 7 iteration 2 (fix7-memory-brief.md): place memory. With the controller on, the 1,097 MiB
    // drive dropped at a dense spot, left it, climbed again and re-entered the SAME spots at the same
    // k -- every drop a pinning drop. `next/cap` is a lower bound over the cells already visited, so
    // it under-prices exactly those spots, and the controller had no memory of where it was wrong.
    TAutoConsoleVariable<int32> CVarCtlMemory(
        TEXT("r.GaussianSplat.Ctl.Memory"),
        1,
        TEXT("1 (default): remember where the gate's pinning (m < 1) forced a step down and from which k, and\n")
        TEXT("hold a raise back to that k while the ego is within Ctl.MemoryRadiusCm of it. 0: no memory, and\n")
        TEXT("the records held are forgotten. Only acts while Ctl.Enable is on."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarCtlMemorySlots(
        TEXT("r.GaussianSplat.Ctl.MemorySlots"),
        32,
        TEXT("Drop records kept; when a new one does not fit, the oldest goes."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarCtlMemoryRadiusCm(
        TEXT("r.GaussianSplat.Ctl.MemoryRadiusCm"),
        15000.0f,
        TEXT("World-XY distance from a drop record, centimetres, within which a raise to its k or above is held.\n")
        TEXT("15000 = 150 m, the scale of the required set's radius at the shipped d_full."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarCtlMemoryTicks(
        TEXT("r.GaussianSplat.Ctl.MemoryTicks"),
        6000,
        TEXT("Gate ticks a drop record lives, so a long drive forgets. 0 = never."),
        ECVF_RenderThreadSafe);

    // Fix 7 iteration 3 (fix7-lookahead-brief.md): price k where the ego is GOING. On the 1,097 MiB
    // drive the controller climbed on open road and entered each dense spot at too high a k; the gate
    // pinned and it dropped twice. Place memory never fired -- the route never revisits. The raise's
    // only VRAM signal, `next`, is priced over the cells around the CURRENT pose, so a dense block a
    // few seconds ahead read "fine" until the car was inside it. The gate already projects look-ahead
    // probes for prefetch; the furthest of them is where the controller now looks too.
    TAutoConsoleVariable<int32> CVarCtlLookAhead(
        TEXT("r.GaussianSplat.Ctl.LookAhead"),
        1,
        TEXT("1 (default): the VRAM limb also prices k around the furthest look-ahead pose. A raise waits while\n")
        TEXT("one step up would not fit there under Ctl.DemandCeiling, and a step down comes early once the\n")
        TEXT("current k would not fit there (Ctl.AheadDropRatio). 0: iteration 2's limb, current pose only, for\n")
        TEXT("A/B -- the profile line's `ahead` is still priced. Only acts while Ctl.Enable is on."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarCtlAheadDropRatio(
        TEXT("r.GaussianSplat.Ctl.AheadDropRatio"),
        0.8f,
        TEXT("Step down early, before the gate pins, once the current k's demand around the furthest look-ahead\n")
        TEXT("pose (the profile line's `ahead` now) over the capacity reaches this. 1.0 = it would not fit the\n")
        TEXT("pool. Never inside a lockout. Logged as \"DOWN, ahead\"."),
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

    // Fix 7 iteration 3: one cell around the FURTHEST look-ahead pose, for the controller's price of
    // k there. The same record as a required cell with Distance measured to that probe instead of
    // probe 0, so PagesAt prices it exactly as it prices the required set and `ahead` and `next`
    // cannot disagree on what a cell costs. Its own list, never merged into RequiredCells (D9).
    using FAheadCell = FRequiredCell;

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

    // Fix 7: the detail controller's multiplier, carried exactly like the overflow one. The gate
    // publishes it at the START of its tick, before the required set is built, so the gate leads:
    // this tick's pages and the views that render after this tick use the same value. A frame the
    // render thread is still drawing when the gate publishes may read the new value against the
    // old pages and draw misses for a tick after a raise -- accepted for the prototype, visible on
    // the profile line, and v3 §4's gate-stamped ramp is the refinement.
    std::atomic<float> GDetailMultiplier{1.0f};

    // Fix 7: the views' feedback to the controller, counted per selection since the gate last
    // drained it. It mirrors LastVisibleCount -> OccMinVisible (GaussianSplatPasses.cpp): the views
    // produce the value, a later frame consumes it, and the last value holds until a newer one
    // arrives (FDetailControl::BoundViews). That channel is per-view render-thread state; this one
    // crosses to the game thread, so it is atomics, relaxed like GOverflowMultiplier -- a report
    // that races the drain lands one tick later, which a window of ticks does not notice.
    std::atomic<int32> GViewReportSelections{0};
    std::atomic<int32> GViewReportBudgetBound{0};
    std::atomic<int32> GViewReportMissed{0};

    // Fix 7: the controller's state. Game thread only -- RunGate is its one reader and writer.
    struct FDetailControl
    {
        float Multiplier = 1.0f;       // k, as the next tick will publish it
        int32 Dwell = 0;               // consecutive ticks with every limit clear
        int32 Lockout = 0;             // ticks left during which no step up is taken

        // The views' last report, HELD across ticks on which no view drew: the rig renders on one
        // gate tick in three, and the two between must not read as "nothing bound, nothing missed".
        int32 BoundViews = 0;
        int32 MissedViews = 0;

        double LastTickStart = 0.0;    // the previous gate tick's start; 0 = nothing to measure from

        // One slot per gate tick over the last WindowTicks, sharing one head, zero-filled so a
        // window that is not full yet reads as clear.
        TArray<float> TickMs;
        TArray<uint8> Pinned;
        TArray<int32> Bound;
        int32 Head = 0;

        // Place memory: one record per pinning drop, oldest first, at most Ctl.MemorySlots -- a ring
        // in effect, kept as a plain array because it is a few dozen entries scanned once per raise.
        // NOT cleared by ResetWindow: a drop spends the window's evidence, not what it learned.
        struct FDropRecord
        {
            FVector2D Where = FVector2D::ZeroVector;   // world XY of the first group's centre at the drop
            float KFrom = 1.0f;                        // the multiplier the drop stepped down FROM
            int32 Born = 0;                            // Ticks at the drop
            bool bAnnounced = false;                   // its first hold has had its Display line
        };
        TArray<FDropRecord> Memory;
        int32 Ticks = 0;               // controller ticks, the clock records age by

        void ResetWindow(int32 Window)
        {
            TickMs.Init(0.0f, Window);
            Pinned.Init(0, Window);
            Bound.Init(0, Window);
            Head = 0;
        }
    };
    FDetailControl GDetailControl;

    void GetDetailLimits(float& OutMin, float& OutMax)
    {
        OutMin = FMath::Max(0.01f, CVarCtlMin.GetValueOnAnyThread());
        OutMax = FMath::Max(OutMin, CVarCtlMax.GetValueOnAnyThread());
    }

    // Every tick, controller on or off, so the counts never pile up while it is off and a later
    // Enable starts from one tick's worth.
    void DrainViewReports(FDetailControl& Ctl)
    {
        const int32 Selections = GViewReportSelections.exchange(0, std::memory_order_relaxed);
        const int32 Bound = GViewReportBudgetBound.exchange(0, std::memory_order_relaxed);
        const int32 Missed = GViewReportMissed.exchange(0, std::memory_order_relaxed);
        if (Selections > 0 || Bound > 0 || Missed > 0)
        {
            Ctl.BoundViews = Bound;
            Ctl.MissedViews = Missed;
        }
    }

    struct FCycleWindow
    {
        float MaxMs = 0.0f;     // the window's longest tick-to-tick wall clock
        int32 OverBar = 0;      // ticks in the window over Ctl.CycleBarMs
    };

    // Fix 7, the cycle limb -- the ONLY non-deterministic one (review M7). Wall clock differs between
    // two runs of the same route, so it lives here alone and r.GaussianSplat.Ctl.UseCycleTime 0 skips
    // this function and nothing else.
    //
    // The gate runs on the game thread inside the sync tick, so the wall clock between two of its
    // ticks is the server's view of the cycle. The rig renders on one tick in three (~75 ms; the other
    // two ~0.5 ms), so a cycle is the max of the last three deltas, and the window's longest cycle is
    // simply its longest delta. The drop rule counts DELTAS over the bar, not max-of-threes: one slow
    // tick sits in three consecutive max-of-threes, which would let a single hitch reach DropCount 3 on
    // its own -- the one-hitch drop the windowed rule exists to prevent (M6a).
    FCycleWindow UpdateCycleLimb(FDetailControl& Ctl, double TickStart, int32 Slot, float BarMs)
    {
        Ctl.TickMs[Slot] = Ctl.LastTickStart > 0.0
            ? static_cast<float>((TickStart - Ctl.LastTickStart) * 1000.0)
            : 0.0f;
        Ctl.LastTickStart = TickStart;

        FCycleWindow Cycle;
        for (const float Ms : Ctl.TickMs)
        {
            Cycle.MaxMs = FMath::Max(Cycle.MaxMs, Ms);
            Cycle.OverBar += Ms > BarMs ? 1 : 0;
        }
        return Cycle;
    }

    // Fix 7 place memory: whether a raise from Before to Next is held, because a pinning drop from Next
    // or lower happened within RadiusCm (world XY) of Where. Each record that holds it says so once,
    // the first time it does; after that only state 6 on the profile line shows the hold.
    bool HoldRaiseByMemory(FDetailControl& Ctl, const FVector2D& Where, float Before, float Next, float RadiusCm)
    {
        // A record dropped from Next ITSELF must hold the raise back to Next, but k returns there through
        // a divide and a multiply by Ctl.Step that float does not undo: 4.0 / 1.25 / 1.25 x 1.25 lands one
        // ulp below 3.2f. Without the slack exactly the case this exists for slips through. 1e-4 is far
        // below any step anyone would set.
        constexpr float KSlack = 1.0e-4f;
        const double RadiusSq = FMath::Square(static_cast<double>(RadiusCm));
        bool bHold = false;
        for (FDetailControl::FDropRecord& Record : Ctl.Memory)
        {
            const double DistSq = FVector2D::DistSquared(Record.Where, Where);
            if (Record.KFrom > Next * (1.0f + KSlack) || DistSq > RadiusSq)
            {
                continue;
            }
            bHold = true;
            if (!Record.bAnnounced)
            {
                Record.bAnnounced = true;
                // Worded so that it never contains "detail control: k" -- the owner's report counts
                // those lines as steps, and a held raise is not one.
                UE_LOG(
                    LogGaussianSplatStream,
                    Display,
                    TEXT("detail control: memory holds the raise k %.3f -> %.3f: a pinning drop from k %.3f at ")
                    TEXT("(%.0f, %.0f) cm, %.0f m away (radius %.0f m), %d ticks ago; dwell kept"),
                    Before, Next, Record.KFrom, Record.Where.X, Record.Where.Y, FMath::Sqrt(DistSq) / 100.0,
                    RadiusCm / 100.0f, Ctl.Ticks - Record.Born);
            }
        }
        return bHold;
    }

    // Fix 7: the law, once per gate tick, after the fit and with this tick's stats filled. Where
    // each limit's number comes from: pre-fit demand one step up over capacity (DemandNextPages /
    // CapacityPages, 4d9d3ad) and, with Ctl.LookAhead, the same around the furthest look-ahead pose
    // (DemandAheadPages, DemandAheadNowPages); the gate's walk (CellsVisited); the views' own budget
    // (ReportViewSelection); the cycle (UpdateCycleLimb). The first three are functions of the
    // pose history and reproduce on a same-route bracket; the cycle does not. Place memory
    // (HoldRaiseByMemory) is a function of the pose history too: Centre is where a drop is recorded
    // and a raise is checked.
    void RunDetailControl(FGaussianSplatGateStats& Stats, double TickStart, float Published, const FVector& Centre)
    {
        FDetailControl& Ctl = GDetailControl;
        const int32 Window = FMath::Clamp(CVarCtlWindowTicks.GetValueOnAnyThread(), 1, 3600);
        if (Ctl.Pinned.Num() != Window)
        {
            Ctl.ResetWindow(Window);
        }
        DrainViewReports(Ctl);

        // Place memory upkeep, before anything reads the records. Off means off, like the cycle limb:
        // nothing held, and nothing stale left for when it comes back on.
        ++Ctl.Ticks;
        const bool bMemory = CVarCtlMemory.GetValueOnAnyThread() != 0;
        const int32 MemorySlots = FMath::Clamp(CVarCtlMemorySlots.GetValueOnAnyThread(), 0, 1024);
        const int32 MemoryTicks = FMath::Max(0, CVarCtlMemoryTicks.GetValueOnAnyThread());
        const float MemoryRadiusCm = FMath::Max(0.0f, CVarCtlMemoryRadiusCm.GetValueOnAnyThread());
        if (!bMemory)
        {
            Ctl.Memory.Reset();
        }
        else
        {
            if (MemoryTicks > 0)
            {
                Ctl.Memory.RemoveAll([&Ctl, MemoryTicks](const FDetailControl::FDropRecord& Record)
                {
                    return Ctl.Ticks - Record.Born >= MemoryTicks;
                });
            }
            // Ctl.MemorySlots lowered at run time: the oldest go (RemoveAll keeps the order).
            if (Ctl.Memory.Num() > MemorySlots)
            {
                Ctl.Memory.RemoveAt(0, Ctl.Memory.Num() - MemorySlots);
            }
        }
        const FVector2D Where(Centre.X, Centre.Y);

        const int32 Slot = Ctl.Head;
        Ctl.Head = (Ctl.Head + 1) % Window;
        Ctl.Pinned[Slot] = Stats.OverflowMultiplier < 1.0f ? 1 : 0;
        Ctl.Bound[Slot] = Ctl.BoundViews;

        const float BarMs = CVarCtlCycleBarMs.GetValueOnAnyThread();
        const bool bUseCycle = CVarCtlUseCycleTime.GetValueOnAnyThread() != 0;
        FCycleWindow Cycle;
        if (bUseCycle)
        {
            Cycle = UpdateCycleLimb(Ctl, TickStart, Slot, BarMs);
        }
        else
        {
            // Off means off: nothing measured, and nothing stale left for when it comes back on.
            Ctl.TickMs.Init(0.0f, Window);
            Ctl.LastTickStart = 0.0;
        }

        int32 PinnedTicks = 0;
        int32 BoundTicks = 0;
        for (int32 Index = 0; Index < Window; ++Index)
        {
            PinnedTicks += Ctl.Pinned[Index];
            BoundTicks += Ctl.Bound[Index] > 0 ? 1 : 0;
        }

        float MinK = 1.0f;
        float MaxK = 1.0f;
        GetDetailLimits(MinK, MaxK);
        const float Step = FMath::Max(1.0f, CVarCtlStep.GetValueOnAnyThread());
        const int32 DwellTicks = FMath::Max(0, CVarCtlDwellTicks.GetValueOnAnyThread());
        const float TargetMs = CVarCtlCycleTargetMs.GetValueOnAnyThread();
        const float Ceiling = CVarCtlDemandCeiling.GetValueOnAnyThread();
        const int32 MaxCells = CVarCtlMaxCells.GetValueOnAnyThread();
        const int32 DropCount = FMath::Max(1, CVarCtlDropCount.GetValueOnAnyThread());
        const int32 PinnedDropCount = FMath::Max(1, CVarCtlPinnedDropCount.GetValueOnAnyThread());
        const bool bLookAhead = CVarCtlLookAhead.GetValueOnAnyThread() != 0;
        const float AheadDropRatio = CVarCtlAheadDropRatio.GetValueOnAnyThread();

        // Pages over the capacity; no capacity reads as "does not fit".
        const auto OverCapacity = [&Stats](int32 Pages) -> double
        {
            return Stats.CapacityPages > 0
                ? static_cast<double>(Pages) / static_cast<double>(Stats.CapacityPages)
                : TNumericLimits<double>::Max();
        };
        // `next` is a LOWER bound on one step up's demand -- cells beyond this tick's walk are not in
        // it -- and ProbeStep is Ctl.Step while the controller is on, so it prices that step.
        const double DemandRatio = OverCapacity(Stats.DemandNextPages);
        // Iteration 3: the same step priced around the furthest look-ahead pose, and the current k priced
        // there. A raise needs room at BOTH poses, because the k it buys is still in force when the ego
        // gets where it is looking -- and the dense blocks `next` could not see are exactly the ones the
        // controller entered too high.
        const double AheadRatio = OverCapacity(Stats.DemandAheadPages);
        const double AheadNowRatio = OverCapacity(Stats.DemandAheadNowPages);
        const double RaiseRatio = bLookAhead
            ? OverCapacity(FMath::Max(Stats.DemandNextPages, Stats.DemandAheadPages))
            : DemandRatio;
        const bool bCycleClear = !bUseCycle || Cycle.MaxMs < TargetMs;
        const bool bDemandClear = RaiseRatio < static_cast<double>(Ceiling);
        const bool bCellsClear = Stats.CellsVisited < MaxCells;
        const bool bBudgetClear = BoundTicks == 0;
        const bool bClear = bCycleClear && bDemandClear && bCellsClear && bBudgetClear;

        const bool bCycleDrop = bUseCycle && Cycle.OverBar >= DropCount;
        const bool bPinnedDrop = PinnedTicks >= PinnedDropCount;

        // Never step while a fill is in progress (a view missed, on the last frame any view drew), and
        // never while no view draws: with nothing drawn every limit reads clear, and dwelling blind
        // would walk k to Max before the first camera frame. Waiting neither counts nor resets dwell.
        const bool bWait = Ctl.MissedViews > 0 || Stats.Views == 0;
        const bool bLocked = Ctl.Lockout > 0;
        // Iteration 3: an EARLY step down -- the current k will not fit where the ego is about to be.
        // It is a prediction from a straight-line extrapolation, not evidence like the two rules above,
        // so it waits out a lockout: at most one early step per lockout, with the pinning rule still the
        // backstop when one step was not enough. A tick both would drop on is the evidence's drop.
        const bool bAheadDrop = bLookAhead && !bLocked && AheadNowRatio >= static_cast<double>(AheadDropRatio);
        const float Before = Ctl.Multiplier;
        bool bDropRuleFired = false;
        uint8 State = bLocked ? 5 : 1;

        if (bWait)
        {
            // Hold. The windows above still advanced, so the evidence keeps accumulating.
        }
        else if (bCycleDrop || bPinnedDrop || bAheadDrop)
        {
            bDropRuleFired = true;
            Ctl.Multiplier = FMath::Max(MinK, Before / Step);
            Ctl.Dwell = 0;
            Ctl.Lockout = FMath::Max(0, CVarCtlLockoutTicks.GetValueOnAnyThread());
            // The window's evidence is spent on this step. Left in place it would fire again next
            // tick, and the same three slow cycles would walk k all the way down to Min.
            Ctl.ResetWindow(Window);
            if (Ctl.Multiplier < Before)
            {
                State = 4;
                FString Reason;
                if (bCycleDrop)
                {
                    Reason = FString::Printf(TEXT("cycle over %.1f ms on %d of %d ticks (max %.1f ms)"),
                        BarMs, Cycle.OverBar, Window, Cycle.MaxMs);
                }
                if (bPinnedDrop)
                {
                    Reason += FString::Printf(TEXT("%sgate m < 1 on %d of %d ticks"),
                        Reason.IsEmpty() ? TEXT("") : TEXT(", "), PinnedTicks, Window);
                }
                // An early drop says "ahead", not "bound", so the step log tells a predicted drop from one
                // the window's evidence forced. Still "detail control: k", which the owner's report counts
                // as a step -- it is one.
                const bool bAheadOnly = !bCycleDrop && !bPinnedDrop;
                if (bAheadOnly)
                {
                    Reason = FString::Printf(
                        TEXT("the current k needs %d of %d pages around the look-ahead pose (%.3f >= %.2f)"),
                        Stats.DemandAheadNowPages, Stats.CapacityPages, AheadNowRatio, AheadDropRatio);
                }
                UE_LOG(
                    LogGaussianSplatStream,
                    Display,
                    TEXT("detail control: k %.3f -> %.3f DOWN, %s: %s; no step up for %d ticks"),
                    Before, Ctl.Multiplier, bAheadOnly ? TEXT("ahead") : TEXT("bound"), *Reason, Ctl.Lockout);

                // Place memory: only a PINNING drop is a fact about this place -- the pool could not
                // hold its required set at Before here. A drop on the cycle rule alone is about the
                // frame, and is not recorded. A drop at Min is not a step, and records nothing either.
                // Nor does an early drop on the look-ahead alone: it is a prediction, about a place
                // ahead of Where, and the pool has not failed anywhere yet.
                if (bPinnedDrop && bMemory && MemorySlots > 0)
                {
                    if (Ctl.Memory.Num() >= MemorySlots)
                    {
                        Ctl.Memory.RemoveAt(0, Ctl.Memory.Num() - MemorySlots + 1);
                    }
                    Ctl.Memory.Add({Where, Before, Ctl.Ticks, false});
                }
            }
            else
            {
                // Already at Min: nothing to step, but the lockout still holds off a raise.
                State = Ctl.Lockout > 0 ? 5 : 1;
            }
        }
        else
        {
            Ctl.Dwell = bClear ? FMath::Min(Ctl.Dwell + 1, DwellTicks) : 0;
            const bool bRaiseDue = !bLocked && bClear && Ctl.Dwell >= DwellTicks && Before < MaxK;
            const float Next = FMath::Min(MaxK, Before * Step);
            if (bRaiseDue && bMemory && HoldRaiseByMemory(Ctl, Where, Before, Next, MemoryRadiusCm))
            {
                // Held. The dwell is NOT reset: it stays full, so the raise fires on the first tick the
                // ego is outside every record that holds it -- if every limit is still clear then.
                State = 6;
            }
            else if (bRaiseDue)
            {
                Ctl.Multiplier = Next;
                Ctl.Dwell = 0;
                State = 3;
                const FString CycleText = bUseCycle
                    ? FString::Printf(TEXT("cycle max %.1f < %.1f ms"), Cycle.MaxMs, TargetMs)
                    : FString(TEXT("cycle limb off"));
                // With Ctl.LookAhead 0 this reads exactly as iteration 2's line did.
                const FString DemandText = bLookAhead
                    ? FString::Printf(TEXT("max(next, ahead)/cap %.3f < %.2f (next %.3f, ahead %.3f)"),
                        RaiseRatio, Ceiling, DemandRatio, AheadRatio)
                    : FString::Printf(TEXT("next/cap %.3f < %.2f"), DemandRatio, Ceiling);
                UE_LOG(
                    LogGaussianSplatStream,
                    Display,
                    TEXT("detail control: k %.3f -> %.3f UP, every limit clear for %d ticks: %s, %s, ")
                    TEXT("cells %d < %d, no budget-bound view in %d ticks"),
                    Before, Ctl.Multiplier, DwellTicks, *CycleText, *DemandText,
                    Stats.CellsVisited, MaxCells, Window);
            }
            else if (!bLocked)
            {
                State = (bClear && Before < MaxK) ? 2 : 1;
            }
        }
        if (!bDropRuleFired && Ctl.Lockout > 0)
        {
            --Ctl.Lockout;
        }

        Stats.CtlMultiplier = Published;
        Stats.CtlCycleMs = Cycle.MaxMs;
        Stats.CtlState = State;
        Stats.CtlBudgetBoundViews = Ctl.BoundViews;
        Stats.CtlMemoryRecords = Ctl.Memory.Num();
    }
}

float FGaussianSplatStreamGate::GetOverflowMultiplier()
{
    return GOverflowMultiplier.load(std::memory_order_relaxed);
}

float FGaussianSplatStreamGate::GetDetailMultiplier()
{
    return GDetailMultiplier.load(std::memory_order_relaxed);
}

void FGaussianSplatStreamGate::ReportViewSelection(bool bBudgetBound, bool bMissed)
{
    if (bBudgetBound)
    {
        GViewReportBudgetBound.fetch_add(1, std::memory_order_relaxed);
    }
    if (bMissed)
    {
        GViewReportMissed.fetch_add(1, std::memory_order_relaxed);
    }
    GViewReportSelections.fetch_add(1, std::memory_order_relaxed);
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

    // Fix 7: a new map starts the detail controller from shipped, with no place memory (the records
    // are the old map's places), and a load is not a cycle.
    GDetailControl = FDetailControl();

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
        GDetailMultiplier.store(1.0f, std::memory_order_relaxed);
        return;
    }

    FGaussianSplatPagePool& Pool = FGaussianSplatPagePool::Get();
    if (!Pool.IsAllocated())
    {
        return;
    }

    const double StartTime = FPlatformTime::Seconds();
    ++TickNumber;

    // Fix 7: publish the detail multiplier FIRST, before the required set is built (see
    // GDetailMultiplier for the ordering); ComputeHalfFull reads it back below, on this thread. With
    // Ctl.Enable 0 it is 1.0, and the formula x 1.0 is exact in IEEE arithmetic, so every walk
    // radius, take and page count below is bit-identical to the gate without the controller.
    const bool bDetailControl = CVarCtlEnable.GetValueOnAnyThread() != 0;
    float DetailMultiplier = 1.0f;
    if (bDetailControl)
    {
        float MinK = 1.0f;
        float MaxK = 1.0f;
        GetDetailLimits(MinK, MaxK);
        GDetailControl.Multiplier = FMath::Clamp(GDetailControl.Multiplier, MinK, MaxK);
        DetailMultiplier = GDetailControl.Multiplier;
    }
    GDetailMultiplier.store(DetailMultiplier, std::memory_order_relaxed);

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
    // Fix 7 iteration 3: the cells the controller prices k at around the furthest look-ahead pose.
    // Filled only while Ctl.Enable is on, and keyed like RequiredCellIndex for the same reason.
    TArray<FAheadCell> AheadCells;
    TMap<uint64, int32> AheadCellIndex;

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
            // Fix 7: ComputeHalfFull applies the detail multiplier itself (bDetail true: every
            // component here is paged and streamed), before its floor and per-actor cap -- the one
            // place the views apply it too (SelectCellsScreen), so a raise respects the cap in both.
            // The walk radius, the required and wanted takes and PagesAt all derive from this one
            // value, so it covers the whole gate.
            const double HalfFull = GaussianSplatLod::ComputeHalfFull(
                Group.MaxFocalPx, PointSize, Asset->SizeRef, ActorScale, MaxFullDistanceM, true);

            // Beyond this distance every cell is floor-only, so there is nothing to
            // visit: keep > R_c needs 4 (HalfFull/d)^2 > MinFraction (review M4).
            //
            // That holds ONLY while MinFraction equals the baked floor. TakeAt clamps the
            // fraction from BELOW at MinFraction, so once MinFraction is raised above the
            // floor (r.GaussianSplat.Lod 0, which asks for 1.0, or LodMinFraction set past
            // the floor) every cell at EVERY distance needs more splats than the asset
            // pinned -- the radius is unbounded, not merely larger. Deriving it from the
            // raised MinFraction instead SHRINKS the walk towards d_full while the demand
            // beyond it grows, and the selection, which reads the same CVar, then asks for
            // tail pages this gate never fetched. Measured 2026-10-09: 26.5M misses on one
            // view, 61.5M across the rig, with the run reporting a healthy m 1.0000 -- and
            // the picture was quietly used as a "full detail" reference for a day.
            const bool bAboveBakedFloor = MinFraction > BakedFloor + UE_SMALL_NUMBER;
            const double FloorRadius = (MinFraction > 0.0f && !bAboveBakedFloor)
                ? HalfFull * 2.0 / FMath::Sqrt(static_cast<double>(MinFraction))
                : TNumericLimits<double>::Max() * 0.5;
            // This path makes the gate walk the whole asset every tick, and on a scene larger
            // than the pool the overflow multiplier will thin it. Both are visible in the
            // stats -- which is the point; the silence is what cost a day. The condition is a
            // CVar, so it applies to every asset equally and once per session is enough.
            static bool bWarnedAboveFloor = false;
            if (bAboveBakedFloor && !bWarnedAboveFloor)
            {
                bWarnedAboveFloor = true;
                UE_LOG(
                    LogGaussianSplatStream,
                    Warning,
                    TEXT("%s: LOD floor %.3f is above the asset's baked floor %.3f, so every cell needs tail ")
                    TEXT("pages at every distance and the gate must walk the whole asset. Frame time and the ")
                    TEXT("overflow multiplier are NOT comparable with a run at the baked floor. For a true ")
                    TEXT("full-detail reference use r.GaussianSplat.Stream 0 on a scene that fits the pool."),
                    *Asset->GetName(),
                    MinFraction,
                    BakedFloor);
            }

            // The current pose is REQUIRED; the look-ahead poses are WANTED. Both
            // are built from this tick's state only, so the required set is a
            // function of the current poses and nothing else (D9).
            // Probe 0 is the CURRENT pose and is the only one the required set
            // may use. The rest are look-ahead and feed the wanted set -- and,
            // while the controller runs, its price of k ahead (Fix 7 iteration 3),
            // which is never this tick's required set either.
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
            // An unbounded reach means "every cell of this asset", which is a walk over the
            // asset's own coordinate box -- NOT a probe box of unbounded half-width. Taking
            // the probe path with Reach near DBL_MAX used to hit a `continue` here and walk
            // NOTHING for the asset: req 0, floor-only, misses everywhere, while the log said
            // the opposite. That is the whole-asset path the old comment asked for.
            const double Reach = FloorRadius + Group.Radius;
            const bool bWholeAsset = bAboveBakedFloor || !(Reach < TNumericLimits<double>::Max() * 0.25);

            FIntVector Lo(MAX_int32, MAX_int32, MAX_int32);
            FIntVector Hi(MIN_int32, MIN_int32, MIN_int32);
            if (bWholeAsset)
            {
                Lo = Asset->GetCoordMin();
                Hi = Asset->GetCoordMax();
            }
            else
            {
                const double LocalReach = Reach / FMath::Max(ActorScale, UE_SMALL_NUMBER);
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
            }

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

                // Fix 7 iteration 3: the cells around the FURTHEST look-ahead pose, for the controller
                // to price k where the ego will be, not only where it is. Probes holds 4 entries or 1,
                // so the last is probe 3, or probe 0 when there is no look-ahead (LookAheadSec 0, or no
                // direction) -- whose distance is RequiredDistance. Inside the floor radius, the walk's
                // own reach: beyond it a cell is floor-only at this k, and like `next` the step-up
                // price is a lower bound. Skipped entirely with the controller off.
                if (bDetailControl)
                {
                    const int32 AheadProbe = Probes.Num() - 1;
                    const double AheadDistance =
                        AheadProbe == 0 ? RequiredDistance : DistanceFrom(Probes[AheadProbe].Centre);
                    if (AheadDistance < FloorRadius)
                    {
                        const uint64 CellKey = PageKey(Asset, CellIndex);
                        if (const int32* Found = AheadCellIndex.Find(CellKey))
                        {
                            // As in the required set: a cell reached twice is priced once, at the
                            // closer distance, which is the larger take.
                            FAheadCell& Existing = AheadCells[*Found];
                            if (AheadDistance < Existing.Distance)
                            {
                                Existing.Distance = AheadDistance;
                                Existing.HalfFull = FMath::Max(Existing.HalfFull, HalfFull);
                            }
                        }
                        else
                        {
                            AheadCellIndex.Add(CellKey, AheadCells.Num());
                            AheadCells.Add({Asset, CellIndex, AheadDistance, HalfFull, MinFraction});
                        }
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
    // The pre-fit demand is THE pressure signal (Fix 7): it is the only number here that keeps
    // moving once the pool is full. It was always computed for the test below; now it is kept.
    const int64 Demand = PagesAtMultiplier(1.0);
    Stats.DemandPages = static_cast<int32>(FMath::Min<int64>(Demand, MAX_int32));
    Stats.CapacityPages = static_cast<int32>(FMath::Min<int64>(Capacity, MAX_int32));
    {
        // Fix 7: with the controller on, the probe IS its step, so `next` prices exactly the
        // candidate it would take. Off, this is the CVar as before.
        const float ProbeStep = FMath::Max(
            1.0f, bDetailControl ? CVarCtlStep.GetValueOnAnyThread() : CVarProbeStep.GetValueOnAnyThread());
        Stats.ProbeStep = ProbeStep;
        Stats.DemandNextPages = ProbeStep > 1.0f
            ? static_cast<int32>(FMath::Min<int64>(PagesAtMultiplier(ProbeStep), MAX_int32))
            : Stats.DemandPages;
    }
    double Multiplier = 1.0;
    if (Demand > Capacity)
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

    // Fix 7: the controller, after the fit and with every stat above filled. Off, it only drains
    // the views' reports and forgets its state, so a later Enable starts from shipped.
    if (bDetailControl)
    {
        // Iteration 3: price the look-ahead cells, here and only here, so off pays nothing. Both sums
        // in one pass. The step is Stats.ProbeStep -- Ctl.Step while the controller runs -- the ratio
        // `next` uses, so `ahead` and `next` price the same candidate k; 1.0 is the current k.
        int64 AheadNow = 0;
        int64 AheadNext = 0;
        for (const FAheadCell& Cell : AheadCells)
        {
            const int32 Now = Cell.PagesAt(1.0);
            AheadNow += Now;
            AheadNext += Stats.ProbeStep > 1.0f ? Cell.PagesAt(Stats.ProbeStep) : Now;
        }
        Stats.DemandAheadNowPages = static_cast<int32>(FMath::Min<int64>(AheadNow, MAX_int32));
        Stats.DemandAheadPages = static_cast<int32>(FMath::Min<int64>(AheadNext, MAX_int32));

        // Place memory locates a tick by ONE point, the first group's centre -- what the walk measures
        // from. With several egos that is the first one's only; one point is what a record can hold.
        // With no group there is no view, the controller waits, and the zero vector is never used.
        const FVector MemoryCentre = Groups.IsEmpty() ? FVector::ZeroVector : Groups[0].Centre;
        RunDetailControl(Stats, StartTime, DetailMultiplier, MemoryCentre);
    }
    else
    {
        GDetailControl = FDetailControl();
        DrainViewReports(GDetailControl);
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
