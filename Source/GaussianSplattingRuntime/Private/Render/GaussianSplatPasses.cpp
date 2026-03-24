#include "Render/GaussianSplatPasses.h"

#include "./GaussianSplatShaders.h"
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
        const TArray<FGaussianSplatRenderPoint>& Points)
    {
        if (!SceneColor.IsValid() || !Output.IsValid() || Points.IsEmpty())
        {
            return SceneColor;
        }

        const int32 PointCount = Points.Num();
        if (PointCount <= 0)
        {
            return SceneColor;
        }

        const uint32 PaddedPointCount = FMath::RoundUpToPowerOfTwo(FMath::Max(1, PointCount));

        TArray<FVector4f> PositionData;
        TArray<FVector4f> Cov3D0Data;
        TArray<FVector4f> Cov3D1Data;
        TArray<FVector4f> ColorData;
        TArray<FVector4f> WorldToLocalRow0Data;
        TArray<FVector4f> WorldToLocalRow1Data;
        TArray<FVector4f> WorldToLocalRow2Data;
        TArray<FVector4f> SHData;
        PositionData.Reserve(PointCount);
        Cov3D0Data.Reserve(PointCount);
        Cov3D1Data.Reserve(PointCount);
        ColorData.Reserve(PointCount);
        WorldToLocalRow0Data.Reserve(PointCount);
        WorldToLocalRow1Data.Reserve(PointCount);
        WorldToLocalRow2Data.Reserve(PointCount);
        SHData.Reserve(PointCount * 15);

        for (int32 Index = 0; Index < PointCount; ++Index)
        {
            PositionData.Add(Points[Index].PositionWS);
            Cov3D0Data.Add(Points[Index].Cov3D0);
            Cov3D1Data.Add(Points[Index].Cov3D1);
            ColorData.Add(Points[Index].Color);
            WorldToLocalRow0Data.Add(Points[Index].WorldToLocalRow0);
            WorldToLocalRow1Data.Add(Points[Index].WorldToLocalRow1);
            WorldToLocalRow2Data.Add(Points[Index].WorldToLocalRow2);
            for (int32 SHIndex = 0; SHIndex < 15; ++SHIndex)
            {
                SHData.Add(Points[Index].SHCoefficients[SHIndex]);
            }
        }

        FRDGBufferRef PositionBuffer = CreateStructuredBuffer(
            GraphBuilder,
            TEXT("GaussianSplat.PositionBuffer"),
            PositionData,
            ERDGInitialDataFlags::None);

        FRDGBufferRef Cov3D0Buffer = CreateStructuredBuffer(
            GraphBuilder,
            TEXT("GaussianSplat.Cov3D0Buffer"),
            Cov3D0Data,
            ERDGInitialDataFlags::None);

        FRDGBufferRef Cov3D1Buffer = CreateStructuredBuffer(
            GraphBuilder,
            TEXT("GaussianSplat.Cov3D1Buffer"),
            Cov3D1Data,
            ERDGInitialDataFlags::None);

        FRDGBufferRef ColorBuffer = CreateStructuredBuffer(
            GraphBuilder,
            TEXT("GaussianSplat.ColorBuffer"),
            ColorData,
            ERDGInitialDataFlags::None);

        FRDGBufferRef WorldToLocalRow0Buffer = CreateStructuredBuffer(
            GraphBuilder,
            TEXT("GaussianSplat.WorldToLocalRow0Buffer"),
            WorldToLocalRow0Data,
            ERDGInitialDataFlags::None);

        FRDGBufferRef WorldToLocalRow1Buffer = CreateStructuredBuffer(
            GraphBuilder,
            TEXT("GaussianSplat.WorldToLocalRow1Buffer"),
            WorldToLocalRow1Data,
            ERDGInitialDataFlags::None);

        FRDGBufferRef WorldToLocalRow2Buffer = CreateStructuredBuffer(
            GraphBuilder,
            TEXT("GaussianSplat.WorldToLocalRow2Buffer"),
            WorldToLocalRow2Data,
            ERDGInitialDataFlags::None);

        FRDGBufferRef SHBuffer = CreateStructuredBuffer(
            GraphBuilder,
            TEXT("GaussianSplat.SHBuffer"),
            SHData,
            ERDGInitialDataFlags::None);

        FRDGBufferRef OrderBuffer = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), PaddedPointCount),
            TEXT("GaussianSplat.OrderBuffer"));
        FRDGBufferRef KeyBuffer = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), PaddedPointCount),
            TEXT("GaussianSplat.KeyBuffer"));

        const FIntRect ViewRect = SceneColor.ViewRect;
        const FMatrix ViewMatrixD = View.ViewMatrices.GetViewMatrix();
        const FMatrix ProjectionMatrixNoAAD = View.ViewMatrices.GetProjectionNoAAMatrix();
        const FMatrix ViewProjectionNoAAD = ViewMatrixD * ProjectionMatrixNoAAD;
        const FMatrix44f ViewMatrix = FMatrix44f(ViewMatrixD);
        const FMatrix44f ProjectionMatrix = FMatrix44f(ProjectionMatrixNoAAD);
        const FMatrix44f ViewProjection = FMatrix44f(ViewProjectionNoAAD);
        const FIntPoint SceneColorExtent = SceneColor.Texture->Desc.Extent;

        FGaussianSplatCullSortCS::FParameters* InitSortParameters = GraphBuilder.AllocParameters<FGaussianSplatCullSortCS::FParameters>();
        InitSortParameters->NumElements = PointCount;
        InitSortParameters->PaddedNumElements = PaddedPointCount;
        InitSortParameters->SortK = 0;
        InitSortParameters->SortJ = 0;
        InitSortParameters->PassType = 0;
        InitSortParameters->ViewWorldOrigin = FVector3f(static_cast<FVector3f>(View.ViewMatrices.GetViewOrigin()));
        InitSortParameters->ViewForward = FVector3f(static_cast<FVector3f>(View.GetViewDirection()));
        InitSortParameters->SplatPositionBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(PositionBuffer, PF_A32B32G32R32F));
        InitSortParameters->SplatOrderBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OrderBuffer, PF_R32_UINT));
        InitSortParameters->SplatKeyBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(KeyBuffer, PF_R32_UINT));

        TShaderMapRef<FGaussianSplatCullSortCS> CullSortCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
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
                SortParameters->NumElements = PointCount;
                SortParameters->PaddedNumElements = PaddedPointCount;
                SortParameters->SortK = K;
                SortParameters->SortJ = J;
                SortParameters->PassType = 1;
                SortParameters->ViewWorldOrigin = FVector3f::ZeroVector;
                SortParameters->ViewForward = FVector3f::ForwardVector;
                SortParameters->SplatPositionBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(PositionBuffer, PF_A32B32G32R32F));
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

        FRDGTextureDesc SplatTextureDesc = FRDGTextureDesc::Create2D(
            SceneColorExtent,
            PF_FloatRGBA,
            FClearValueBinding(FLinearColor::Transparent),
            TexCreate_ShaderResource | TexCreate_RenderTargetable);
        FRDGTextureRef SplatTexture = GraphBuilder.CreateTexture(SplatTextureDesc, TEXT("GaussianSplat.SplatTexture"));
        const FScreenPassRenderTarget SplatOutput(SplatTexture, ERenderTargetLoadAction::EClear);

        FGaussianSplatRasterVS::FParameters* RasterParameters = GraphBuilder.AllocParameters<FGaussianSplatRasterVS::FParameters>();
        RasterParameters->ViewRectMin = FVector2f(static_cast<float>(ViewRect.Min.X), static_cast<float>(ViewRect.Min.Y));
        RasterParameters->ViewSize = FVector2f(static_cast<float>(ViewRect.Width()), static_cast<float>(ViewRect.Height()));
        RasterParameters->ViewWorldOrigin = FVector3f(static_cast<FVector3f>(View.ViewMatrices.GetViewOrigin()));
        RasterParameters->PreExposure = FMath::Max(View.GetLastEyeAdaptationExposure(), 1.0e-4f);
        RasterParameters->ViewMatrix = ViewMatrix;
        RasterParameters->ProjectionMatrix = ProjectionMatrix;
        RasterParameters->ViewProjectionMatrix = ViewProjection;
        RasterParameters->SplatOrderBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(OrderBuffer, PF_R32_UINT));
        RasterParameters->SplatPositionBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(PositionBuffer, PF_A32B32G32R32F));
        RasterParameters->SplatCov3D0Buffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Cov3D0Buffer, PF_A32B32G32R32F));
        RasterParameters->SplatCov3D1Buffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Cov3D1Buffer, PF_A32B32G32R32F));
        RasterParameters->SplatColorBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ColorBuffer, PF_A32B32G32R32F));
        RasterParameters->SplatWorldToLocalRow0Buffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(WorldToLocalRow0Buffer, PF_A32B32G32R32F));
        RasterParameters->SplatWorldToLocalRow1Buffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(WorldToLocalRow1Buffer, PF_A32B32G32R32F));
        RasterParameters->SplatWorldToLocalRow2Buffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(WorldToLocalRow2Buffer, PF_A32B32G32R32F));
        RasterParameters->SplatSHBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SHBuffer, PF_A32B32G32R32F));
        RasterParameters->RenderTargets[0] = FRenderTargetBinding(SplatOutput.Texture, SplatOutput.LoadAction);

        const FVector2f SceneColorTextureSize(
            static_cast<float>(FMath::Max(1, SceneColorExtent.X)),
            static_cast<float>(FMath::Max(1, SceneColorExtent.Y)));

        TShaderMapRef<FGaussianSplatRasterVS> RasterVS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        TShaderMapRef<FGaussianSplatRasterPS> RasterPS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        GraphBuilder.AddPass(
            RDG_EVENT_NAME("GaussianSplatRaster.DrawInstanced"),
            RasterParameters,
            ERDGPassFlags::Raster,
            [RasterParameters, RasterVS, RasterPS, ViewRect, PointCount](FRHICommandList& RHICmdList)
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
                RHICmdList.DrawPrimitive(0, 2, PointCount);
            });

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
