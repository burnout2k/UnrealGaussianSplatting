#include "Render/GaussianSplatPasses.h"

#include "./GaussianSplatShaders.h"
#include "PixelShaderUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

namespace GaussianSplatPasses
{
    void AddPreRenderPasses(FRDGBuilder& GraphBuilder, const FSceneViewFamily& ViewFamily, const TArray<FGaussianSplatRenderPoint>& Points)
    {
        if (!ViewFamily.RenderTarget || ViewFamily.Views.IsEmpty() || Points.IsEmpty())
        {
            return;
        }

        FRHITexture* SceneColorRHI = ViewFamily.RenderTarget->GetRenderTargetTexture();
        if (!SceneColorRHI)
        {
            return;
        }

        FRDGTextureRef SceneColor = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(SceneColorRHI, TEXT("GaussianSplat.SceneColor")));
        FRDGTextureRef SceneColorCopy = GraphBuilder.CreateTexture(SceneColor->Desc, TEXT("GaussianSplat.SceneColorCopy"));
        AddCopyTexturePass(GraphBuilder, SceneColor, SceneColorCopy);

        const int32 PointCount = FMath::Min(Points.Num(), 2048);
        if (PointCount <= 0)
        {
            return;
        }

        TArray<FVector4f> PositionRadiusData;
        TArray<FVector4f> ColorData;
        PositionRadiusData.Reserve(PointCount);
        ColorData.Reserve(PointCount);

        for (int32 Index = 0; Index < PointCount; ++Index)
        {
            PositionRadiusData.Add(Points[Index].PositionRadiusWS);
            ColorData.Add(Points[Index].Color);
        }

        FRDGBufferRef PositionRadiusBuffer = CreateStructuredBuffer(
            GraphBuilder,
            TEXT("GaussianSplat.PositionRadiusBuffer"),
            PositionRadiusData,
            ERDGInitialDataFlags::None);

        FRDGBufferRef ColorBuffer = CreateStructuredBuffer(
            GraphBuilder,
            TEXT("GaussianSplat.ColorBuffer"),
            ColorData,
            ERDGInitialDataFlags::None);

        const FIntRect ViewRect = ViewFamily.Views[0]->UnscaledViewRect;
        const float ViewHeight = FMath::Max(1.0f, static_cast<float>(ViewRect.Height()));
        const float ProjY = ViewFamily.Views[0]->ViewMatrices.GetProjectionMatrix().M[1][1];
        const FMatrix44f ViewProjection = FMatrix44f(ViewFamily.Views[0]->ViewMatrices.GetViewProjectionMatrix());

        FGaussianSplatRasterPS::FParameters* Parameters = GraphBuilder.AllocParameters<FGaussianSplatRasterPS::FParameters>();
        Parameters->PointCount = static_cast<uint32>(PointCount);
        Parameters->ViewSize = FVector2f(static_cast<float>(ViewRect.Width()), static_cast<float>(ViewRect.Height()));
        Parameters->ViewProjYScale = 0.5f * ViewHeight * ProjY;
        Parameters->ViewProjectionMatrix = ViewProjection;
        Parameters->SplatPositionRadiusBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(PositionRadiusBuffer, PF_A32B32G32R32F));
        Parameters->SplatColorBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ColorBuffer, PF_A32B32G32R32F));
        Parameters->SceneColorTexture = SceneColorCopy;
        Parameters->SceneColorSampler = TStaticSamplerState<SF_Point>::GetRHI();
        Parameters->RenderTargets[0] = FRenderTargetBinding(SceneColor, ERenderTargetLoadAction::ELoad);

        TShaderMapRef<FGaussianSplatRasterPS> PixelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FPixelShaderUtils::AddFullscreenPass(
            GraphBuilder,
            GetGlobalShaderMap(GMaxRHIFeatureLevel),
            RDG_EVENT_NAME("GaussianSplatRaster.Composite"),
            PixelShader,
            Parameters,
            ViewRect);
    }
}
