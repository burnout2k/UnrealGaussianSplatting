#include "Render/GaussianSplatProbe.h"

#include "Components/SceneCaptureComponent2D.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "ShaderParameterStruct.h"
#include "UObject/UObjectIterator.h"

#include "Render/GaussianSplatDeviceMemory.h"

#if GAUSSIANSPLAT_WITH_VULKAN
#include "IVulkanDynamicRHI.h"
#endif

// What the probe measures (fix5-plan.md v2, Step 0):
//  - r.GaussianSplat.Probe.PoolMB: a dummy pool allocated once, laid out as Fix 5's pool (PackedA, PackedB and SH index
//    regions), and whether it landed in device-local memory: VK_EXT_memory_budget's usage before and after.
//  - r.GaussianSplat.Probe.UploadMB: every tick, that many MiB uploaded into the pool in 96 KiB pages (4,096 splats).
//    Method 0 = a dynamic (host-visible) upload buffer read by one scatter dispatch; 1 = a static upload buffer (a
//    staging copy at unlock, two of them alternating so a tick never overwrites what the previous tick's scatter may
//    still read) and the same scatter; 2 = the static buffer and one copy pass per page region (three per page).
//    GPU time comes from the probe's own Vulkan timestamps, render-thread time from the CPU clock.
//  - r.GaussianSplat.Probe.BudgetLog: the device-local heaps' usage and budget once a second.
//  - r.GaussianSplat.Probe.DumpMemoryAtTick N: r.Vulkan.DumpMemory on the Nth tick that has a camera.
// One line per second reports ticks and render commands, so it also shows that a render command runs on every tick,
// camera or not.

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatProbe, Log, All);

// The budget query moved to Render/GaussianSplatDeviceMemory.h so the pool sizes itself
// from the same numbers the probe reports.
using namespace GaussianSplatDeviceMemory;

class FGaussianSplatProbeScatterCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatProbeScatterCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatProbeScatterCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // Raw: a dynamic buffer is host memory written before the graph, and a static one was copied at unlock,
        // which the RHI follows with a transfer-to-all-commands barrier.
        SHADER_PARAMETER_SRV(StructuredBuffer<uint4>, ProbeSource)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ProbePageSlots)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, ProbePool)
        SHADER_PARAMETER(uint32, ProbePageCount)
        SHADER_PARAMETER(uint32, ProbePoolPages)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

IMPLEMENT_GLOBAL_SHADER(FGaussianSplatProbeScatterCS, "/GaussianSplatting/Private/GaussianSplatProbeScatter.usf", "MainCS", SF_Compute);

namespace GaussianSplatProbe
{
namespace
{
    TAutoConsoleVariable<int32> CVarPoolMB(
        TEXT("r.GaussianSplat.Probe.PoolMB"),
        0,
        TEXT("Fix 5 probe: allocate a dummy splat pool of this many MiB once and log where it landed (0 = off; the ")
        TEXT("upload probe then uses 512)."),
        ECVF_Default);

    TAutoConsoleVariable<int32> CVarUploadMB(
        TEXT("r.GaussianSplat.Probe.UploadMB"),
        0,
        TEXT("Fix 5 probe: upload this many MiB into the pool every tick, in 96 KiB pages (0 = off)."),
        ECVF_Default);

    TAutoConsoleVariable<int32> CVarMethod(
        TEXT("r.GaussianSplat.Probe.Method"),
        0,
        TEXT("Fix 5 probe upload path: 0 = dynamic upload buffer + one scatter dispatch, 1 = static upload buffer ")
        TEXT("(staging copy) + the scatter, 2 = static upload buffer + one copy pass per page region."),
        ECVF_Default);

    TAutoConsoleVariable<int32> CVarBudgetLog(
        TEXT("r.GaussianSplat.Probe.BudgetLog"),
        0,
        TEXT("Fix 5 probe: 1 = log the device-local heaps' usage and budget (VK_EXT_memory_budget) once a second."),
        ECVF_Default);

    TAutoConsoleVariable<int32> CVarDumpMemoryAtTick(
        TEXT("r.GaussianSplat.Probe.DumpMemoryAtTick"),
        0,
        TEXT("Fix 5 probe: run r.Vulkan.DumpMemory on the Nth tick that has a camera (0 = never)."),
        ECVF_Default);

    constexpr uint32 PageBytes = 96 * 1024;         // 4,096 splats: PackedA 16 B, PackedB 4 B, SH index 4 B each
    constexpr uint32 PageUint4s = PageBytes / 16;
    constexpr uint32 PartABytes = 64 * 1024;
    constexpr uint32 PartSmallBytes = 16 * 1024;
    constexpr uint32 DefaultPoolMB = 512;           // the upload probe's pool when PoolMB is 0
    constexpr uint32 TimerRing = 16;                // a timestamp pair is read back this many ticks after it is written

#if GAUSSIANSPLAT_WITH_VULKAN
    // The probe's own timestamp queries. The RHI's timing queries keep a four-deep ring per query object and report
    // the newest resolved slot, which cannot pair a start with an end per tick; a query pool of our own can.
    struct FGpuTimer
    {
        VkQueryPool Pool = VK_NULL_HANDLE;
        double NanosecondsPerTick = 0.0;
        bool bWritten[TimerRing] = {};
        PFN_vkCmdResetQueryPool CmdReset = nullptr;
        PFN_vkCmdWriteTimestamp CmdWrite = nullptr;
        PFN_vkGetQueryPoolResults GetResults = nullptr;
        bool bFailed = false;

        bool Init()
        {
            if (Pool != VK_NULL_HANDLE)
            {
                return true;
            }
            if (bFailed || RHIGetInterfaceType() != ERHIInterfaceType::Vulkan)
            {
                return false;
            }
            IVulkanDynamicRHI* Rhi = GetIVulkanDynamicRHI();
            const auto Create = reinterpret_cast<PFN_vkCreateQueryPool>(Rhi->RHIGetVkDeviceProcAddr("vkCreateQueryPool"));
            const auto GetProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                Rhi->RHIGetVkInstanceProcAddr("vkGetPhysicalDeviceProperties"));
            CmdReset = reinterpret_cast<PFN_vkCmdResetQueryPool>(Rhi->RHIGetVkDeviceProcAddr("vkCmdResetQueryPool"));
            CmdWrite = reinterpret_cast<PFN_vkCmdWriteTimestamp>(Rhi->RHIGetVkDeviceProcAddr("vkCmdWriteTimestamp"));
            GetResults = reinterpret_cast<PFN_vkGetQueryPoolResults>(Rhi->RHIGetVkDeviceProcAddr("vkGetQueryPoolResults"));
            if (Create == nullptr || GetProperties == nullptr || CmdReset == nullptr || CmdWrite == nullptr
                || GetResults == nullptr)
            {
                bFailed = true;
                return false;
            }
            VkPhysicalDeviceProperties Properties{};
            GetProperties(Rhi->RHIGetVkPhysicalDevice(), &Properties);
            NanosecondsPerTick = Properties.limits.timestampPeriod;

            VkQueryPoolCreateInfo Info{};
            Info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            Info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            Info.queryCount = 2 * TimerRing;
            if (Create(Rhi->RHIGetVkDevice(), &Info, Rhi->RHIGetVkAllocationCallbacks(), &Pool) != VK_SUCCESS)
            {
                Pool = VK_NULL_HANDLE;
                bFailed = true;
                return false;
            }
            return true;
        }

        // A timestamp once all earlier GPU work is done (bottom of pipe). A query must be reset before each write, and
        // a reset is legal only outside a render pass, which holds between RDG graphs, where the probe writes.
        void Write(FRHICommandListImmediate& RHICmdList, uint32 Query)
        {
            const VkQueryPool QueryPool = Pool;
            const PFN_vkCmdResetQueryPool Reset = CmdReset;
            const PFN_vkCmdWriteTimestamp WriteTimestamp = CmdWrite;
            RHICmdList.EnqueueLambda(TEXT("GaussianSplatProbe.Timestamp"),
                [QueryPool, Query, Reset, WriteTimestamp](auto&)
                {
                    const VkCommandBuffer CommandBuffer = GetIVulkanDynamicRHI()->RHIGetActiveVkCommandBuffer();
                    Reset(CommandBuffer, QueryPool, Query, 1);
                    WriteTimestamp(CommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, QueryPool, Query);
                });
        }

        // Pair Slot's GPU milliseconds once the GPU has written both timestamps; never waits.
        bool Read(uint32 Slot, double& OutMs)
        {
            if (Pool == VK_NULL_HANDLE || !bWritten[Slot])
            {
                return false;
            }
            uint64 Values[2] = {};
            const VkResult Result = GetResults(GetIVulkanDynamicRHI()->RHIGetVkDevice(), Pool, 2 * Slot, 2,
                sizeof(Values), Values, sizeof(uint64), VK_QUERY_RESULT_64_BIT);
            if (Result != VK_SUCCESS)
            {
                return false;
            }
            bWritten[Slot] = false;
            OutMs = static_cast<double>(Values[1] - Values[0]) * NanosecondsPerTick * 1e-6;
            return true;
        }
    };
#endif

    // Render thread only.
    struct FRenderState
    {
        TRefCountPtr<FRDGPooledBuffer> Pool;
        uint32 PoolMB = 0;
        uint32 PoolPages = 0;
        FBufferRHIRef DynamicUpload;
        TRefCountPtr<FRDGPooledBuffer> StaticUpload[2];
        uint32 UploadBytes = 0;
        TArray<uint8> Source;
        uint64 Commands = 0;

        // The current one-second window.
        double WindowStart = 0.0;
        uint64 WindowFirstTick = 0;
        uint64 WindowLastTick = 0;
        uint32 WindowCommands = 0;
        uint32 WindowUploads = 0;
        double CpuSum = 0.0;
        double CpuMax = 0.0;
        double GpuSum = 0.0;
        double GpuMax = 0.0;
        uint32 GpuSamples = 0;
        int32 WindowMethod = -1;
        uint32 WindowPages = 0;
#if GAUSSIANSPLAT_WITH_VULKAN
        FGpuTimer Timer;
#endif
    };

    // Never destroyed: a static's destructor would release RHI resources after the RHI is gone at exit. World cleanup
    // releases them in time (HandleWorldCleanup); the struct itself is left to the process exit.
    FRenderState& GetRenderState()
    {
        static FRenderState* State = new FRenderState();
        return *State;
    }

    void ReleaseRenderResources(FRenderState& State)
    {
        State.Pool.SafeRelease();
        State.PoolMB = 0;
        State.PoolPages = 0;
        State.DynamicUpload.SafeRelease();
        State.StaticUpload[0].SafeRelease();
        State.StaticUpload[1].SafeRelease();
        State.UploadBytes = 0;
        State.Source.Empty();
    }

    void EnsurePool(FRenderState& State, uint32 PoolMB)
    {
        if (PoolMB == State.PoolMB)
        {
            return;
        }
        State.Pool.SafeRelease();
        State.PoolMB = 0;
        State.PoolPages = 0;
        if (PoolMB == 0)
        {
            return;
        }

        const uint32 Pages = static_cast<uint32>((static_cast<uint64>(PoolMB) << 20) / PageBytes);
        const uint64 Bytes = static_cast<uint64>(Pages) * PageBytes;
        // Nothing in the RHI or RDG checks this (the size is a uint32 and the multiply wraps), so the plugin must.
        if (Bytes >= (1ull << 32))
        {
            UE_LOG(LogGaussianSplatProbe, Error, TEXT("GaussianSplatProbe pool: %u MiB does not fit one buffer (4 GiB)."), PoolMB);
            State.PoolMB = PoolMB;  // do not retry every tick
            return;
        }

        FDeviceMemory Before;
        const bool bBefore = QueryDeviceMemory(Before);
        State.Pool = AllocatePooledBuffer(
            FRDGBufferDesc::CreateStructuredDesc(16, Pages * PageUint4s), TEXT("GaussianSplat.ProbePool"));
        State.PoolMB = PoolMB;
        State.PoolPages = Pages;
        FDeviceMemory After;
        const bool bAfter = QueryDeviceMemory(After);
        if (bBefore && bAfter)
        {
            const double Delta = ToMiB(After.Usage) - ToMiB(Before.Usage);
            UE_LOG(LogGaussianSplatProbe, Display,
                TEXT("GaussianSplatProbe pool: %u MiB (%u pages of 96 KiB) | device-local usage %.0f -> %.0f MiB (%+.0f) | ")
                TEXT("budget %.0f of %.0f MiB | placement %s"),
                PoolMB, Pages, ToMiB(Before.Usage), ToMiB(After.Usage), Delta, ToMiB(After.Budget), ToMiB(After.Size),
                Delta >= 0.9 * ToMiB(Bytes) ? TEXT("VRAM") : TEXT("NOT VRAM (or not allocated yet; see the budget lines)"));
        }
        else
        {
            UE_LOG(LogGaussianSplatProbe, Display,
                TEXT("GaussianSplatProbe pool: %u MiB (%u pages of 96 KiB) | no memory budget query on this RHI"),
                PoolMB, Pages);
        }
    }

    void FillSource(FRenderState& State, uint32 Bytes)
    {
        if (static_cast<uint32>(State.Source.Num()) == Bytes)
        {
            return;
        }
        State.Source.SetNumUninitialized(Bytes);
        uint32 Value = 0x9E3779B9u;
        uint32* Words = reinterpret_cast<uint32*>(State.Source.GetData());
        for (uint32 Index = 0; Index < Bytes / 4; ++Index)
        {
            Value = Value * 1664525u + 1013904223u;
            Words[Index] = Value;
        }
    }

    void FlushWindow(FRenderState& State)
    {
        if (State.WindowCommands > 0)
        {
            const double Uploads = FMath::Max(1u, State.WindowUploads);
            UE_LOG(LogGaussianSplatProbe, Display,
                TEXT("GaussianSplatProbe upload: method %d, %u pages (%.1f MiB) per tick | ticks %llu..%llu, %u render ")
                TEXT("commands, %u uploads | render thread %.3f ms avg %.3f max | GPU %.3f ms avg %.3f max (%u timed) | ")
                TEXT("pool %u MiB"),
                State.WindowMethod,
                State.WindowPages,
                State.WindowPages * (PageBytes / (1024.0 * 1024.0)),
                State.WindowFirstTick,
                State.WindowLastTick,
                State.WindowCommands,
                State.WindowUploads,
                State.CpuSum / Uploads,
                State.CpuMax,
                State.GpuSamples > 0 ? State.GpuSum / State.GpuSamples : 0.0,
                State.GpuMax,
                State.GpuSamples,
                State.PoolMB);
        }
        State.WindowCommands = 0;
        State.WindowUploads = 0;
        State.CpuSum = State.CpuMax = State.GpuSum = State.GpuMax = 0.0;
        State.GpuSamples = 0;
    }

    void Tick_RenderThread(FRHICommandListImmediate& RHICmdList, uint32 PoolMB, uint32 UploadMB, int32 Method, uint64 Tick)
    {
        FRenderState& State = GetRenderState();
        if (PoolMB == 0 && UploadMB == 0)
        {
            FlushWindow(State);
            ReleaseRenderResources(State);
            return;
        }

        const double Now = FPlatformTime::Seconds();
        if (State.WindowCommands == 0)
        {
            State.WindowStart = Now;
            State.WindowFirstTick = Tick;
        }
        State.WindowLastTick = Tick;
        ++State.WindowCommands;
        ++State.Commands;

        EnsurePool(State, PoolMB > 0 ? PoolMB : DefaultPoolMB);

#if GAUSSIANSPLAT_WITH_VULKAN
        // Read the pair written TimerRing ticks ago before writing it again.
        const uint32 TimerSlot = static_cast<uint32>(Tick % TimerRing);
        double GpuMs = 0.0;
        if (State.Timer.Read(TimerSlot, GpuMs))
        {
            State.GpuSum += GpuMs;
            State.GpuMax = FMath::Max(State.GpuMax, GpuMs);
            ++State.GpuSamples;
        }
#endif

        if (UploadMB > 0 && State.Pool.IsValid() && State.PoolPages > 0)
        {
            const uint32 Pages = FMath::Min(static_cast<uint32>((static_cast<uint64>(UploadMB) << 20) / PageBytes), State.PoolPages);
            const uint32 Bytes = Pages * PageBytes;
            FillSource(State, Bytes);
            State.WindowMethod = Method;
            State.WindowPages = Pages;

            TArray<uint32> Slots;
            Slots.SetNumUninitialized(Pages);
            const uint32 Base = static_cast<uint32>((Tick * 7919u) % State.PoolPages);
            for (uint32 Page = 0; Page < Pages; ++Page)
            {
                Slots[Page] = (Base + Page) % State.PoolPages;
            }

            const uint64 CpuStart = FPlatformTime::Cycles64();
#if GAUSSIANSPLAT_WITH_VULKAN
            const bool bTimed = State.Timer.Init();
            if (bTimed)
            {
                State.Timer.Write(RHICmdList, 2 * TimerSlot);
            }
#endif
            // The upload buffer this tick: written before the graph, so the staging copy (methods 1 and 2) falls
            // between the timestamps.
            FRHIBuffer* SourceBuffer = nullptr;
            const int32 StaticIndex = static_cast<int32>(Tick & 1);
            if (Method == 0)
            {
                if (!State.DynamicUpload.IsValid() || State.UploadBytes != Bytes)
                {
                    FRHIResourceCreateInfo CreateInfo(TEXT("GaussianSplat.ProbeUploadDynamic"));
                    State.DynamicUpload = RHICmdList.CreateStructuredBuffer(
                        16, Bytes, BUF_Dynamic | BUF_ShaderResource, ERHIAccess::SRVMask, CreateInfo);
                }
                SourceBuffer = State.DynamicUpload;
            }
            else
            {
                if (!State.StaticUpload[StaticIndex].IsValid() || State.UploadBytes != Bytes)
                {
                    State.StaticUpload[0] = AllocatePooledBuffer(
                        FRDGBufferDesc::CreateStructuredDesc(16, Bytes / 16), TEXT("GaussianSplat.ProbeUploadStatic0"));
                    State.StaticUpload[1] = AllocatePooledBuffer(
                        FRDGBufferDesc::CreateStructuredDesc(16, Bytes / 16), TEXT("GaussianSplat.ProbeUploadStatic1"));
                }
                SourceBuffer = State.StaticUpload[StaticIndex]->GetRHI();
            }
            State.UploadBytes = Bytes;

            void* Mapped = RHICmdList.LockBuffer(SourceBuffer, 0, Bytes, RLM_WriteOnly);
            FMemory::Memcpy(Mapped, State.Source.GetData(), Bytes);
            RHICmdList.UnlockBuffer(SourceBuffer);

            {
                FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("GaussianSplatProbe.Upload"));
                FRDGBufferRef PoolBuffer = GraphBuilder.RegisterExternalBuffer(State.Pool);
                if (Method == 2)
                {
                    FRDGBufferRef Upload = GraphBuilder.RegisterExternalBuffer(State.StaticUpload[StaticIndex]);
                    const uint64 RegionB = static_cast<uint64>(State.PoolPages) * PartABytes;
                    const uint64 RegionS = static_cast<uint64>(State.PoolPages) * (PartABytes + PartSmallBytes);
                    for (uint32 Page = 0; Page < Pages; ++Page)
                    {
                        const uint64 Source = static_cast<uint64>(Page) * PageBytes;
                        const uint64 Slot = Slots[Page];
                        AddCopyBufferPass(GraphBuilder, PoolBuffer, Slot * PartABytes, Upload, Source, PartABytes);
                        AddCopyBufferPass(GraphBuilder, PoolBuffer, RegionB + Slot * PartSmallBytes, Upload,
                            Source + PartABytes, PartSmallBytes);
                        AddCopyBufferPass(GraphBuilder, PoolBuffer, RegionS + Slot * PartSmallBytes, Upload,
                            Source + PartABytes + PartSmallBytes, PartSmallBytes);
                    }
                }
                else
                {
                    FRDGBufferRef SlotBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("GaussianSplat.ProbePageSlots"),
                        sizeof(uint32), Pages, Slots.GetData(), Pages * sizeof(uint32));
                    FGaussianSplatProbeScatterCS::FParameters* Parameters =
                        GraphBuilder.AllocParameters<FGaussianSplatProbeScatterCS::FParameters>();
                    Parameters->ProbeSource = RHICmdList.CreateShaderResourceView(SourceBuffer);
                    Parameters->ProbePageSlots = GraphBuilder.CreateSRV(SlotBuffer);
                    Parameters->ProbePool = GraphBuilder.CreateUAV(PoolBuffer);
                    Parameters->ProbePageCount = Pages;
                    Parameters->ProbePoolPages = State.PoolPages;
                    TShaderMapRef<FGaussianSplatProbeScatterCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
                    FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GaussianSplatProbe.Scatter %u pages", Pages),
                        Shader, Parameters, FComputeShaderUtils::GetGroupCount(Pages * PageUint4s, 256));
                }
                GraphBuilder.Execute();
            }

#if GAUSSIANSPLAT_WITH_VULKAN
            if (bTimed)
            {
                State.Timer.Write(RHICmdList, 2 * TimerSlot + 1);
                State.Timer.bWritten[TimerSlot] = true;
            }
#endif
            const double CpuMs = FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - CpuStart);
            State.CpuSum += CpuMs;
            State.CpuMax = FMath::Max(State.CpuMax, CpuMs);
            ++State.WindowUploads;
        }

        if (Now - State.WindowStart >= 1.0)
        {
            FlushWindow(State);
        }
    }

    // Game thread.
    struct FGameState
    {
        uint64 Ticks = 0;
        uint64 CameraTicks = 0;
        double LastBudgetLog = 0.0;
        bool bRenderActive = false;
        uint32 LoggedPoolMB = MAX_uint32;
        uint32 LoggedUploadMB = MAX_uint32;
        int32 LoggedMethod = -1;
    };

    FGameState GGame;

    bool HasCamera(const UWorld& World)
    {
        for (TObjectIterator<USceneCaptureComponent2D> It; It; ++It)
        {
            const USceneCaptureComponent2D* Capture = *It;
            if (Capture->GetWorld() == &World && Capture->IsRegistered() && Capture->TextureTarget != nullptr)
            {
                return true;
            }
        }
        return false;
    }

    void Tick_GameThread(UWorld& World)
    {
        const uint64 Tick = ++GGame.Ticks;

        const int32 DumpAt = CVarDumpMemoryAtTick.GetValueOnGameThread();
        if (DumpAt > 0 && HasCamera(World))
        {
            if (++GGame.CameraTicks == static_cast<uint64>(DumpAt))
            {
                UE_LOG(LogGaussianSplatProbe, Display, TEXT("GaussianSplatProbe: r.Vulkan.DumpMemory at camera tick %d (tick %llu)"),
                    DumpAt, Tick);
                GEngine->Exec(&World, TEXT("r.Vulkan.DumpMemory"));
            }
        }

        if (CVarBudgetLog.GetValueOnGameThread() > 0)
        {
            const double Now = FPlatformTime::Seconds();
            if (Now - GGame.LastBudgetLog >= 1.0)
            {
                GGame.LastBudgetLog = Now;
                FDeviceMemory Memory;
                if (QueryDeviceMemory(Memory))
                {
                    UE_LOG(LogGaussianSplatProbe, Display,
                        TEXT("GaussianSplatProbe budget: tick %llu | device-local usage %.0f MiB | budget %.0f MiB | heap %.0f MiB"),
                        Tick, ToMiB(Memory.Usage), ToMiB(Memory.Budget), ToMiB(Memory.Size));
                }
            }
        }

        const uint32 PoolMB = static_cast<uint32>(FMath::Clamp(CVarPoolMB.GetValueOnGameThread(), 0, 65535));
        const uint32 UploadMB = static_cast<uint32>(FMath::Clamp(CVarUploadMB.GetValueOnGameThread(), 0, 4095));
        const int32 Method = FMath::Clamp(CVarMethod.GetValueOnGameThread(), 0, 2);
        if (PoolMB != GGame.LoggedPoolMB || UploadMB != GGame.LoggedUploadMB || Method != GGame.LoggedMethod)
        {
            GGame.LoggedPoolMB = PoolMB;
            GGame.LoggedUploadMB = UploadMB;
            GGame.LoggedMethod = Method;
            if (PoolMB > 0 || UploadMB > 0)
            {
                UE_LOG(LogGaussianSplatProbe, Display, TEXT("GaussianSplatProbe on at tick %llu: PoolMB %u UploadMB %u Method %d"),
                    Tick, PoolMB, UploadMB, Method);
            }
        }
        if (PoolMB == 0 && UploadMB == 0 && !GGame.bRenderActive)
        {
            return;
        }
        GGame.bRenderActive = PoolMB > 0 || UploadMB > 0;
        ENQUEUE_RENDER_COMMAND(GaussianSplatProbeTick)(
            [PoolMB, UploadMB, Method, Tick](FRHICommandListImmediate& RHICmdList)
            {
                Tick_RenderThread(RHICmdList, PoolMB, UploadMB, Method, Tick);
            });
    }

    // A plain FTickFunction (not a UObject's tick) so it can sit in TG_LastDemotable: the world's tickable objects tick
    // earlier, before TG_PostUpdateWork.
    struct FLateTickFunction final : public FTickFunction
    {
        TWeakObjectPtr<UWorld> World;

        virtual void ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread,
            const FGraphEventRef& MyCompletionGraphEvent) override
        {
            if (UWorld* TickWorld = World.Get())
            {
                Tick_GameThread(*TickWorld);
            }
        }

        virtual FString DiagnosticMessage() override
        {
            return TEXT("GaussianSplatProbe late tick");
        }
    };

    TMap<UWorld*, TUniquePtr<FLateTickFunction>> GLateTicks;
    FDelegateHandle GPostWorldInitHandle;
    FDelegateHandle GWorldCleanupHandle;

    void HandlePostWorldInitialization(UWorld* World, const UWorld::InitializationValues)
    {
        if (World == nullptr || !World->IsGameWorld() || World->PersistentLevel == nullptr || GLateTicks.Contains(World))
        {
            return;
        }
        TUniquePtr<FLateTickFunction> TickFunction = MakeUnique<FLateTickFunction>();
        TickFunction->TickGroup = TG_LastDemotable;
        TickFunction->EndTickGroup = TG_LastDemotable;
        TickFunction->bCanEverTick = true;
        TickFunction->bStartWithTickEnabled = true;
        TickFunction->bTickEvenWhenPaused = true;
        TickFunction->World = World;
        TickFunction->RegisterTickFunction(World->PersistentLevel);
        GLateTicks.Add(World, MoveTemp(TickFunction));
    }

    void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
    {
        if (TUniquePtr<FLateTickFunction>* Found = GLateTicks.Find(World))
        {
            (*Found)->UnRegisterTickFunction();
            GLateTicks.Remove(World);
        }
        if (GLateTicks.IsEmpty() && GGame.bRenderActive)
        {
            // Free the probe's buffers while the RHI still exists; the next world starts them again if asked.
            GGame.bRenderActive = false;
            ENQUEUE_RENDER_COMMAND(GaussianSplatProbeRelease)([](FRHICommandListImmediate&)
            {
                FRenderState& State = GetRenderState();
                FlushWindow(State);
                ReleaseRenderResources(State);
            });
        }
    }
}

void Startup()
{
    GPostWorldInitHandle = FWorldDelegates::OnPostWorldInitialization.AddStatic(&HandlePostWorldInitialization);
    GWorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddStatic(&HandleWorldCleanup);
}

void Shutdown()
{
    FWorldDelegates::OnPostWorldInitialization.Remove(GPostWorldInitHandle);
    FWorldDelegates::OnWorldCleanup.Remove(GWorldCleanupHandle);
    for (TPair<UWorld*, TUniquePtr<FLateTickFunction>>& Pair : GLateTicks)
    {
        Pair.Value->UnRegisterTickFunction();
    }
    GLateTicks.Empty();
}
}
