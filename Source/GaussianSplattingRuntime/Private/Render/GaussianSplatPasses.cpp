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

            FRDGBufferRef OrderBuffer = GraphBuilder.CreateBuffer(
                FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), PaddedPointCount),
                TEXT("GaussianSplat.OrderBuffer"));
            FRDGBufferRef KeyBuffer = GraphBuilder.CreateBuffer(
                FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), PaddedPointCount),
                TEXT("GaussianSplat.KeyBuffer"));
            FRDGBufferDesc IndirectArgsDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 4);
            IndirectArgsDesc.Usage |= BUF_DrawIndirect | BUF_UnorderedAccess;
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
                InitSortParameters->SplatOrderBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OrderBuffer, PF_R32_UINT));
                InitSortParameters->SplatKeyBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(KeyBuffer, PF_R32_UINT));
                InitSortParameters->SplatIndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsBuffer, PF_R32_UINT));

                FComputeShaderUtils::AddPass(
                    GraphBuilder,
                    RDG_EVENT_NAME("GaussianSplatPointsSort.Init"),
                    PointsCullCS,
                    InitSortParameters,
                    FComputeShaderUtils::GetGroupCount(PaddedPointCount, 64));

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

                FGaussianSplatPointsRasterPassParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianSplatPointsRasterPassParameters>();
                FGaussianSplatPointsRasterVS::FParameters* RasterParameters = &PassParameters->VS;
                RasterParameters->ViewRectMin = FVector2f(static_cast<float>(ViewRect.Min.X), static_cast<float>(ViewRect.Min.Y));
                RasterParameters->ViewSize = FVector2f(static_cast<float>(ViewRect.Width()), static_cast<float>(ViewRect.Height()));
                RasterParameters->PointSize = Batch.PointSize;
                RasterParameters->OpacityScale = Batch.OpacityScale;
                RasterParameters->Stride = Stride;
                RasterParameters->LocalToWorldMatrix = Batch.LocalToWorld;
                RasterParameters->ViewProjectionMatrix = ViewProjection;
                RasterParameters->SplatOrderBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(OrderBuffer, PF_R32_UINT));
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
                InitSortParameters->SplatOrderBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OrderBuffer, PF_R32_UINT));
                InitSortParameters->SplatKeyBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(KeyBuffer, PF_R32_UINT));
                InitSortParameters->SplatIndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsBuffer, PF_R32_UINT));

                FComputeShaderUtils::AddPass(
                    GraphBuilder,
                    RDG_EVENT_NAME("GaussianSplatBillboardsSort.Init"),
                    BillboardsCullCS,
                    InitSortParameters,
                    FComputeShaderUtils::GetGroupCount(PaddedPointCount, 64));

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
                RasterParameters->SplatOrderBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(OrderBuffer, PF_R32_UINT));
                RasterParameters->SplatPositionBuffer = Resources->GetPositionSRV();
                RasterParameters->SplatCovariance0Buffer = Resources->GetCovariance0SRV();
                RasterParameters->SplatCovariance1Buffer = Resources->GetCovariance1SRV();
                RasterParameters->SplatColorBuffer = Resources->GetColorSRV();
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
