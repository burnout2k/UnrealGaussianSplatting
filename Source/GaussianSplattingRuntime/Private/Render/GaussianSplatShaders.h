#pragma once

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

// 计算着色器：同一份 shader 代码承担两个阶段。
// 1. PassType == 0：做可见性判断、深度 key 编码和 indirect args 初始化。
// 2. PassType == 1：做 bitonic sort。
//
// 对应文件：
// Shaders/Private/GaussianSplatCullSort.usf
// 对应入口：
// MainCS
//
// 这个 shader 在 GaussianSplatPasses::AddPostProcessPass() 里通过
// FComputeShaderUtils::AddPass(...) 被调用。
class FGaussianSplatCullSortCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatCullSortCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatCullSortCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // 当前参与本批处理的逻辑元素个数。
        // 这个值已经考虑了组件的 DensityScale 和 MaxRenderPoints，但还没有经过可见性筛选。
        SHADER_PARAMETER(uint32, NumElements)

        // 为 bitonic sort 补齐到 2 的幂后的元素个数。
        SHADER_PARAMETER(uint32, PaddedNumElements)

        // 逻辑索引映射回资产真实点索引时使用的步长。
        // 例如 Stride == 4 时，逻辑索引 0/1/2/... 对应真实点 0/4/8/...。
        SHADER_PARAMETER(uint32, Stride)

        // bitonic sort 当前轮次使用的 K/J。
        // 只有 PassType == 1 时才有意义。
        SHADER_PARAMETER(uint32, SortK)
        SHADER_PARAMETER(uint32, SortJ)

        // 0 = 初始化/可见性筛选
        // 1 = bitonic 排序
        SHADER_PARAMETER(uint32, PassType)

        // 当前视图的相机世界坐标和前向方向。
        // 它们来自 AddPostProcessPass() 中的 FSceneView。
        // 使用 FVector4f 而不是 FVector3f，是为了满足 UE uniform buffer 的 16 字节对齐规则。
        SHADER_PARAMETER(FVector4f, ViewWorldOrigin)
        SHADER_PARAMETER(FVector4f, ViewForward)

        // 当前视图在目标纹理中的像素矩形信息。
        SHADER_PARAMETER(FVector2f, ViewRectMin)
        SHADER_PARAMETER(FVector2f, ViewSize)

        // 当前组件批次的点大小缩放。
        SHADER_PARAMETER(float, PointSize)

        // 当前视图的矩阵，以及当前组件的 LocalToWorld。
        // compute shader 用这些矩阵把资产局部点投到世界空间，再投到屏幕空间做 cull。
        SHADER_PARAMETER(FMatrix44f, ViewMatrix)
        SHADER_PARAMETER(FMatrix44f, ProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, LocalToWorldMatrix)

        // 来自 FGaussianSplatRenderResources 的只读 StructuredBuffer SRV。
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatPositionBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatRotationBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatScaleBuffer)

        // compute shader 的输出目标：
        // - SplatOrderBufferUAV：保存排序后的逻辑实例索引
        // - SplatKeyBufferUAV：保存对应的深度排序 key
        // - SplatIndirectArgsUAV：保存后续 DrawPrimitiveIndirect 需要的参数
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SplatOrderBufferUAV)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SplatKeyBufferUAV)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, SplatIndirectArgsUAV)
    END_SHADER_PARAMETER_STRUCT()
};

// 顶点着色器：每个可见高斯实例展开成一个四边形 billboard。
//
// 对应文件：
// Shaders/Private/GaussianSplatRaster.usf
// 对应入口：
// MainVS
//
// 这个 shader 在 GaussianSplatPasses::AddPostProcessPass() 的 raster pass 里，
// 通过 GraphBuilder.AddPass(... ERDGPassFlags::Raster ...) 绑定并调用。
class FGaussianSplatRasterVS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatRasterVS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatRasterVS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // 当前视图的像素矩形。
        SHADER_PARAMETER(FVector2f, ViewRectMin)
        SHADER_PARAMETER(FVector2f, ViewSize)

        // 相机世界坐标，用于构造观察方向，给 SH 颜色评估使用。
        SHADER_PARAMETER(FVector4f, ViewWorldOrigin)

        // 当前组件批次的外观控制参数。
        SHADER_PARAMETER(float, PointSize)
        SHADER_PARAMETER(float, OpacityScale)
        SHADER_PARAMETER(uint32, Stride)

        // 世界/投影矩阵。
        SHADER_PARAMETER(FMatrix44f, ViewMatrix)
        SHADER_PARAMETER(FMatrix44f, LocalToWorldMatrix)
        SHADER_PARAMETER(FMatrix44f, ProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)

        // 只保留旋转部分的 WorldToLocal 三行。
        // shader 用它把世界空间观察方向变回高斯局部空间，再评估 SH。
        SHADER_PARAMETER(FVector4f, WorldToLocalRow0)
        SHADER_PARAMETER(FVector4f, WorldToLocalRow1)
        SHADER_PARAMETER(FVector4f, WorldToLocalRow2)

        // 排序结果和资产属性缓冲。
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SplatOrderBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatPositionBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatRotationBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatScaleBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatColorBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatSHBuffer)

        // compute shader 写出的间接绘制参数。
        // raster pass 通过 DrawPrimitiveIndirect 使用它。
        RDG_BUFFER_ACCESS(IndirectArgsBuffer, ERHIAccess::IndirectArgs)

        // 当前 raster pass 的渲染目标绑定槽。
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()
};

// 像素着色器：对 billboard 四边形内的像素应用二维高斯权重。
//
// 对应文件：
// Shaders/Private/GaussianSplatRaster.usf
// 对应入口：
// MainPS
//
// 当前实现不需要单独的参数结构，颜色、透明度和 quad 坐标都由 VS 输出插值而来。
class FGaussianSplatRasterPS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatRasterPS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatRasterPS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
    END_SHADER_PARAMETER_STRUCT()
};

// 全屏合成像素着色器：把中间 SplatTexture 叠加回 SceneColor。
//
// 对应文件：
// Shaders/Private/GaussianSplatComposite.usf
// 对应入口：
// MainPS
//
// 这个 shader 在 GaussianSplatPasses::AddPostProcessPass() 末尾通过
// FPixelShaderUtils::AddFullscreenPass(...) 被调用。
class FGaussianSplatCompositePS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatCompositePS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatCompositePS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // SceneColor 纹理大小，供 shader 做归一化采样或像素坐标换算。
        SHADER_PARAMETER(FVector2f, SceneColorTextureSize)

        // 当前后处理阶段的输入 SceneColor。
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)

        // 前面所有 Gaussian batch 累积出来的中间纹理。
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SplatTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SplatSampler)

        // 最终输出 render target 绑定槽。
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()
};
