#include "Render/GaussianSplatPasses.h"

#include "./GaussianSplatShaders.h"
#include "Render/GaussianSplatRenderResources.h"
#include "PipelineStateCache.h"
#include "PixelShaderUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderGraphResources.h"
#include "RenderUtils.h"
#include "ScreenPass.h"

namespace GaussianSplatPasses
{
    FScreenPassTexture AddPostProcessPass(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        const FScreenPassTexture& SceneColor,
        const FScreenPassRenderTarget& Output,
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

        const FVector2f SceneColorTextureSize(
            static_cast<float>(FMath::Max(1, SceneColorExtent.X)),
            static_cast<float>(FMath::Max(1, SceneColorExtent.Y)));

        TShaderMapRef<FGaussianSplatCullSortCS> CullSortCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        TShaderMapRef<FGaussianSplatRasterVS> RasterVS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        TShaderMapRef<FGaussianSplatRasterPS> RasterPS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
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

            FGaussianSplatCullSortCS::FParameters* InitSortParameters = GraphBuilder.AllocParameters<FGaussianSplatCullSortCS::FParameters>();
            InitSortParameters->NumElements = RenderPointCount;
            InitSortParameters->PaddedNumElements = PaddedPointCount;
            InitSortParameters->Stride = Stride;
            InitSortParameters->SortK = 0;
            InitSortParameters->SortJ = 0;
            InitSortParameters->PassType = 0;
            InitSortParameters->ViewWorldOrigin = FVector3f(static_cast<FVector3f>(View.ViewMatrices.GetViewOrigin()));
            InitSortParameters->ViewForward = FVector3f(static_cast<FVector3f>(View.GetViewDirection()));
            InitSortParameters->LocalToWorldMatrix = Batch.LocalToWorld;
            InitSortParameters->SplatPositionBuffer = Resources->GetPositionSRV();
            InitSortParameters->SplatOrderBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OrderBuffer, PF_R32_UINT));
            InitSortParameters->SplatKeyBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(KeyBuffer, PF_R32_UINT));

            FComputeShaderUtils::AddPass(
                GraphBuilder,
                RDG_EVENT_NAME("GaussianSplatSort.Init"),
                CullSortCS,
                InitSortParameters,
                FComputeShaderUtils::GetGroupCount(PaddedPointCount, 64));

            for (uint32 K = 2; K <= PaddedPointCount; K <<= 1)
            {
                for (uint32 J = K >> 1; J > 0; J >>= 1)
                {
                    FGaussianSplatCullSortCS::FParameters* SortParameters = GraphBuilder.AllocParameters<FGaussianSplatCullSortCS::FParameters>();
                    SortParameters->NumElements = RenderPointCount;
                    SortParameters->PaddedNumElements = PaddedPointCount;
                    SortParameters->Stride = Stride;
                    SortParameters->SortK = K;
                    SortParameters->SortJ = J;
                    SortParameters->PassType = 1;
                    SortParameters->ViewWorldOrigin = FVector3f::ZeroVector;
                    SortParameters->ViewForward = FVector3f::ForwardVector;
                    SortParameters->LocalToWorldMatrix = Batch.LocalToWorld;
                    SortParameters->SplatPositionBuffer = Resources->GetPositionSRV();
                    SortParameters->SplatOrderBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OrderBuffer, PF_R32_UINT));
                    SortParameters->SplatKeyBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(KeyBuffer, PF_R32_UINT));

                    FComputeShaderUtils::AddPass(
                        GraphBuilder,
                        RDG_EVENT_NAME("GaussianSplatSort.Bitonic K=%u J=%u", K, J),
                        CullSortCS,
                        SortParameters,
                        FComputeShaderUtils::GetGroupCount(PaddedPointCount, 64));
                }
            }

            FGaussianSplatRasterVS::FParameters* RasterParameters = GraphBuilder.AllocParameters<FGaussianSplatRasterVS::FParameters>();
            RasterParameters->ViewRectMin = FVector2f(static_cast<float>(ViewRect.Min.X), static_cast<float>(ViewRect.Min.Y));
            RasterParameters->ViewSize = FVector2f(static_cast<float>(ViewRect.Width()), static_cast<float>(ViewRect.Height()));
            RasterParameters->ViewWorldOrigin = FVector3f(static_cast<FVector3f>(View.ViewMatrices.GetViewOrigin()));
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
            RasterParameters->SplatRotationBuffer = Resources->GetRotationSRV();
            RasterParameters->SplatScaleBuffer = Resources->GetScaleSRV();
            RasterParameters->SplatColorBuffer = Resources->GetColorSRV();
            RasterParameters->SplatSHBuffer = Resources->GetSHSRV();
            RasterParameters->RenderTargets[0] = FRenderTargetBinding(
                SplatOutput.Texture,
                bFirstBatch ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad);

            GraphBuilder.AddPass(
                RDG_EVENT_NAME("GaussianSplatRaster.DrawInstanced"),
                RasterParameters,
                ERDGPassFlags::Raster,
                [RasterParameters, RasterVS, RasterPS, ViewRect, RenderPointCount](FRHICommandList& RHICmdList)
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
                    GraphicsPSOInit.BoundShaderState.VertexShaderRHI = RasterVS.GetVertexShader();
                    GraphicsPSOInit.BoundShaderState.PixelShaderRHI = RasterPS.GetPixelShader();

                    SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
                    SetShaderParameters(RHICmdList, RasterVS, RasterVS.GetVertexShader(), *RasterParameters);
                    SetShaderParameters(RHICmdList, RasterPS, RasterPS.GetPixelShader(), FGaussianSplatRasterPS::FParameters());
                    RHICmdList.DrawPrimitive(0, 2, RenderPointCount);
                });

            bFirstBatch = false;
        }

        FGaussianSplatCompositePS::FParameters* CompositeParameters = GraphBuilder.AllocParameters<FGaussianSplatCompositePS::FParameters>();
        CompositeParameters->SceneColorTextureSize = SceneColorTextureSize;
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

        return Output;
    }
}
