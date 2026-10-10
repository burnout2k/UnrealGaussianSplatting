#pragma once

// The settle gate (plan D4): every tick, decide which pages must be resident for
// the poses the cameras are ALREADY at, upload the missing ones, and evict what
// is no longer wanted.
//
// Where it runs, and why it matters: a raw FTickFunction at TG_LastDemotable, on
// the persistent level. That is after every actor, sensor and camera has its
// final pose for this tick, and before OnWorldPostActorTick, where CARLA
// captures. A UTickableWorldSubsystem would tick too early (LevelTick.cpp:1567),
// and a delegate would depend on registration order. CARLA needs no change.
//
// The contract it exists to keep (D9): the DRAWN picture is a function of the
// current poses only. Residency may depend on history -- LRU, the protected age,
// the view stamps all do -- but the required set is computed from this tick's
// poses alone, so the same pose reached forwards, backwards or by teleport asks
// for the same pages.

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"

class AActor;
class UWorld;
class UGaussianSplatPagedAsset;

// One view that actually drew splats, stamped by the splat pass. Grouping and the
// required set are built from these rather than from every USceneCaptureComponent2D
// in the world, because a non-sensor capture -- tartu_demo's sky blueprint carries
// one, and it is copied into every test map -- would otherwise join the view set,
// form its own group and demand pages for most of the map every tick (review M6).
struct FGaussianSplatGateView
{
    // The capture's view owner (SceneCaptureRendering.cpp:1284 sets it from
    // GetViewOwner(); FSceneView::ViewActor carries it). For CARLA's sensors this
    // is the sensor actor, whose attach root is the ego.
    //
    // WEAK, not raw: this is written on the render thread and read on the game
    // thread a tick later, by which time the actor may be gone. A raw pointer
    // would be an IsValid() call on freed memory.
    TWeakObjectPtr<const AActor> ViewActor;

    // False for an EDITOR viewport, which has no ViewActor at all -- that field is
    // set only for scene captures (from GetViewOwner()) and for a player's view
    // target. Without this the two cases are indistinguishable from a null weak
    // pointer and the editor's view is dropped as "its actor was destroyed",
    // which is why the gate saw 0 views on its first run.
    bool bHasOwner = true;

    FVector Origin = FVector::ZeroVector;

    // The view's focal length in pixels. HalfFull is NOT stored here because it
    // depends on the component too (PointSize, SizeRef, the actor scale), so the
    // gate computes it per component from the group's LARGEST focal -- the take
    // rises with HalfFull, and the gate has to bound every view.
    double FocalPx = 1.0;

    // The gate tick this was last seen on. Views older than one tick are dropped:
    // a camera that stopped rendering stops holding pages.
    int32 Stamp = 0;
};

// The cameras of one ego, reduced to a sphere. Every camera x with
// |x - c| + 2 m <= R satisfies dist(c, box) - R <= dist(x, box), so the rule's
// take is >= that camera's take for every cell (plan D4).
struct FGaussianSplatGateGroup
{
    const AActor* Root = nullptr;
    FVector Centre = FVector::ZeroVector;
    double Radius = 0.0;          // the largest camera offset, plus the margin
    double MaxFocalPx = 1.0;      // the largest in the group
    FVector Velocity = FVector::ZeroVector;

    // Where the ego FACES, which is where it will go next even when it is not
    // moving. A car stopped at a red light has zero velocity but is about to
    // drive forward, and prefetching from velocity alone drops the road ahead
    // exactly when it is least affordable to re-upload it (Allan, 2026-10-07).
    FVector Forward = FVector::ZeroVector;

    int32 ViewCount = 0;
};

// What one gate tick decided and what it cost, for the stats line (D8).
struct FGaussianSplatGateStats
{
    int32 Groups = 0;
    int32 Views = 0;
    int32 CellsVisited = 0;
    int32 RequiredPages = 0;
    int32 WantedPages = 0;
    int32 ResidentPages = 0;
    float OverflowMultiplier = 1.0f;
    // Fix 7 step 1: the pressure signals a controller needs, which m and RequiredPages are
    // not. DemandPages is the required set BEFORE the fit (PagesAtMultiplier(1.0)) -- m only
    // moves once this exceeds the pool, and RequiredPages is counted AFTER the fit, so it
    // saturates at capacity exactly when it is needed. DemandNextPages is the same set with
    // d_full scaled by ProbeStep, over the cells this tick visited: a LOWER bound on what one
    // step up would need (cells beyond the current walk radius are not in it).
    int32 DemandPages = 0;
    int32 CapacityPages = 0;
    int32 DemandNextPages = 0;
    float ProbeStep = 1.0f;
    // Fix 7: the detail controller (r.GaussianSplat.Ctl.*). CtlMultiplier is k, the d_full
    // multiple THIS tick's required set was built with (1.0 = shipped); a step taken this tick
    // applies from the next one, which is why state 3/4 print the k before the step. CtlCycleMs
    // is the window's largest tick-to-tick wall clock (0 with Ctl.UseCycleTime 0). CtlState:
    // 0 off, 1 hold, 2 dwell, 3 raised this tick, 4 dropped this tick, 5 lockout, 6 a due raise
    // held by place memory (Ctl.Memory).
    // CtlBudgetBoundViews: views whose OWN budget bisection bound, on the last frame any drew.
    // CtlMemoryRecords: the place-memory drop records held at the end of this tick.
    float CtlMultiplier = 1.0f;
    float CtlCycleMs = 0.0f;
    uint8 CtlState = 0;
    int32 CtlBudgetBoundViews = 0;
    int32 CtlMemoryRecords = 0;
    double GateMs = 0.0;

    // Hash of the required set (asset, cell, page count) over every group. The
    // picture-free determinism check (review m7): the same pose reached forwards
    // and backwards must hash equal, which catches gate nondeterminism directly
    // and costs nothing to compare.
    uint64 RequiredHash = 0;
};

class FGaussianSplatStreamGate
{
public:
    static FGaussianSplatStreamGate& Get();

    // Called from the splat pass for every view it ran on. Cheap and lock-guarded:
    // the render thread writes, the game thread reads once a tick.
    void StampView(const AActor* ViewActor, const FVector& Origin, double FocalPx);

    // Registered by the world subsystem's Initialize/Deinitialize, so the tick
    // function's lifetime is exactly the world's.
    static void OnWorldInitialized(UWorld* World);
    static void OnWorldTornDown(UWorld* World);

    // The tick function lives for as long as the world does.
    void RegisterWorld(UWorld* World);
    void UnregisterWorld(UWorld* World);

    // The gate itself. Public so the tick function can call it, and so a test can.
    void RunGate(UWorld* World);

    const FGaussianSplatGateStats& GetLastStats() const { return LastStats; }

    // D4: "The same multiplier is the upper bound of every view's bisection that
    // frame." The gate solves it on the game thread; the selection reads it on the
    // render thread. 1.0 means the required set fitted and nothing is clamped.
    //
    // Without this the gate trims the required set to m and uploads exactly that,
    // and the views then select at 1.0 and ask for the pages it deliberately did
    // not upload -- which is what 1.9M misses at a 600 MiB cap turned out to be.
    static float GetOverflowMultiplier();

    // Fix 7: the detail controller's actuator, a second multiplier on d_full beside the
    // overflow one and carried the same way -- solved by the gate on the game thread, read by
    // the selection on the render thread. GaussianSplatLod::ComputeHalfFull reads it and scales
    // its formula term BEFORE the floor and the per-actor cap, so the gate and the views apply
    // it in one place and cannot drift. 1.0 unless r.GaussianSplat.Ctl.Enable.
    static float GetDetailMultiplier();

    // Fix 7: the views' half of the controller's feedback, once per selection the gate serves
    // (render thread). Whether the view's OWN budget bisection bound -- before the gate's
    // clamp, which the controller already sees as m -- and whether it drew a miss.
    static void ReportViewSelection(bool bBudgetBound, bool bMissed);

private:
    // The stamps, written on the render thread and drained on the game thread.
    mutable FCriticalSection StampLock;
    TArray<FGaussianSplatGateView> StampedViews;

    // Groups the stamped views by their ego, and drops views older than one tick.
    void BuildGroups(TArray<FGaussianSplatGateGroup>& OutGroups);

    FGaussianSplatGateStats LastStats;
    int32 TickNumber = 0;
};
