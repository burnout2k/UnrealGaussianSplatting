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

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatProfile, Log, All);


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
    };

    static FVisibleCountState GVisibleCount;

    void EnqueueVisibleCountReadback(
        FRDGBuilder& GraphBuilder,
        FRDGBufferRef IndirectArgsBuffer,
        uint32 RenderPointCount,
        uint32 PaddedPointCount)
    {
        FVisibleCountState& State = GVisibleCount;
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
                static_cast<float>(FMath::Max(1u, RenderPointCount));
            UE_LOG(
                LogGaussianSplatProfile,
                Display,
                TEXT("visible=%u of %u drawn (%.1f%%) | sort runs over %u padded"),
                State.LastVisibleCount,
                RenderPointCount,
                VisiblePercent,
                PaddedPointCount);
        }
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
        1,
        TEXT("0 = bitonic (253 dispatches), 1 = UE GPU radix sort (8 passes)."),
        ECVF_RenderThreadSafe);

    bool ShouldUseRadixSort()
    {
        return CVarSortMode.GetValueOnRenderThread() == 1;
    }

    static TAutoConsoleVariable<int32> CVarSortKeyBits(
        TEXT("r.GaussianSplat.SortKeyBits"),
        20,
        TEXT("Significant bits of the depth key (8-32). The radix sort spends one ")
        TEXT("pass per 4 bits, so the default 20 costs 5 passes instead of 8. Fewer ")
        TEXT("bits means more ties: 20 was indistinguishable from 32 on tartu_demo, ")
        TEXT("16 was visibly wrong. Raise to 32 if close splats misorder."),
        ECVF_RenderThreadSafe);

    uint32 GetSortKeyBits()
    {
        return static_cast<uint32>(FMath::Clamp(CVarSortKeyBits.GetValueOnRenderThread(), 8, 32));
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
        const uint32 KeyMask = (KeyBits >= 32u) ? 0xFFFFFFFFu : ((1u << KeyBits) - 1u);
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

        for (const FGaussianSplatRenderBatch& Batch : Batches)
        {
            const FGaussianSplatRenderResources* Resources = Batch.Resources;
            if (Resources == nullptr || Resources->GetPointCount() == 0 || Resources->GetPositionSRV() == nullptr)
            {
                continue;
            }

            const uint32 Stride = FMath::Max(1u, Batch.Stride);
            const uint32 RenderPointCount = FMath::Min(
                Batch.MaxRenderPoints,
                FMath::DivideAndRoundUp(Batch.AssetPointCount, Stride));
            if (RenderPointCount == 0)
            {
                continue;
            }

            const uint32 PaddedPointCount = FMath::RoundUpToPowerOfTwo(FMath::Max(1u, RenderPointCount));

            // Typed (PF_R32_UINT), not structured: the radix sort binds these as
            // Buffer<uint>/RWBuffer<uint>. The Alt pair is its ping-pong target and
            // is untouched by the bitonic path.
            const auto CreateSortBuffer = [&GraphBuilder, PaddedPointCount](const TCHAR* Name)
            {
                return GraphBuilder.CreateBuffer(
                    FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), PaddedPointCount), Name);
            };
            FRDGBufferRef OrderBuffer = CreateSortBuffer(TEXT("GaussianSplat.OrderBuffer"));
            FRDGBufferRef KeyBuffer = CreateSortBuffer(TEXT("GaussianSplat.KeyBuffer"));
            FRDGBufferRef OrderBufferAlt = CreateSortBuffer(TEXT("GaussianSplat.OrderBufferAlt"));
            FRDGBufferRef KeyBufferAlt = CreateSortBuffer(TEXT("GaussianSplat.KeyBufferAlt"));
            FRDGBufferDesc IndirectArgsDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 4);
            IndirectArgsDesc.Usage |= BUF_DrawIndirect | BUF_UnorderedAccess | BUF_SourceCopy;
            FRDGBufferRef IndirectArgsBuffer = GraphBuilder.CreateBuffer(
                IndirectArgsDesc,
                TEXT("GaussianSplat.IndirectArgs"));

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
            AddClearUAVPass(
                GraphBuilder,
                GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsBuffer, PF_R32_UINT)),
                0u);

            const FVector3f ViewOrigin = static_cast<FVector3f>(View.ViewMatrices.GetViewOrigin());
            const FVector3f Forward = static_cast<FVector3f>(View.GetViewDirection());

            if (Batch.RenderMode == EGaussianSplatRenderMode::Points)
            {
                FGaussianSplatPointsCullCS::FParameters* InitSortParameters = GraphBuilder.AllocParameters<FGaussianSplatPointsCullCS::FParameters>();
                InitSortParameters->NumElements = RenderPointCount;
                InitSortParameters->PaddedNumElements = PaddedPointCount;
                InitSortParameters->Stride = Stride;
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
                InitSortParameters->SplatPositionBuffer = Resources->GetPositionSRV();
                InitSortParameters->SortKeyShift = 32u - GaussianSplatProfiling::GetSortKeyBits();
                InitSortParameters->SplatOrderBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OrderBuffer, PF_R32_UINT));
                InitSortParameters->SplatKeyBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(KeyBuffer, PF_R32_UINT));
                InitSortParameters->SplatIndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsBuffer, PF_R32_UINT));

                FComputeShaderUtils::AddPass(
                    GraphBuilder,
                    RDG_EVENT_NAME("GaussianSplatPointsSort.Init"),
                    PointsCullCS,
                    InitSortParameters,
                    FComputeShaderUtils::GetGroupCount(PaddedPointCount, 64));

                FRDGBufferRef SortedOrderBuffer = OrderBuffer;
                if (GaussianSplatProfiling::ShouldUseRadixSort())
                {
                    SortedOrderBuffer = GaussianSplatSorting::AddRadixSortPass(
                        GraphBuilder,
                        View.GetFeatureLevel(),
                        OrderBuffer,
                        OrderBufferAlt,
                        KeyBuffer,
                        KeyBufferAlt,
                        RenderPointCount);
                }
                else if (!GaussianSplatProfiling::ShouldSkipSort())
                {
                    for (uint32 K = 2; K <= PaddedPointCount; K <<= 1)
                    {
                        for (uint32 J = K >> 1; J > 0; J >>= 1)
                        {
                            FGaussianSplatPointsCullCS::FParameters* SortParameters = GraphBuilder.AllocParameters<FGaussianSplatPointsCullCS::FParameters>();
                            SortParameters->NumElements = RenderPointCount;
                            SortParameters->PaddedNumElements = PaddedPointCount;
                            SortParameters->Stride = Stride;
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
                            SortParameters->SplatPositionBuffer = Resources->GetPositionSRV();
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

                FGaussianSplatPointsRasterPassParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianSplatPointsRasterPassParameters>();
                FGaussianSplatPointsRasterVS::FParameters* RasterParameters = &PassParameters->VS;
                RasterParameters->ViewRectMin = FVector2f(static_cast<float>(ViewRect.Min.X), static_cast<float>(ViewRect.Min.Y));
                RasterParameters->ViewSize = FVector2f(static_cast<float>(ViewRect.Width()), static_cast<float>(ViewRect.Height()));
                RasterParameters->PointSize = Batch.PointSize;
                RasterParameters->OpacityScale = Batch.OpacityScale;
                RasterParameters->Stride = Stride;
                RasterParameters->LocalToWorldMatrix = Batch.LocalToWorld;
                RasterParameters->ViewProjectionMatrix = ViewProjection;
                RasterParameters->SplatOrderBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SortedOrderBuffer, PF_R32_UINT));
                RasterParameters->SplatPositionBuffer = Resources->GetPositionSRV();
                RasterParameters->SplatColorBuffer = Resources->GetColorSRV();
                SetDepthTestParameters(PassParameters->PS.DepthTest);
                PassParameters->IndirectArgsBuffer = IndirectArgsBuffer;
                PassParameters->RenderTargets[0] = FRenderTargetBinding(
                    SplatOutput.Texture,
                    bFirstBatch ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad);

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
                InitSortParameters->NumElements = RenderPointCount;
                InitSortParameters->PaddedNumElements = PaddedPointCount;
                InitSortParameters->Stride = Stride;
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
                InitSortParameters->SplatPositionBuffer = Resources->GetPositionSRV();
                InitSortParameters->SplatCovariance0Buffer = Resources->GetCovariance0SRV();
                InitSortParameters->SplatCovariance1Buffer = Resources->GetCovariance1SRV();
                InitSortParameters->SortKeyShift = 32u - GaussianSplatProfiling::GetSortKeyBits();
                InitSortParameters->SplatOrderBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OrderBuffer, PF_R32_UINT));
                InitSortParameters->SplatKeyBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(KeyBuffer, PF_R32_UINT));
                InitSortParameters->SplatIndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsBuffer, PF_R32_UINT));

                FComputeShaderUtils::AddPass(
                    GraphBuilder,
                    RDG_EVENT_NAME("GaussianSplatBillboardsSort.Init"),
                    BillboardsCullCS,
                    InitSortParameters,
                    FComputeShaderUtils::GetGroupCount(PaddedPointCount, 64));

                FRDGBufferRef SortedOrderBuffer = OrderBuffer;
                if (GaussianSplatProfiling::ShouldUseRadixSort())
                {
                    SortedOrderBuffer = GaussianSplatSorting::AddRadixSortPass(
                        GraphBuilder,
                        View.GetFeatureLevel(),
                        OrderBuffer,
                        OrderBufferAlt,
                        KeyBuffer,
                        KeyBufferAlt,
                        RenderPointCount);
                }
                else if (!GaussianSplatProfiling::ShouldSkipSort())
                {
                    for (uint32 K = 2; K <= PaddedPointCount; K <<= 1)
                    {
                        for (uint32 J = K >> 1; J > 0; J >>= 1)
                        {
                            FGaussianSplatBillboardsCullCS::FParameters* SortParameters = GraphBuilder.AllocParameters<FGaussianSplatBillboardsCullCS::FParameters>();
                            SortParameters->NumElements = RenderPointCount;
                            SortParameters->PaddedNumElements = PaddedPointCount;
                            SortParameters->Stride = Stride;
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
                            SortParameters->SplatPositionBuffer = Resources->GetPositionSRV();
                            SortParameters->SplatCovariance0Buffer = Resources->GetCovariance0SRV();
                            SortParameters->SplatCovariance1Buffer = Resources->GetCovariance1SRV();
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

                FGaussianSplatBillboardsRasterPassParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianSplatBillboardsRasterPassParameters>();
                FGaussianSplatBillboardsRasterVS::FParameters* RasterParameters = &PassParameters->VS;
                RasterParameters->ViewRectMin = FVector2f(static_cast<float>(ViewRect.Min.X), static_cast<float>(ViewRect.Min.Y));
                RasterParameters->ViewSize = FVector2f(static_cast<float>(ViewRect.Width()), static_cast<float>(ViewRect.Height()));
                RasterParameters->ViewWorldOrigin = FVector4f(ViewOrigin.X, ViewOrigin.Y, ViewOrigin.Z, 0.0f);
                RasterParameters->PointSize = Batch.PointSize;
                RasterParameters->OpacityScale = Batch.OpacityScale;
                RasterParameters->Stride = Stride;
                RasterParameters->ViewMatrix = ViewMatrix;
                RasterParameters->LocalToWorldMatrix = Batch.LocalToWorld;
                RasterParameters->ProjectionMatrix = ProjectionMatrix;
                RasterParameters->ViewProjectionMatrix = ViewProjection;
                RasterParameters->WorldToLocalRow0 = Batch.WorldToLocalRow0;
                RasterParameters->WorldToLocalRow1 = Batch.WorldToLocalRow1;
                RasterParameters->WorldToLocalRow2 = Batch.WorldToLocalRow2;
                RasterParameters->SplatOrderBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SortedOrderBuffer, PF_R32_UINT));
                RasterParameters->SplatPositionBuffer = Resources->GetPositionSRV();
                RasterParameters->SplatCovariance0Buffer = Resources->GetCovariance0SRV();
                RasterParameters->SplatCovariance1Buffer = Resources->GetCovariance1SRV();
                RasterParameters->SplatColorBuffer = Resources->GetColorSRV();
                RasterParameters->PerPixelDepth = GaussianSplatProfiling::ShouldUsePerPixelDepth() ? 1u : 0u;
                RasterParameters->SplatSHBuffer = Resources->GetSHSRV();
                SetDepthTestParameters(PassParameters->PS.DepthTest);
                PassParameters->IndirectArgsBuffer = IndirectArgsBuffer;
                PassParameters->RenderTargets[0] = FRenderTargetBinding(
                    SplatOutput.Texture,
                    bFirstBatch ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad);

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

            if (bFirstBatch)
            {
                GaussianSplatProfiling::EnqueueVisibleCountReadback(
                    GraphBuilder,
                    IndirectArgsBuffer,
                    RenderPointCount,
                    PaddedPointCount);
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
