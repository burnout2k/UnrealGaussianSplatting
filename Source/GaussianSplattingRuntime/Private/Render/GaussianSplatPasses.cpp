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
#include "ScreenPass.h"

namespace GaussianSplatPasses
{
    // 这个函数是 billboard 模式 Gaussian 的渲染总入口。
    // 它运行在 ViewExtension 注册的后处理阶段，不直接遍历 UObject，而是消费游戏线程提前构建好的 Batches 快照。
    //
    // 参数来源：
    // 1. GraphBuilder
    //    由 FGaussianSplatViewExtension::PostProcessPass_RenderThread() 传入。
    //    它是本帧当前 View 的 RDG 构图器，本函数往里面追加 compute / raster / fullscreen pass。
    // 2. View
    //    同样来自 ViewExtension 的后处理回调，是当前正在渲染的视图。
    //    本函数从中提取视图矩阵、投影矩阵、相机位置、视口大小等 shader 常量。
    // 3. SceneColor
    //    来自 ViewExtension 对 Inputs.SceneColor 的包装。
    //    它代表当前后处理链走到这一阶段时的场景颜色输入，也是最终合成 Gaussian 的底图。
    // 4. Output
    //    来自 ViewExtension 的 Inputs.OverrideOutput 或基于 SceneColor 创建的默认输出 RT。
    //    本函数最后把合成结果写到这里，并把它作为返回值交回 UE 的后处理链。
    // 5. Batches
    //    来自 ViewExtension::BuildPointSnapshot_GameThread() 在游戏线程采样出的只读快照。
    //    每个 Batch 基本对应一个当前可渲染的 UGaussianSplatComponent，里面只保留渲染线程需要的数据：
    //    RenderResources、LocalToWorld、WorldToLocal、PointSize、OpacityScale、Stride、MaxRenderPoints 等。
    //
    // 整体流程：
    // 1. 为所有 Gaussian 创建一个独立的中间累积纹理 SplatTexture。
    // 2. 对每个 Batch 运行一次“可见性筛选 + 深度排序 + 间接绘制参数生成”。
    // 3. 对每个 Batch 运行一次 raster pass，把高斯 billboard 画到 SplatTexture。
    // 4. 最后跑一次全屏 composite pass，把 SplatTexture 叠回 SceneColor/Output。
    //
    // shader 调用点：
    // 1. FComputeShaderUtils::AddPass(...)
    //    调用 FGaussianSplatCullSortCS，对应 /Shaders/Private/GaussianSplatCullSort.usf 的 MainCS。
    //    第一次 PassType=0 做可见性筛选和初始化，后续多次 PassType=1 做 bitonic sort。
    // 2. GraphBuilder.AddPass(..., ERDGPassFlags::Raster, lambda)
    //    在 lambda 里绑定 FGaussianSplatRasterVS / FGaussianSplatRasterPS，
    //    对应 /Shaders/Private/GaussianSplatRaster.usf 的 MainVS / MainPS。
    // 3. FPixelShaderUtils::AddFullscreenPass(...)
    //    调用 FGaussianSplatCompositePS，对应 /Shaders/Private/GaussianSplatComposite.usf 的 MainPS。
    FScreenPassTexture AddPostProcessPass(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        const FScreenPassTexture& SceneColor,
        const FScreenPassRenderTarget& Output,
        const TArray<FGaussianSplatRenderBatch>& Batches)
    {
        // 任一关键输入无效时直接退回原始 SceneColor。
        // 这里不报错，而是让后处理链继续走，避免 Gaussian 插件把整个视图渲染搞挂。
        if (!SceneColor.IsValid() || !Output.IsValid() || Batches.IsEmpty())
        {
            return SceneColor;
        }

        // 先从 View 和 SceneColor 中提取本函数后续多个 pass 都会重复使用的公共参数。
        // 这些值最终会被写进 shader 参数结构：
        // - ViewRect：当前视图在目标纹理中的像素矩形
        // - ViewMatrix / ProjectionMatrix / ViewProjection：用于世界空间 -> 裁剪空间/屏幕空间投影
        // - SceneColorExtent：中间纹理和最终合成纹理的大小
        const FIntRect ViewRect = SceneColor.ViewRect;
        const FMatrix ViewMatrixD = View.ViewMatrices.GetViewMatrix();
        const FMatrix ProjectionMatrixNoAAD = View.ViewMatrices.GetProjectionNoAAMatrix();
        const FMatrix ViewProjectionNoAAD = ViewMatrixD * ProjectionMatrixNoAAD;
        const FMatrix44f ViewMatrix = FMatrix44f(ViewMatrixD);
        const FMatrix44f ProjectionMatrix = FMatrix44f(ProjectionMatrixNoAAD);
        const FMatrix44f ViewProjection = FMatrix44f(ViewProjectionNoAAD);
        const FIntPoint SceneColorExtent = SceneColor.Texture->Desc.Extent;

        // 所有 Gaussian 先画到独立的浮点 RT，再统一与 SceneColor 合成。
        // 这样做而不是直接写 Output，有几个原因：
        // 1. 便于多个 Batch 在同一个中间目标上做透明累积。
        // 2. 避免在还没完成全部 Batch 前就污染后处理链的主输出。
        // 3. 让最后一步全屏合成逻辑保持简单，后续如果要改 blend 策略也更集中。
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

        // 这里拿到本函数会用到的 shader 编译结果句柄。
        // 真正“调用 shader”的动作不是这几行本身，而是后面的 AddPass / SetShaderParameters / AddFullscreenPass。
        TShaderMapRef<FGaussianSplatCullSortCS> CullSortCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        TShaderMapRef<FGaussianSplatRasterVS> RasterVS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        TShaderMapRef<FGaussianSplatRasterPS> RasterPS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        bool bFirstBatch = true;

        // 每个 Batch 基本对应一个组件的一批高斯。
        // 这里按 Batch 循环，是因为每个组件的变换、点大小、透明度和采样密度都可能不同。
        for (const FGaussianSplatRenderBatch& Batch : Batches)
        {
            const FGaussianSplatRenderResources* Resources = Batch.Resources;

            // RenderResources 由 UGaussianSplatAsset 持有并在加载/刷新时创建。
            // 其中封装了 Position / Rotation / Scale / Color / SH 的 GPU StructuredBuffer 和对应 SRV。
            if (Resources == nullptr || Resources->GetPointCount() == 0 || Resources->GetPositionSRV() == nullptr)
            {
                continue;
            }

            // Stride / AssetPointCount / MaxRenderPoints 都来自 Batch：
            // - AssetPointCount：资源原始总点数
            // - Stride：密度抽样步长，例如 4 表示每 4 个点取 1 个
            // - MaxRenderPoints：组件级上限，避免一次绘制太多
            //
            // 这里先算出“理论上最多会参与本批处理的点数”，还不是最终可见点数。
            // 最终真正要画多少实例，要等 compute shader 把 indirect args 写出来。
            const uint32 Stride = FMath::Max(1u, Batch.Stride);
            const uint32 RenderPointCount = FMath::Min(
                Batch.MaxRenderPoints,
                FMath::DivideAndRoundUp(Batch.AssetPointCount, Stride));
            if (RenderPointCount == 0)
            {
                continue;
            }

            // bitonic sort 要求排序数组长度是 2 的幂。
            // 所以这里把逻辑点数扩充到 >= RenderPointCount 的最近一个 2^n。
            const uint32 PaddedPointCount = FMath::RoundUpToPowerOfTwo(FMath::Max(1u, RenderPointCount));

            // 这三个 RDG buffer 是本批次 compute/raster 阶段共享的中间数据：
            // 1. OrderBuffer：保存排序后的“逻辑实例索引”
            // 2. KeyBuffer：保存与之对应的深度排序 key
            // 3. IndirectArgsBuffer：保存 DrawPrimitiveIndirect 参数，供后面的 raster pass 直接发起间接绘制
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

            // RDG 创建出的 UAV（无序访问视图）内容默认未定义，必须先初始化。
            // - OrderBuffer 清 0
            // - KeyBuffer 清成 0xffffffff，作为“极远/无效”的哨兵值
            // - IndirectArgsBuffer 清 0，稍后由 compute shader 写成 DrawPrimitiveIndirect 需要的四元组
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

            // 这是第一次 compute shader 调用。
            // 对应 shader：FGaussianSplatCullSortCS
            // 对应文件：Shaders/Private/GaussianSplatCullSort.usf
            // 对应入口：MainCS
            //
            // PassType == 0 时，这个 shader 负责：
            // 1. 把逻辑实例索引乘以 Stride，映射回真实资产点索引
            // 2. 做简单可见性判断：深度、屏幕包围盒、投影合法性
            // 3. 为可见点写入排序 key 和原始索引
            // 4. 通过原子加法写出最终可见实例数到 IndirectArgsBuffer
            FGaussianSplatCullSortCS::FParameters* InitSortParameters = GraphBuilder.AllocParameters<FGaussianSplatCullSortCS::FParameters>();
            InitSortParameters->NumElements = RenderPointCount;
            InitSortParameters->PaddedNumElements = PaddedPointCount;
            InitSortParameters->Stride = Stride;
            InitSortParameters->SortK = 0;
            InitSortParameters->SortJ = 0;
            InitSortParameters->PassType = 0;
            {
                // 这些值来自当前 View，用于在 compute shader 中估计深度和判断投影后是否落在当前视口内。
                const FVector3f ViewOrigin = static_cast<FVector3f>(View.ViewMatrices.GetViewOrigin());
                const FVector3f Forward = static_cast<FVector3f>(View.GetViewDirection());
                InitSortParameters->ViewWorldOrigin = FVector4f(ViewOrigin.X, ViewOrigin.Y, ViewOrigin.Z, 0.0f);
                InitSortParameters->ViewForward = FVector4f(Forward.X, Forward.Y, Forward.Z, 0.0f);
            }
            InitSortParameters->ViewRectMin = FVector2f(static_cast<float>(ViewRect.Min.X), static_cast<float>(ViewRect.Min.Y));
            InitSortParameters->ViewSize = FVector2f(static_cast<float>(ViewRect.Width()), static_cast<float>(ViewRect.Height()));
            InitSortParameters->PointSize = Batch.PointSize;
            InitSortParameters->ViewMatrix = ViewMatrix;
            InitSortParameters->ProjectionMatrix = ProjectionMatrix;
            InitSortParameters->ViewProjectionMatrix = ViewProjection;

            // LocalToWorld 来自 Batch，也就是来自对应组件当前帧的变换快照。
            // compute shader 会用它把资产本地空间中的点投到世界空间，再进一步投影到屏幕。
            InitSortParameters->LocalToWorldMatrix = Batch.LocalToWorld;

            // 这些 SRV 来自 FGaussianSplatRenderResources，是资产上传到 GPU 后的只读结构化缓冲。
            InitSortParameters->SplatPositionBuffer = Resources->GetPositionSRV();
            InitSortParameters->SplatRotationBuffer = Resources->GetRotationSRV();
            InitSortParameters->SplatScaleBuffer = Resources->GetScaleSRV();

            // 这些 UAV 是这次 compute pass 的输出目标。
            InitSortParameters->SplatOrderBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OrderBuffer, PF_R32_UINT));
            InitSortParameters->SplatKeyBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(KeyBuffer, PF_R32_UINT));
            InitSortParameters->SplatIndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsBuffer, PF_R32_UINT));

            // 真正提交 compute shader 的地方在这里。
            FComputeShaderUtils::AddPass(
                GraphBuilder,
                RDG_EVENT_NAME("GaussianSplatSort.Init"),
                CullSortCS,
                InitSortParameters,
                FComputeShaderUtils::GetGroupCount(PaddedPointCount, 64));

            // 后续多轮 compute pass 做 bitonic sort。
            // 仍然调用同一个 compute shader，只是把 PassType 改成 1，并传入当前轮次的 K/J。
            // shader 内部会走 compare-and-swap 分支，把 KeyBuffer / OrderBuffer 按深度重排。
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
                    SortParameters->ViewWorldOrigin = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
                    SortParameters->ViewForward = FVector4f(1.0f, 0.0f, 0.0f, 0.0f);
                    SortParameters->ViewRectMin = FVector2f::ZeroVector;
                    SortParameters->ViewSize = FVector2f::ZeroVector;
                    SortParameters->PointSize = 0.0f;
                    SortParameters->ViewMatrix = FMatrix44f::Identity;
                    SortParameters->ProjectionMatrix = FMatrix44f::Identity;
                    SortParameters->ViewProjectionMatrix = FMatrix44f::Identity;
                    SortParameters->LocalToWorldMatrix = Batch.LocalToWorld;
                    SortParameters->SplatPositionBuffer = Resources->GetPositionSRV();
                    SortParameters->SplatRotationBuffer = Resources->GetRotationSRV();
                    SortParameters->SplatScaleBuffer = Resources->GetScaleSRV();
                    SortParameters->SplatOrderBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OrderBuffer, PF_R32_UINT));
                    SortParameters->SplatKeyBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(KeyBuffer, PF_R32_UINT));
                    SortParameters->SplatIndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsBuffer, PF_R32_UINT));

                    // 这里是第二类 compute shader 调用点：同一个 shader 的排序阶段。
                    FComputeShaderUtils::AddPass(
                        GraphBuilder,
                        RDG_EVENT_NAME("GaussianSplatSort.Bitonic K=%u J=%u", K, J),
                        CullSortCS,
                        SortParameters,
                        FComputeShaderUtils::GetGroupCount(PaddedPointCount, 64));
                }
            }

            // 接下来进入 raster 阶段。
            // 这里准备的是顶点 shader 需要的大部分参数，像素 shader 本身没有额外参数结构。
            //
            // 对应 shader：
            // - 顶点：FGaussianSplatRasterVS -> Shaders/Private/GaussianSplatRaster.usf 的 MainVS
            // - 像素：FGaussianSplatRasterPS -> Shaders/Private/GaussianSplatRaster.usf 的 MainPS
            //
            // Raster 阶段职责：
            // 1. 按排序后的顺序读取每个可见实例
            // 2. 在 VS 中把每个高斯展开成一个 4 顶点 billboard
            // 3. 在 PS 中计算二维高斯权重并输出预乘 alpha 颜色
            // 4. 把结果累积到前面创建的 SplatTexture
            FGaussianSplatRasterVS::FParameters* RasterParameters = GraphBuilder.AllocParameters<FGaussianSplatRasterVS::FParameters>();
            RasterParameters->ViewRectMin = FVector2f(static_cast<float>(ViewRect.Min.X), static_cast<float>(ViewRect.Min.Y));
            RasterParameters->ViewSize = FVector2f(static_cast<float>(ViewRect.Width()), static_cast<float>(ViewRect.Height()));
            {
                const FVector3f ViewOrigin = static_cast<FVector3f>(View.ViewMatrices.GetViewOrigin());
                RasterParameters->ViewWorldOrigin = FVector4f(ViewOrigin.X, ViewOrigin.Y, ViewOrigin.Z, 0.0f);
            }
            RasterParameters->PointSize = Batch.PointSize;
            RasterParameters->OpacityScale = Batch.OpacityScale;
            RasterParameters->Stride = Stride;
            RasterParameters->ViewMatrix = ViewMatrix;
            RasterParameters->LocalToWorldMatrix = Batch.LocalToWorld;
            RasterParameters->ProjectionMatrix = ProjectionMatrix;
            RasterParameters->ViewProjectionMatrix = ViewProjection;

            // WorldToLocalRow0/1/2 同样来自 Batch。
            // 它们不是完整的 4x4 逆矩阵，而是只保留旋转部分，用于在 shader 里把世界方向转回局部空间，
            // 这样可以按高斯本地坐标系正确评估 SH 光照方向。
            RasterParameters->WorldToLocalRow0 = Batch.WorldToLocalRow0;
            RasterParameters->WorldToLocalRow1 = Batch.WorldToLocalRow1;
            RasterParameters->WorldToLocalRow2 = Batch.WorldToLocalRow2;

            // 这些 SRV/UAV 都是前面 compute 阶段的产物或资产资源：
            // - SplatOrderBuffer：排序后的实例顺序
            // - SplatPosition/Rotation/Scale/Color/SHBuffer：资产本身的属性缓冲
            // - IndirectArgsBuffer：compute shader 写出的真实可见实例数
            RasterParameters->SplatOrderBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(OrderBuffer, PF_R32_UINT));
            RasterParameters->SplatPositionBuffer = Resources->GetPositionSRV();
            RasterParameters->SplatRotationBuffer = Resources->GetRotationSRV();
            RasterParameters->SplatScaleBuffer = Resources->GetScaleSRV();
            RasterParameters->SplatColorBuffer = Resources->GetColorSRV();
            RasterParameters->SplatSHBuffer = Resources->GetSHSRV();
            RasterParameters->IndirectArgsBuffer = IndirectArgsBuffer;

            // 第一个 Batch 清屏，后续 Batch 继续在同一个 SplatTexture 上累积。
            RasterParameters->RenderTargets[0] = FRenderTargetBinding(
                SplatOutput.Texture,
                bFirstBatch ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad);

            // 这里把 raster pass 挂进 RDG。
            // 真正绑定 VS/PS 并发起 DrawPrimitiveIndirect 的地方在下面这个 lambda。
            GraphBuilder.AddPass(
                RDG_EVENT_NAME("GaussianSplatRaster.DrawInstanced"),
                RasterParameters,
                ERDGPassFlags::Raster,
                [RasterParameters, RasterVS, RasterPS, ViewRect, IndirectArgsBuffer](FRHICommandList& RHICmdList)
                {
                    // RDG lambda 内部已经位于正确的 render pass 作用域中，只需要设置 viewport 和图形管线状态。
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

                    // 这里是图形 shader 真正被绑定和调用的地方：
                    // 1. SetGraphicsPipelineState 绑定 VS/PS 和固定管线状态
                    // 2. SetShaderParameters 把上面准备的参数结构传给 shader
                    // 3. DrawPrimitiveIndirect 根据 compute shader 写好的参数发起实例绘制
                    SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
                    SetShaderParameters(RHICmdList, RasterVS, RasterVS.GetVertexShader(), *RasterParameters);
                    SetShaderParameters(RHICmdList, RasterPS, RasterPS.GetPixelShader(), FGaussianSplatRasterPS::FParameters());
                    RHICmdList.DrawPrimitiveIndirect(IndirectArgsBuffer->GetIndirectRHICallBuffer(), 0);
                });

            bFirstBatch = false;
        }

        // 所有 Batch 都已经画进 SplatTexture 后，最后再做一次全屏合成。
        // 这一步对应的是 FGaussianSplatCompositePS，作用是把中间高斯颜色叠加回 SceneColor。
        FGaussianSplatCompositePS::FParameters* CompositeParameters = GraphBuilder.AllocParameters<FGaussianSplatCompositePS::FParameters>();
        CompositeParameters->SceneColorTextureSize = SceneColorTextureSize;
        CompositeParameters->SceneColorTexture = SceneColor.Texture;
        CompositeParameters->SceneColorSampler = TStaticSamplerState<SF_Point>::GetRHI();
        CompositeParameters->SplatTexture = SplatTexture;
        CompositeParameters->SplatSampler = TStaticSamplerState<SF_Point>::GetRHI();
        CompositeParameters->RenderTargets[0] = FRenderTargetBinding(Output.Texture, Output.LoadAction);

        TShaderMapRef<FGaussianSplatCompositePS> CompositeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

        // 这里是 fullscreen pixel shader 的调用点。
        FPixelShaderUtils::AddFullscreenPass(
            GraphBuilder,
            GetGlobalShaderMap(GMaxRHIFeatureLevel),
            RDG_EVENT_NAME("GaussianSplatRaster.Composite"),
            CompositeShader,
            CompositeParameters,
            ViewRect);

        // 返回值交回 ViewExtension，作为这个后处理阶段的新 SceneColor。
        return FScreenPassTexture(Output);
    }
}
