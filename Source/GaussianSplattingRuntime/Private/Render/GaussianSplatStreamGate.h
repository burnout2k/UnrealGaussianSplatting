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

private:
    // The stamps, written on the render thread and drained on the game thread.
    mutable FCriticalSection StampLock;
    TArray<FGaussianSplatGateView> StampedViews;

    // Groups the stamped views by their ego, and drops views older than one tick.
    void BuildGroups(TArray<FGaussianSplatGateGroup>& OutGroups);

    FGaussianSplatGateStats LastStats;
    int32 TickNumber = 0;
};
