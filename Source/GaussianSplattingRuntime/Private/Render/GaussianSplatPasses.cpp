#include "Render/GaussianSplatPasses.h"

#include "GaussianSplatPagedAsset.h"
#include "Render/GaussianSplatPagePool.h"
#include "Render/GaussianSplatStreamGate.h"

#include "./GaussianSplatShaders.h"
#include "Render/GaussianSplatRenderResources.h"
#include "PipelineStateCache.h"
#include "PixelShaderUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderGraphResources.h"
#include "RenderUtils.h"
#include "RHI.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "SystemTextures.h"
#include "HAL/IConsoleManager.h"
#include "GPUSort.h"
#include "RHIGPUReadback.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatProfile, Log, All);

// Per-phase GPU timing for `stat GPU`. Without scopes the splat cost landed in whatever engine
// bucket happened to be open (it showed up as SortLights). Mode 0's sort is left unscoped on
// purpose: a scope around its 253 dispatches once broke stat GPU's [TOTAL], suspected
// timestamp-query exhaustion.
DECLARE_GPU_STAT_NAMED(GaussianSplatCull, TEXT("GaussianSplat/Cull"));
DECLARE_GPU_STAT_NAMED(GaussianSplatSort, TEXT("GaussianSplat/Sort"));
DECLARE_GPU_STAT_NAMED(GaussianSplatRaster, TEXT("GaussianSplat/Raster"));
DECLARE_GPU_STAT_NAMED(GaussianSplatComposite, TEXT("GaussianSplat/Composite"));
DECLARE_GPU_STAT_NAMED(GaussianSplatOcc, TEXT("GaussianSplat/Occ"));


namespace GaussianSplatProfiling
{
    // Instrumentation only. The cull pass already writes the number of splats that
    // survive frustum rejection into IndirectArgsBuffer[1], but nothing read it
    // back, so the sort is sized from the padded point count rather than from what
    // is actually on screen. Copy that counter to the CPU a few frames late.
    static constexpr int32 NumReadbackSlots = 4;

    // The hidden-splat cull's per-phase draw arguments: four phases of four uints (the instance count is the second).
    static constexpr uint32 OccMaxPhases = 4;
    static constexpr uint32 OccDrawArgsCount = OccMaxPhases * 4;

    uint32 GetSortKeyMode();  // r.GaussianSplat.SortKeyMode, defined with the sort CVars; the profile line prints it
    uint32 GetLodSplitRuns();  // r.GaussianSplat.LodSplitRuns (Fix 5 probe), defined with the sort CVars

    // What the profile line reports about the LOD selection. Mode 1's fields stay 0 in mode 0.
    struct FLodStats
    {
        int32 Mode = 0;
        float K = 0.0f;
        double Multiplier = 1.0;
        double FocalPx = 0.0;
        double FullDistance = 0.0;
        double SelectMicros = 0.0;
    };

    // The hidden-splat cull on the batch whose visible count the readback carries: its phase draw arguments, copied
    // along.
    struct FOccReadback
    {
        FRDGBufferRef DrawArgs = nullptr;
        int32 Phases = 0;
        bool bBoxPath = false;
        bool bSkipped = false;  // OccPhases asked for the cull, but the view was under r.GaussianSplat.OccMinVisible
    };

    struct FVisibleCountState
    {
        TUniquePtr<FRHIGPUBufferReadback> Slots[NumReadbackSlots];
        TUniquePtr<FRHIGPUBufferReadback> OccSlots[NumReadbackSlots];
        int32 OccPhasesInSlot[NumReadbackSlots] = {};
        bool OccBoxPathInSlot[NumReadbackSlots] = {};
        bool OccSkippedInSlot[NumReadbackSlots] = {};
        int32 WriteSlot = 0;
        uint32 LastVisibleCount = 0;
        int32 LastOccPhases = 0;
        bool bLastOccBoxPath = false;
        bool bLastOccSkipped = false;
        uint32 LastOccKept[OccMaxPhases] = {};
        uint32 FrameCounter = 0;

        // Budget feedback (Cesium's trick): the multiplier applied to every
        // cell's keep-fraction. Overshoot the budget and it tightens for the
        // next frame; undershoot and it relaxes. Converges in a few frames and
        // turns MaxRenderPoints from a guess into an enforced ceiling.
        float LodBias = 1.0f;
    };

    // Keyed per view, NOT global. A level renders more than one view -- the editor
    // viewport plus, here, the sky/reflection capture on BP_Carla_Sky, which sees
    // most of the map. With one shared slot each view overwrote the other's count,
    // so the viewport sized its sort from the sky capture's ~62% and vice versa.
    // That mis-sizing left regions the sort never covered, drawn in cull order, as
    // a haze band sliding with the camera.
    static TMap<uint32, FVisibleCountState> GVisibleCountByView;

    FVisibleCountState& GetViewState(uint32 ViewKey)
    {
        return GVisibleCountByView.FindOrAdd(ViewKey);
    }

    void EnqueueVisibleCountReadback(
        FRDGBuilder& GraphBuilder,
        FRDGBufferRef IndirectArgsBuffer,
        uint32 RenderPointCount,
        uint32 SelectedCount,
        uint32 SelectedCells,
        uint32 ViewKey,
        const TCHAR* SortModeLabel,
        int32 BatchIndex,
        const FIntRect& ViewRect,
        const FLodStats& LodStats,
        const FOccReadback& Occ,
        // Fix 5. Entries is what D6's trigger reads -- the range table's length,
        // which is what decides whether the lookup stays in L1 -- and Misses must
        // be 0 on a camera frame in synchronous mode.
        bool bPaged = false,
        uint32 PagedEntries = 0,
        uint32 PagedMisses = 0)
    {
        FVisibleCountState& State = GetViewState(ViewKey);
        const uint32 IndirectArgsBytes = 4 * sizeof(uint32);
        const uint32 OccDrawArgsBytes = OccDrawArgsCount * sizeof(uint32);

        // Drain the oldest slot before reusing it, so this never stalls the GPU. The cull's copy is enqueued in the
        // same frame, so both are read together or not at all.
        const int32 ReadSlot = (State.WriteSlot + 1) % NumReadbackSlots;
        const bool bOccInSlot = State.OccPhasesInSlot[ReadSlot] > 0 && State.OccSlots[ReadSlot].IsValid();
        if (State.Slots[ReadSlot].IsValid() && State.Slots[ReadSlot]->IsReady()
            && (!bOccInSlot || State.OccSlots[ReadSlot]->IsReady()))
        {
            if (const uint32* Data = static_cast<const uint32*>(State.Slots[ReadSlot]->Lock(IndirectArgsBytes)))
            {
                State.LastVisibleCount = Data[1];
            }
            State.Slots[ReadSlot]->Unlock();

            State.LastOccPhases = 0;
            State.bLastOccSkipped = State.OccSkippedInSlot[ReadSlot];
            if (bOccInSlot)
            {
                if (const uint32* Data = static_cast<const uint32*>(State.OccSlots[ReadSlot]->Lock(OccDrawArgsBytes)))
                {
                    for (uint32 Phase = 0; Phase < OccMaxPhases; ++Phase)
                    {
                        State.LastOccKept[Phase] = Data[Phase * 4 + 1];
                    }
                    State.LastOccPhases = State.OccPhasesInSlot[ReadSlot];
                    State.bLastOccBoxPath = State.OccBoxPathInSlot[ReadSlot];
                }
                State.OccSlots[ReadSlot]->Unlock();
            }
        }

        if (!State.Slots[State.WriteSlot].IsValid())
        {
            State.Slots[State.WriteSlot] = MakeUnique<FRHIGPUBufferReadback>(TEXT("GaussianSplat.VisibleCount"));
        }
        AddEnqueueCopyPass(GraphBuilder, State.Slots[State.WriteSlot].Get(), IndirectArgsBuffer, IndirectArgsBytes);
        State.OccPhasesInSlot[State.WriteSlot] = 0;
        State.OccSkippedInSlot[State.WriteSlot] = Occ.bSkipped;
        if (Occ.DrawArgs != nullptr && Occ.Phases > 0)
        {
            if (!State.OccSlots[State.WriteSlot].IsValid())
            {
                State.OccSlots[State.WriteSlot] = MakeUnique<FRHIGPUBufferReadback>(TEXT("GaussianSplat.OccDrawArgs"));
            }
            AddEnqueueCopyPass(GraphBuilder, State.OccSlots[State.WriteSlot].Get(), Occ.DrawArgs, OccDrawArgsBytes);
            State.OccPhasesInSlot[State.WriteSlot] = Occ.Phases;
            State.OccBoxPathInSlot[State.WriteSlot] = Occ.bBoxPath;
        }
        State.WriteSlot = (State.WriteSlot + 1) % NumReadbackSlots;

        if ((State.FrameCounter++ % 60) == 0 && State.LastVisibleCount > 0)
        {
            const float VisiblePercent = 100.0f * static_cast<float>(State.LastVisibleCount) /
                static_cast<float>(FMath::Max(1u, SelectedCount));
            // Appended fields only, so parsers of the older line keep working. rect is the view's pixel size; the occ
            // counts are the instances each phase drew, from the same frame as visible.
            const FString LodText = LodStats.Mode == 1
                ? FString::Printf(TEXT(" k %.3f m %.4f focal %.1f d_full %.0f"),
                    LodStats.K, LodStats.Multiplier, LodStats.FocalPx, LodStats.FullDistance)
                : FString();
            FString OccText;
            if (State.LastOccPhases > 0)
            {
                uint64 Kept = 0;
                for (int32 Phase = 0; Phase < State.LastOccPhases; ++Phase)
                {
                    Kept += State.LastOccKept[Phase];
                }
                const uint64 Culled = State.LastVisibleCount > Kept ? State.LastVisibleCount - Kept : 0;
                OccText = FString::Printf(
                    TEXT(" | occ %d phases %s path kept %u+%u+%u+%u of %u culled %llu (%.1f%%)"),
                    State.LastOccPhases,
                    State.bLastOccBoxPath ? TEXT("box") : TEXT("recompute"),
                    State.LastOccKept[0],
                    State.LastOccKept[1],
                    State.LastOccKept[2],
                    State.LastOccKept[3],
                    State.LastVisibleCount,
                    Culled,
                    100.0 * static_cast<double>(Culled) / static_cast<double>(FMath::Max(1u, State.LastVisibleCount)));
            }
            else if (State.bLastOccSkipped)
            {
                OccText = TEXT(" | occ off (min visible)");
            }
            const TCHAR* KeyText = GetSortKeyMode() == 1 ? TEXT(" | key offset16") : TEXT("");
            const uint32 SplitRuns = GetLodSplitRuns();
            const FString SplitText = SplitRuns > 0 ? FString::Printf(TEXT(" | split %u"), SplitRuns) : FString();
            const FString PagedText = bPaged
                ? FString::Printf(TEXT(" | pool entries %u misses %u"), PagedEntries, PagedMisses)
                : FString();

            // Fix 5 Step 2 (plan 2e, review M1/M4/M5/m7): streaming's own costs, kept
            // SEPARATE from render ms. The headroom at the districts' worst spot is
            // ~17-20 ms, so "the frame got slower" is not a usable signal -- which of
            // the gate, the uploads or the raster grew has to be readable directly.
            //   drawn       what the raster actually drew, so the cost model can be
            //               re-fitted on that rather than on the selection (M1)
            //   gate/upload the two halves of the streaming cost; gate is barred at 0.5 ms (M4)
            //   thrash      pages re-uploaded within the protected age of their own
            //               eviction; invisible in the picture and in the frame time (M5)
            //   req#        the required-set hash: the same pose reached forwards and
            //               backwards must print the same number (m7)
            //   ctl         Fix 7's detail controller: k, the window's longest cycle, the
            //               budget-bound views, its state (0 off ... 5 lockout, 6 held by
            //               place memory) and the drop records it holds. The owner's
            //               parser reads this exact format.
            FString StreamText;
            if (bPaged && FGaussianSplatPagePool::IsStreamingEnabled())
            {
                const FGaussianSplatStreamStats& Up = FGaussianSplatPagePool::Get().GetLastStreamStats();
                const FGaussianSplatGateStats& Gate = FGaussianSplatStreamGate::Get().GetLastStats();
                StreamText = FString::Printf(
                    TEXT(" | stream req %d want %d resident %d | up %d (%.1f MiB) evict %d short %d thrash %d")
                    TEXT(" | gate %.3f ms upload %.3f ms | m %.4f | cells %d | demand %d cap %d next %d @x%.1f")
                    TEXT(" | ctl k %.3f cycle %.1f ms bound %d state %d mem %d | req# %llx"),
                    Gate.RequiredPages,
                    Gate.WantedPages,
                    Gate.ResidentPages,
                    Up.PagesUploaded,
                    Up.UploadBytes / (1024.0 * 1024.0),
                    Up.PagesEvicted,
                    Up.PagesRequiredNotUploaded,
                    Up.ThrashCount,
                    Gate.GateMs,
                    Up.UploadMs,
                    Gate.OverflowMultiplier,
                    Gate.CellsVisited,
                    Gate.DemandPages,
                    Gate.CapacityPages,
                    Gate.DemandNextPages,
                    Gate.ProbeStep,
                    Gate.CtlMultiplier,
                    Gate.CtlCycleMs,
                    Gate.CtlBudgetBoundViews,
                    static_cast<int32>(Gate.CtlState),
                    Gate.CtlMemoryRecords,
                    Gate.RequiredHash);
            }
            UE_LOG(
                LogGaussianSplatProfile,
                Display,
                TEXT("view %u: lod selected %u of %u budget (%.0f MiB sort scratch) ")
                TEXT("across %u cells | visible=%u (%.1f%% of selected) | sort mode %s | batch %d")
                TEXT(" | rect %dx%d | lod mode %d%s | select %.0f us%s%s%s%s%s"),
                ViewKey,
                SelectedCount,
                RenderPointCount,
                // Four uint32 buffers -- key and order, doubled for the radix
                // ping-pong. This is the entire VRAM cost of raising the budget,
                // and it buys nothing once it exceeds what LOD ever selects.
                RenderPointCount * 16.0 / (1024.0 * 1024.0),
                SelectedCells,
                State.LastVisibleCount,
                VisiblePercent,
                SortModeLabel,
                BatchIndex,
                ViewRect.Width(),
                ViewRect.Height(),
                LodStats.Mode,
                *LodText,
                LodStats.SelectMicros,
                *OccText,
                KeyText,
                *SplitText,
                *PagedText,
                *StreamText);
        }
    }

    // The sort is sized by how many splats were UPLOADED, but only the ones that
    // survive frustum culling matter -- the rest are padding slots holding
    // 0xffffffff, sorted to the end and then ignored. At street level only ~8% of
    // splats are visible, so ~92% of the sort is wasted: measured 37.4 ms/frame of
    // which only ~3.5 ms was actually rasterising 2.5M splats.
    //
    // The count is a GPU value and the sort needs it CPU-side, so use the readback
    // (a frame or two stale) plus a margin. If the true count overshoots the margin,
    // the excess splats are still drawn but in cull order rather than depth order --
    // a transient blending artefact that corrects itself next frame, not a crash.
    static TAutoConsoleVariable<int32> CVarSortVisibleOnly(
        TEXT("r.GaussianSplat.SortVisibleOnly"),
        1,
        TEXT("1 = size the depth sort to the last known visible count plus a margin, ")
        TEXT("0 = sort every uploaded splat (always correct, ~2x slower). Modes 0/1 ")
        TEXT("without cells only: mode 2 sorts the exact GPU visible count."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<float> CVarSortVisibleMargin(
        TEXT("r.GaussianSplat.SortVisibleMargin"),
        2.0f,
        TEXT("Safety factor on the stale visible count when sizing the sort. If the ")
        TEXT("true count outruns it, the uncovered splats still draw -- in cull ")
        TEXT("order rather than depth order -- which shows as a transient band ")
        TEXT("during fast camera moves. 1.25/1.5/2.0 all measured the same, so ")
        TEXT("there is no reason to run tight. Modes 0/1 without cells only."),
        ECVF_RenderThreadSafe);

    // Sized from the readback, which lags a frame or two.
    //
    // This is a prediction and it can be wrong: the visible count swings ~100x
    // within a second when free-flying the editor camera over a 2.65 km capture.
    // When it undershoots, the uncovered splats are a contiguous REGION of the map
    // (cull order follows file order, which is spatially ordered), so the error is
    // a visible band rather than scattered noise. A driving camera moves smoothly
    // and does not provoke it; editor navigation does.
    //
    // The proper fix is spatial chunking, which makes the count exact and known on
    // the CPU before the frame -- no prediction at all. Until then this is the best
    // available: 16.75 ms against 35.57 ms for sorting everything.
    uint32 GetSortCount(uint32 RenderPointCount, uint32 ViewKey)
    {
        if (CVarSortVisibleOnly.GetValueOnRenderThread() == 0)
        {
            return RenderPointCount;
        }

        const uint32 LastVisible = GetViewState(ViewKey).LastVisibleCount;
        if (LastVisible == 0)
        {
            // No readback yet (first frames after a load): sort everything.
            return RenderPointCount;
        }

        const float Margin = FMath::Clamp(CVarSortVisibleMargin.GetValueOnRenderThread(), 1.0f, 4.0f);
        const uint64 Sized = static_cast<uint64>(LastVisible * Margin) + 4096u;
        return static_cast<uint32>(FMath::Min<uint64>(Sized, RenderPointCount));
    }

    static TAutoConsoleVariable<float> CVarLodFullDistance(
        TEXT("r.GaussianSplat.LodFullDistance"),
        6000.0f,
        TEXT("World units within which a cell renders every resident splat. ")
        TEXT("Beyond it the keep-fraction falls off as the inverse square of ")
        TEXT("distance, matching how a cell's projected area shrinks."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<float> CVarLodMinFraction(
        TEXT("r.GaussianSplat.LodMinFraction"),
        0.02f,
        TEXT("Floor on a cell's keep-fraction, so distant geometry thins but ")
        TEXT("never disappears. 0 lets far cells drop out entirely."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<int32> CVarLodEnabled(
        TEXT("r.GaussianSplat.Lod"),
        1,
        TEXT("1 = per-cell frustum culling and distance LOD, 0 = draw every ")
        TEXT("resident splat (the pre-cell behaviour, for A/B)."),
        ECVF_RenderThreadSafe);

    // Screen-space LOD: the full-detail distance from each view's own resolution instead of one world distance.
    // Mode 0 is the distance LOD from LodFullDistance, unchanged.
    static TAutoConsoleVariable<int32> CVarLodMode(
        TEXT("r.GaussianSplat.LodMode"),
        1,
        TEXT("0 = distance LOD from LodFullDistance with a budget-feedback bias. 1 (default) = screen-space: every ")
        TEXT("visible cell keeps all its splats out to d_full = LodScreenK x focal (px) x 0.35 x PointSize x s_ref x ")
        TEXT("actor scale (at least LodMinFullDistance) and (d_full / d)^2 of them beyond, with no feedback state; a ")
        TEXT("binding MaxRenderPoints thins the far cells first. From far above, the thinned far field looks ")
        TEXT("see-through; set 0 for top-down views. Other values mean 0."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<float> CVarLodScreenK(
        TEXT("r.GaussianSplat.LodScreenK"),
        2.6f,
        TEXT("LodMode 1: 1 = full detail ends where the median splat's largest axis (s_ref, logged at upload) projects ")
        TEXT("to one pixel; 2 = twice as far. 2.6 matches LodFullDistance 1400 on a 1625 px wide, 90 degree view of a ")
        TEXT("capture whose median splat is 12.6 mm across (full detail to 28 m)."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<float> CVarLodMinFullDistance(
        TEXT("r.GaussianSplat.LodMinFullDistance"),
        2000.0f,
        TEXT("LodMode 1: floor on d_full in world units, so every cell within it stays whole for any camera, including ")
        TEXT("low-resolution sensors. 2000 = 20 m."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<float> CVarLodCullMargin(
        TEXT("r.GaussianSplat.LodCullMargin"),
        3.0f,
        TEXT("LodMode 1: the cell frustum test grows each cell box by this many sigma of the 99th-percentile splat ")
        TEXT("(0.35 x PointSize x s_p99), so splats centred just outside the view still draw. The distance still uses ")
        TEXT("the plain box. 0 = centre-only boxes, as mode 0."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<float> CVarLodDebugHalfFullDistance(
        TEXT("r.GaussianSplat.LodDebugHalfFullDistance"),
        0.0f,
        TEXT("Debug, LodMode 1: > 0 uses this (world units) in place of d_full / 2, so with LodMinFullDistance 0 and ")
        TEXT("LodCullMargin 0 mode 1 selects exactly what LodFullDistance of the same value selects once the bias has ")
        TEXT("saturated, for checking mode 1 against mode 0. 0 = off."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<int32> CVarLodLogNext(
        TEXT("r.GaussianSplat.LodLogNext"),
        0,
        TEXT("Debug, LodMode 1: N > 0 logs the next N selections of every view and batch (focal, d_full, budget ")
        TEXT("multiplier, counts, time). Set 0 and then N again to re-arm."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<int32> CVarLodDumpCells(
        TEXT("r.GaussianSplat.LodDumpCells"),
        0,
        TEXT("Debug, LodMode 1: when set to a new value > 0, logs every selected cell's distance, fraction and take ")
        TEXT("for one frame, in every view and batch."),
        ECVF_RenderThreadSafe);

    int32 GetLodMode()
    {
        return CVarLodMode.GetValueOnRenderThread() == 1 ? 1 : 0;
    }

    // Hidden-splat cull: skip splats drawn behind pixels that earlier splats already made opaque.
    static TAutoConsoleVariable<int32> CVarOccPhases(
        TEXT("r.GaussianSplat.OccPhases"),
        4,
        TEXT("Hidden-splat cull, billboards only. 0 = one draw. 2 = draw the nearest 25% of the ")
        TEXT("sorted splats, then drop every later splat whose quad covers only tiles already opaque (T <= 1/256) and ")
        TEXT("draw the rest. 4 (default) = phases at 10/25/50%. Other values mean 0. Vulkan only; it keeps the single draw ")
        TEXT("wherever its shaders are missing."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<int32> CVarOccTestPath(
        TEXT("r.GaussianSplat.OccTestPath"),
        1,
        TEXT("Hidden-splat cull: 1 = the box path (the cull stores each visible splat's tile box, 4 B per budget ")
        TEXT("splat per view and batch, and a sequential pass tests it); 0 = the recompute path (the test rebuilds ")
        TEXT("each box from the splat in sorted order, no extra buffer)."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<float> CVarOccBoxPad(
        TEXT("r.GaussianSplat.OccBoxPad"),
        1.0f,
        TEXT("Debug, hidden-splat cull: pixels added to every side of each tested box, to absorb float and ")
        TEXT("vertex-snapping differences between the test and the vertex shader. Larger only culls less; at least ")
        TEXT("0.05, which covers the vertex snapping and the NDC round trip."),
        ECVF_RenderThreadSafe);

    int32 GetOccPhases()
    {
        const int32 Phases = CVarOccPhases.GetValueOnRenderThread();
        return (Phases == 2 || Phases == 4) ? Phases : 0;
    }

    // Fix 4 Step 3 lever 2 (fix4-step3-plan.md v2): the cull's Test pass runs once per later phase over the whole LOD
    // selection, so a view that selects millions of splats but sees few pays for it and culls almost nothing (the six
    // 800x450 cameras: -2.05 ms for the four sparse ones, ckptG-1005-1629).
    static TAutoConsoleVariable<int32> CVarOccMinVisible(
        TEXT("r.GaussianSplat.OccMinVisible"),
        750000,
        TEXT("Hidden-splat cull, per view. 0 = the cull runs wherever OccPhases says. N > 0 (default 750000) = a view ")
        TEXT("whose visible count, from its last readback (a few of its frames old), is below N draws in one pass instead; ")
        TEXT("while a view has no readback yet the cull stays on. The visible count is taken before the cull, so ")
        TEXT("turning the cull off does not change it."),
        ECVF_RenderThreadSafe);

    bool ShouldSkipOccForView(uint32 ViewKey)
    {
        const int32 MinVisible = CVarOccMinVisible.GetValueOnRenderThread();
        if (MinVisible <= 0)
        {
            return false;
        }
        const uint32 LastVisible = GetViewState(ViewKey).LastVisibleCount;
        return LastVisible > 0 && LastVisible < static_cast<uint32>(MinVisible);
    }

    bool ShouldUseOccBoxPath()
    {
        return CVarOccTestPath.GetValueOnRenderThread() != 0;
    }

    float GetOccBoxPad()
    {
        return FMath::Clamp(CVarOccBoxPad.GetValueOnRenderThread(), 0.05f, 64.0f);
    }

    // A/B measurement switch. The bitonic sort records 253 dispatches per frame
    // at full density, and per-pass barriers are suspected to dominate over the
    // sort maths itself. Skipping the sort renders splats in cull order, which
    // blends wrongly but isolates the sort's true cost in wall-clock terms.
    static TAutoConsoleVariable<int32> CVarSkipSort(
        TEXT("r.GaussianSplat.SkipSort"),
        0,
        TEXT("1 = skip the depth sort entirely (blending will be wrong; for profiling only)."),
        ECVF_RenderThreadSafe);

    bool ShouldSkipSort()
    {
        return CVarSkipSort.GetValueOnRenderThread() != 0;
    }

    static TAutoConsoleVariable<int32> CVarSortMode(
        TEXT("r.GaussianSplat.SortMode"),
        2,
        TEXT("0 = bitonic network, 1 = UE GPU radix sort (SortGPUBuffers, 4 bits per pass), ")
        TEXT("2 = plugin DeviceRadixSort (8 bits per pass, the default). Mode 2 runs only on ")
        TEXT("NVIDIA GPUs with wave size 32 unless r.GaussianSplat.RadixAllowAnyVendor is set, ")
        TEXT("and falls back to 1 anywhere else. Values above 2 mean bitonic."),
        ECVF_RenderThreadSafe);

    int32 GetSortMode()
    {
        return CVarSortMode.GetValueOnRenderThread();
    }

    static TAutoConsoleVariable<int32> CVarSortKeyBits(
        TEXT("r.GaussianSplat.SortKeyBits"),
        24,
        TEXT("Significant bits of the depth key (8-32). Mode 1 spends one pass per 4 bits ")
        TEXT("and rounds an odd count up to even (20 and 24 bits both take 6); mode 2 spends ")
        TEXT("one pass per 8 bits (20 and 24 both take 3), so 24 costs no more than 20 and has ")
        TEXT("16x finer depth steps. Fewer bits means more ties: 20 was indistinguishable from 32 ")
        TEXT("on tartu_demo, 16 was visibly wrong. Ignored when r.GaussianSplat.SortKeyMode is 1."),
        ECVF_RenderThreadSafe);

    // Fix 4 Step 3 lever 1 (fix4-step3-plan.md v2): 16 bits spent only on the depths a street scene has, so mode 2
    // sorts in 2 passes instead of 3 (-2.68 ms on the six 800x450 cameras, ckptG-1005-1629).
    static TAutoConsoleVariable<int32> CVarSortKeyMode(
        TEXT("r.GaussianSplat.SortKeyMode"),
        1,
        TEXT("Depth key. 0 = the depth's float bits, the top SortKeyBits kept. 1 (default) = 16 bits over ")
        TEXT("8 cm .. 5.24 km, 4,096 steps per doubling of the depth (2x finer than the 20-bit key), so mode 2 ")
        TEXT("sorts in 2 passes; nearer depths share the first key and farther ones the last."),
        ECVF_RenderThreadSafe);

    uint32 GetSortKeyMode()
    {
        return CVarSortKeyMode.GetValueOnRenderThread() == 1 ? 1u : 0u;
    }

    // Fix 5 Step 0 probes (fix5-plan.md v2), both off by default.
    static TAutoConsoleVariable<int32> CVarLodSplitRuns(
        TEXT("r.GaussianSplat.LodSplitRuns"),
        0,
        TEXT("Fix 5 probe: write each selected cell's range as consecutive runs of this many splats (0 = one run per ")
        TEXT("cell). The same splats in the same order, so the pictures do not change; only the lookup's binary search ")
        TEXT("gets as deep as a pool of pages this size would make it."),
        ECVF_RenderThreadSafe);

    // Fix 5 Step 3 gate G3b: turns the single sort OFF, back to a sort per district.
    //
    // It exists so the gate can show the BEFORE and the AFTER in one binary. Without
    // it, "flipping the draw order changes nothing" is indistinguishable from a test
    // that was never measuring anything -- districts that do not overlap in depth, or
    // a switch that does nothing, give the same clean pass. With the merge off the two
    // orders MUST differ, and that difference is critic C10.
    static TAutoConsoleVariable<int32> CVarMergeDistricts(
        TEXT("r.GaussianSplat.MergeDistricts"),
        1,
        TEXT("1 = one cull, sort and raster across a view's paged assets (plan D7). 0 = a sort per asset, blended ")
        TEXT("in registration order, which is what gate G3b records as the 'before'."),
        ECVF_RenderThreadSafe);

    bool ShouldMergeDistricts()
    {
        return CVarMergeDistricts.GetValueOnRenderThread() != 0;
    }

    // Debug: puts back the bug 36f230b fixed, for the same reason MergeDistricts 0 exists -- so the
    // palette test has a BEFORE in the same binary. With r.GaussianSplat.Debug.FlattenPaletteSlot 1 on a
    // two-palette map, the flattened asset must change colour with this off and must not with it on.
    static TAutoConsoleVariable<int32> CVarDebugForceSinglePalette(
        TEXT("r.GaussianSplat.Debug.ForceSinglePalette"),
        0,
        TEXT("Debug: 1 = a merged draw whose selected cells span two or more SH palettes still takes the ")
        TEXT("single-palette permutation, bound to the lowest slot, so every batch reads that slot's palette ")
        TEXT("(the behaviour before 36f230b; wrong colours for the others). 0 (default) = off."),
        ECVF_RenderThreadSafe);

    bool ShouldForceSinglePalette()
    {
        return CVarDebugForceSinglePalette.GetValueOnRenderThread() != 0;
    }

    static TAutoConsoleVariable<int32> CVarMaxRenderPointsOverride(
        TEXT("r.GaussianSplat.MaxRenderPointsOverride"),
        0,
        TEXT("Fix 5 probe: use this draw budget instead of every component's MaxRenderPoints (0 = off). It also sizes ")
        TEXT("the sort scratch, so it shows what the scratch costs."),
        ECVF_RenderThreadSafe);

    uint32 GetLodSplitRuns()
    {
        return static_cast<uint32>(FMath::Max(0, CVarLodSplitRuns.GetValueOnRenderThread()));
    }

    uint32 GetMaxRenderPointsOverride()
    {
        return static_cast<uint32>(FMath::Max(0, CVarMaxRenderPointsOverride.GetValueOnRenderThread()));
    }

    uint32 GetSortKeyBits()
    {
        if (GetSortKeyMode() == 1)
        {
            return 16u;
        }
        return static_cast<uint32>(FMath::Clamp(CVarSortKeyBits.GetValueOnRenderThread(), 8, 32));
    }

    // SortMode 2 (DeviceRadixSort). Every default is the production path.
    static TAutoConsoleVariable<int32> CVarSortGpuCount(
        TEXT("r.GaussianSplat.SortGpuCount"),
        1,
        TEXT("Mode 2 only. 1 = sort exactly the GPU visible count, and skip the sort-buffer ")
        TEXT("clears since nothing reads past it. 0 = sort the CPU-known LOD selection with ")
        TEXT("the clears kept (the straight port, kept as a staging and fallback path)."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<int32> CVarRadixSafeBarriers(
        TEXT("r.GaussianSplat.RadixSafeBarriers"),
        1,
        TEXT("Mode 2 only. 1 = aras-p's barrier layout (a barrier per bit in the multisplit); ")
        TEXT("0 = upstream b0nes164's (one barrier before ranking). Keep 1 unless 0 has passed ")
        TEXT("validation at the higher frame count and measures faster."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<int32> CVarSortRepeat(
        TEXT("r.GaussianSplat.SortRepeat"),
        1,
        TEXT("Modes 1 and 2: sort N times (1-8). A stable re-sort of sorted data changes ")
        TEXT("nothing, so this only costs time: for measuring the sort, and for testing that ")
        TEXT("mode 2 re-zeroes its histogram between sorts."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<int32> CVarRadixForceUnsupported(
        TEXT("r.GaussianSplat.RadixForceUnsupported"),
        0,
        TEXT("Debug: 1 = treat mode 2 as unsupported, to exercise the fallback to mode 1."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<int32> CVarRadixAllowAnyVendor(
        TEXT("r.GaussianSplat.RadixAllowAnyVendor"),
        0,
        TEXT("1 = allow mode 2 on non-NVIDIA or non-wave-32 GPUs (wave sizes 16-64). Mode 2 ")
        TEXT("is validated only on NVIDIA wave 32; this is for testing elsewhere."),
        ECVF_RenderThreadSafe);

    // Debug-only sort checks. They share one readback slot, so at most one is in flight.
    static TAutoConsoleVariable<int32> CVarSortValidate(
        TEXT("r.GaussianSplat.SortValidate"),
        0,
        TEXT("N > 0: every Nth frame, also sort the selected view and batch with mode 1 and ")
        TEXT("compare the two on the GPU (effective mode 2 only). Logs one line per check with ")
        TEXT("running totals; any difference logs at Error level."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<int32> CVarSortValidateView(
        TEXT("r.GaussianSplat.SortValidateView"),
        0,
        TEXT("Which view SortValidate and SortSelfCheck use: 0 = the first non-capture view ")
        TEXT("of the frame, otherwise that view key. Every view key seen is logged once."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<int32> CVarSortValidateBatch(
        TEXT("r.GaussianSplat.SortValidateBatch"),
        0,
        TEXT("Which splat actor (batch index within the view) SortValidate and SortSelfCheck use."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<int32> CVarSortValidateCount(
        TEXT("r.GaussianSplat.SortValidateCount"),
        0,
        TEXT("K > 0: on validated frames, sort and draw only the first K keys, for boundary ")
        TEXT("tests (those frames flicker). 0 = the full count."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<int32> CVarSortValidateGpuCap(
        TEXT("r.GaussianSplat.SortValidateGpuCap"),
        0,
        TEXT("K > 0: on validated frames, cap the GPU key count at K without changing the ")
        TEXT("dispatch, so idle partitions follow a controlled boundary. Needs SortGpuCount 1."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<int32> CVarSortSelfCheck(
        TEXT("r.GaussianSplat.SortSelfCheck"),
        0,
        TEXT("N > 0: every Nth frame, check without a reference that the drawn keys are ")
        TEXT("non-decreasing and the splat indices in range, in any sort mode."),
        ECVF_RenderThreadSafe);

    bool ShouldUseGpuSortCount()
    {
        return CVarSortGpuCount.GetValueOnRenderThread() != 0;
    }

    bool ShouldUseRadixSafeBarriers()
    {
        return CVarRadixSafeBarriers.GetValueOnRenderThread() != 0;
    }

    int32 GetSortRepeat()
    {
        return FMath::Clamp(CVarSortRepeat.GetValueOnRenderThread(), 1, 8);
    }

    bool IsRadixForcedUnsupported()
    {
        return CVarRadixForceUnsupported.GetValueOnRenderThread() != 0;
    }

    bool IsRadixAnyVendorAllowed()
    {
        return CVarRadixAllowAnyVendor.GetValueOnRenderThread() != 0;
    }

    int32 GetSortValidateEvery()
    {
        return FMath::Max(0, CVarSortValidate.GetValueOnRenderThread());
    }

    uint32 GetSortValidateView()
    {
        return static_cast<uint32>(FMath::Max(0, CVarSortValidateView.GetValueOnRenderThread()));
    }

    int32 GetSortValidateBatch()
    {
        return FMath::Max(0, CVarSortValidateBatch.GetValueOnRenderThread());
    }

    uint32 GetSortValidateCount()
    {
        return static_cast<uint32>(FMath::Max(0, CVarSortValidateCount.GetValueOnRenderThread()));
    }

    uint32 GetSortValidateGpuCap()
    {
        return static_cast<uint32>(FMath::Max(0, CVarSortValidateGpuCap.GetValueOnRenderThread()));
    }

    int32 GetSortSelfCheckEvery()
    {
        return FMath::Max(0, CVarSortSelfCheck.GetValueOnRenderThread());
    }

    static TAutoConsoleVariable<int32> CVarPerPixelDepth(
        TEXT("r.GaussianSplat.PerPixelDepth"),
        1,
        TEXT("1 = depth-test each splat pixel against the Gaussian's own depth there; ")
        TEXT("0 = test the whole splat against its center depth (the original ")
        TEXT("behaviour, which lets splats bleed over nearer geometry)."),
        ECVF_RenderThreadSafe);

    bool ShouldUsePerPixelDepth()
    {
        return CVarPerPixelDepth.GetValueOnRenderThread() != 0;
    }

    // Raster-side levers, aimed at dense captures. Millions of faint,
    // overlapping Gaussians accumulate into a milky veil that washes out the
    // scene; these three attack it from different angles. All default to
    // existing behaviour.
    // Defaults moved 2026-10-10 (AlphaCutoff 1/255 -> 0.031, MinScreenVariance 1.0 -> 0.25): measured awake
    // on five captures (San Juan, Perry, Uno, the mixed city, Lublin), the pair is the best frame time on
    // every one (-6 to -17 %) and, judged by eye, better or equal on every one. MinScreenVariance 1.0 was
    // inflating every thin splat to a 5.7 px minimum, so distant cables drew as fat strokes; 0.25 draws them
    // nearer their true size. Record: splat-work/fix5-status.md, 2026-10-10.
    static TAutoConsoleVariable<float> CVarAlphaCutoff(
        TEXT("r.GaussianSplat.AlphaCutoff"),
        0.031f,
        TEXT("Discard splat pixels below this alpha before blending (0-1). ")
        TEXT("Trims the faint outer tails of every Gaussian. Too high and ")
        TEXT("splats stop fading softly and thin structures go patchy."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<float> CVarMinSplatOpacity(
        TEXT("r.GaussianSplat.MinSplatOpacity"),
        0.0f,
        TEXT("Reject splats whose effective opacity is below this (0 = off). ")
        TEXT("Unlike AlphaCutoff this drops the whole splat during culling, so ")
        TEXT("it never rasterises at all -- cheaper, and aimed at splats that ")
        TEXT("are faint everywhere rather than just at their edges."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<float> CVarMaxSplatDistance(
        TEXT("r.GaussianSplat.MaxSplatDistance"),
        0.0f,
        TEXT("Reject splats beyond this view depth in Unreal units (0 = off). ")
        TEXT("Bounds the work for street-level views, which otherwise draw the ")
        TEXT("far side of the capture at full density."),
        ECVF_RenderThreadSafe);

    float GetAlphaCutoff()
    {
        return FMath::Clamp(CVarAlphaCutoff.GetValueOnRenderThread(), 0.0f, 1.0f);
    }

    float GetMinSplatOpacity()
    {
        return FMath::Clamp(CVarMinSplatOpacity.GetValueOnRenderThread(), 0.0f, 1.0f);
    }

    float GetMaxSplatDistance()
    {
        return FMath::Max(CVarMaxSplatDistance.GetValueOnRenderThread(), 0.0f);
    }

    // Every splat is drawn as a quad sized from its projected covariance, and this
    // floors that covariance. At the historical 1.0 the quad bottoms out around
    // 5.7x5.7 px regardless of distance: 8M splats then cover ~256M pixels against
    // a 2M-pixel screen, so the renderer is fill-rate bound and distant splats cost
    // as much as near ones. CalcCovariance2D already applies the reference 3DGS
    // low-pass of 0.3, so 0.0 here is reference behaviour (~3.1x3.1 px), not an
    // absent low-pass. Lower means cheaper far field but thinner surfaces.
    static TAutoConsoleVariable<float> CVarMinScreenVariance(
        TEXT("r.GaussianSplat.MinScreenVariance"),
        0.25f,
        TEXT("Floor on projected screen-space variance in px^2. 1.0 = historical ")
        TEXT("behaviour, 0.0 = reference 3DGS (the 0.3 low-pass alone). Lower cuts ")
        TEXT("overdraw sharply but can open holes in distant surfaces."),
        ECVF_RenderThreadSafe);

    float GetMinScreenVariance()
    {
        return FMath::Clamp(CVarMinScreenVariance.GetValueOnRenderThread(), 0.0f, 16.0f);
    }
}

namespace GaussianSplatSorting
{
    // Only buffer *access* is declared, not views: RDG rejects a resource bound as
    // both SRV and UAV in one pass, and the radix sort needs both. Views come from
    // the pooled buffers inside the pass, where SortGPUBuffers does its own
    // transitions.
    BEGIN_SHADER_PARAMETER_STRUCT(FRadixSortPassParameters, )
        RDG_BUFFER_ACCESS(Keys0, ERHIAccess::UAVCompute)
        RDG_BUFFER_ACCESS(Keys1, ERHIAccess::UAVCompute)
        RDG_BUFFER_ACCESS(Values0, ERHIAccess::UAVCompute)
        RDG_BUFFER_ACCESS(Values1, ERHIAccess::UAVCompute)
    END_SHADER_PARAMETER_STRUCT()

    // GetGPUSortPassCount() is declared without ENGINE_API, so it does not link
    // from outside the Engine module. Mirror it here; the constants are private
    // #defines in GPUSort.cpp (GPUSORT_BITCOUNT 32, RADIX_BITS 4). The prediction
    // is checked against SortGPUBuffers' actual return value at runtime.
    static int32 GetRadixSortPassCount(uint32 KeyMask)
    {
        constexpr int32 SortBitCount = 32;
        constexpr int32 RadixBits = 4;
        int32 PassesRequired = 0;
        uint32 PassBits = (1u << RadixBits) - 1u;
        for (int32 PassIndex = 0; PassIndex < SortBitCount / RadixBits; ++PassIndex)
        {
            if ((PassBits & KeyMask) != 0)
            {
                ++PassesRequired;
            }
            PassBits <<= RadixBits;
        }
        return PassesRequired;
    }

    // Replaces the bitonic network with UE's 4-bit-digit radix sort: 8 passes for a
    // full 32-bit key instead of 253, and no power-of-two padding. Returns whichever
    // value buffer ends up holding the sorted splat indices.
    FRDGBufferRef AddRadixSortPass(
        FRDGBuilder& GraphBuilder,
        ERHIFeatureLevel::Type FeatureLevel,
        FRDGBufferRef Values0,
        FRDGBufferRef Values1,
        FRDGBufferRef Keys0,
        FRDGBufferRef Keys1,
        uint32 Count)
    {
        const uint32 KeyBits = GaussianSplatProfiling::GetSortKeyBits();
        uint32 KeyMask = (KeyBits >= 32u) ? 0xFFFFFFFFu : ((1u << KeyBits) - 1u);

        // Force an EVEN pass count so the sort ends in the buffer it started in.
        //
        // The radix sort ping-pongs. With an odd pass count the result lands in the
        // Alt buffer, whose tail beyond the sorted range is cleared to 0 -- so any
        // splat the sort did not reach reads index 0 and renders as garbage. That
        // was the black-mesh artefact on fast camera moves, where the stale visible
        // count underestimates by up to 10x and no safety margin can cover it.
        //
        // With an even count the result is the primary buffer, which the cull pass
        // just filled, so the unreached tail still holds valid indices in cull
        // order. Overshoot then costs a few splats blended out of depth order
        // instead of a hole in the scene. The extra digit is all zeros for every
        // key, so the added pass is a stable no-op.
        if ((GetRadixSortPassCount(KeyMask) % 2) != 0 && KeyMask != 0xFFFFFFFFu)
        {
            KeyMask = (KeyMask << 4) | 0xFu;
        }
        const int32 PassCount = GetRadixSortPassCount(KeyMask);
        if (Count == 0 || PassCount == 0)
        {
            return Values0;
        }

        const TRefCountPtr<FRDGPooledBuffer> PooledKeys0 = GraphBuilder.ConvertToExternalBuffer(Keys0);
        const TRefCountPtr<FRDGPooledBuffer> PooledKeys1 = GraphBuilder.ConvertToExternalBuffer(Keys1);
        const TRefCountPtr<FRDGPooledBuffer> PooledValues0 = GraphBuilder.ConvertToExternalBuffer(Values0);
        const TRefCountPtr<FRDGPooledBuffer> PooledValues1 = GraphBuilder.ConvertToExternalBuffer(Values1);

        FRadixSortPassParameters* Parameters = GraphBuilder.AllocParameters<FRadixSortPassParameters>();
        Parameters->Keys0 = Keys0;
        Parameters->Keys1 = Keys1;
        Parameters->Values0 = Values0;
        Parameters->Values1 = Values1;

        GraphBuilder.AddPass(
            RDG_EVENT_NAME("GaussianSplatRadixSort (%u keys, %d passes)", Count, PassCount),
            Parameters,
            ERDGPassFlags::Compute,
            [PooledKeys0, PooledKeys1, PooledValues0, PooledValues1, FeatureLevel, Count, KeyMask,
             PredictedResultIndex = PassCount % 2]
            (FRHICommandList& RHICmdList)
            {
                const FRHIBufferSRVCreateInfo SRVInfo(PF_R32_UINT);
                const FRHIBufferUAVCreateInfo UAVInfo(PF_R32_UINT);

                FGPUSortBuffers SortBuffers;
                SortBuffers.RemoteKeySRVs[0] = PooledKeys0->GetOrCreateSRV(RHICmdList, SRVInfo);
                SortBuffers.RemoteKeySRVs[1] = PooledKeys1->GetOrCreateSRV(RHICmdList, SRVInfo);
                SortBuffers.RemoteKeyUAVs[0] = PooledKeys0->GetOrCreateUAV(RHICmdList, UAVInfo);
                SortBuffers.RemoteKeyUAVs[1] = PooledKeys1->GetOrCreateUAV(RHICmdList, UAVInfo);
                SortBuffers.RemoteValueSRVs[0] = PooledValues0->GetOrCreateSRV(RHICmdList, SRVInfo);
                SortBuffers.RemoteValueSRVs[1] = PooledValues1->GetOrCreateSRV(RHICmdList, SRVInfo);
                SortBuffers.RemoteValueUAVs[0] = PooledValues0->GetOrCreateUAV(RHICmdList, UAVInfo);
                SortBuffers.RemoteValueUAVs[1] = PooledValues1->GetOrCreateUAV(RHICmdList, UAVInfo);

                const int32 ResultIndex = SortGPUBuffers(
                    RHICmdList,
                    SortBuffers,
                    /*BufferIndex=*/ 0,
                    KeyMask,
                    static_cast<int32>(Count),
                    FeatureLevel);

                // The rasterizer was bound at record time using the predicted index.
                // If these ever disagree the splats would draw from the wrong buffer,
                // so surface it loudly rather than rendering silent garbage.
                if (ResultIndex != PredictedResultIndex)
                {
                    UE_LOG(
                        LogGaussianSplatProfile,
                        Error,
                        TEXT("Radix sort landed in buffer %d but %d was predicted; splat order is wrong."),
                        ResultIndex,
                        PredictedResultIndex);
                }
            });

        return (PassCount % 2) == 0 ? Values0 : Values1;
    }

    // Whether mode 2 can run here. Cheap enough to ask per batch; the reason is logged whenever it
    // changes, so a fallback to mode 1 is never silent in the log.
    bool IsDeviceRadixSortSupported()
    {
        const TCHAR* Reason = nullptr;
        if (GaussianSplatProfiling::IsRadixForcedUnsupported())
        {
            Reason = TEXT("r.GaussianSplat.RadixForceUnsupported is set");
        }
        else if (!GRHISupportsWaveOperations)
        {
            // Checked unconditionally: a platform that guarantees wave operations at compile time
            // can still run on a device that reports none.
            Reason = TEXT("the RHI reports no wave operations");
        }
        else if (!IsVulkanPlatform(GMaxRHIShaderPlatform))
        {
            Reason = TEXT("not a Vulkan shader platform (mode 2 is compiled for Vulkan only)");
        }
        else if (GRHIMinimumWaveSize < 16 || GRHIMaximumWaveSize > 64)
        {
            Reason = TEXT("the wave size is outside 16-64");
        }
        else if (!GaussianSplatProfiling::IsRadixAnyVendorAllowed()
            && !(IsRHIDeviceNVIDIA() && GRHIMinimumWaveSize == 32 && GRHIMaximumWaveSize == 32))
        {
            Reason = TEXT("not an NVIDIA GPU with wave size 32, the only hardware it is validated on ")
                TEXT("(r.GaussianSplat.RadixAllowAnyVendor 1 overrides)");
        }
        else if (GetMaxComputeSharedMemory() < 32768)
        {
            Reason = TEXT("less than 32 KiB of compute shared memory");
        }
        else
        {
            // A TShaderMapRef on a missing shader asserts, so every type and permutation is checked
            // first. The validation shaders are deliberately not required here.
            const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
            FGaussianSplatRadixDownsweepCS::FPermutationDomain SafeBarriers;
            SafeBarriers.Set<FGaussianSplatRadixDownsweepCS::FSafeBarriersDim>(true);
            FGaussianSplatRadixDownsweepCS::FPermutationDomain UpstreamBarriers;
            UpstreamBarriers.Set<FGaussianSplatRadixDownsweepCS::FSafeBarriersDim>(false);
            if (ShaderMap == nullptr
                || !ShaderMap->HasShader(&FGaussianSplatRadixSetupCS::GetStaticType(), 0)
                || !ShaderMap->HasShader(&FGaussianSplatRadixUpsweepCS::GetStaticType(), 0)
                || !ShaderMap->HasShader(&FGaussianSplatRadixScanCS::GetStaticType(), 0)
                || !ShaderMap->HasShader(&FGaussianSplatRadixDownsweepCS::GetStaticType(), SafeBarriers.ToDimensionValueId())
                || !ShaderMap->HasShader(&FGaussianSplatRadixDownsweepCS::GetStaticType(), UpstreamBarriers.ToDimensionValueId()))
            {
                Reason = TEXT("its shaders are missing (compile errors are logged under LogShaders)");
            }
        }

        static bool bLoggedOnce = false;
        static const TCHAR* LastReason = nullptr;
        if (!bLoggedOnce || Reason != LastReason)
        {
            if (Reason != nullptr)
            {
                UE_LOG(LogGaussianSplatProfile, Warning, TEXT("SortMode 2 unavailable, using mode 1: %s."), Reason);
            }
            else
            {
                UE_LOG(LogGaussianSplatProfile, Display, TEXT("SortMode 2 (DeviceRadixSort) active."));
            }
            bLoggedOnce = true;
            LastReason = Reason;
        }
        return Reason == nullptr;
    }

    struct FDeviceRadixSortResult
    {
        FRDGBufferRef Keys = nullptr;
        FRDGBufferRef Values = nullptr;
    };

    // Mode 2. Sorts (Keys0, Values0) through the (Keys1, Values1) pair, 8 bits per pass, and returns
    // whichever pair holds the result. The host swaps the refs itself, so unlike mode 1 nothing is
    // predicted. The key count is IndirectArgs[1] when bUseGpuCount, else MaxKeys; the dispatch is
    // sized from MaxKeys either way and partitions past the GPU count do no work.
    FDeviceRadixSortResult AddDeviceRadixSortPasses(
        FRDGBuilder& GraphBuilder,
        FRDGBufferRef Keys0,
        FRDGBufferRef Values0,
        FRDGBufferRef Keys1,
        FRDGBufferRef Values1,
        FRDGBufferRef IndirectArgs,
        uint32 MaxKeys,
        bool bUseGpuCount,
        uint32 GpuCap,
        uint32 KeyBits,
        int32 Repeat,
        bool bSafeBarriers)
    {
        FDeviceRadixSortResult Src{Keys0, Values0};
        FDeviceRadixSortResult Dst{Keys1, Values1};
        const uint32 PassCount = FMath::DivideAndRoundUp(KeyBits, 8u);
        if (MaxKeys == 0 || PassCount == 0)
        {
            return Src;
        }
        const uint32 ThreadBlocks = FMath::Max(1u, FMath::DivideAndRoundUp(MaxKeys, GSRadixPartSize));

        // One 256-digit column per partition. Sized from the key buffer (the budget), never from
        // MaxKeys, so the pool sees one stable size per asset.
        const uint32 BudgetBlocks = FMath::Max(1u, FMath::DivideAndRoundUp(Keys0->Desc.NumElements, GSRadixPartSize));
        FRDGBufferRef PassHist = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 256u * BudgetBlocks),
            TEXT("GaussianSplat.RadixPassHist"));
        FRDGBufferRef GlobalHist = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1024u),
            TEXT("GaussianSplat.RadixGlobalHist"));

        // Pooled, like mode 1. The transient allocator gives any buffer that fits no existing heap
        // a heap of at least 128 MB, so four live 84 MB sort buffers could cost 512 MB instead of
        // 335 MB. The four sort buffers also share mode 1's pool entries.
        for (FRDGBufferRef Buffer : {Keys0, Values0, Keys1, Values1, PassHist, GlobalHist})
        {
            GraphBuilder.ConvertToExternalBuffer(Buffer);
        }

        FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
        TShaderMapRef<FGaussianSplatRadixSetupCS> SetupCS(ShaderMap);
        TShaderMapRef<FGaussianSplatRadixUpsweepCS> UpsweepCS(ShaderMap);
        TShaderMapRef<FGaussianSplatRadixScanCS> ScanCS(ShaderMap);
        FGaussianSplatRadixDownsweepCS::FPermutationDomain Permutation;
        Permutation.Set<FGaussianSplatRadixDownsweepCS::FSafeBarriersDim>(bSafeBarriers);
        TShaderMapRef<FGaussianSplatRadixDownsweepCS> DownsweepCS(ShaderMap, Permutation);

        FRDGBufferSRVRef CountSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(IndirectArgs, PF_R32_UINT));
        FRDGBufferUAVRef CountUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgs, PF_R32_UINT));
        FRDGBufferUAVRef GlobalHistUAV = GraphBuilder.CreateUAV(GlobalHist);
        FRDGBufferUAVRef PassHistUAV = GraphBuilder.CreateUAV(PassHist);
        const FIntVector PartitionGroups(static_cast<int32>(ThreadBlocks), 1, 1);

        const auto AllocSortParameters = [&GraphBuilder, ThreadBlocks, MaxKeys, bUseGpuCount, GpuCap](uint32 RadixShift)
        {
            FGaussianSplatRadixSortParameters* Parameters = GraphBuilder.AllocParameters<FGaussianSplatRadixSortParameters>();
            Parameters->e_radixShift = RadixShift;
            Parameters->e_threadBlocks = ThreadBlocks;
            Parameters->e_maxKeys = MaxKeys;
            Parameters->e_useGpuCount = bUseGpuCount ? 1u : 0u;
            Parameters->e_gpuCap = GpuCap;
            return Parameters;
        };

        RDG_EVENT_SCOPE(GraphBuilder, "GaussianSplatDeviceRadixSort (%u keys max, %u passes x %d)", MaxKeys, PassCount, Repeat);
        for (int32 RepeatIndex = 0; RepeatIndex < Repeat; ++RepeatIndex)
        {
            // Each pass adds into its own 256-entry region of GlobalHist, so every repeat must
            // start from zero or the offsets double.
            FGaussianSplatRadixSortParameters* SetupParameters = AllocSortParameters(0);
            SetupParameters->b_sortCountUAV = CountUAV;
            SetupParameters->b_globalHist = GlobalHistUAV;
            FComputeShaderUtils::AddPass(
                GraphBuilder,
                RDG_EVENT_NAME("GaussianSplatRadix.Setup"),
                SetupCS,
                SetupParameters,
                FIntVector(1, 1, 1));

            for (uint32 PassIndex = 0; PassIndex < PassCount; ++PassIndex)
            {
                const uint32 RadixShift = PassIndex * 8u;
                FRDGBufferSRVRef SrcKeysSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Src.Keys, PF_R32_UINT));

                FGaussianSplatRadixSortParameters* UpsweepParameters = AllocSortParameters(RadixShift);
                UpsweepParameters->b_sortCount = CountSRV;
                UpsweepParameters->b_sort = SrcKeysSRV;
                UpsweepParameters->b_passHist = PassHistUAV;
                UpsweepParameters->b_globalHist = GlobalHistUAV;
                FComputeShaderUtils::AddPass(
                    GraphBuilder,
                    RDG_EVENT_NAME("GaussianSplatRadix.Upsweep (shift %u)", RadixShift),
                    UpsweepCS,
                    UpsweepParameters,
                    PartitionGroups);

                // One group per digit, each scanning that digit's column across the partitions.
                FGaussianSplatRadixSortParameters* ScanParameters = AllocSortParameters(RadixShift);
                ScanParameters->b_passHist = PassHistUAV;
                FComputeShaderUtils::AddPass(
                    GraphBuilder,
                    RDG_EVENT_NAME("GaussianSplatRadix.Scan"),
                    ScanCS,
                    ScanParameters,
                    FIntVector(256, 1, 1));

                FGaussianSplatRadixSortParameters* DownsweepParameters = AllocSortParameters(RadixShift);
                DownsweepParameters->b_sortCount = CountSRV;
                DownsweepParameters->b_sort = SrcKeysSRV;
                DownsweepParameters->b_sortPayload = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Src.Values, PF_R32_UINT));
                DownsweepParameters->b_alt = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Dst.Keys, PF_R32_UINT));
                DownsweepParameters->b_altPayload = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Dst.Values, PF_R32_UINT));
                DownsweepParameters->b_globalHist = GlobalHistUAV;
                DownsweepParameters->b_passHist = PassHistUAV;
                FComputeShaderUtils::AddPass(
                    GraphBuilder,
                    RDG_EVENT_NAME("GaussianSplatRadix.Downsweep (shift %u)", RadixShift),
                    DownsweepCS,
                    DownsweepParameters,
                    PartitionGroups);

                Swap(Src, Dst);
                // RDG would not catch aliasing here: it merges a buffer bound as SRV and UAV in the
                // same pass into UAV access instead of rejecting it.
                check(Src.Keys != Dst.Keys && Src.Values != Dst.Values);
            }
        }
        return Src;
    }
}

// Debug-only checks of the sort (r.GaussianSplat.SortValidate, SortSelfCheck), read back through
// one shared slot, so at most one check is in flight. Render thread only, like GVisibleCountByView.
namespace GaussianSplatSortCheck
{
    enum class EKind : uint8
    {
        None,
        Validate,
        SelfCheck
    };

    struct FCheckInfo
    {
        uint32 ViewKey = 0;
        int32 BatchIndex = 0;
        int32 SortMode = 0;
        uint32 KeyBits = 0;
        uint32 Passes = 0;
        int32 Repeat = 1;
    };

    struct FState
    {
        TUniquePtr<FRHIGPUBufferReadback> Readback;
        EKind Pending = EKind::None;
        FCheckInfo Info;
        uint32 LastStartedFrame = MAX_uint32;
        uint64 ValidatedFrames = 0;
        uint64 ValidateFailures = 0;
        uint64 CheckedFrames = 0;
        uint64 CheckFailures = 0;
        TSet<uint32> LoggedViews;
        const TCHAR* LastSkipReason = nullptr;
    };

    static constexpr uint32 NumStats = 8;
    static constexpr uint32 ThreadsPerGroup = 256;

    FState& GetState()
    {
        static FState State;
        return State;
    }

    // The kernels loop grid-stride, so the group count is capped rather than the work.
    uint32 GetGroupCount(uint32 Count)
    {
        return FMath::Clamp(FMath::DivideAndRoundUp(Count, ThreadsPerGroup), 1u, 65535u);
    }

    void LogSkipOnce(const TCHAR* Reason)
    {
        FState& State = GetState();
        if (State.LastSkipReason != Reason)
        {
            UE_LOG(LogGaussianSplatProfile, Warning, TEXT("Sort check skipped: %s."), Reason);
            State.LastSkipReason = Reason;
        }
    }

    // GPUSort.DebugSort / DebugOffsets corrupt mode 1, which SortValidate uses as its reference.
    bool IsGpuSortDebugOn()
    {
        static IConsoleVariable* DebugSort = IConsoleManager::Get().FindConsoleVariable(TEXT("GPUSort.DebugSort"));
        static IConsoleVariable* DebugOffsets = IConsoleManager::Get().FindConsoleVariable(TEXT("GPUSort.DebugOffsets"));
        return (DebugSort != nullptr && DebugSort->GetInt() != 0)
            || (DebugOffsets != nullptr && DebugOffsets->GetInt() != 0);
    }

    bool HasCheckShaders()
    {
        const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
        const bool bHasAll = ShaderMap != nullptr
            && ShaderMap->HasShader(&FGaussianSplatSortPrepareCS::GetStaticType(), 0)
            && ShaderMap->HasShader(&FGaussianSplatSortCompareCS::GetStaticType(), 0)
            && ShaderMap->HasShader(&FGaussianSplatSortSelfCheckCS::GetStaticType(), 0);
        if (!bHasAll)
        {
            LogSkipOnce(TEXT("the validation shaders are missing (compile errors are logged under LogShaders)"));
        }
        return bHasAll;
    }

    // Logs a finished check. Called once at the start of every splat post-process pass.
    void PollReadback()
    {
        FState& State = GetState();
        if (State.Pending == EKind::None || !State.Readback.IsValid() || !State.Readback->IsReady())
        {
            return;
        }

        uint32 Stats[NumStats] = {};
        if (const uint32* Data = static_cast<const uint32*>(State.Readback->Lock(NumStats * sizeof(uint32))))
        {
            FMemory::Memcpy(Stats, Data, sizeof(Stats));
        }
        State.Readback->Unlock();

        const FCheckInfo& Info = State.Info;
        FString Message;
        bool bFailed = false;
        if (State.Pending == EKind::Validate)
        {
            // Slot 1 holds ~(first mismatch index); it stays 0 when nothing mismatched.
            bFailed = Stats[0] != 0 || Stats[2] != 0 || Stats[3] != 0 || Stats[4] != 0 || Stats[5] != 0 || Stats[7] != 0;
            ++State.ValidatedFrames;
            State.ValidateFailures += bFailed ? 1 : 0;
            const FString First = Stats[0] != 0 ? FString::Printf(TEXT("%u"), ~Stats[1]) : FString(TEXT("none"));
            Message = FString::Printf(
                TEXT("SortValidate view=%u batch=%d mode=%d bits=%u passes=%u repeat=%d M=%u mismatches=%u ")
                TEXT("first=%s keyMismatch=%u unsorted2=%u unsorted1=%u badIdx=%u tail=%u | frames=%llu failures=%llu"),
                Info.ViewKey, Info.BatchIndex, Info.SortMode, Info.KeyBits, Info.Passes, Info.Repeat, Stats[6],
                Stats[0], *First, Stats[4], Stats[2], Stats[3], Stats[5], Stats[7],
                State.ValidatedFrames, State.ValidateFailures);
        }
        else
        {
            bFailed = Stats[0] != 0 || Stats[1] != 0;
            ++State.CheckedFrames;
            State.CheckFailures += bFailed ? 1 : 0;
            Message = FString::Printf(
                TEXT("SortSelfCheck view=%u batch=%d mode=%d bits=%u count=%u decreasing=%u badIdx=%u ")
                TEXT("| frames=%llu failures=%llu"),
                Info.ViewKey, Info.BatchIndex, Info.SortMode, Info.KeyBits, Stats[6], Stats[0], Stats[1],
                State.CheckedFrames, State.CheckFailures);
        }

        if (bFailed)
        {
            UE_LOG(LogGaussianSplatProfile, Error, TEXT("%s"), *Message);
        }
        else
        {
            UE_LOG(LogGaussianSplatProfile, Display, TEXT("%s"), *Message);
        }
        State.Pending = EKind::None;
    }

    // Whether this view and batch should run a check of this cadence on this frame.
    bool ShouldRun(const FSceneView& View, int32 BatchIndex, int32 EveryNFrames)
    {
        if (EveryNFrames <= 0)
        {
            return false;
        }

        FState& State = GetState();
        const uint32 ViewKey = View.GetViewKey();
        if (!State.LoggedViews.Contains(ViewKey))
        {
            State.LoggedViews.Add(ViewKey);
            UE_LOG(
                LogGaussianSplatProfile,
                Display,
                TEXT("Sort check: view %u is a %s view."),
                ViewKey,
                View.bIsSceneCapture ? TEXT("scene-capture") : TEXT("non-capture"));
        }

        if (State.Pending != EKind::None
            || State.LastStartedFrame == GFrameNumberRenderThread
            || (GFrameNumberRenderThread % static_cast<uint32>(EveryNFrames)) != 0)
        {
            return false;
        }

        // View 0 means the first non-capture view: deferred scene captures (BP_Carla_Sky, CARLA
        // sensors) render before the main view each frame, so "first view" would be the sky.
        const uint32 WantedView = GaussianSplatProfiling::GetSortValidateView();
        const bool bViewMatches = WantedView == 0 ? !View.bIsSceneCapture : ViewKey == WantedView;
        return bViewMatches && BatchIndex == GaussianSplatProfiling::GetSortValidateBatch();
    }

    FRDGBufferRef CreateStatsBuffer(FRDGBuilder& GraphBuilder)
    {
        // BUF_SourceCopy: the RHI validation layer requires it on any copy source.
        FRDGBufferDesc StatsDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumStats);
        StatsDesc.Usage |= BUF_SourceCopy;
        FRDGBufferRef Stats = GraphBuilder.CreateBuffer(StatsDesc, TEXT("GaussianSplat.SortCheckStats"));
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Stats), 0u);
        return Stats;
    }

    void EnqueueReadback(FRDGBuilder& GraphBuilder, FRDGBufferRef Stats, EKind Kind, const FCheckInfo& Info)
    {
        FState& State = GetState();
        if (!State.Readback.IsValid())
        {
            State.Readback = MakeUnique<FRHIGPUBufferReadback>(TEXT("GaussianSplat.SortCheck"));
        }
        AddEnqueueCopyPass(GraphBuilder, State.Readback.Get(), Stats, NumStats * sizeof(uint32));
        State.Pending = Kind;
        State.Info = Info;
        State.LastStartedFrame = GFrameNumberRenderThread;
    }

    struct FValidationInputs
    {
        FRDGBufferRef TmpKeys = nullptr;
        FRDGBufferRef TmpOrder = nullptr;

        bool IsValid() const
        {
            return TmpKeys != nullptr && TmpOrder != nullptr;
        }
    };

    // Copies the cull's unsorted prefix, exactly what mode 2 will sort, into a scratch pair padded
    // with sentinels. Must run before mode 2, whose second pass overwrites the originals.
    FValidationInputs AddPrepare(
        FRDGBuilder& GraphBuilder,
        FRDGBufferRef Keys,
        FRDGBufferRef Order,
        FRDGBufferRef IndirectArgs,
        uint32 MaxKeys,
        uint32 GpuCap)
    {
        FValidationInputs Inputs;
        if (!HasCheckShaders())
        {
            return Inputs;
        }

        const uint32 NumElements = Keys->Desc.NumElements;
        Inputs.TmpKeys = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumElements), TEXT("GaussianSplat.ValidateTmpKeys"));
        Inputs.TmpOrder = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumElements), TEXT("GaussianSplat.ValidateTmpOrder"));

        FGaussianSplatSortValidateParameters* Parameters = GraphBuilder.AllocParameters<FGaussianSplatSortValidateParameters>();
        Parameters->MaxKeys = MaxKeys;
        Parameters->GpuCap = GpuCap;
        Parameters->GridStride = GetGroupCount(MaxKeys) * ThreadsPerGroup;
        Parameters->SortCount = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(IndirectArgs, PF_R32_UINT));
        Parameters->SrcKeys = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Keys, PF_R32_UINT));
        Parameters->SrcOrder = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Order, PF_R32_UINT));
        Parameters->TmpKeys = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Inputs.TmpKeys, PF_R32_UINT));
        Parameters->TmpOrder = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Inputs.TmpOrder, PF_R32_UINT));

        TShaderMapRef<FGaussianSplatSortPrepareCS> PrepareCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("GaussianSplatSortValidate.Prepare"),
            PrepareCS,
            Parameters,
            FIntVector(static_cast<int32>(GetGroupCount(MaxKeys)), 1, 1));
        return Inputs;
    }

    // Sorts the prepared copy with mode 1 and compares it with the pair the rasterizer binds.
    // SpareKeys/SpareOrder must be the pair mode 2 did NOT end in; mode 1 uses it as scratch.
    void AddCompare(
        FRDGBuilder& GraphBuilder,
        ERHIFeatureLevel::Type FeatureLevel,
        const FValidationInputs& Inputs,
        FRDGBufferRef CheckKeys,
        FRDGBufferRef CheckOrder,
        FRDGBufferRef SpareKeys,
        FRDGBufferRef SpareOrder,
        FRDGBufferRef IndirectArgs,
        uint32 MaxKeys,
        uint32 GpuCap,
        uint32 OrderBound,
        bool bTailCheck,
        const FCheckInfo& Info)
    {
        const FRDGBufferRef RefOrder = GaussianSplatSorting::AddRadixSortPass(
            GraphBuilder,
            FeatureLevel,
            Inputs.TmpOrder,
            SpareOrder,
            Inputs.TmpKeys,
            SpareKeys,
            MaxKeys);
        const FRDGBufferRef RefKeys = (RefOrder == Inputs.TmpOrder) ? Inputs.TmpKeys : SpareKeys;
        const FRDGBufferRef Stats = CreateStatsBuffer(GraphBuilder);

        FGaussianSplatSortValidateParameters* Parameters = GraphBuilder.AllocParameters<FGaussianSplatSortValidateParameters>();
        Parameters->MaxKeys = MaxKeys;
        Parameters->GpuCap = GpuCap;
        Parameters->OrderBound = OrderBound;
        Parameters->TailCheck = bTailCheck ? 1u : 0u;
        Parameters->GridStride = GetGroupCount(MaxKeys) * ThreadsPerGroup;
        Parameters->SortCount = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(IndirectArgs, PF_R32_UINT));
        Parameters->CheckKeys = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CheckKeys, PF_R32_UINT));
        Parameters->CheckOrder = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CheckOrder, PF_R32_UINT));
        Parameters->RefKeys = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RefKeys, PF_R32_UINT));
        Parameters->RefOrder = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RefOrder, PF_R32_UINT));
        Parameters->Stats = GraphBuilder.CreateUAV(Stats);

        TShaderMapRef<FGaussianSplatSortCompareCS> CompareCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("GaussianSplatSortValidate.Compare"),
            CompareCS,
            Parameters,
            FIntVector(static_cast<int32>(GetGroupCount(MaxKeys)), 1, 1));
        EnqueueReadback(GraphBuilder, Stats, EKind::Validate, Info);
    }

    // Reference-free: are the keys the rasterizer's pair holds non-decreasing and its splat
    // indices in range? Works for every sort mode, including the fallback.
    void AddSelfCheck(
        FRDGBuilder& GraphBuilder,
        FRDGBufferRef Keys,
        FRDGBufferRef Order,
        FRDGBufferRef IndirectArgs,
        uint32 Budget,
        uint32 OrderBound,
        const FCheckInfo& Info)
    {
        if (!HasCheckShaders())
        {
            return;
        }
        const FRDGBufferRef Stats = CreateStatsBuffer(GraphBuilder);

        FGaussianSplatSortValidateParameters* Parameters = GraphBuilder.AllocParameters<FGaussianSplatSortValidateParameters>();
        Parameters->SelfCheckCount = Budget;
        Parameters->OrderBound = OrderBound;
        Parameters->GridStride = GetGroupCount(Budget) * ThreadsPerGroup;
        Parameters->SortCount = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(IndirectArgs, PF_R32_UINT));
        Parameters->CheckKeys = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Keys, PF_R32_UINT));
        Parameters->CheckOrder = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Order, PF_R32_UINT));
        Parameters->Stats = GraphBuilder.CreateUAV(Stats);

        TShaderMapRef<FGaussianSplatSortSelfCheckCS> SelfCheckCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("GaussianSplatSortValidate.SelfCheck"),
            SelfCheckCS,
            Parameters,
            FIntVector(static_cast<int32>(GetGroupCount(Budget)), 1, 1));
        EnqueueReadback(GraphBuilder, Stats, EKind::SelfCheck, Info);
    }
}

BEGIN_SHADER_PARAMETER_STRUCT(FGaussianSplatPointsRasterPassParameters, )
    SHADER_PARAMETER_STRUCT_INCLUDE(FGaussianSplatPointsRasterVS::FParameters, VS)
    SHADER_PARAMETER_STRUCT_INCLUDE(FGaussianSplatPointsRasterPS::FParameters, PS)
    RDG_BUFFER_ACCESS(IndirectArgsBuffer, ERHIAccess::IndirectArgs)
    RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FGaussianSplatBillboardsRasterPassParameters, )
    SHADER_PARAMETER_STRUCT_INCLUDE(FGaussianSplatBillboardsRasterVS::FParameters, VS)
    SHADER_PARAMETER_STRUCT_INCLUDE(FGaussianSplatBillboardsRasterPS::FParameters, PS)
    RDG_BUFFER_ACCESS(IndirectArgsBuffer, ERHIAccess::IndirectArgs)
    RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

namespace GaussianSplatLod
{
    struct FSelection
    {
        // Always sized to the cell count so the RDG pool sees one buffer size
        // per asset instead of a new one whenever the camera moves. Only the
        // first CellCount entries are meaningful.
        //
        // uint4 rather than uint2 since quantization: positions are 16-bit
        // fractions of their own cell, so the shader needs the cell index to
        // find the origin and extent to decode against.
        TArray<FUintVector4> Ranges;
        uint32 CellCount = 0;
        uint32 TotalCount = 0;

        // Fix 5 Step 3 (D7). CellBase turns this asset's own cell index into an index
        // into the pool's ONE cell-bounds buffer, so ResolveSplat's
        // SplatCellBounds[Cell * 2] is right whichever asset the splat came from.
        // BatchId goes in .w, which was written 0 until now, and selects the batch
        // table entry carrying this asset's transform, point size and palette slot.
        // Both are 0 on the legacy path, which binds its own bounds and draws alone.
        uint32 CellBase = 0;
        uint32 BatchId = 0;
    };

    // Fix 5 probe (r.GaussianSplat.LodSplitRuns): how many entries a selection can need. One per cell, or with runs of
    // N, at most one per cell plus one per N splats, since the sum of ceil(take / N) is at most cells + total / N.
    // Fixed per asset, like the one-per-cell size, so the RDG pool still sees one buffer size.
    // A paged cell emits its floor run and its tail run -- two entries when the
    // allocator could place its pages contiguously, which it does whenever there is
    // untouched space. Four per cell leaves room for a fragmented pool.
    constexpr uint32 PagedRangesPerCell = 4;

    uint32 RangeCapacity(const TArray<FGaussianSplatCell>& Cells, uint32 SplitRuns)
    {
        if (SplitRuns == 0)
        {
            return static_cast<uint32>(FMath::Max(1, Cells.Num()));
        }
        uint64 Total = 0;
        for (const FGaussianSplatCell& Cell : Cells)
        {
            Total += static_cast<uint64>(FMath::Max(0, Cell.Count));
        }
        return static_cast<uint32>(FMath::Min<uint64>(MAX_int32, FMath::Max<uint64>(1, Cells.Num() + Total / SplitRuns)));
    }

    // One cell's take as one entry, or as consecutive runs of SplitRuns splats: the same splats in the same dispatch
    // order, only more entries for ResolveSplat to search.
    void AddRange(FSelection& Out, uint32 First, uint32 Prefix, uint32 CellIndex, uint32 Take, uint32 SplitRuns)
    {
        const uint32 GlobalCell = Out.CellBase + CellIndex;
        if (SplitRuns == 0)
        {
            Out.Ranges[Out.CellCount++] = FUintVector4(First, Prefix, GlobalCell, Out.BatchId);
            return;
        }
        for (uint32 Offset = 0; Offset < Take; Offset += SplitRuns)
        {
            Out.Ranges[Out.CellCount++] = FUintVector4(First + Offset, Prefix + Offset, GlobalCell, Out.BatchId);
        }
    }

    // The paged form: the same splats in the same dispatch order, but addressed by
    // POOL SLOT rather than by position in the asset's own buffer. Returns how many
    // splats it could actually cover -- the shortfall is a miss, which must be zero
    // on a camera frame in synchronous mode.
    uint32 AddPagedRange(
        FSelection& Out,
        const UGaussianSplatPagedAsset& Asset,
        const FGaussianSplatPoolResidency& Residency,
        uint32 CellIndex,
        uint32 Take,
        uint32& InOutPrefix,
        TArray<FGaussianSplatPoolRun>& Scratch)
    {
        const uint32 Covered = BuildPoolRuns(Asset, Residency, static_cast<int32>(CellIndex), Take, Scratch);
        for (const FGaussianSplatPoolRun& Run : Scratch)
        {
            if (Out.CellCount >= static_cast<uint32>(Out.Ranges.Num()))
            {
                break;   // the capacity estimate was short; better a missing run than a write past the end
            }
            Out.Ranges[Out.CellCount++] = FUintVector4(Run.FirstSlot, InOutPrefix, Out.CellBase + CellIndex, Out.BatchId);
            InOutPrefix += Run.Count;
        }
        return Covered;
    }

    // Per-cell frustum cull + distance LOD + budget feedback, replacing the
    // single global stride.
    //
    // The stride was spatially blind: it thinned the road under the bumper
    // exactly as hard as the forest two kilometres away, and from a whole-map
    // view it drew every resident splat (measured: 40M drawn, 94.6 ms). Cells
    // let each region answer for itself.
    void SelectCells(
        const TArray<FGaussianSplatCell>& Cells,
        const FMatrix& LocalToWorld,
        const FSceneView& View,
        uint32 Budget,
        float& InOutBias,
        FSelection& Out)
    {
        const uint32 SplitRuns = GaussianSplatProfiling::GetLodSplitRuns();
        Out.Ranges.Reset();
        Out.Ranges.SetNumZeroed(RangeCapacity(Cells, SplitRuns));
        Out.CellCount = 0;
        Out.TotalCount = 0;

        if (Cells.IsEmpty() || Budget == 0)
        {
            return;
        }

        const FMatrix& ToWorld = LocalToWorld;   // already double (review M7)
        const FVector ViewOrigin = View.ViewMatrices.GetViewOrigin();
        const double FullDistance =
            FMath::Max(1.0f, GaussianSplatProfiling::CVarLodFullDistance.GetValueOnRenderThread());
        // r.GaussianSplat.Lod 0 means no distance thinning: every visible cell
        // keeps all of its splats. The budget clamp below still applies, so this
        // cannot overrun the sort buffers.
        const float MinFraction =
            (GaussianSplatProfiling::CVarLodEnabled.GetValueOnRenderThread() != 0)
                ? FMath::Clamp(GaussianSplatProfiling::CVarLodMinFraction.GetValueOnRenderThread(), 0.0f, 1.0f)
                : 1.0f;
        const float Bias = FMath::Clamp(InOutBias, 0.001f, 4.0f);

        TArray<uint32> Firsts;
        TArray<uint32> Takes;
        TArray<uint32> CellIndices;
        Firsts.Reserve(Cells.Num());
        Takes.Reserve(Cells.Num());
        CellIndices.Reserve(Cells.Num());

        uint64 Total = 0;
        for (int32 CellIndex = 0; CellIndex < Cells.Num(); ++CellIndex)
        {
            const FGaussianSplatCell& Cell = Cells[CellIndex];
            if (Cell.Count <= 0)
            {
                continue;
            }

            // Shrink-wrapped bounds, so a cell holding a few floaters presents a
            // one-metre target rather than the full grid slot.
            const FBox WorldBox =
                FBox(FVector(Cell.BoundsMin), FVector(Cell.BoundsMax)).TransformBy(ToWorld);
            if (!View.ViewFrustum.IntersectBox(WorldBox.GetCenter(), WorldBox.GetExtent()))
            {
                continue;
            }

            // Inverse square, because that is how a cell's projected area falls
            // off; keeping splats proportional to it keeps screen-space density
            // roughly constant instead of over-drawing the far field.
            const double Distance = FMath::Max(
                FMath::Sqrt(ComputeSquaredDistanceFromBoxToPoint(WorldBox.Min, WorldBox.Max, ViewOrigin)),
                FullDistance);
            const double Falloff = (FullDistance / Distance) * (FullDistance / Distance);
            const float Fraction = FMath::Clamp(static_cast<float>(Bias * Falloff), MinFraction, 1.0f);

            // Never fewer than one: a cell that is visible at all should leave
            // some trace rather than pop out completely.
            const uint32 Take = static_cast<uint32>(
                FMath::Clamp(FMath::RoundToInt(Cell.Count * Fraction), 1, Cell.Count));

            Firsts.Add(static_cast<uint32>(Cell.FirstIndex));
            Takes.Add(Take);
            CellIndices.Add(static_cast<uint32>(CellIndex));
            Total += Take;
        }

        // The bias only corrects the NEXT frame, and the sort buffers are sized
        // to the budget, so an overshoot has to be absorbed here and now.
        if (Total > Budget)
        {
            const double Scale = static_cast<double>(Budget) / static_cast<double>(Total);
            Total = 0;
            for (uint32& Take : Takes)
            {
                Take = static_cast<uint32>(FMath::Max<int64>(1, FMath::RoundToInt(Take * Scale)));
                Total += Take;
            }

            // Rounding and the one-splat floor can still leave a handful over.
            for (int32 Index = Takes.Num() - 1; Index >= 0 && Total > Budget; --Index)
            {
                const uint32 Drop = static_cast<uint32>(FMath::Min<uint64>(Takes[Index] - 1, Total - Budget));
                Takes[Index] -= Drop;
                Total -= Drop;
            }
        }

        uint32 Prefix = 0;
        for (int32 Index = 0; Index < Takes.Num(); ++Index)
        {
            AddRange(Out, Firsts[Index], Prefix, CellIndices[Index], Takes[Index], SplitRuns);
            Prefix += Takes[Index];
        }
        Out.TotalCount = Prefix;

        // Cesium's feedback loop. Tighten immediately when over budget, relax
        // slowly when under, so it settles rather than oscillating. The bias
        // never tightens while the selection fits: it grows 5% a frame up to 4,
        // which is why a settled view keeps everything out to 2 x LodFullDistance.
        if (Out.TotalCount > 0)
        {
            const float Desired = static_cast<float>(Budget) / static_cast<float>(Out.TotalCount);
            InOutBias = FMath::Clamp(Bias * FMath::Min(Desired, 1.05f), 0.001f, 4.0f);
        }
    }

    // r.GaussianSplat.LodMode 1's per-view inputs.
    struct FScreenLodInputs
    {
        // P[0][0] x ViewRect width / 2; 0 for an orthographic view.
        double FocalPx = 0.0;
        double ActorScale = 1.0;
        float PointSize = 1.0f;
        float SizeRef = 0.0f;
        float SizeP99 = 0.0f;
        // Points mode and orthographic views keep whole cells; only the budget clamp applies.
        bool bFullCells = false;
        uint32 ViewKey = 0;
        int32 BatchIndex = 0;
        FIntRect ViewRect;
    };

    // Screen-space per-cell keep, without feedback state. Each visible cell keeps
    // clamp(min(1, (d_full / d)^2), LodMinFraction, 1) of its importance-ordered prefix, with d_full from the view's
    // own focal length and the asset's median splat size, so a 800 px sensor stops full detail at half the distance
    // of a 1600 px one. d is measured to the plain cell box, as mode 0 does; only the frustum test uses the box grown
    // by LodCullMargin.
    //
    // The take is mode 0's expression with its types and multiplication order: Bias 4 (mode 0's saturated value)
    // times the double falloff (F'/max(d, F'))^2, cast to float, with F' = d_full / 2. So LodDebugHalfFullDistance
    // F selects exactly what LodFullDistance F selects after the bias has settled. Over budget, a bisection
    // lowers one multiplier m on d_full, which thins the far cells first; mode 0's uniform clamp stays behind it as
    // the last safety net, and drops the one-splat floor when there are more visible cells than budget (the sort
    // buffers hold only the budget, and the cull does not check).
    // Fix 5 Step 3 (plan D7): one asset taking part in a view's selection. Several of
    // these are solved TOGETHER, under one multiplier and one budget, so a single sort
    // can cover a whole district map. With one group it is exactly what the per-asset
    // call used to be, which is what lets the change land before the merged draw does.
    struct FScreenLodGroup
    {
        const TArray<FGaussianSplatCell>* Cells = nullptr;
        FMatrix LocalToWorld = FMatrix::Identity;   // absolute and double: cell boxes are measured against the real view origin

        // Per asset, because d_full is: it comes from the asset's own median splat size
        // and the actor's scale, so a drone capture and a street capture reach full
        // detail at very different distances under the SAME multiplier.
        float PointSize = 1.0f;
        float SizeRef = 0.0f;
        float SizeP99 = 0.0f;
        double ActorScale = 1.0;

        // Fix 5 Step 4: this asset's d_full ceiling in METRES, 0 = none. Per asset, not
        // per view: it exists because a drone capture's own median splat size reaches
        // full detail hundreds of metres out while a street capture's does not.
        float MaxFullDistanceM = 0.0f;

        // Set together, and only for a paged asset: the selection itself is unchanged
        // -- it still works on the asset's FULL cell counts, so a view's take does not
        // move when a page is evicted -- but the ranges it emits then address pool
        // slots, and anything the pool does not hold is counted as a miss rather than
        // silently dropped.
        const UGaussianSplatPagedAsset* PagedAsset = nullptr;
        const FGaussianSplatPoolResidency* PagedResidency = nullptr;

        uint32 CellBase = 0;      // where this asset's cells start in the pool's shared bounds
        uint32 BatchId = 0;       // its entry in the per-batch table

        double HalfFull = 0.0;    // computed below; kept so the stats line can report it
        uint32 Missed = 0;        // out: splats selected that the pool does not hold
    };

    void SelectCellsScreen(
        TArrayView<FScreenLodGroup> Groups,
        const FSceneView& View,
        uint32 Budget,
        const FScreenLodInputs& In,
        FSelection& Out,
        GaussianSplatProfiling::FLodStats& Stats)
    {
        const uint32 SplitRuns = GaussianSplatProfiling::GetLodSplitRuns();
        int32 RangeSlots = 0;
        for (const FScreenLodGroup& Group : Groups)
        {
            if (Group.Cells == nullptr)
            {
                continue;
            }
            RangeSlots += Group.PagedAsset != nullptr
                ? FMath::Max(1, Group.Cells->Num() * static_cast<int32>(PagedRangesPerCell))
                : static_cast<int32>(RangeCapacity(*Group.Cells, SplitRuns));
        }
        Out.Ranges.Reset();
        Out.Ranges.SetNumZeroed(FMath::Max(1, RangeSlots));
        Out.CellCount = 0;
        Out.TotalCount = 0;
        Stats.Mode = 1;

        if (Groups.IsEmpty() || Budget == 0)
        {
            return;
        }

        const FVector ViewOrigin = View.ViewMatrices.GetViewOrigin();
        const FVector ViewForward = View.GetViewDirection();
        // As mode 0: r.GaussianSplat.Lod 0 keeps every visible cell whole.
        const float MinFraction =
            (GaussianSplatProfiling::CVarLodEnabled.GetValueOnRenderThread() != 0)
                ? FMath::Clamp(GaussianSplatProfiling::CVarLodMinFraction.GetValueOnRenderThread(), 0.0f, 1.0f)
                : 1.0f;
        const float K = FMath::Max(0.0f, GaussianSplatProfiling::CVarLodScreenK.GetValueOnRenderThread());
        const float MinFull = FMath::Max(0.0f, GaussianSplatProfiling::CVarLodMinFullDistance.GetValueOnRenderThread());
        const float DebugHalf = GaussianSplatProfiling::CVarLodDebugHalfFullDistance.GetValueOnRenderThread();
        const float CullMargin = FMath::Max(0.0f, GaussianSplatProfiling::CVarLodCullMargin.GetValueOnRenderThread());

        struct FCandidate
        {
            int32 GroupIndex = 0;
            int32 CellIndex = 0;
            double Distance = 0.0;
            // The group's F' = d_full / 2 at m = 1, carried per candidate because the
            // bisection solves ONE multiplier across groups whose d_full differ.
            double HalfFull = 0.0;
        };
        TArray<FCandidate> Candidates;

        // Whether the settle gate serves this selection: decided once, here, because Fix 7's
        // detail multiplier scales d_full BEFORE the bisection and the overflow clamp bounds it
        // AFTER, and the two must hold under the same condition or they drift apart.
        bool bAnyPaged = false;
        for (const FScreenLodGroup& Group : Groups)
        {
            bAnyPaged = bAnyPaged || Group.PagedAsset != nullptr;
        }
        const bool bGateServed = bAnyPaged && FGaussianSplatPagePool::IsStreamingEnabled();

        for (int32 GroupIndex = 0; GroupIndex < Groups.Num(); ++GroupIndex)
        {
        FScreenLodGroup& Group = Groups[GroupIndex];
        if (Group.Cells == nullptr || Group.Cells->IsEmpty())
        {
            continue;
        }
        const TArray<FGaussianSplatCell>& Cells = *Group.Cells;
        const FMatrix& ToWorld = Group.LocalToWorld;   // absolute and double (review M7)
        // Local (asset) units, like the cell bounds it grows.
        const double Margin = static_cast<double>(CullMargin)
            * 0.35 * static_cast<double>(Group.PointSize) * static_cast<double>(Group.SizeP99);

        // F' = d_full / 2 at m = 1. The debug value goes through the same float Max and widening as mode 0's
        // FullDistance, so the two modes compare identical doubles.
        // Shared with the settle gate (GaussianSplatLod), so the two cannot drift.
        // Fix 7: ComputeHalfFull applies the detail multiplier itself, before its floor and cap, when the
        // gate serves this view. It reads the atomic once per group, so a view whose groups straddle a
        // gate publish sees two k for one frame -- the one-frame staleness GDetailMultiplier documents.
        Group.HalfFull = GaussianSplatLod::ComputeHalfFull(
            In.FocalPx, Group.PointSize, Group.SizeRef, Group.ActorScale, Group.MaxFullDistanceM, bGateServed);

        Candidates.Reserve(Candidates.Num() + Cells.Num());
        for (int32 CellIndex = 0; CellIndex < Cells.Num(); ++CellIndex)
        {
            const FGaussianSplatCell& Cell = Cells[CellIndex];
            if (Cell.Count <= 0)
            {
                continue;
            }

            const FBox LocalBox(FVector(Cell.BoundsMin), FVector(Cell.BoundsMax));
            const FBox WorldBox = LocalBox.TransformBy(ToWorld);
            const FBox CullBox = Margin > 0.0 ? LocalBox.ExpandBy(Margin).TransformBy(ToWorld) : WorldBox;
            if (!View.ViewFrustum.IntersectBox(CullBox.GetCenter(), CullBox.GetExtent()))
            {
                continue;
            }
            // The grown box would also reach back through the frustum's near plane, which sits at the camera, and
            // take whole cells behind it: the cull rejects every splat at depth <= 0, so a cell whose plain box lies
            // entirely behind the camera draws nothing (at street, one 1.49M-splat cell for 2,949 visible splats).
            if (Margin > 0.0
                && FVector::DotProduct(WorldBox.GetCenter() - ViewOrigin, ViewForward)
                    + FVector::DotProduct(WorldBox.GetExtent(), ViewForward.GetAbs()) <= 0.0)
            {
                continue;
            }
            Candidates.Add({GroupIndex, CellIndex,
                FMath::Sqrt(ComputeSquaredDistanceFromBoxToPoint(WorldBox.Min, WorldBox.Max, ViewOrigin)),
                Group.HalfFull});
        }
        }   // groups

        // The bisection's variable is the MULTIPLIER itself now, not d_full: each
        // candidate scales its OWN group's d_full by it, so one m thins every district
        // the same way in screen terms while respecting each asset's full distance.
        const auto TakeAt = [&Groups, &In, MinFraction](const FCandidate& Candidate, double Multiplier) -> uint32
        {
            const FGaussianSplatCell& Cell = (*Groups[Candidate.GroupIndex].Cells)[Candidate.CellIndex];
            if (In.bFullCells)
            {
                return static_cast<uint32>(Cell.Count);
            }
            // Shared with the settle gate, so the gate's bound is the same arithmetic.
            return GaussianSplatLod::TakeAt(Cell.Count, Candidate.Distance, Candidate.HalfFull * Multiplier, MinFraction);
        };
        const auto TotalAt = [&Candidates, &TakeAt](double Multiplier) -> uint64
        {
            uint64 Sum = 0;
            for (const FCandidate& Candidate : Candidates)
            {
                Sum += TakeAt(Candidate, Multiplier);
            }
            return Sum;
        };

        // Budget: the largest m (to 2^-20 in 20 halvings) whose selection fits.
        double Multiplier = 1.0;
        if (!In.bFullCells && TotalAt(1.0) > Budget)
        {
            constexpr double MinMultiplier = 1.0 / 1048576.0;
            if (TotalAt(MinMultiplier) > Budget)
            {
                Multiplier = MinMultiplier;
            }
            else
            {
                double Fits = MinMultiplier;
                double Over = 1.0;
                for (int32 Pass = 0; Pass < 20; ++Pass)
                {
                    const double Mid = 0.5 * (Fits + Over);
                    if (TotalAt(Mid) <= Budget)
                    {
                        Fits = Mid;
                    }
                    else
                    {
                        Over = Mid;
                    }
                }
                Multiplier = Fits;
            }
        }

        // Fix 7's budget limb: whether this view's OWN bisection bound. Read here, before the
        // gate's clamp below -- Stats.Multiplier is the clamped value, and a pinned gate already
        // reaches the controller as its m.
        const bool bBudgetBound = Multiplier < 1.0;

        // D4's overflow rule, second half: the gate's multiplier is the UPPER BOUND
        // of this view's. When the pool cannot hold what the current poses want, the
        // gate trims the required set and uploads exactly that; a view selecting
        // above the same bound would ask for pages that were deliberately left out
        // and draw them as misses (measured: 1.9M at a 600 MiB cap). TakeAt is
        // monotone in the full distance, so clamping after the bisection still fits
        // the budget.
        if (bGateServed)
        {
            Multiplier = FMath::Min(Multiplier, static_cast<double>(FGaussianSplatStreamGate::GetOverflowMultiplier()));
        }

        TArray<uint32> Takes;
        Takes.Reserve(Candidates.Num());
        uint64 Total = 0;
        for (const FCandidate& Candidate : Candidates)
        {
            Takes.Add(TakeAt(Candidate, Multiplier));
            Total += Takes.Last();
        }

        // Mode 0's clamp, the last safety net. Only a selection that still overflows at the smallest m, or whole
        // cells, gets here.
        if (Total > Budget && static_cast<uint64>(Takes.Num()) <= Budget)
        {
            const double Scale = static_cast<double>(Budget) / static_cast<double>(Total);
            Total = 0;
            for (uint32& Take : Takes)
            {
                Take = static_cast<uint32>(FMath::Max<int64>(1, FMath::RoundToInt(Take * Scale)));
                Total += Take;
            }
            for (int32 Index = Takes.Num() - 1; Index >= 0 && Total > Budget; --Index)
            {
                const uint32 Drop = static_cast<uint32>(FMath::Min<uint64>(Takes[Index] - 1, Total - Budget));
                Takes[Index] -= Drop;
                Total -= Drop;
            }
        }
        else if (Total > Budget)
        {
            // More visible cells than budget, so the one-splat floor cannot hold (the sort buffers hold only the
            // budget and the cull does not check). A uniform scale would round every take to 0 and draw nothing, so
            // the budget goes to the nearest cells instead, each up to its take.
            TArray<int32> ByDistance;
            ByDistance.Reserve(Candidates.Num());
            for (int32 Index = 0; Index < Candidates.Num(); ++Index)
            {
                ByDistance.Add(Index);
            }
            ByDistance.Sort([&Candidates](int32 A, int32 B)
            {
                return Candidates[A].Distance != Candidates[B].Distance
                    ? Candidates[A].Distance < Candidates[B].Distance
                    : A < B;
            });
            uint64 Left = Budget;
            for (const int32 Index : ByDistance)
            {
                const uint32 Give = static_cast<uint32>(FMath::Min<uint64>(Takes[Index], Left));
                Takes[Index] = Give;
                Left -= Give;
            }
            Total = static_cast<uint64>(Budget) - Left;
        }

        uint32 Prefix = 0;
        TArray<FGaussianSplatPoolRun> Runs;
        for (int32 Index = 0; Index < Takes.Num(); ++Index)
        {
            if (Takes[Index] == 0)
            {
                continue;
            }
            const uint32 CellIndex = static_cast<uint32>(Candidates[Index].CellIndex);
            FScreenLodGroup& Group = Groups[Candidates[Index].GroupIndex];
            // Each entry carries its own asset's cell base and batch id, so one sorted
            // stream can hold splats from every district (D7).
            Out.CellBase = Group.CellBase;
            Out.BatchId = Group.BatchId;
            if (Group.PagedAsset != nullptr && Group.PagedResidency != nullptr)
            {
                // Prefix advances by what each run actually covers, so a partly
                // resident cell leaves no hole in dispatch space.
                Group.Missed += Takes[Index]
                    - AddPagedRange(Out, *Group.PagedAsset, *Group.PagedResidency, CellIndex, Takes[Index], Prefix, Runs);
                continue;
            }
            const FGaussianSplatCell& Cell = (*Group.Cells)[Candidates[Index].CellIndex];
            AddRange(Out, static_cast<uint32>(Cell.FirstIndex), Prefix, CellIndex, Takes[Index], SplitRuns);
            Prefix += Takes[Index];
        }
        Out.TotalCount = Prefix;

        Stats.K = K;
        Stats.Multiplier = Multiplier;
        Stats.FocalPx = In.FocalPx;
        Stats.FullDistance = 2.0 * (Groups.Num() > 0 ? Groups[0].HalfFull : 0.0) * Multiplier;

        // Fix 7: the controller's feedback, from exactly the views the gate serves -- the set
        // StampView stamps (paged, streaming, not whole cells). A whole-cell view never
        // bisects and misses by design, and would hold the controller forever.
        if (bGateServed && !In.bFullCells)
        {
            bool bMissed = false;
            for (const FScreenLodGroup& Group : Groups)
            {
                bMissed = bMissed || Group.Missed > 0;
            }
            FGaussianSplatStreamGate::ReportViewSelection(bBudgetBound, bMissed);
        }

        // Debug logging. Render thread only, like the rest of this file's static state.
        static int32 LogNextValue = 0;
        static TMap<uint64, int32> LogNextLeft;
        const int32 LogNext = GaussianSplatProfiling::CVarLodLogNext.GetValueOnRenderThread();
        if (LogNext != LogNextValue)
        {
            LogNextValue = LogNext;
            LogNextLeft.Reset();
        }
        const uint64 LogKey = (static_cast<uint64>(In.ViewKey) << 32) | static_cast<uint32>(In.BatchIndex);
        if (LogNext > 0)
        {
            int32& Left = LogNextLeft.FindOrAdd(LogKey, LogNext);
            if (Left > 0)
            {
                --Left;
                UE_LOG(
                    LogGaussianSplatProfile,
                    Display,
                    TEXT("LodSelect view %u batch %d rect %dx%d: k %.4f focal %.3f PointSize %.3f actor scale %.4f ")
                    TEXT("s_ref %.6f s_p99 %.6f d_full %.2f (min %.0f) m %.6f margin %.2f | selected %u of %u budget ")
                    TEXT("across %u of %d cells%s"),
                    In.ViewKey,
                    In.BatchIndex,
                    In.ViewRect.Width(),
                    In.ViewRect.Height(),
                    K,
                    In.FocalPx,
                    Groups[0].PointSize,
                    Groups[0].ActorScale,
                    Groups[0].SizeRef,
                    Groups[0].SizeP99,
                    Stats.FullDistance,
                    MinFull,
                    Multiplier,
                    static_cast<double>(CullMargin) * 0.35 * Groups[0].PointSize * Groups[0].SizeP99,
                    Out.TotalCount,
                    Budget,
                    Out.CellCount,
                    Candidates.Num(),
                    In.bFullCells ? TEXT(" (whole cells)") : (DebugHalf > 0.0f ? TEXT(" (debug d_full)") : TEXT("")));
            }
        }

        static int32 DumpValue = 0;
        static uint32 DumpFrame = MAX_uint32;
        const int32 Dump = GaussianSplatProfiling::CVarLodDumpCells.GetValueOnRenderThread();
        if (Dump != DumpValue)
        {
            DumpValue = Dump;
            DumpFrame = Dump > 0 ? GFrameNumberRenderThread : MAX_uint32;
        }
        if (DumpFrame == GFrameNumberRenderThread)
        {
            UE_LOG(
                LogGaussianSplatProfile,
                Display,
                TEXT("LodDumpCells view %u batch %d: d_full %.2f m %.6f, %d cells in the frustum, %u selected"),
                In.ViewKey,
                In.BatchIndex,
                Stats.FullDistance,
                Multiplier,
                Candidates.Num(),
                Out.TotalCount);
            for (int32 Index = 0; Index < Candidates.Num(); ++Index)
            {
                const FGaussianSplatCell& Cell =
                    (*Groups[Candidates[Index].GroupIndex].Cells)[Candidates[Index].CellIndex];
                UE_LOG(
                    LogGaussianSplatProfile,
                    Display,
                    TEXT("LodDumpCells view %u batch %d cell %d: count %d dist %.1f fraction %.6f take %u%s"),
                    In.ViewKey,
                    In.BatchIndex,
                    Candidates[Index].CellIndex,
                    Cell.Count,
                    Candidates[Index].Distance,
                    static_cast<double>(Takes[Index]) / static_cast<double>(FMath::Max(1, Cell.Count)),
                    Takes[Index],
                    Takes[Index] == static_cast<uint32>(Cell.Count) ? TEXT(" whole") : TEXT(""));
            }
        }
    }
}

// The passes bind their shaders through unconditional TShaderMapRefs, and a missing shader asserts in
// GetShader and takes the editor down. After a compile error in one of them, draw no splats and log one
// Error line instead (the details are under LogShaders).
static bool HasGaussianSplatShaders()
{
    const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
    const bool bHasAll = ShaderMap != nullptr
        && ShaderMap->HasShader(&FGaussianSplatPointsCullCS::GetStaticType(), 0)
        && ShaderMap->HasShader(&FGaussianSplatBillboardsCullCS::GetStaticType(), 0)
        && ShaderMap->HasShader(&FGaussianSplatPointsRasterVS::GetStaticType(), 0)
        && ShaderMap->HasShader(&FGaussianSplatPointsRasterPS::GetStaticType(), 0)
        && ShaderMap->HasShader(&FGaussianSplatBillboardsRasterVS::GetStaticType(), 0)
        // The multi-palette permutation is picked per draw now, so it has to exist too.
        && ShaderMap->HasShader(&FGaussianSplatBillboardsRasterVS::GetStaticType(), 1)
        && ShaderMap->HasShader(&FGaussianSplatBillboardsRasterPS::GetStaticType(), 0)
        && ShaderMap->HasShader(&FGaussianSplatCompositePS::GetStaticType(), 0);
    static bool bLoggedMissing = false;
    if (!bHasAll && !bLoggedMissing)
    {
        UE_LOG(
            LogGaussianSplatProfile,
            Error,
            TEXT("A Gaussian splat shader is missing (compile errors are logged under LogShaders); drawing no splats."));
        bLoggedMissing = true;
    }
    return bHasAll;
}

// The hidden-splat cull needs its kernels and, for the box path, the cull's OCC_BOX permutation (Vulkan only).
// Only the chosen test path's shaders are required, so one failed permutation leaves the other path usable.
// Without them the batch keeps its single draw, as with OccPhases 0, and one Warning per path says why.
static bool HasOcclusionShaders(bool bBoxPath)
{
    const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
    FGaussianSplatBillboardsCullCS::FPermutationDomain CullBox;
    CullBox.Set<FGaussianSplatBillboardsCullCS::FOccBoxDim>(true);
    FGaussianSplatOccFlagCS::FPermutationDomain Flag;
    Flag.Set<FGaussianSplatOccFlagCS::FRecomputeDim>(!bBoxPath);
    const bool bHasAll = ShaderMap != nullptr
        && ShaderMap->HasShader(&FGaussianSplatOccArgsCS::GetStaticType(), 0)
        && ShaderMap->HasShader(&FGaussianSplatOccReduceCS::GetStaticType(), 0)
        && ShaderMap->HasShader(&FGaussianSplatOccSatCS::GetStaticType(), 0)
        && ShaderMap->HasShader(&FGaussianSplatOccFlagCS::GetStaticType(), Flag.ToDimensionValueId())
        && ShaderMap->HasShader(&FGaussianSplatOccScanCS::GetStaticType(), 0)
        && ShaderMap->HasShader(&FGaussianSplatOccScatterCS::GetStaticType(), 0)
        && (!bBoxPath
            || (ShaderMap->HasShader(&FGaussianSplatBillboardsCullCS::GetStaticType(), CullBox.ToDimensionValueId())
                && ShaderMap->HasShader(&FGaussianSplatOccTestCS::GetStaticType(), 0)));
    static bool bLoggedMissing[2] = {false, false};
    if (!bHasAll && !bLoggedMissing[bBoxPath ? 1 : 0])
    {
        UE_LOG(
            LogGaussianSplatProfile,
            Warning,
            TEXT("r.GaussianSplat.OccPhases is set but a hidden-splat cull shader of the %s path is missing ")
            TEXT("(Vulkan only; compile errors are logged under LogShaders); drawing in one pass."),
            bBoxPath ? TEXT("box") : TEXT("recompute"));
        bLoggedMissing[bBoxPath ? 1 : 0] = true;
    }
    return bHasAll;
}

namespace GaussianSplatLod
{
    float GetMinFraction()
    {
        // r.GaussianSplat.Lod 0 keeps every visible cell whole, as mode 0 does.
        return (GaussianSplatProfiling::CVarLodEnabled.GetValueOnAnyThread() != 0)
            ? FMath::Clamp(GaussianSplatProfiling::CVarLodMinFraction.GetValueOnAnyThread(), 0.0f, 1.0f)
            : 1.0f;
    }

    double ComputeHalfFull(double FocalPx, float PointSize, float SizeRef, double ActorScale,
                           float MaxFullDistanceM, bool bDetail)
    {
        const float K = FMath::Max(0.0f, GaussianSplatProfiling::CVarLodScreenK.GetValueOnAnyThread());
        const float MinFull = FMath::Max(0.0f, GaussianSplatProfiling::CVarLodMinFullDistance.GetValueOnAnyThread());
        const float DebugHalf = GaussianSplatProfiling::CVarLodDebugHalfFullDistance.GetValueOnAnyThread();

        // The debug value goes through the same float Max and widening as mode 0's
        // FullDistance, so the two modes compare identical doubles.
        double HalfFull = DebugHalf > 0.0f
            ? static_cast<double>(FMath::Max(1.0f, DebugHalf))
            : 0.5 * (static_cast<double>(K) * FocalPx * 0.35 * static_cast<double>(PointSize)
                * static_cast<double>(SizeRef) * ActorScale);
        // Fix 7: the detail multiplier, on the formula term and BEFORE the floor and the
        // per-actor cap below, so k > 1 never takes d_full past LodMaxFullDistance (v3 §3).
        // The floor then floors the RAISED value: on a floored scene a small k changes
        // nothing until k x formula clears it -- the plan's known dead zone, not a bug.
        // The debug override is scaled too, as it was when the multiply sat after this call.
        // Ctl.Enable 0 publishes exactly 1.0f, and x 1.0 is exact in IEEE arithmetic, so the
        // floor and the cap see the same double they did before the controller existed.
        if (bDetail)
        {
            HalfFull *= static_cast<double>(FGaussianSplatStreamGate::GetDetailMultiplier());
        }
        HalfFull = FMath::Max(HalfFull, 0.5 * static_cast<double>(MinFull));
        HalfFull = FMath::Max(HalfFull, 1.0);
        // Fix 5 Step 4: the per-actor ceiling, in metres, applied AFTER the detail
        // multiplier and BEFORE the budget one (the caller does d_full = 2 x HalfFull x m).
        // Centimetres here: d_full lives in world units, the property is metres because
        // that is what anyone setting it is thinking in.
        if (MaxFullDistanceM > 0.0f)
        {
            HalfFull = FMath::Min(HalfFull, 0.5 * static_cast<double>(MaxFullDistanceM) * 100.0);
            HalfFull = FMath::Max(HalfFull, 1.0);
        }
        return HalfFull;
    }

    uint32 TakeAt(int32 CellCount, double Distance, double FullDistance, float MinFraction)
    {
        constexpr float Bias = 4.0f;
        const double Clamped = FMath::Max(Distance, FullDistance);
        const double Falloff = (FullDistance / Clamped) * (FullDistance / Clamped);
        const float Fraction = FMath::Clamp(static_cast<float>(Bias * Falloff), MinFraction, 1.0f);
        return static_cast<uint32>(FMath::Clamp(FMath::RoundToInt(CellCount * Fraction), 1, CellCount));
    }
}

namespace GaussianSplatPasses
{
    FScreenPassTexture AddPostProcessPass(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        const FScreenPassTexture& SceneColor,
        const FScreenPassRenderTarget& Output,
        FRDGTextureRef SceneDepthTexture,
        const TArray<FGaussianSplatRenderBatch>& Batches)
    {
        if (!SceneColor.IsValid() || !Output.IsValid() || Batches.IsEmpty())
        {
            return SceneColor;
        }

        GaussianSplatSortCheck::PollReadback();
        if (!HasGaussianSplatShaders())
        {
            return SceneColor;
        }

        const FIntRect ViewRect = SceneColor.ViewRect;

        // Fix 5 Step 3 (D7, review M7): TRANSLATED world. Everything the shaders see
        // is relative to the view origin, so a district 100 km out is as precise in
        // float as one at the origin -- absolute float matrices lose 1 cm per ulp
        // there, about 11 px at 1 m.
        //
        // NOT View.ViewMatrices.GetTranslatedViewProjectionMatrix(): SceneView.h:707
        // builds that from GetProjectionMatrix(), the JITTERED projection, while this
        // pass deliberately uses the no-AA one. Taking the accessor would fold TAA
        // jitter into every splat and move every picture.
        //
        // PreViewTranslation is -ViewOrigin and TranslatedViewMatrix is the rotation
        // alone (SceneView.cpp:683, :686-690), so the translated view origin is
        // exactly (0,0,0) -- see TranslatedViewOrigin below.
        const FVector PreViewTranslation = View.ViewMatrices.GetPreViewTranslation();
        const FMatrix ViewMatrixD = View.ViewMatrices.GetTranslatedViewMatrix();
        const FMatrix ProjectionMatrixNoAAD = View.ViewMatrices.GetProjectionNoAAMatrix();
        const FMatrix ViewProjectionNoAAD = ViewMatrixD * ProjectionMatrixNoAAD;
        const FMatrix44f ViewMatrix = FMatrix44f(ViewMatrixD);
        const FMatrix44f ProjectionMatrix = FMatrix44f(ProjectionMatrixNoAAD);
        const FMatrix44f ViewProjection = FMatrix44f(ViewProjectionNoAAD);
        const FIntPoint SceneColorExtent = SceneColor.Texture->Desc.Extent;

        FRDGTextureDesc SplatTextureDesc = FRDGTextureDesc::Create2D(
            SceneColorExtent,
            PF_FloatRGBA,
            FClearValueBinding(FLinearColor::Transparent),
            TexCreate_ShaderResource | TexCreate_RenderTargetable);
        FRDGTextureRef SplatTexture = GraphBuilder.CreateTexture(SplatTextureDesc, TEXT("GaussianSplat.SplatTexture"));
        const FScreenPassRenderTarget SplatOutput(SplatTexture, ERenderTargetLoadAction::EClear);
        const bool bUseSceneDepth =
            SceneDepthTexture != nullptr &&
            EnumHasAnyFlags(SceneDepthTexture->Desc.Flags, TexCreate_ShaderResource) &&
            SceneDepthTexture->Desc.NumSamples == 1;
        FRDGTextureRef DepthTextureForSampling = bUseSceneDepth
            ? SceneDepthTexture
            : GSystemTextures.GetDepthDummy(GraphBuilder);
        const TUniformBufferRef<FViewUniformShaderParameters> ViewUniformBuffer = View.ViewUniformBuffer;

        const auto SetDepthTestParameters = [
            bUseSceneDepth,
            ViewRect,
            DepthTextureForSampling,
            ViewUniformBuffer](FGaussianSplatDepthTestParameters& Parameters)
        {
            Parameters.View = ViewUniformBuffer;
            Parameters.UseSceneDepth = bUseSceneDepth ? 1u : 0u;
            Parameters.OutputViewRectMin = FVector2f(
                static_cast<float>(ViewRect.Min.X),
                static_cast<float>(ViewRect.Min.Y));
            Parameters.OutputViewSize = FVector2f(
                static_cast<float>(ViewRect.Width()),
                static_cast<float>(ViewRect.Height()));
            Parameters.SceneDepthTextureSize = FVector2f(
                static_cast<float>(DepthTextureForSampling->Desc.Extent.X),
                static_cast<float>(DepthTextureForSampling->Desc.Extent.Y));
            Parameters.SceneDepthTexture = DepthTextureForSampling;
        };

        const FVector2f SceneColorTextureSize(
            static_cast<float>(FMath::Max(1, SceneColorExtent.X)),
            static_cast<float>(FMath::Max(1, SceneColorExtent.Y)));

        TShaderMapRef<FGaussianSplatPointsCullCS> PointsCullCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        // Permutation 0 (no tile box): the bitonic passes, and the cull whenever the box path is off.
        const FGaussianSplatBillboardsCullCS::FPermutationDomain CullWithoutBox;
        TShaderMapRef<FGaussianSplatBillboardsCullCS> BillboardsCullCS(GetGlobalShaderMap(GMaxRHIFeatureLevel), CullWithoutBox);
        TShaderMapRef<FGaussianSplatPointsRasterVS> PointsRasterVS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        TShaderMapRef<FGaussianSplatPointsRasterPS> PointsRasterPS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        // BOTH permutations, picked per draw below. This used to be hardcoded false under a
        // comment claiming landing 3 chose it; landing 3 never did, so every SH3 batch in a
        // merged draw read slot 0's palette whatever its own slot said -- harmless only while
        // every map held copies of ONE capture. The switch is not free (3.52 ms on a degree-3
        // capture when it compiles), so the cheap permutation stays the default and the
        // expensive one is used only when a view's SELECTED cells really span two palettes.
        FGaussianSplatBillboardsRasterVS::FPermutationDomain SinglePalettePerm, MultiPalettePerm;
        SinglePalettePerm.Set<FGaussianSplatBillboardsRasterVS::FMultiPalette>(false);
        MultiPalettePerm.Set<FGaussianSplatBillboardsRasterVS::FMultiPalette>(true);
        TShaderMapRef<FGaussianSplatBillboardsRasterVS> BillboardsRasterVS(
            GetGlobalShaderMap(GMaxRHIFeatureLevel), SinglePalettePerm);
        TShaderMapRef<FGaussianSplatBillboardsRasterVS> BillboardsRasterVSMulti(
            GetGlobalShaderMap(GMaxRHIFeatureLevel), MultiPalettePerm);
        TShaderMapRef<FGaussianSplatBillboardsRasterPS> BillboardsRasterPS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        bool bFirstBatch = true;

        // Fix 5 Step 3 (plan D7): the per-asset table, one entry per batch, indexed by
        // the batch id the range entries now carry. Built for EVERY batch including the
        // ones that early-out below, so a batch's id is its index here and nothing has
        // to renumber; an unused entry is never referenced. The legacy path reads it
        // too -- it is simply the only batch in its own draw.
        TArray<FGaussianSplatBatchEntry> BatchEntries;
        BatchEntries.SetNum(FMath::Max(1, Batches.Num()));
        for (int32 Index = 0; Index < Batches.Num(); ++Index)
        {
            const FGaussianSplatRenderBatch& Entry = Batches[Index];
            FGaussianSplatBatchEntry& Out = BatchEntries[Index];
            Out.LocalToWorld = FMatrix44f(Entry.LocalToWorld * FTranslationMatrix(PreViewTranslation));
            Out.WorldToLocalRow0 = Entry.WorldToLocalRow0;
            Out.WorldToLocalRow1 = Entry.WorldToLocalRow1;
            Out.WorldToLocalRow2 = Entry.WorldToLocalRow2;
            Out.PointSize = Entry.PointSize;
            Out.OpacityScale = Entry.OpacityScale;
            Out.Stride = Entry.Stride;
            const UGaussianSplatPagedAsset* const EntryPaged = Entry.PagedAsset;
            const bool bEntryPaged = EntryPaged != nullptr && Entry.PagedResidency != nullptr;
            if (bEntryPaged)
            {
                Out.ColorEncoding = EntryPaged->ColorEncoding;
                // HasSH needs a REAL slot, not just SH in the asset. A ninth SH3 asset is refused
                // a slot (GaussianSplatPagePool.cpp: "SH PALETTE SLOTS FULL ... will draw WITHOUT
                // them") but still answers HasSH() true, and Max(0, INDEX_NONE) used to send it to
                // slot 0 -- reading another capture's palette with its own entry indices, past the
                // end whenever its palette is the larger. Gate on the slot and the refusal keeps
                // the promise its warning makes.
                const bool bEntryHasSH =
                    EntryPaged->HasSH() && Entry.PagedResidency->PaletteSlot != INDEX_NONE;
                Out.HasSH = bEntryHasSH ? 1u : 0u;
                Out.PaletteSlot = bEntryHasSH
                    ? static_cast<uint32>(Entry.PagedResidency->PaletteSlot)
                    : 0u;
            }
            else if (Entry.Resources != nullptr)
            {
                Out.ColorEncoding = Entry.Resources->GetColorEncoding();
                Out.HasSH = Entry.Resources->HasSH() ? 1u : 0u;
                Out.PaletteSlot = 0;   // a legacy draw binds its own palette into slot 0
            }
        }
        FRDGBufferRef BatchTable = CreateStructuredBuffer(
            GraphBuilder,
            TEXT("GaussianSplat.BatchTable"),
            sizeof(FGaussianSplatBatchEntry),
            BatchEntries.Num(),
            BatchEntries.GetData(),
            BatchEntries.Num() * sizeof(FGaussianSplatBatchEntry));
        FRDGBufferSRVRef BatchTableSRV = GraphBuilder.CreateSRV(BatchTable);

        // Fix 5 Step 3 (review M4): ONE draw budget for the view's paged batches, the
        // MAX of their own. A single sort means one set of sort buffers, and those are
        // sized from the budget, so the budget has to be the view's rather than each
        // asset's -- five districts at 25M each would be five 381 MiB scratch sets.
        //
        // Max, not sum: with one paged asset it IS that asset's budget, so a
        // single-asset scene is bit for bit what it was and G3a holds by construction.
        // The distribution across districts is then the LOD rule itself -- near cells
        // of any district keep more, far cells keep less -- which is what a frame-time
        // budget is for. A per-batch budget would do the opposite and protect a far
        // district's share against the one the car is actually in.
        uint32 ViewPagedBudget = 0;
        for (const FGaussianSplatRenderBatch& Entry : Batches)
        {
            if (Entry.PagedAsset != nullptr && Entry.PagedResidency != nullptr)
            {
                ViewPagedBudget = FMath::Max(ViewPagedBudget, Entry.MaxRenderPoints);
            }
        }

        // Fix 5 Step 3 (plan D7): ONE cull, ONE sort, ONE raster for every paged
        // billboard batch of this view.
        //
        // This is what fixes C10. Until now each batch was culled, sorted and rastered
        // on its own and blended into the shared texture in registration order, and the
        // blend is an under-blend -- a later batch is composited BEHIND an earlier one
        // -- so a district registered second drew behind one registered first whatever
        // their depths. Alpha compositing is order-dependent, and the only way to get
        // the order right across assets is to sort them together.
        //
        // It is also where the memory goes: the four sort buffers are sized from the
        // budget rather than the frame's visible count (they must be, or the RDG pool
        // collects one buffer per distinct size until VRAM runs out), so N batches cost
        // N x 381 MiB at a 25M budget. One sort costs it once.
        //
        // Everything it needs is already in place: range entries carry a global cell
        // index and a batch id, every per-asset value is in the batch table that the
        // cull, the raster and the occlusion recompute all read, and for paged assets
        // the packed buffers and the cell bounds are the pool's and therefore identical
        // across batches. Only LOD mode 1 merges: mode 0's selection is still per asset.
        TArray<GaussianSplatLod::FScreenLodGroup> MergedGroups;
        TArray<int32> MergedBatches;
        GaussianSplatLod::FSelection MergedSelection;
        GaussianSplatProfiling::FLodStats MergedStats;
        uint64 MergedAssetPoints = 0;
        uint32 MergedMisses = 0;
        uint32 MergedBudget = 0;
        int32 MergedRepresentative = INDEX_NONE;
        const bool bMergePaged =
            GaussianSplatProfiling::GetLodMode() == 1 && FGaussianSplatPagePool::ArePagedAssetsEnabled()
            && GaussianSplatProfiling::ShouldMergeDistricts();
        if (bMergePaged && View.ViewMatrices.IsPerspectiveProjection())
        {
            FGaussianSplatPagePool& MergePool = FGaussianSplatPagePool::Get();
            for (int32 Index = 0; Index < Batches.Num(); ++Index)
            {
                const FGaussianSplatRenderBatch& Entry = Batches[Index];
                if (Entry.PagedAsset == nullptr || Entry.PagedResidency == nullptr
                    || Entry.RenderMode != EGaussianSplatRenderMode::Billboards
                    || !MergePool.IsAllocated() || Entry.PagedAsset->Cells.IsEmpty())
                {
                    continue;   // the per-batch path below still handles it
                }
                GaussianSplatLod::FScreenLodGroup Group;
                Group.Cells = &Entry.PagedAsset->SelectionCells;
                Group.LocalToWorld = Entry.LocalToWorld;
                Group.PointSize = Entry.PointSize;
                Group.SizeRef = Entry.PagedAsset->SizeRef;
                Group.SizeP99 = Entry.PagedAsset->SizeP99;
                Group.ActorScale = static_cast<double>(Entry.LocalToWorld.GetMaximumAxisScale());
                Group.MaxFullDistanceM = Entry.LodMaxFullDistance;
                Group.PagedAsset = Entry.PagedAsset;
                Group.PagedResidency = Entry.PagedResidency;
                Group.CellBase = static_cast<uint32>(Entry.PagedResidency->CellBoundsBase);
                Group.BatchId = static_cast<uint32>(Index);
                MergedGroups.Add(Group);
                MergedBatches.Add(Index);
                MergedAssetPoints += static_cast<uint64>(Entry.AssetPointCount);
            }
        }
        if (MergedGroups.Num() > 0)
        {
            // Diagnostic for the eight-asset GPU hang (2026-10-09). Registration and the shared cell-bounds
            // table are both provably correct at eight assets, so whatever is wrong is here, in what the draw
            // is handed. Once per process, so a working seven-asset run and a hanging eight-asset one can be
            // diffed line for line. Remove with the CELLBOUNDS log once the hang is understood.
            static bool bLoggedMergeOnce = false;
            if (!bLoggedMergeOnce)
            {
                bLoggedMergeOnce = true;
                for (int32 G = 0; G < MergedGroups.Num(); ++G)
                {
                    const GaussianSplatLod::FScreenLodGroup& Grp = MergedGroups[G];
                    UE_LOG(LogGaussianSplatProfile, Display,
                           TEXT("MERGEDRAW group %2d batch %2d cellBase %7u cells %6d points %10u ")
                           TEXT("paletteSlot %2d pointSize %.2f sizeRef %.4f maxFull %.1f"),
                           G, Grp.BatchId, Grp.CellBase, Grp.Cells ? Grp.Cells->Num() : -1,
                           Batches[MergedBatches[G]].AssetPointCount,
                           Grp.PagedResidency ? Grp.PagedResidency->PaletteSlot : -2,
                           Grp.PointSize, Grp.SizeRef, Grp.MaxFullDistanceM);
                }
                UE_LOG(LogGaussianSplatProfile, Display,
                       TEXT("MERGEDRAW %d groups, assetPoints %llu, viewPagedBudget %u"),
                       MergedGroups.Num(), static_cast<unsigned long long>(MergedAssetPoints), ViewPagedBudget);
            }

            MergedRepresentative = MergedBatches[0];
            const uint32 Override = GaussianSplatProfiling::GetMaxRenderPointsOverride();
            MergedBudget = static_cast<uint32>(FMath::Min<uint64>(
                Override > 0 ? Override : ViewPagedBudget, MergedAssetPoints));

            GaussianSplatLod::FScreenLodInputs LodInputs;
            // focal = P[0][0] x width / 2 (PerspectiveMatrix.h).
            LodInputs.FocalPx = ProjectionMatrixNoAAD.M[0][0] * 0.5 * static_cast<double>(ViewRect.Width());
            LodInputs.bFullCells = false;
            LodInputs.ViewKey = View.GetViewKey();
            LodInputs.BatchIndex = MergedRepresentative;
            LodInputs.ViewRect = ViewRect;
            // Per-asset now, so these are only what the log line prints.
            LodInputs.PointSize = MergedGroups[0].PointSize;
            LodInputs.SizeRef = MergedGroups[0].SizeRef;
            LodInputs.SizeP99 = MergedGroups[0].SizeP99;
            LodInputs.ActorScale = MergedGroups[0].ActorScale;

            // Once per view, not once per batch: the gate's required set is the view's.
            if (FGaussianSplatPagePool::IsStreamingEnabled())
            {
                FGaussianSplatStreamGate::Get().StampView(
                    View.ViewActor, View.ViewMatrices.GetViewOrigin(), LodInputs.FocalPx);
            }

            const uint64 SelectStart = FPlatformTime::Cycles64();
            GaussianSplatLod::SelectCellsScreen(
                MergedGroups, View, MergedBudget, LodInputs, MergedSelection, MergedStats);
            MergedStats.SelectMicros = FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - SelectStart) * 1000.0;
            for (const GaussianSplatLod::FScreenLodGroup& Group : MergedGroups)
            {
                MergedMisses += Group.Missed;
            }
        }

        // Which SH palettes this view's merged draw really reads. Ranges[].W is the batch id and
        // BatchEntries is indexed by it, so the selected cells name their own batches -- a few
        // thousand entries at most. One palette (or none) keeps the cheap permutation; two or more
        // need the switch, and then slot 0 must NOT be overridden because every slot is read.
        uint32 MergedPaletteMask = 0;
        for (uint32 RangeIndex = 0; RangeIndex < MergedSelection.CellCount; ++RangeIndex)
        {
            const uint32 EntryIndex = MergedSelection.Ranges[RangeIndex].W;
            if (BatchEntries.IsValidIndex(static_cast<int32>(EntryIndex))
                && BatchEntries[EntryIndex].HasSH != 0u
                && BatchEntries[EntryIndex].PaletteSlot < FGaussianSplatPagePool::MaxSHPalettes)
            {
                MergedPaletteMask |= 1u << BatchEntries[EntryIndex].PaletteSlot;
            }
        }
        bool bMergedMultiPalette = FMath::CountBits(MergedPaletteMask) > 1;
        if (bMergedMultiPalette && GaussianSplatProfiling::ShouldForceSinglePalette())
        {
            // Said once, and only when it really overrides a multi-palette draw, so the line's absence
            // means the test never put two palettes in one draw.
            static bool bLoggedForceSingleOnce = false;
            if (!bLoggedForceSingleOnce)
            {
                bLoggedForceSingleOnce = true;
                UE_LOG(LogGaussianSplatProfile, Display,
                       TEXT("DEBUG: r.GaussianSplat.Debug.ForceSinglePalette is on: a merged draw spanning palette ")
                       TEXT("mask 0x%02x reads slot %d's palette for every SH batch."),
                       MergedPaletteMask, static_cast<int32>(FMath::CountTrailingZeros(MergedPaletteMask)));
            }
            bMergedMultiPalette = false;
        }
        const int32 MergedOnlyPalette =
            MergedPaletteMask != 0u ? FMath::CountTrailingZeros(MergedPaletteMask) : INDEX_NONE;

        for (int32 BatchIndex = 0; BatchIndex < Batches.Num(); ++BatchIndex)
        {
            const FGaussianSplatRenderBatch& Batch = Batches[BatchIndex];
            const FGaussianSplatRenderResources* Resources = Batch.Resources;

            // The merged paged item is drawn ONCE, by the first batch it covers; the
            // others contributed their cells to the union above and have nothing left
            // to do here.
            const bool bInMerge = MergedBatches.Contains(BatchIndex);
            if (bInMerge && BatchIndex != MergedRepresentative)
            {
                continue;
            }
            const bool bMerged = bInMerge && MergedSelection.CellCount > 0;

            // The ONE cast (review M7). Built in double from the component's absolute
            // matrix, so the float the shaders get carries no accumulated error from
            // the actor's distance to the origin. The CPU selection below keeps the
            // ABSOLUTE double instead -- it measures cell boxes against the real view
            // origin, and translating both would cancel out anyway.
            const FMatrix44f LocalToTranslatedWorld =
                FMatrix44f(Batch.LocalToWorld * FTranslationMatrix(PreViewTranslation));

            // Fix 5's seam. A paged batch draws from the process-wide pool and
            // carries no resources of its own; with r.GaussianSplat.PagedAssets 0
            // nothing ever sets these, every Src* below IS the old expression, and
            // the legacy path runs line for line as before.
            const UGaussianSplatPagedAsset* const PagedAsset = Batch.PagedAsset;
            const FGaussianSplatPoolResidency* const PagedResidency = Batch.PagedResidency;
            const bool bPaged = PagedAsset != nullptr && PagedResidency != nullptr;
            FGaussianSplatPagePool& Pool = FGaussianSplatPagePool::Get();

            const FGaussianSplatRenderResources* const Legacy = bPaged ? nullptr : Resources;
            if (bPaged
                    ? (!Pool.IsAllocated() || PagedAsset->Cells.IsEmpty())
                    : (Legacy == nullptr || Legacy->GetPointCount() == 0 || Legacy->GetPackedASRV() == nullptr))
            {
                continue;
            }

            const TArray<FGaussianSplatCell>& SrcCells = bPaged ? PagedAsset->SelectionCells : Legacy->GetCells();
            FRHIShaderResourceView* const SrcPackedA =
                bPaged ? Pool.GetPackedASRV() : Legacy->GetPackedASRV().GetReference();
            FRHIShaderResourceView* const SrcPackedB =
                bPaged ? Pool.GetPackedBSRV() : Legacy->GetPackedBSRV().GetReference();
            FRHIShaderResourceView* const SrcCellBounds =
                bPaged ? Pool.GetCellBoundsSRV() : Legacy->GetCellBoundsSRV().GetReference();
            FRHIShaderResourceView* const SrcSHIndex =
                bPaged ? Pool.GetSHIndexSRV() : Legacy->GetSHIndexSRV().GetReference();
            FRHIShaderResourceView* const SrcSHPalette =
                bPaged ? Pool.GetSHPaletteSRV(PagedResidency->PaletteSlot)
                       : Legacy->GetSHPaletteSRV().GetReference();
            const FVector2f SrcColorEncoding = bPaged ? PagedAsset->ColorEncoding : Legacy->GetColorEncoding();
            const float SrcSizeRef = bPaged ? PagedAsset->SizeRef : Legacy->GetSizeRef();
            const float SrcSizeP99 = bPaged ? PagedAsset->SizeP99 : Legacy->GetSizeP99();
            const bool bSrcHasSH = bPaged ? PagedAsset->HasSH() : Legacy->HasSH();
            const uint32 SrcPointCount = bPaged
                ? static_cast<uint32>(FMath::Min<int64>(PagedAsset->TotalSplats, MAX_uint32))
                : Legacy->GetPointCount();

            // Any splat a view selected but the pool does not hold. It must be 0 on a
            // camera frame in synchronous mode -- that is the gate's assertion, and
            // in Step 1a it is also how G1e shows the clamp did what it claims.
            uint32 PagedMisses = 0;

            // Cells are MANDATORY once the asset has them. The 20 B record stores
            // each position as a 16-bit fraction of its OWN cell, so without a cell
            // index there is nothing to decode against -- the legacy stride path
            // renders an empty screen. r.GaussianSplat.Lod therefore selects the
            // distance falloff only (see SelectCells), never whether cells are used.
            const bool bUseCells = !SrcCells.IsEmpty();

            // The component's Stride is vestigial once cells exist: the cull
            // resolves thread -> splat through the per-frame ranges, and LOD
            // already sets density per cell from distance. Leaving it in place
            // silently deformed the budget -- MaxRenderPoints 6M over 85.8M
            // resident derived a stride of 15 and turned the budget into
            // 5,721,924, while 90M derived a stride of 1 and made it the whole
            // 85.8M, whose sort buffers (4 x 343 MB) then failed to allocate.
            const uint32 Stride = bUseCells ? 1u : FMath::Max(1u, Batch.Stride);

            // RenderPointCount is the BUDGET: what the renderer may draw, stable
            // frame to frame, and therefore what sizes the buffers.
            // r.GaussianSplat.MaxRenderPointsOverride (Fix 5 probe) replaces every component's budget.
            const uint32 BudgetOverride = GaussianSplatProfiling::GetMaxRenderPointsOverride();
            // A paged batch draws from the view's shared budget; a legacy one keeps its
            // own, since it still gets its own sort and its own buffers.
            const uint32 OwnBudget = bPaged ? ViewPagedBudget : Batch.MaxRenderPoints;
            const uint32 MaxRenderPoints = BudgetOverride > 0 ? BudgetOverride : OwnBudget;
            const uint32 RenderPointCount = bMerged
                ? MergedBudget
                : (bUseCells
                    ? FMath::Min(MaxRenderPoints, Batch.AssetPointCount)
                    : FMath::Min(MaxRenderPoints,
                                 FMath::DivideAndRoundUp(Batch.AssetPointCount, Stride)));
            if (RenderPointCount == 0)
            {
                continue;
            }

            // DispatchCount is what per-cell LOD actually selected this frame.
            // It swings with the camera, so it sizes ONLY the dispatch -- never
            // an allocation. Sizing a pooled buffer from a per-frame count is
            // what exhausted VRAM and stuttered when it was tried before.
            GaussianSplatLod::FSelection Selection;
            // Where this asset's cells start in the pool's shared bounds, and which
            // batch-table entry its splats belong to. The legacy path leaves both 0:
            // it binds its own cell bounds and is the only batch in its own draw.
            Selection.CellBase = bPaged ? static_cast<uint32>(PagedResidency->CellBoundsBase) : 0u;
            Selection.BatchId = static_cast<uint32>(BatchIndex);
            GaussianSplatProfiling::FLodStats LodStats;
            uint32 DispatchCount = RenderPointCount;
            if (bMerged)
            {
                // Already solved for the whole view, under one multiplier (D7).
                Selection = MoveTemp(MergedSelection);
                LodStats = MergedStats;
                PagedMisses = MergedMisses;
                DispatchCount = Selection.TotalCount;
            }
            else if (bUseCells)
            {
                const uint64 SelectStart = FPlatformTime::Cycles64();
                if (GaussianSplatProfiling::GetLodMode() == 1)
                {
                    GaussianSplatLod::FScreenLodInputs LodInputs;
                    // focal = P[0][0] x width / 2 (PerspectiveMatrix.h); an orthographic view keeps whole cells.
                    const bool bPerspective = View.ViewMatrices.IsPerspectiveProjection();
                    LodInputs.FocalPx = bPerspective
                        ? ProjectionMatrixNoAAD.M[0][0] * 0.5 * static_cast<double>(ViewRect.Width())
                        : 0.0;
                    LodInputs.ActorScale = static_cast<double>(Batch.LocalToWorld.GetMaximumAxisScale());
                    LodInputs.PointSize = Batch.PointSize;
                    LodInputs.SizeRef = SrcSizeRef;
                    LodInputs.SizeP99 = SrcSizeP99;
                    LodInputs.bFullCells = !bPerspective || Batch.RenderMode == EGaussianSplatRenderMode::Points;

                    // Fix 5 Step 2 (review M6): tell the settle gate this view drew
                    // splats. Only views the gate may serve are stamped -- perspective,
                    // Billboards, a paged asset -- so an orthographic or Points view,
                    // or a capture that never reaches here at all, can never inflate
                    // the required set. The owner comes from the capture component
                    // (SceneCaptureRendering.cpp:1284) and the gate holds it weakly.
                    if (!LodInputs.bFullCells && Batch.PagedAsset != nullptr
                        && FGaussianSplatPagePool::IsStreamingEnabled())
                    {
                        FGaussianSplatStreamGate::Get().StampView(
                            View.ViewActor, View.ViewMatrices.GetViewOrigin(), LodInputs.FocalPx);
                    }
                    LodInputs.ViewKey = View.GetViewKey();
                    LodInputs.BatchIndex = BatchIndex;
                    LodInputs.ViewRect = ViewRect;
                    // One group for now. The merged draw passes every paged batch of the
                    // view here instead, and the bisection then solves ONE multiplier
                    // across all of them against the shared budget (plan D7).
                    GaussianSplatLod::FScreenLodGroup LodGroup;
                    LodGroup.Cells = &SrcCells;
                    LodGroup.LocalToWorld = Batch.LocalToWorld;
                    LodGroup.PointSize = Batch.PointSize;
                    LodGroup.SizeRef = SrcSizeRef;
                    LodGroup.SizeP99 = SrcSizeP99;
                    LodGroup.ActorScale = LodInputs.ActorScale;
                    LodGroup.MaxFullDistanceM = Batch.LodMaxFullDistance;
                    LodGroup.PagedAsset = bPaged ? PagedAsset : nullptr;
                    LodGroup.PagedResidency = bPaged ? PagedResidency : nullptr;
                    LodGroup.CellBase = Selection.CellBase;
                    LodGroup.BatchId = Selection.BatchId;
                    GaussianSplatLod::SelectCellsScreen(
                        MakeArrayView(&LodGroup, 1),
                        View,
                        RenderPointCount,
                        LodInputs,
                        Selection,
                        LodStats);
                    PagedMisses = LodGroup.Missed;
                }
                else if (bPaged)
                {
                    // LOD mode 0 addresses splats by their position in the asset's
                    // own buffer, which a paged asset does not have. Mode 1 is the
                    // shipping mode; rather than emit ranges that point at the wrong
                    // slots, say so once and draw nothing this batch.
                    static bool bWarned = false;
                    if (!bWarned)
                    {
                        bWarned = true;
                        UE_LOG(
                            LogGaussianSplatProfile,
                            Warning,
                            TEXT("A paged asset needs r.GaussianSplat.LodMode 1; mode 0 cannot address pool slots. ")
                            TEXT("Nothing was drawn for this batch."));
                    }
                    continue;
                }
                else
                {
                    GaussianSplatLod::SelectCells(
                        SrcCells,
                        Batch.LocalToWorld,
                        View,
                        RenderPointCount,
                        GaussianSplatProfiling::GetViewState(View.GetViewKey()).LodBias,
                        Selection);
                }
                LodStats.SelectMicros = FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - SelectStart) * 1000.0;
                DispatchCount = Selection.TotalCount;
                if (DispatchCount == 0)
                {
                    // Every cell outside the frustum: nothing to draw at all.
                    continue;
                }
            }

            FRDGBufferRef CellRangeBuffer = CreateStructuredBuffer(
                GraphBuilder,
                TEXT("GaussianSplat.CellRanges"),
                sizeof(FUintVector4),
                FMath::Max(1, Selection.Ranges.Num()),
                Selection.Ranges.IsEmpty() ? nullptr : Selection.Ranges.GetData(),
                Selection.Ranges.Num() * sizeof(FUintVector4));
            FRDGBufferSRVRef CellRangeSRV = GraphBuilder.CreateSRV(CellRangeBuffer);

            const uint32 SortKeyBits = GaussianSplatProfiling::GetSortKeyBits();
            const int32 RequestedSortMode = GaussianSplatProfiling::GetSortMode();
            // Mode 2 needs cells. Every loaded asset has them (they are built whenever missing),
            // so the non-cell path cannot be tested and simply keeps mode 1 as before.
            const bool bUseDRS = RequestedSortMode == 2
                && Selection.CellCount > 0
                && GaussianSplatSorting::IsDeviceRadixSortSupported()
                && FMath::DivideAndRoundUp(RenderPointCount, GSRadixPartSize)
                    <= static_cast<uint32>(GRHIMaxDispatchThreadGroupsPerDimension.X);
            const bool bUseRadixSort = RequestedSortMode == 1 || (RequestedSortMode == 2 && !bUseDRS);
            const bool bSkipSort = GaussianSplatProfiling::ShouldSkipSort();

            // With cells, how many splats the cull can possibly emit is known on
            // the CPU before the frame: it is DispatchCount. So the sort simply
            // covers all of them, and the stale-visible-count prediction goes
            // away -- along with the transient band of unsorted splats it
            // produced whenever the view jumped from a near-empty screen to a
            // full one faster than the readback could follow.
            //
            // It costs the difference between selected and actually-visible,
            // about 25% more keys at street level, to remove an artefact class
            // outright. Without cells there is no exact count, so the old
            // prediction still applies.
            //
            // Mode 2 goes further and sorts the exact GPU visible count, with MaxKeys
            // below as its upper bound, so SortCount feeds modes 0 and 1 only.
            const uint32 SortCount = (Selection.CellCount > 0)
                ? DispatchCount
                : GaussianSplatProfiling::GetSortCount(RenderPointCount, View.GetViewKey());

            // Bitonic compares each element against Index ^ K, so its buffers must
            // be a power of two. Radix needs no padding at all: SortGPUBuffers
            // splits the count into whole tiles plus an ExtraKeyCount remainder,
            // and every global write in RadixSortShaders.usf is guarded by that
            // remainder, with scatter destinations from a prefix sum over exactly
            // Count. Rounding 85.8M up to 134.2M was pure bitonic tax.
            const uint32 PaddedPointCount = (bUseRadixSort || bUseDRS)
                ? RenderPointCount
                : FMath::RoundUpToPowerOfTwo(FMath::Max(1u, RenderPointCount));

            // Mode 2's key count: the LOD selection, never GetSortCount's stale prediction. On the
            // batch being validated it may be capped further for boundary tests.
            const bool bUseGpuCount = GaussianSplatProfiling::ShouldUseGpuSortCount();
            bool bValidateBatch = false;
            if (bUseDRS && !bSkipSort && GaussianSplatSortCheck::ShouldRun(View, BatchIndex, GaussianSplatProfiling::GetSortValidateEvery()))
            {
                if (GaussianSplatSortCheck::IsGpuSortDebugOn())
                {
                    GaussianSplatSortCheck::LogSkipOnce(TEXT("GPUSort.DebugSort/DebugOffsets corrupt mode 1, the SortValidate reference"));
                }
                else
                {
                    bValidateBatch = true;
                }
            }
            else if (!bUseDRS && GaussianSplatProfiling::GetSortValidateEvery() > 0)
            {
                GaussianSplatSortCheck::LogSkipOnce(TEXT("SortValidate needs effective sort mode 2 (SortSelfCheck works in any mode)"));
            }
            const bool bSelfCheckBatch = !bValidateBatch && !bSkipSort
                && GaussianSplatSortCheck::ShouldRun(View, BatchIndex, GaussianSplatProfiling::GetSortSelfCheckEvery());

            const uint32 UncappedMaxKeys = FMath::Min(DispatchCount, RenderPointCount);
            uint32 MaxKeys = UncappedMaxKeys;
            if (bValidateBatch && GaussianSplatProfiling::GetSortValidateCount() > 0)
            {
                MaxKeys = FMath::Min(MaxKeys, GaussianSplatProfiling::GetSortValidateCount());
            }
            // The GPU cap only means something when mode 2 sorts the GPU count; with the CPU count
            // it sorts every key regardless and the reference would differ by design.
            const uint32 GpuCap = (bValidateBatch && bUseGpuCount) ? GaussianSplatProfiling::GetSortValidateGpuCap() : 0u;
            // Bound on order values for the checks: they are cull thread indices, limited by what
            // the cull dispatched, not by how many keys were sorted.
            const uint32 OrderBound = FMath::Min(DispatchCount, PaddedPointCount);
            // A capped frame would feed K into the visible-count readback (the profile line and the
            // non-cell sort prediction), so it skips that readback.
            const bool bSkipVisibleReadback = bValidateBatch && (MaxKeys < UncappedMaxKeys || GpuCap > 0);

            // Both pairs are sized from the upload, never from the per-frame visible
            // count. Sizing the Alt pair to the sort was tried and reverted: the
            // visible count swings from ~11 K to ~32 M within a single camera move,
            // so every distinct value asked RDG for a differently-sized buffer, the
            // pool accumulated all of them, and VRAM ran out -- "Failed to allocate
            // Device Memory" for 24M/25M/26M/32M-element buffers, plus stutter. A
            // pooled buffer is only free if its size is stable across frames.
            //
            // The Alt pair is untouched by the bitonic path, but allocating it at 1
            // there would just make the size flip whenever SortMode changes, so it
            // tracks the primary pair unconditionally.
            const uint32 AltCount = PaddedPointCount;

            // Typed (PF_R32_UINT), not structured: the radix sort binds these as
            // Buffer<uint>/RWBuffer<uint>.
            const auto CreateSortBuffer = [&GraphBuilder](uint32 NumElements, const TCHAR* Name)
            {
                return GraphBuilder.CreateBuffer(
                    FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(1u, NumElements)), Name);
            };
            FRDGBufferRef OrderBuffer = CreateSortBuffer(PaddedPointCount, TEXT("GaussianSplat.OrderBuffer"));
            FRDGBufferRef KeyBuffer = CreateSortBuffer(PaddedPointCount, TEXT("GaussianSplat.KeyBuffer"));
            FRDGBufferRef OrderBufferAlt = CreateSortBuffer(AltCount, TEXT("GaussianSplat.OrderBufferAlt"));
            FRDGBufferRef KeyBufferAlt = CreateSortBuffer(AltCount, TEXT("GaussianSplat.KeyBufferAlt"));
            FRDGBufferDesc IndirectArgsDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 4);
            IndirectArgsDesc.Usage |= BUF_DrawIndirect | BUF_UnorderedAccess | BUF_SourceCopy;
            FRDGBufferRef IndirectArgsBuffer = GraphBuilder.CreateBuffer(
                IndirectArgsDesc,
                TEXT("GaussianSplat.IndirectArgs"));

            // Mode 2 with the GPU count reads nothing past the visible count, so the four
            // sort-buffer clears (about 1 ms at 20.9M) are skipped. SortGpuCount 0 needs their
            // sentinels, and SkipSort keeps them so its baseline stays comparable.
            const bool bClearSortBuffers = !(bUseDRS && bUseGpuCount && !bSkipSort);
            if (bClearSortBuffers)
            {
                AddClearUAVPass(
                    GraphBuilder,
                    GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OrderBuffer, PF_R32_UINT)),
                    0u);
                AddClearUAVPass(
                    GraphBuilder,
                    GraphBuilder.CreateUAV(FRDGBufferUAVDesc(KeyBuffer, PF_R32_UINT)),
                    0xffffffffu);
                AddClearUAVPass(
                    GraphBuilder,
                    GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OrderBufferAlt, PF_R32_UINT)),
                    0u);
                AddClearUAVPass(
                    GraphBuilder,
                    GraphBuilder.CreateUAV(FRDGBufferUAVDesc(KeyBufferAlt, PF_R32_UINT)),
                    0xffffffffu);
            }
            AddClearUAVPass(
                GraphBuilder,
                GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsBuffer, PF_R32_UINT)),
                0u);

            // Mode 1's radix passes round an odd count up to even, see AddRadixSortPass.
            const uint32 ModeOnePasses = FMath::DivideAndRoundUp(SortKeyBits, 4u);
            GaussianSplatSortCheck::FCheckInfo CheckInfo;
            CheckInfo.ViewKey = View.GetViewKey();
            CheckInfo.BatchIndex = BatchIndex;
            CheckInfo.SortMode = bSkipSort ? -1 : (bUseDRS ? 2 : (bUseRadixSort ? 1 : 0));
            CheckInfo.KeyBits = SortKeyBits;
            CheckInfo.Repeat = GaussianSplatProfiling::GetSortRepeat();
            CheckInfo.Passes = bUseDRS
                ? FMath::DivideAndRoundUp(SortKeyBits, 8u)
                : (ModeOnePasses + ((ModeOnePasses % 2u) != 0u && SortKeyBits < 32u ? 1u : 0u));
            const TCHAR* SortModeLabel = bSkipSort ? TEXT("skip")
                : bUseDRS ? TEXT("2")
                : bUseRadixSort ? (RequestedSortMode == 2 ? TEXT("1 (fallback from 2)") : TEXT("1"))
                : TEXT("0");

            // Modes 1 and 2, plus mode 2's same-frame validation. Returns false when neither runs,
            // leaving the bitonic network to each branch below. Scoped as GaussianSplat/Sort.
            const auto AddRadixSorts = [&](FRDGBufferRef& OutOrder, FRDGBufferRef& OutKeys) -> bool
            {
                if (bSkipSort || !(bUseRadixSort || bUseDRS))
                {
                    return false;
                }
                RDG_GPU_STAT_SCOPE(GraphBuilder, GaussianSplatSort);
                RDG_EVENT_SCOPE(GraphBuilder, "GaussianSplat.Sort (mode %d)", bUseDRS ? 2 : 1);
                if (bUseDRS)
                {
                    GaussianSplatSortCheck::FValidationInputs Validation;
                    if (bValidateBatch)
                    {
                        Validation = GaussianSplatSortCheck::AddPrepare(
                            GraphBuilder, KeyBuffer, OrderBuffer, IndirectArgsBuffer, MaxKeys, GpuCap);
                    }
                    const GaussianSplatSorting::FDeviceRadixSortResult Result = GaussianSplatSorting::AddDeviceRadixSortPasses(
                        GraphBuilder,
                        KeyBuffer,
                        OrderBuffer,
                        KeyBufferAlt,
                        OrderBufferAlt,
                        IndirectArgsBuffer,
                        MaxKeys,
                        bUseGpuCount,
                        GpuCap,
                        SortKeyBits,
                        CheckInfo.Repeat,
                        GaussianSplatProfiling::ShouldUseRadixSafeBarriers());
                    OutOrder = Result.Values;
                    OutKeys = Result.Keys;
                    if (Validation.IsValid())
                    {
                        const bool bResultInPrimary = Result.Values == OrderBuffer;
                        GaussianSplatSortCheck::AddCompare(
                            GraphBuilder,
                            View.GetFeatureLevel(),
                            Validation,
                            Result.Keys,
                            Result.Values,
                            bResultInPrimary ? KeyBufferAlt : KeyBuffer,
                            bResultInPrimary ? OrderBufferAlt : OrderBuffer,
                            IndirectArgsBuffer,
                            MaxKeys,
                            GpuCap,
                            OrderBound,
                            /*bTailCheck=*/ !bUseGpuCount,
                            CheckInfo);
                    }
                }
                else
                {
                    for (int32 RepeatIndex = 0; RepeatIndex < CheckInfo.Repeat; ++RepeatIndex)
                    {
                        OutOrder = GaussianSplatSorting::AddRadixSortPass(
                            GraphBuilder,
                            View.GetFeatureLevel(),
                            OrderBuffer,
                            OrderBufferAlt,
                            KeyBuffer,
                            KeyBufferAlt,
                            SortCount);
                    }
                    // Mode 1's pass count is even, so its result is back in the primary pair.
                    OutKeys = (OutOrder == OrderBuffer) ? KeyBuffer : KeyBufferAlt;
                }
                return true;
            };

            // Reference-free check of whatever sort ran, on the buffers the rasterizer binds.
            const auto AddSortSelfCheck = [&](FRDGBufferRef SortedOrder, FRDGBufferRef SortedKeys)
            {
                if (bSelfCheckBatch)
                {
                    GaussianSplatSortCheck::AddSelfCheck(
                        GraphBuilder, SortedKeys, SortedOrder, IndirectArgsBuffer, RenderPointCount, OrderBound, CheckInfo);
                }
            };

            // Zero by construction in translated world (PreViewTranslation = -ViewOrigin).
            // The cull's Depth = dot(CenterWS - Origin, Forward) keeps its form, and the
            // SH view direction is then just the splat centre, normalised.
            const FVector3f ViewOrigin = FVector3f::ZeroVector;
            const FVector3f Forward = static_cast<FVector3f>(View.GetViewDirection());
            GaussianSplatProfiling::FOccReadback OccReadback;

            if (Batch.RenderMode == EGaussianSplatRenderMode::Points)
            {
                FGaussianSplatPointsCullCS::FParameters* InitSortParameters = GraphBuilder.AllocParameters<FGaussianSplatPointsCullCS::FParameters>();
                InitSortParameters->NumElements = DispatchCount;
                InitSortParameters->PaddedNumElements = PaddedPointCount;
                InitSortParameters->Stride = Stride;
                InitSortParameters->SplatCellRanges = CellRangeSRV;
                InitSortParameters->SplatCellCount = Selection.CellCount;
                InitSortParameters->SortK = 0;
                InitSortParameters->SortJ = 0;
                InitSortParameters->PassType = 0;
                InitSortParameters->ViewWorldOrigin = FVector4f(ViewOrigin.X, ViewOrigin.Y, ViewOrigin.Z, 0.0f);
                InitSortParameters->ViewForward = FVector4f(Forward.X, Forward.Y, Forward.Z, 0.0f);
                InitSortParameters->ViewRectMin = FVector2f(static_cast<float>(ViewRect.Min.X), static_cast<float>(ViewRect.Min.Y));
                InitSortParameters->ViewSize = FVector2f(static_cast<float>(ViewRect.Width()), static_cast<float>(ViewRect.Height()));
                InitSortParameters->PointSize = Batch.PointSize;
                InitSortParameters->ViewProjectionMatrix = ViewProjection;
                InitSortParameters->LocalToWorldMatrix = LocalToTranslatedWorld;
                InitSortParameters->SplatPackedA = SrcPackedA;
                InitSortParameters->SplatPackedB = SrcPackedB;
                InitSortParameters->SplatCellBounds = SrcCellBounds;
                InitSortParameters->SortKeyShift = 32u - SortKeyBits;
                InitSortParameters->SortKeyMode = GaussianSplatProfiling::GetSortKeyMode();
                InitSortParameters->SplatOrderBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OrderBuffer, PF_R32_UINT));
                InitSortParameters->SplatKeyBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(KeyBuffer, PF_R32_UINT));
                InitSortParameters->SplatIndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsBuffer, PF_R32_UINT));

                {
                    RDG_GPU_STAT_SCOPE(GraphBuilder, GaussianSplatCull);
                    FComputeShaderUtils::AddPass(
                        GraphBuilder,
                        RDG_EVENT_NAME("GaussianSplatPointsSort.Init"),
                        PointsCullCS,
                        InitSortParameters,
                        FComputeShaderUtils::GetGroupCount(DispatchCount, 64));
                }

                FRDGBufferRef SortedOrderBuffer = OrderBuffer;
                FRDGBufferRef SortedKeyBuffer = KeyBuffer;
                const bool bRadixSorted = AddRadixSorts(SortedOrderBuffer, SortedKeyBuffer);
                if (!bRadixSorted && !bSkipSort)
                {
                    for (uint32 K = 2; K <= PaddedPointCount; K <<= 1)
                    {
                        for (uint32 J = K >> 1; J > 0; J >>= 1)
                        {
                            FGaussianSplatPointsCullCS::FParameters* SortParameters = GraphBuilder.AllocParameters<FGaussianSplatPointsCullCS::FParameters>();
                            SortParameters->NumElements = RenderPointCount;
                            SortParameters->PaddedNumElements = PaddedPointCount;
                            SortParameters->Stride = Stride;
                            SortParameters->SplatCellRanges = CellRangeSRV;
                            SortParameters->SplatCellCount = Selection.CellCount;
                            SortParameters->SortK = K;
                            SortParameters->SortJ = J;
                            SortParameters->PassType = 1;
                            SortParameters->ViewWorldOrigin = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
                            SortParameters->ViewForward = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
                            SortParameters->ViewRectMin = FVector2f::ZeroVector;
                            SortParameters->ViewSize = FVector2f::ZeroVector;
                            SortParameters->PointSize = 0.0f;
                            SortParameters->ViewProjectionMatrix = FMatrix44f::Identity;
                            SortParameters->LocalToWorldMatrix = LocalToTranslatedWorld;
                            SortParameters->SplatPackedA = SrcPackedA;
                            SortParameters->SplatPackedB = SrcPackedB;
                            SortParameters->SplatCellBounds = SrcCellBounds;
                            SortParameters->SplatOrderBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OrderBuffer, PF_R32_UINT));
                            SortParameters->SplatKeyBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(KeyBuffer, PF_R32_UINT));
                            SortParameters->SplatIndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsBuffer, PF_R32_UINT));

                            FComputeShaderUtils::AddPass(
                                GraphBuilder,
                                RDG_EVENT_NAME("GaussianSplatPointsSort.Bitonic K=%u J=%u", K, J),
                                PointsCullCS,
                                SortParameters,
                                FComputeShaderUtils::GetGroupCount(PaddedPointCount, 64));
                        }
                    }
                }

                AddSortSelfCheck(SortedOrderBuffer, SortedKeyBuffer);

                FGaussianSplatPointsRasterPassParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianSplatPointsRasterPassParameters>();
                FGaussianSplatPointsRasterVS::FParameters* RasterParameters = &PassParameters->VS;
                RasterParameters->ViewRectMin = FVector2f(static_cast<float>(ViewRect.Min.X), static_cast<float>(ViewRect.Min.Y));
                RasterParameters->ViewSize = FVector2f(static_cast<float>(ViewRect.Width()), static_cast<float>(ViewRect.Height()));
                RasterParameters->PointSize = Batch.PointSize;
                RasterParameters->OpacityScale = Batch.OpacityScale;
                RasterParameters->Stride = Stride;
                RasterParameters->SplatCellRanges = CellRangeSRV;
                RasterParameters->SplatCellCount = Selection.CellCount;
                RasterParameters->LocalToWorldMatrix = LocalToTranslatedWorld;
                RasterParameters->ViewProjectionMatrix = ViewProjection;
                RasterParameters->SplatOrderBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SortedOrderBuffer, PF_R32_UINT));
                RasterParameters->SplatPackedA = SrcPackedA;
                RasterParameters->SplatPackedB = SrcPackedB;
                RasterParameters->SplatCellBounds = SrcCellBounds;
                RasterParameters->SplatColorEncoding = SrcColorEncoding;
                SetDepthTestParameters(PassParameters->PS.DepthTest);
                PassParameters->IndirectArgsBuffer = IndirectArgsBuffer;
                PassParameters->RenderTargets[0] = FRenderTargetBinding(
                    SplatOutput.Texture,
                    bFirstBatch ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad);

                // Lasts to the end of this branch, which is this pass alone.
                RDG_GPU_STAT_SCOPE(GraphBuilder, GaussianSplatRaster);
                GraphBuilder.AddPass(
                    RDG_EVENT_NAME("GaussianSplatPointsRaster.DrawInstanced"),
                    PassParameters,
                    ERDGPassFlags::Raster,
                    [PassParameters, PointsRasterVS, PointsRasterPS, ViewRect, IndirectArgsBuffer](FRHICommandList& RHICmdList)
                    {
                        RHICmdList.SetViewport(
                            static_cast<float>(ViewRect.Min.X),
                            static_cast<float>(ViewRect.Min.Y),
                            0.0f,
                            static_cast<float>(ViewRect.Max.X),
                            static_cast<float>(ViewRect.Max.Y),
                            1.0f);

                        FGraphicsPipelineStateInitializer GraphicsPSOInit;
                        RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
                        GraphicsPSOInit.BlendState = TStaticBlendState<
                            CW_RGBA,
                            BO_Add, BF_InverseDestAlpha, BF_One,
                            BO_Add, BF_InverseDestAlpha, BF_One>::GetRHI();
                        GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
                        GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
                        GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;
                        GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
                        GraphicsPSOInit.BoundShaderState.VertexShaderRHI = PointsRasterVS.GetVertexShader();
                        GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PointsRasterPS.GetPixelShader();

                        SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
                        SetShaderParameters(RHICmdList, PointsRasterVS, PointsRasterVS.GetVertexShader(), PassParameters->VS);
                        SetShaderParameters(RHICmdList, PointsRasterPS, PointsRasterPS.GetPixelShader(), PassParameters->PS);
                        RHICmdList.DrawPrimitiveIndirect(IndirectArgsBuffer->GetIndirectRHICallBuffer(), 0);
                    });
            }
            else
            {
                // Hidden-splat cull. Tiles are the smallest power of two >= 8 px with at most 256 per axis, so a
                // box packs into four bytes (8 px up to 2048 px wide, 16 px up to 4096); bigger tiles only cull less.
                const bool bOccBoxWanted = GaussianSplatProfiling::ShouldUseOccBoxPath();
                const int32 OccPhases = (GaussianSplatProfiling::GetOccPhases() > 0 && HasOcclusionShaders(bOccBoxWanted))
                    ? GaussianSplatProfiling::GetOccPhases()
                    : 0;
                const uint32 ViewWidth = static_cast<uint32>(FMath::Max(0, ViewRect.Width()));
                const uint32 ViewHeight = static_cast<uint32>(FMath::Max(0, ViewRect.Height()));
                uint32 OccTileShift = 3;
                while (OccTileShift < 8
                    && (FMath::DivideAndRoundUp(ViewWidth, 1u << OccTileShift) > 256u
                        || FMath::DivideAndRoundUp(ViewHeight, 1u << OccTileShift) > 256u))
                {
                    ++OccTileShift;
                }
                const uint32 OccTilesX = FMath::DivideAndRoundUp(ViewWidth, 1u << OccTileShift);
                const uint32 OccTilesY = FMath::DivideAndRoundUp(ViewHeight, 1u << OccTileShift);
                // The per-splat passes run one 256-thread group per 256 slots, as one-dimensional dispatches. Mode 0 can
                // select more than the budget (one splat per visible cell, more cells than budget); its cull already
                // overruns the sort buffers then, and the cull's budget-sized buffers would too, so it stays off.
                // Fix 4 Step 3 lever 2: a view that sees few splats skips the cull (r.GaussianSplat.OccMinVisible).
                const bool bOccSkipped = OccPhases > 0 && GaussianSplatProfiling::ShouldSkipOccForView(View.GetViewKey());
                OccReadback.bSkipped = bOccSkipped;
                const bool bOcc = OccPhases > 0
                    && !bOccSkipped
                    && OccTilesX > 0 && OccTilesY > 0 && OccTilesX <= 256u && OccTilesY <= 256u
                    && DispatchCount <= PaddedPointCount
                    && FMath::DivideAndRoundUp(PaddedPointCount, 256u)
                        <= static_cast<uint32>(GRHIMaxDispatchThreadGroupsPerDimension.X);
                const bool bOccBox = bOcc && bOccBoxWanted;
                const float OccBoxPad = GaussianSplatProfiling::GetOccBoxPad();
                FRDGBufferRef OccBoxBuffer = nullptr;
                if (bOccBox)
                {
                    // Written by the cull, before the sort, so it cannot live in the sort's pairs. Budget-sized and
                    // pooled like them: 4 B per budget splat per view and batch, 84 MB at Uno's 20.92M.
                    OccBoxBuffer = CreateSortBuffer(PaddedPointCount, TEXT("GaussianSplat.OccBox"));
                    GraphBuilder.ConvertToExternalBuffer(OccBoxBuffer);
                }

                FGaussianSplatBillboardsCullCS::FParameters* InitSortParameters = GraphBuilder.AllocParameters<FGaussianSplatBillboardsCullCS::FParameters>();
                InitSortParameters->NumElements = DispatchCount;
                InitSortParameters->PaddedNumElements = PaddedPointCount;
                InitSortParameters->Stride = Stride;
                InitSortParameters->SplatCellRanges = CellRangeSRV;
                InitSortParameters->SplatCellCount = Selection.CellCount;
                InitSortParameters->SortK = 0;
                InitSortParameters->SortJ = 0;
                InitSortParameters->PassType = 0;
                InitSortParameters->ViewWorldOrigin = FVector4f(ViewOrigin.X, ViewOrigin.Y, ViewOrigin.Z, 0.0f);
                InitSortParameters->ViewForward = FVector4f(Forward.X, Forward.Y, Forward.Z, 0.0f);
                InitSortParameters->ViewRectMin = FVector2f(static_cast<float>(ViewRect.Min.X), static_cast<float>(ViewRect.Min.Y));
                InitSortParameters->ViewSize = FVector2f(static_cast<float>(ViewRect.Width()), static_cast<float>(ViewRect.Height()));
                InitSortParameters->PointSize = Batch.PointSize;
                InitSortParameters->ViewMatrix = ViewMatrix;
                InitSortParameters->ProjectionMatrix = ProjectionMatrix;
                InitSortParameters->ViewProjectionMatrix = ViewProjection;
                InitSortParameters->LocalToWorldMatrix = LocalToTranslatedWorld;
                InitSortParameters->SplatPackedA = SrcPackedA;
                InitSortParameters->SplatPackedB = SrcPackedB;
                InitSortParameters->SplatCellBounds = SrcCellBounds;
                InitSortParameters->SplatBatches = BatchTableSRV;
                InitSortParameters->SplatColorEncoding = SrcColorEncoding;
                InitSortParameters->OpacityScale = Batch.OpacityScale;
                InitSortParameters->MinSplatOpacity = GaussianSplatProfiling::GetMinSplatOpacity();
                InitSortParameters->MaxSplatDistance = GaussianSplatProfiling::GetMaxSplatDistance();
                InitSortParameters->MinScreenVariance = GaussianSplatProfiling::GetMinScreenVariance();
                InitSortParameters->SortKeyShift = 32u - SortKeyBits;
                InitSortParameters->SortKeyMode = GaussianSplatProfiling::GetSortKeyMode();
                InitSortParameters->SplatOrderBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OrderBuffer, PF_R32_UINT));
                InitSortParameters->SplatKeyBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(KeyBuffer, PF_R32_UINT));
                InitSortParameters->SplatIndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsBuffer, PF_R32_UINT));
                InitSortParameters->OccTileShift = OccTileShift;
                InitSortParameters->OccBoxPad = OccBoxPad;
                InitSortParameters->OccBoxUAV = bOccBox
                    ? GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OccBoxBuffer, PF_R32_UINT))
                    : nullptr;

                // Permutation 0 (no box) is the plain cull.
                FGaussianSplatBillboardsCullCS::FPermutationDomain CullPermutation;
                CullPermutation.Set<FGaussianSplatBillboardsCullCS::FOccBoxDim>(bOccBox);
                TShaderMapRef<FGaussianSplatBillboardsCullCS> InitCullCS(GetGlobalShaderMap(GMaxRHIFeatureLevel), CullPermutation);

                {
                    RDG_GPU_STAT_SCOPE(GraphBuilder, GaussianSplatCull);
                    FComputeShaderUtils::AddPass(
                        GraphBuilder,
                        RDG_EVENT_NAME("GaussianSplatBillboardsSort.Init"),
                        InitCullCS,
                        InitSortParameters,
                        FComputeShaderUtils::GetGroupCount(DispatchCount, 64));
                }

                FRDGBufferRef SortedOrderBuffer = OrderBuffer;
                FRDGBufferRef SortedKeyBuffer = KeyBuffer;
                const bool bRadixSorted = AddRadixSorts(SortedOrderBuffer, SortedKeyBuffer);
                if (!bRadixSorted && !bSkipSort)
                {
                    for (uint32 K = 2; K <= PaddedPointCount; K <<= 1)
                    {
                        for (uint32 J = K >> 1; J > 0; J >>= 1)
                        {
                            FGaussianSplatBillboardsCullCS::FParameters* SortParameters = GraphBuilder.AllocParameters<FGaussianSplatBillboardsCullCS::FParameters>();
                            SortParameters->NumElements = RenderPointCount;
                            SortParameters->PaddedNumElements = PaddedPointCount;
                            SortParameters->Stride = Stride;
                            SortParameters->SplatCellRanges = CellRangeSRV;
                            SortParameters->SplatCellCount = Selection.CellCount;
                            SortParameters->SortK = K;
                            SortParameters->SortJ = J;
                            SortParameters->PassType = 1;
                            SortParameters->ViewWorldOrigin = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
                            SortParameters->ViewForward = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
                            SortParameters->ViewRectMin = FVector2f::ZeroVector;
                            SortParameters->ViewSize = FVector2f::ZeroVector;
                            SortParameters->PointSize = 0.0f;
                            SortParameters->ViewMatrix = FMatrix44f::Identity;
                            SortParameters->ProjectionMatrix = FMatrix44f::Identity;
                            SortParameters->ViewProjectionMatrix = FMatrix44f::Identity;
                            SortParameters->LocalToWorldMatrix = LocalToTranslatedWorld;
                            SortParameters->SplatPackedA = SrcPackedA;
                            SortParameters->SplatPackedB = SrcPackedB;
                            SortParameters->SplatCellBounds = SrcCellBounds;
                            SortParameters->SplatBatches = BatchTableSRV;
                            SortParameters->SplatOrderBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OrderBuffer, PF_R32_UINT));
                            SortParameters->SplatKeyBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(KeyBuffer, PF_R32_UINT));
                            SortParameters->SplatIndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsBuffer, PF_R32_UINT));

                            FComputeShaderUtils::AddPass(
                                GraphBuilder,
                                RDG_EVENT_NAME("GaussianSplatBillboardsSort.Bitonic K=%u J=%u", K, J),
                                BillboardsCullCS,
                                SortParameters,
                                FComputeShaderUtils::GetGroupCount(PaddedPointCount, 64));
                        }
                    }
                }

                AddSortSelfCheck(SortedOrderBuffer, SortedKeyBuffer);

                // One billboard draw of DrawArgs' instances at DrawArgsOffset, reading splats through DrawOrder.
                // Without the hidden-splat cull this is the single draw (Phase -1 keeps its event name).
                const auto AddBillboardsRaster = [&](
                    FRDGBufferRef DrawOrder,
                    FRDGBufferRef DrawArgs,
                    uint32 DrawArgsOffset,
                    ERenderTargetLoadAction LoadAction,
                    int32 Phase)
                {
                    FGaussianSplatBillboardsRasterPassParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianSplatBillboardsRasterPassParameters>();
                    FGaussianSplatBillboardsRasterVS::FParameters* RasterParameters = &PassParameters->VS;
                    RasterParameters->ViewRectMin = FVector2f(static_cast<float>(ViewRect.Min.X), static_cast<float>(ViewRect.Min.Y));
                    RasterParameters->ViewSize = FVector2f(static_cast<float>(ViewRect.Width()), static_cast<float>(ViewRect.Height()));
                    RasterParameters->ViewWorldOrigin = FVector4f(ViewOrigin.X, ViewOrigin.Y, ViewOrigin.Z, 0.0f);
                    RasterParameters->PointSize = Batch.PointSize;
                    RasterParameters->OpacityScale = Batch.OpacityScale;
                    RasterParameters->Stride = Stride;
                    RasterParameters->SplatCellRanges = CellRangeSRV;
                    RasterParameters->SplatCellCount = Selection.CellCount;
                    RasterParameters->ViewMatrix = ViewMatrix;
                    RasterParameters->LocalToWorldMatrix = LocalToTranslatedWorld;
                    RasterParameters->ProjectionMatrix = ProjectionMatrix;
                    RasterParameters->ViewProjectionMatrix = ViewProjection;
                    RasterParameters->WorldToLocalRow0 = Batch.WorldToLocalRow0;
                    RasterParameters->WorldToLocalRow1 = Batch.WorldToLocalRow1;
                    RasterParameters->WorldToLocalRow2 = Batch.WorldToLocalRow2;
                    RasterParameters->SplatOrderBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DrawOrder, PF_R32_UINT));
                    RasterParameters->SplatPackedA = SrcPackedA;
                    RasterParameters->SplatPackedB = SrcPackedB;
                    RasterParameters->SplatCellBounds = SrcCellBounds;
                    RasterParameters->SplatColorEncoding = SrcColorEncoding;
                    RasterParameters->PerPixelDepth = GaussianSplatProfiling::ShouldUsePerPixelDepth() ? 1u : 0u;
                    RasterParameters->HasSH = bSrcHasSH ? 1u : 0u;
                    RasterParameters->MinScreenVariance = GaussianSplatProfiling::GetMinScreenVariance();
                    RasterParameters->SplatSHIndexBuffer = SrcSHIndex;
                    // NEVER BIND A NULL PALETTE. This used to copy the representative batch's
                    // SRV into all eight slots, which was fine while a draw covered one batch.
                    // The merged path broke that: the representative is MergedBatches[0], and an
                    // SH0 asset has no palette slot, so GetSHPaletteSRV(INDEX_NONE) returned
                    // nullptr and EVERY palette parameter of a draw containing SH3 batches was
                    // null. In a Shipping bindless build a null SRV is simply never written into
                    // the shader's packed globals (RHIShaderParametersShared.h), so the handle is
                    // uninitialised heap and each SH3 splat dereferenced it 45 times -- an
                    // undefined device read, which hung the GPU with Xid 109 (2026-10-09).
                    //
                    // It looked like an eight-asset threshold and was not: it depended on whether
                    // the FIRST-registered component happened to be SH0, which is actor order.
                    // 16 identical SH0 assets were immune because no batch ever read the handle.
                    //
                    // The pool's accessor fills every slot and substitutes the one-float dummy for
                    // slots no asset owns, which is what the legacy path has always done
                    // (GaussianSplatRenderResources.cpp:313-319). Whether this draw takes the
                    // cheap single-palette permutation or the multi-palette one is decided per
                    // draw from the SELECTED cells (bMergedMultiPalette, 36f230b), not here.
                    FRHIShaderResourceView* Palettes[FGaussianSplatPagePool::MaxSHPalettes] = {};
                    if (bPaged)
                    {
                        FGaussianSplatPagePool::Get().GetSHPaletteSRVs(Palettes);
                    }
                    // SLOT 0 MUST BE THIS DRAW'S PALETTE, not the pool's slot 0. The cheap permutation
                    // reads SplatSHPalette0 unconditionally, so filling purely from the pool made every
                    // single-batch paged draw whose asset sits in pool slot >= 1 read slot 0's palette --
                    // a regression I introduced in the null-palette fix, latent only because every map
                    // today holds copies of ONE capture whose palettes are byte-identical. The code this
                    // replaced bound SrcSHPalette everywhere and was right about this and wrong about null.
                    //
                    // The MERGED draw's batches span several assets, so there is no single "own" palette.
                    // When the selected cells name more than one palette the multi-palette permutation is
                    // taken and every batch reads its own slot (36f230b); when they name exactly one, the
                    // cheap permutation is bound to THAT slot (c01364d), never blindly to slot 0.
                    const bool bDrawMultiPalette = bMerged && bMergedMultiPalette;
                    if (!bDrawMultiPalette)
                    {
                        // The cheap permutation reads slot 0 only, so slot 0 must be the ONE palette
                        // this draw reads: its own batch's when unmerged, and when merged the single
                        // slot its selected cells named. Left alone when the switch is compiled,
                        // because then every slot is addressed by Batch.PaletteSlot.
                        FRHIShaderResourceView* const DrawPalette0 = bMerged
                            ? FGaussianSplatPagePool::Get().GetSHPaletteSRV(MergedOnlyPalette)
                            : SrcSHPalette;
                        if (DrawPalette0 != nullptr)
                        {
                            Palettes[0] = DrawPalette0;
                        }
                    }
                    for (int32 Slot = 0; Slot < FGaussianSplatPagePool::MaxSHPalettes; ++Slot)
                    {
                        if (Palettes[Slot] == nullptr)
                        {
                            Palettes[Slot] = SrcSHPalette;      // the legacy path's own dummy
                        }
                    }
                    RasterParameters->SplatSHPalette0 = Palettes[0];
                    RasterParameters->SplatSHPalette1 = Palettes[1];
                    RasterParameters->SplatSHPalette2 = Palettes[2];
                    RasterParameters->SplatSHPalette3 = Palettes[3];
                    RasterParameters->SplatSHPalette4 = Palettes[4];
                    RasterParameters->SplatSHPalette5 = Palettes[5];
                    RasterParameters->SplatSHPalette6 = Palettes[6];
                    RasterParameters->SplatSHPalette7 = Palettes[7];
                    static_assert(FGaussianSplatPagePool::MaxSHPalettes == 8, "bind every palette slot");
                    RasterParameters->SplatBatches = BatchTableSRV;
                    PassParameters->PS.AlphaCutoff = GaussianSplatProfiling::GetAlphaCutoff();
                    SetDepthTestParameters(PassParameters->PS.DepthTest);
                    PassParameters->IndirectArgsBuffer = DrawArgs;
                    PassParameters->RenderTargets[0] = FRenderTargetBinding(SplatOutput.Texture, LoadAction);

                    // Lasts to the end of this lambda, which is this pass alone.
                    RDG_GPU_STAT_SCOPE(GraphBuilder, GaussianSplatRaster);
                    GraphBuilder.AddPass(
                        Phase < 0
                            ? RDG_EVENT_NAME("GaussianSplatBillboardsRaster.DrawInstanced")
                            : RDG_EVENT_NAME("GaussianSplatBillboardsRaster.DrawInstanced (occ phase %d)", Phase),
                        PassParameters,
                        ERDGPassFlags::Raster,
                        [PassParameters, DrawVS = bDrawMultiPalette ? BillboardsRasterVSMulti : BillboardsRasterVS,
                         BillboardsRasterPS, ViewRect, DrawArgs, DrawArgsOffset](FRHICommandList& RHICmdList)
                        {
                            RHICmdList.SetViewport(
                                static_cast<float>(ViewRect.Min.X),
                                static_cast<float>(ViewRect.Min.Y),
                                0.0f,
                                static_cast<float>(ViewRect.Max.X),
                                static_cast<float>(ViewRect.Max.Y),
                                1.0f);

                            FGraphicsPipelineStateInitializer GraphicsPSOInit;
                            RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
                            GraphicsPSOInit.BlendState = TStaticBlendState<
                                CW_RGBA,
                                BO_Add, BF_InverseDestAlpha, BF_One,
                                BO_Add, BF_InverseDestAlpha, BF_One>::GetRHI();
                            GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
                            GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
                            GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;
                            GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
                            GraphicsPSOInit.BoundShaderState.VertexShaderRHI = DrawVS.GetVertexShader();
                            GraphicsPSOInit.BoundShaderState.PixelShaderRHI = BillboardsRasterPS.GetPixelShader();

                            SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
                            SetShaderParameters(RHICmdList, DrawVS, DrawVS.GetVertexShader(), PassParameters->VS);
                            SetShaderParameters(RHICmdList, BillboardsRasterPS, BillboardsRasterPS.GetPixelShader(), PassParameters->PS);
                            RHICmdList.DrawPrimitiveIndirect(DrawArgs->GetIndirectRHICallBuffer(), DrawArgsOffset);
                        });
                };

                const ERenderTargetLoadAction FirstLoad = bFirstBatch ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad;
                if (!bOcc)
                {
                    AddBillboardsRaster(SortedOrderBuffer, IndirectArgsBuffer, 0, FirstLoad, -1);
                }
                else
                {
                    RDG_EVENT_SCOPE(GraphBuilder, "GaussianSplat.Occlusion (%d phases, %s path)",
                        OccPhases, bOccBox ? TEXT("box") : TEXT("recompute"));
                    FGlobalShaderMap* OccShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
                    TShaderMapRef<FGaussianSplatOccArgsCS> OccArgsCS(OccShaderMap);
                    TShaderMapRef<FGaussianSplatOccReduceCS> OccReduceCS(OccShaderMap);
                    TShaderMapRef<FGaussianSplatOccSatCS> OccSatCS(OccShaderMap);
                    TShaderMapRef<FGaussianSplatOccTestCS> OccTestCS(OccShaderMap);
                    FGaussianSplatOccFlagCS::FPermutationDomain FlagPermutation;
                    FlagPermutation.Set<FGaussianSplatOccFlagCS::FRecomputeDim>(!bOccBox);
                    TShaderMapRef<FGaussianSplatOccFlagCS> OccFlagCS(OccShaderMap, FlagPermutation);
                    TShaderMapRef<FGaussianSplatOccScanCS> OccScanCS(OccShaderMap);
                    TShaderMapRef<FGaussianSplatOccScatterCS> OccScatterCS(OccShaderMap);

                    // Phase draw arguments in their own buffer: IndirectArgs[1] stays the visible count, so the
                    // profile line, the LOD and the sort checks are untouched. Every other buffer is
                    // transient and sized from the budget or the view, never from a per-frame count.
                    FRDGBufferDesc OccDrawArgsDesc = FRDGBufferDesc::CreateIndirectDesc(GaussianSplatProfiling::OccDrawArgsCount);
                    OccDrawArgsDesc.Usage |= BUF_SourceCopy;
                    FRDGBufferRef OccDrawArgs = GraphBuilder.CreateBuffer(OccDrawArgsDesc, TEXT("GaussianSplat.OccDrawArgs"));
                    FRDGBufferRef OccDispatchArgs = GraphBuilder.CreateBuffer(
                        FRDGBufferDesc::CreateIndirectDesc((GaussianSplatProfiling::OccMaxPhases - 1) * 4),
                        TEXT("GaussianSplat.OccDispatchArgs"));
                    const uint32 OccGroups = FMath::DivideAndRoundUp(PaddedPointCount, 256u);
                    FRDGBufferRef TileUnsat = CreateSortBuffer(OccTilesX * OccTilesY, TEXT("GaussianSplat.OccTileUnsat"));
                    FRDGBufferRef TileSat = CreateSortBuffer((OccTilesX + 1) * (OccTilesY + 1), TEXT("GaussianSplat.OccTileSat"));
                    FRDGBufferRef CullBits = bOccBox ? CreateSortBuffer(OccGroups * 8, TEXT("GaussianSplat.OccCullBits")) : nullptr;
                    FRDGBufferRef FlagWords = CreateSortBuffer(OccGroups * 8, TEXT("GaussianSplat.OccFlagWords"));
                    FRDGBufferRef GroupCounts = CreateSortBuffer(OccGroups, TEXT("GaussianSplat.OccGroupCounts"));
                    FRDGBufferRef GroupOffsets = CreateSortBuffer(OccGroups, TEXT("GaussianSplat.OccGroupOffsets"));

                    // Where each later phase's compacted order goes. The sort's result keys are dead once
                    // the sort is done, since the raster reads only the order; but SortValidate and SortSelfCheck read
                    // them, mode 1 moves its pair with raw RHI barriers RDG does not track, and SkipSort has no result.
                    // So only mode 2 on a batch where neither check runs writes into them; every other case uses a
                    // transient buffer of the budget's size, that frame only.
                    const bool bCompactIntoKeys = bUseDRS && !bSkipSort && !bValidateBatch && !bSelfCheckBatch;
                    FRDGBufferRef Compacted = bCompactIntoKeys
                        ? SortedKeyBuffer
                        : CreateSortBuffer(PaddedPointCount, TEXT("GaussianSplat.OccCompacted"));
                    check(Compacted != SortedOrderBuffer);

                    const FUintVector4 OccSplit = OccPhases == 4 ? FUintVector4(2, 5, 10, 20) : FUintVector4(5, 20, 20, 20);
                    const FRDGBufferSRVRef SortCountSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(IndirectArgsBuffer, PF_R32_UINT));
                    const FRDGBufferSRVRef SortedOrderSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SortedOrderBuffer, PF_R32_UINT));
                    const auto AllocOcc = [&](uint32 Phase)
                    {
                        FGaussianSplatOcclusionParameters* Parameters = GraphBuilder.AllocParameters<FGaussianSplatOcclusionParameters>();
                        Parameters->OccPhaseIndex = Phase;
                        Parameters->OccPhaseCount = static_cast<uint32>(OccPhases);
                        Parameters->OccPhaseSplit = OccSplit;
                        Parameters->OccViewRectMin = ViewRect.Min;
                        Parameters->OccViewSize = ViewRect.Size();
                        Parameters->OccTileShift = OccTileShift;
                        Parameters->OccTilesX = OccTilesX;
                        Parameters->OccTilesY = OccTilesY;
                        Parameters->OccNumElements = DispatchCount;
                        Parameters->OccBoxPad = OccBoxPad;
                        return Parameters;
                    };

                    {
                        RDG_GPU_STAT_SCOPE(GraphBuilder, GaussianSplatOcc);
                        FGaussianSplatOcclusionParameters* ArgsParameters = AllocOcc(0);
                        ArgsParameters->OccSortCount = SortCountSRV;
                        ArgsParameters->OccDrawArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OccDrawArgs, PF_R32_UINT));
                        ArgsParameters->OccDispatchArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OccDispatchArgs, PF_R32_UINT));
                        FComputeShaderUtils::AddPass(
                            GraphBuilder, RDG_EVENT_NAME("GaussianSplatOcc.Args"), OccArgsCS, ArgsParameters, FIntVector(1, 1, 1));
                    }
                    AddBillboardsRaster(SortedOrderBuffer, OccDrawArgs, 0, FirstLoad, 0);

                    for (uint32 Phase = 1; Phase < static_cast<uint32>(OccPhases); ++Phase)
                    {
                        {
                            RDG_GPU_STAT_SCOPE(GraphBuilder, GaussianSplatOcc);

                            // The target's SRV flag lets the reduce read it between two raster passes; RDG adds the
                            // transitions and does not merge the raster passes across it.
                            FGaussianSplatOcclusionParameters* ReduceParameters = AllocOcc(Phase);
                            ReduceParameters->OccSplatTexture = SplatTexture;
                            ReduceParameters->OccTileUnsatUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TileUnsat, PF_R32_UINT));
                            FComputeShaderUtils::AddPass(
                                GraphBuilder, RDG_EVENT_NAME("GaussianSplatOcc.Reduce (%ux%u tiles of %u px)", OccTilesX, OccTilesY, 1u << OccTileShift),
                                OccReduceCS, ReduceParameters, FIntVector(static_cast<int32>(OccTilesX), static_cast<int32>(OccTilesY), 1));

                            FGaussianSplatOcclusionParameters* SatParameters = AllocOcc(Phase);
                            SatParameters->OccTileUnsat = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TileUnsat, PF_R32_UINT));
                            SatParameters->OccTileSatUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TileSat, PF_R32_UINT));
                            FComputeShaderUtils::AddPass(
                                GraphBuilder, RDG_EVENT_NAME("GaussianSplatOcc.Sat"), OccSatCS, SatParameters, FIntVector(1, 1, 1));

                            const FRDGBufferSRVRef TileSatSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TileSat, PF_R32_UINT));
                            if (bOccBox)
                            {
                                // Over every dispatch index the cull ran (CPU-known), reading the boxes in order.
                                FGaussianSplatOcclusionParameters* TestParameters = AllocOcc(Phase);
                                TestParameters->OccBox = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(OccBoxBuffer, PF_R32_UINT));
                                TestParameters->OccTileSat = TileSatSRV;
                                TestParameters->OccCullBitsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CullBits, PF_R32_UINT));
                                FComputeShaderUtils::AddPass(
                                    GraphBuilder, RDG_EVENT_NAME("GaussianSplatOcc.Test (%u)", DispatchCount), OccTestCS, TestParameters,
                                    FIntVector(static_cast<int32>(FMath::DivideAndRoundUp(DispatchCount, 256u)), 1, 1));
                            }

                            // The per-slot passes cover this phase's slots of the sorted list, a GPU-only range, so
                            // they are dispatched indirectly from the args pass.
                            const uint32 DispatchArgsOffset = (Phase - 1) * 4 * sizeof(uint32);
                            FGaussianSplatOcclusionParameters* FlagParameters = AllocOcc(Phase);
                            FlagParameters->OccSortCount = SortCountSRV;
                            FlagParameters->OccSortedOrder = SortedOrderSRV;
                            FlagParameters->OccFlagWordsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(FlagWords, PF_R32_UINT));
                            FlagParameters->OccGroupCountsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(GroupCounts, PF_R32_UINT));
                            FlagParameters->OccIndirectArgs = OccDispatchArgs;
                            if (bOccBox)
                            {
                                FlagParameters->OccCullBits = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CullBits, PF_R32_UINT));
                            }
                            else
                            {
                                FlagParameters->OccTileSat = TileSatSRV;
                                FlagParameters->Stride = Stride;
                                FlagParameters->SplatCellRanges = CellRangeSRV;
                                FlagParameters->SplatCellCount = Selection.CellCount;
                                FlagParameters->SplatPackedA = SrcPackedA;
                                FlagParameters->SplatCellBounds = SrcCellBounds;
                                FlagParameters->SplatBatches = BatchTableSRV;
                                FlagParameters->ViewRectMin = InitSortParameters->ViewRectMin;
                                FlagParameters->ViewSize = InitSortParameters->ViewSize;
                                FlagParameters->PointSize = InitSortParameters->PointSize;
                                FlagParameters->MinScreenVariance = InitSortParameters->MinScreenVariance;
                                FlagParameters->ViewMatrix = InitSortParameters->ViewMatrix;
                                FlagParameters->ProjectionMatrix = InitSortParameters->ProjectionMatrix;
                                FlagParameters->ViewProjectionMatrix = InitSortParameters->ViewProjectionMatrix;
                                FlagParameters->LocalToWorldMatrix = InitSortParameters->LocalToWorldMatrix;
                            }
                            FComputeShaderUtils::AddPass(
                                GraphBuilder, RDG_EVENT_NAME("GaussianSplatOcc.Flag (phase %u)", Phase), OccFlagCS, FlagParameters,
                                OccDispatchArgs, DispatchArgsOffset);

                            FGaussianSplatOcclusionParameters* ScanParameters = AllocOcc(Phase);
                            ScanParameters->OccSortCount = SortCountSRV;
                            ScanParameters->OccGroupCounts = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(GroupCounts, PF_R32_UINT));
                            ScanParameters->OccGroupOffsetsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(GroupOffsets, PF_R32_UINT));
                            ScanParameters->OccDrawArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OccDrawArgs, PF_R32_UINT));
                            FComputeShaderUtils::AddPass(
                                GraphBuilder, RDG_EVENT_NAME("GaussianSplatOcc.Scan"), OccScanCS, ScanParameters, FIntVector(1, 1, 1));

                            FGaussianSplatOcclusionParameters* ScatterParameters = AllocOcc(Phase);
                            ScatterParameters->OccSortCount = SortCountSRV;
                            ScatterParameters->OccSortedOrder = SortedOrderSRV;
                            ScatterParameters->OccFlagWords = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(FlagWords, PF_R32_UINT));
                            ScatterParameters->OccGroupOffsets = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(GroupOffsets, PF_R32_UINT));
                            ScatterParameters->OccCompactedUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Compacted, PF_R32_UINT));
                            ScatterParameters->OccIndirectArgs = OccDispatchArgs;
                            FComputeShaderUtils::AddPass(
                                GraphBuilder, RDG_EVENT_NAME("GaussianSplatOcc.Scatter (phase %u)", Phase), OccScatterCS, ScatterParameters,
                                OccDispatchArgs, DispatchArgsOffset);
                        }

                        // ELoad, as later batches do: a later phase is composited behind everything the earlier
                        // phases drew into this target, which is what makes the cull conservative.
                        AddBillboardsRaster(Compacted, OccDrawArgs, Phase * 4 * sizeof(uint32), ERenderTargetLoadAction::ELoad, static_cast<int32>(Phase));
                    }
                    OccReadback.DrawArgs = OccDrawArgs;
                    OccReadback.Phases = OccPhases;
                    OccReadback.bBoxPath = bOccBox;
                }
            }

            if (bFirstBatch && !bSkipVisibleReadback)
            {
                GaussianSplatProfiling::EnqueueVisibleCountReadback(
                    GraphBuilder,
                    IndirectArgsBuffer,
                    RenderPointCount,
                    DispatchCount,
                    Selection.CellCount,
                    View.GetViewKey(),
                    SortModeLabel,
                    BatchIndex,
                    ViewRect,
                    LodStats,
                    OccReadback,
                    bPaged,
                    Selection.CellCount,
                    PagedMisses);
            }

            bFirstBatch = false;
        }

        // No batch drew (every cell outside the frustum, or no usable resources), so nothing wrote
        // SplatTexture: every batch that gets past its early-outs adds a raster pass and clears
        // bFirstBatch. The composite would then read undefined memory -- an RDG validation ensure in the
        // editor ("... has a read dependency on GaussianSplat.SplatTexture, but it was never written
        // to"), and whatever the transient allocator left there (typically a stale frame) without it.
        if (bFirstBatch)
        {
            AddClearRenderTargetPass(GraphBuilder, SplatTexture, FLinearColor::Transparent);
        }

        FGaussianSplatCompositePS::FParameters* CompositeParameters = GraphBuilder.AllocParameters<FGaussianSplatCompositePS::FParameters>();
        CompositeParameters->SceneColorTextureSize = SceneColorTextureSize;
        // CARLA RGB captures use tone-mapped linear sRGB here, whereas the
        // normal SDR viewport uses display-encoded colours. SplatTexture is
        // accumulated in the splat data's display space for both views.
        CompositeParameters->ConvertSplatToLinear =
            View.Family != nullptr && View.Family->SceneCaptureSource == SCS_FinalToneCurveHDR ? 1u : 0u;
        CompositeParameters->SceneColorTexture = SceneColor.Texture;
        CompositeParameters->SceneColorSampler = TStaticSamplerState<SF_Point>::GetRHI();
        CompositeParameters->SplatTexture = SplatTexture;
        CompositeParameters->SplatSampler = TStaticSamplerState<SF_Point>::GetRHI();
        CompositeParameters->RenderTargets[0] = FRenderTargetBinding(Output.Texture, Output.LoadAction);

        TShaderMapRef<FGaussianSplatCompositePS> CompositeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

        RDG_GPU_STAT_SCOPE(GraphBuilder, GaussianSplatComposite);
        FPixelShaderUtils::AddFullscreenPass(
            GraphBuilder,
            GetGlobalShaderMap(GMaxRHIFeatureLevel),
            RDG_EVENT_NAME("GaussianSplatRaster.Composite"),
            CompositeShader,
            CompositeParameters,
            ViewRect);

        return FScreenPassTexture(Output);
    }
}
