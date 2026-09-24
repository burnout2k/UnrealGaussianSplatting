#include "Render/GaussianSplatPasses.h"

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


namespace GaussianSplatProfiling
{
    // Instrumentation only. The cull pass already writes the number of splats that
    // survive frustum rejection into IndirectArgsBuffer[1], but nothing read it
    // back, so the sort is sized from the padded point count rather than from what
    // is actually on screen. Copy that counter to the CPU a few frames late.
    static constexpr int32 NumReadbackSlots = 4;

    struct FVisibleCountState
    {
        TUniquePtr<FRHIGPUBufferReadback> Slots[NumReadbackSlots];
        int32 WriteSlot = 0;
        uint32 LastVisibleCount = 0;
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
        int32 BatchIndex)
    {
        FVisibleCountState& State = GetViewState(ViewKey);
        const uint32 IndirectArgsBytes = 4 * sizeof(uint32);

        // Drain the oldest slot before reusing it, so this never stalls the GPU.
        const int32 ReadSlot = (State.WriteSlot + 1) % NumReadbackSlots;
        if (State.Slots[ReadSlot].IsValid() && State.Slots[ReadSlot]->IsReady())
        {
            if (const uint32* Data = static_cast<const uint32*>(State.Slots[ReadSlot]->Lock(IndirectArgsBytes)))
            {
                State.LastVisibleCount = Data[1];
            }
            State.Slots[ReadSlot]->Unlock();
        }

        if (!State.Slots[State.WriteSlot].IsValid())
        {
            State.Slots[State.WriteSlot] = MakeUnique<FRHIGPUBufferReadback>(TEXT("GaussianSplat.VisibleCount"));
        }
        AddEnqueueCopyPass(GraphBuilder, State.Slots[State.WriteSlot].Get(), IndirectArgsBuffer, IndirectArgsBytes);
        State.WriteSlot = (State.WriteSlot + 1) % NumReadbackSlots;

        if ((State.FrameCounter++ % 60) == 0 && State.LastVisibleCount > 0)
        {
            const float VisiblePercent = 100.0f * static_cast<float>(State.LastVisibleCount) /
                static_cast<float>(FMath::Max(1u, SelectedCount));
            UE_LOG(
                LogGaussianSplatProfile,
                Display,
                TEXT("view %u: lod selected %u of %u budget (%.0f MiB sort scratch) ")
                TEXT("across %u cells | visible=%u (%.1f%% of selected) | sort mode %s | batch %d"),
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
                BatchIndex);
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
        1,
        TEXT("0 = bitonic network, 1 = UE GPU radix sort (SortGPUBuffers, 4 bits per pass), ")
        TEXT("2 = plugin DeviceRadixSort (8 bits per pass). Mode 2 runs only on NVIDIA GPUs ")
        TEXT("with wave size 32 unless r.GaussianSplat.RadixAllowAnyVendor is set, and falls ")
        TEXT("back to 1 anywhere else. Values above 2 mean bitonic."),
        ECVF_RenderThreadSafe);

    int32 GetSortMode()
    {
        return CVarSortMode.GetValueOnRenderThread();
    }

    static TAutoConsoleVariable<int32> CVarSortKeyBits(
        TEXT("r.GaussianSplat.SortKeyBits"),
        20,
        TEXT("Significant bits of the depth key (8-32). Mode 1 spends one pass per 4 bits ")
        TEXT("and rounds an odd count up to even (20 and 24 bits both take 6); mode 2 spends ")
        TEXT("one pass per 8 bits (20 and 24 both take 3). Fewer bits means more ties: 20 was ")
        TEXT("indistinguishable from 32 on tartu_demo, 16 was visibly wrong."),
        ECVF_RenderThreadSafe);

    uint32 GetSortKeyBits()
    {
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
    static TAutoConsoleVariable<float> CVarAlphaCutoff(
        TEXT("r.GaussianSplat.AlphaCutoff"),
        1.0f / 255.0f,
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
        1.0f,
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
    };

    // Per-cell frustum cull + distance LOD + budget feedback, replacing the
    // single global stride.
    //
    // The stride was spatially blind: it thinned the road under the bumper
    // exactly as hard as the forest two kilometres away, and from a whole-map
    // view it drew every resident splat (measured: 40M drawn, 94.6 ms). Cells
    // let each region answer for itself.
    void SelectCells(
        const TArray<FGaussianSplatCell>& Cells,
        const FMatrix44f& LocalToWorld,
        const FSceneView& View,
        uint32 Budget,
        float& InOutBias,
        FSelection& Out)
    {
        Out.Ranges.Reset();
        Out.Ranges.SetNumZeroed(FMath::Max(1, Cells.Num()));
        Out.CellCount = 0;
        Out.TotalCount = 0;

        if (Cells.IsEmpty() || Budget == 0)
        {
            return;
        }

        const FMatrix ToWorld(LocalToWorld);
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
            Out.Ranges[Out.CellCount++] = FUintVector4(Firsts[Index], Prefix, CellIndices[Index], 0u);
            Prefix += Takes[Index];
        }
        Out.TotalCount = Prefix;

        // Cesium's feedback loop. Tighten immediately when over budget, relax
        // slowly when under, so it settles rather than oscillating.
        if (Out.TotalCount > 0)
        {
            const float Desired = static_cast<float>(Budget) / static_cast<float>(Out.TotalCount);
            InOutBias = FMath::Clamp(Bias * FMath::Min(Desired, 1.05f), 0.001f, 4.0f);
        }
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

        const FIntRect ViewRect = SceneColor.ViewRect;
        const FMatrix ViewMatrixD = View.ViewMatrices.GetViewMatrix();
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
        TShaderMapRef<FGaussianSplatBillboardsCullCS> BillboardsCullCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        TShaderMapRef<FGaussianSplatPointsRasterVS> PointsRasterVS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        TShaderMapRef<FGaussianSplatPointsRasterPS> PointsRasterPS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        TShaderMapRef<FGaussianSplatBillboardsRasterVS> BillboardsRasterVS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        TShaderMapRef<FGaussianSplatBillboardsRasterPS> BillboardsRasterPS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        bool bFirstBatch = true;

        for (int32 BatchIndex = 0; BatchIndex < Batches.Num(); ++BatchIndex)
        {
            const FGaussianSplatRenderBatch& Batch = Batches[BatchIndex];
            const FGaussianSplatRenderResources* Resources = Batch.Resources;
            if (Resources == nullptr || Resources->GetPointCount() == 0 || Resources->GetPackedASRV() == nullptr)
            {
                continue;
            }

            // Cells are MANDATORY once the asset has them. The 20 B record stores
            // each position as a 16-bit fraction of its OWN cell, so without a cell
            // index there is nothing to decode against -- the legacy stride path
            // renders an empty screen. r.GaussianSplat.Lod therefore selects the
            // distance falloff only (see SelectCells), never whether cells are used.
            const bool bUseCells = !Resources->GetCells().IsEmpty();

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
            const uint32 RenderPointCount = bUseCells
                ? FMath::Min(Batch.MaxRenderPoints, Batch.AssetPointCount)
                : FMath::Min(Batch.MaxRenderPoints,
                             FMath::DivideAndRoundUp(Batch.AssetPointCount, Stride));
            if (RenderPointCount == 0)
            {
                continue;
            }

            // DispatchCount is what per-cell LOD actually selected this frame.
            // It swings with the camera, so it sizes ONLY the dispatch -- never
            // an allocation. Sizing a pooled buffer from a per-frame count is
            // what exhausted VRAM and stuttered when it was tried before.
            GaussianSplatLod::FSelection Selection;
            uint32 DispatchCount = RenderPointCount;
            if (bUseCells)
            {
                GaussianSplatLod::SelectCells(
                    Resources->GetCells(),
                    Batch.LocalToWorld,
                    View,
                    RenderPointCount,
                    GaussianSplatProfiling::GetViewState(View.GetViewKey()).LodBias,
                    Selection);
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

            const FVector3f ViewOrigin = static_cast<FVector3f>(View.ViewMatrices.GetViewOrigin());
            const FVector3f Forward = static_cast<FVector3f>(View.GetViewDirection());

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
                InitSortParameters->LocalToWorldMatrix = Batch.LocalToWorld;
                InitSortParameters->SplatPackedA = Resources->GetPackedASRV();
                InitSortParameters->SplatPackedB = Resources->GetPackedBSRV();
                InitSortParameters->SplatCellBounds = Resources->GetCellBoundsSRV();
                InitSortParameters->SortKeyShift = 32u - SortKeyBits;
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
                            SortParameters->LocalToWorldMatrix = Batch.LocalToWorld;
                            SortParameters->SplatPackedA = Resources->GetPackedASRV();
                            SortParameters->SplatPackedB = Resources->GetPackedBSRV();
                            SortParameters->SplatCellBounds = Resources->GetCellBoundsSRV();
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
                RasterParameters->LocalToWorldMatrix = Batch.LocalToWorld;
                RasterParameters->ViewProjectionMatrix = ViewProjection;
                RasterParameters->SplatOrderBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SortedOrderBuffer, PF_R32_UINT));
                RasterParameters->SplatPackedA = Resources->GetPackedASRV();
                RasterParameters->SplatPackedB = Resources->GetPackedBSRV();
                RasterParameters->SplatCellBounds = Resources->GetCellBoundsSRV();
                RasterParameters->SplatColorEncoding = Resources->GetColorEncoding();
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
                InitSortParameters->LocalToWorldMatrix = Batch.LocalToWorld;
                InitSortParameters->SplatPackedA = Resources->GetPackedASRV();
                InitSortParameters->SplatPackedB = Resources->GetPackedBSRV();
                InitSortParameters->SplatCellBounds = Resources->GetCellBoundsSRV();
                InitSortParameters->SplatColorEncoding = Resources->GetColorEncoding();
                InitSortParameters->OpacityScale = Batch.OpacityScale;
                InitSortParameters->MinSplatOpacity = GaussianSplatProfiling::GetMinSplatOpacity();
                InitSortParameters->MaxSplatDistance = GaussianSplatProfiling::GetMaxSplatDistance();
                InitSortParameters->MinScreenVariance = GaussianSplatProfiling::GetMinScreenVariance();
                InitSortParameters->SortKeyShift = 32u - SortKeyBits;
                InitSortParameters->SplatOrderBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OrderBuffer, PF_R32_UINT));
                InitSortParameters->SplatKeyBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(KeyBuffer, PF_R32_UINT));
                InitSortParameters->SplatIndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsBuffer, PF_R32_UINT));

                {
                    RDG_GPU_STAT_SCOPE(GraphBuilder, GaussianSplatCull);
                    FComputeShaderUtils::AddPass(
                        GraphBuilder,
                        RDG_EVENT_NAME("GaussianSplatBillboardsSort.Init"),
                        BillboardsCullCS,
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
                            SortParameters->LocalToWorldMatrix = Batch.LocalToWorld;
                            SortParameters->SplatPackedA = Resources->GetPackedASRV();
                            SortParameters->SplatPackedB = Resources->GetPackedBSRV();
                            SortParameters->SplatCellBounds = Resources->GetCellBoundsSRV();
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
                RasterParameters->LocalToWorldMatrix = Batch.LocalToWorld;
                RasterParameters->ProjectionMatrix = ProjectionMatrix;
                RasterParameters->ViewProjectionMatrix = ViewProjection;
                RasterParameters->WorldToLocalRow0 = Batch.WorldToLocalRow0;
                RasterParameters->WorldToLocalRow1 = Batch.WorldToLocalRow1;
                RasterParameters->WorldToLocalRow2 = Batch.WorldToLocalRow2;
                RasterParameters->SplatOrderBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SortedOrderBuffer, PF_R32_UINT));
                RasterParameters->SplatPackedA = Resources->GetPackedASRV();
                RasterParameters->SplatPackedB = Resources->GetPackedBSRV();
                RasterParameters->SplatCellBounds = Resources->GetCellBoundsSRV();
                RasterParameters->SplatColorEncoding = Resources->GetColorEncoding();
                RasterParameters->PerPixelDepth = GaussianSplatProfiling::ShouldUsePerPixelDepth() ? 1u : 0u;
                RasterParameters->HasSH = Resources->HasSH() ? 1u : 0u;
                RasterParameters->MinScreenVariance = GaussianSplatProfiling::GetMinScreenVariance();
                RasterParameters->SplatSHIndexBuffer = Resources->GetSHIndexSRV();
                RasterParameters->SplatSHPaletteBuffer = Resources->GetSHPaletteSRV();
                PassParameters->PS.AlphaCutoff = GaussianSplatProfiling::GetAlphaCutoff();
                SetDepthTestParameters(PassParameters->PS.DepthTest);
                PassParameters->IndirectArgsBuffer = IndirectArgsBuffer;
                PassParameters->RenderTargets[0] = FRenderTargetBinding(
                    SplatOutput.Texture,
                    bFirstBatch ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad);

                // Lasts to the end of this branch, which is this pass alone.
                RDG_GPU_STAT_SCOPE(GraphBuilder, GaussianSplatRaster);
                GraphBuilder.AddPass(
                    RDG_EVENT_NAME("GaussianSplatBillboardsRaster.DrawInstanced"),
                    PassParameters,
                    ERDGPassFlags::Raster,
                    [PassParameters, BillboardsRasterVS, BillboardsRasterPS, ViewRect, IndirectArgsBuffer](FRHICommandList& RHICmdList)
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
                        GraphicsPSOInit.BoundShaderState.VertexShaderRHI = BillboardsRasterVS.GetVertexShader();
                        GraphicsPSOInit.BoundShaderState.PixelShaderRHI = BillboardsRasterPS.GetPixelShader();

                        SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
                        SetShaderParameters(RHICmdList, BillboardsRasterVS, BillboardsRasterVS.GetVertexShader(), PassParameters->VS);
                        SetShaderParameters(RHICmdList, BillboardsRasterPS, BillboardsRasterPS.GetPixelShader(), PassParameters->PS);
                        RHICmdList.DrawPrimitiveIndirect(IndirectArgsBuffer->GetIndirectRHICallBuffer(), 0);
                    });
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
                    BatchIndex);
            }

            bFirstBatch = false;
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
