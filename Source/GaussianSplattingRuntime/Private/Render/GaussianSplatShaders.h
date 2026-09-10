#pragma once

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

BEGIN_SHADER_PARAMETER_STRUCT(FGaussianSplatDepthTestParameters, )
    SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
    SHADER_PARAMETER(uint32, UseSceneDepth)
    SHADER_PARAMETER(FVector2f, OutputViewRectMin)
    SHADER_PARAMETER(FVector2f, OutputViewSize)
    SHADER_PARAMETER(FVector2f, SceneDepthTextureSize)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
END_SHADER_PARAMETER_STRUCT()

class FGaussianSplatPointsCullCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatPointsCullCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatPointsCullCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, NumElements)
        SHADER_PARAMETER(uint32, PaddedNumElements)
        SHADER_PARAMETER(uint32, Stride)
        SHADER_PARAMETER(uint32, SortK)
        SHADER_PARAMETER(uint32, SortJ)
        SHADER_PARAMETER(uint32, PassType)
        SHADER_PARAMETER(FVector4f, ViewWorldOrigin)
        SHADER_PARAMETER(FVector4f, ViewForward)
        SHADER_PARAMETER(FVector2f, ViewRectMin)
        SHADER_PARAMETER(FVector2f, ViewSize)
        SHADER_PARAMETER(float, PointSize)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, LocalToWorldMatrix)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatPositionBuffer)
        SHADER_PARAMETER(uint32, SortKeyShift)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, SplatOrderBufferUAV)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, SplatKeyBufferUAV)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, SplatIndirectArgsUAV)
    END_SHADER_PARAMETER_STRUCT()
};

class FGaussianSplatBillboardsCullCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatBillboardsCullCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatBillboardsCullCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, NumElements)
        SHADER_PARAMETER(uint32, PaddedNumElements)
        SHADER_PARAMETER(uint32, Stride)
        SHADER_PARAMETER(uint32, SortK)
        SHADER_PARAMETER(uint32, SortJ)
        SHADER_PARAMETER(uint32, PassType)
        SHADER_PARAMETER(FVector4f, ViewWorldOrigin)
        SHADER_PARAMETER(FVector4f, ViewForward)
        SHADER_PARAMETER(FVector2f, ViewRectMin)
        SHADER_PARAMETER(FVector2f, ViewSize)
        SHADER_PARAMETER(float, PointSize)
        SHADER_PARAMETER(FMatrix44f, ViewMatrix)
        SHADER_PARAMETER(FMatrix44f, ProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, LocalToWorldMatrix)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatPositionBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatCovariance0Buffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatCovariance1Buffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatColorBuffer)
        SHADER_PARAMETER(float, OpacityScale)
        SHADER_PARAMETER(float, MinSplatOpacity)
        SHADER_PARAMETER(float, MaxSplatDistance)
        SHADER_PARAMETER(uint32, SortKeyShift)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, SplatOrderBufferUAV)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, SplatKeyBufferUAV)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, SplatIndirectArgsUAV)
    END_SHADER_PARAMETER_STRUCT()
};

class FGaussianSplatPointsRasterVS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatPointsRasterVS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatPointsRasterVS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(FVector2f, ViewRectMin)
        SHADER_PARAMETER(FVector2f, ViewSize)
        SHADER_PARAMETER(float, PointSize)
        SHADER_PARAMETER(float, OpacityScale)
        SHADER_PARAMETER(uint32, Stride)
        SHADER_PARAMETER(FMatrix44f, LocalToWorldMatrix)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplatOrderBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatPositionBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatColorBuffer)
    END_SHADER_PARAMETER_STRUCT()
};

class FGaussianSplatPointsRasterPS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatPointsRasterPS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatPointsRasterPS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_STRUCT_INCLUDE(FGaussianSplatDepthTestParameters, DepthTest)
    END_SHADER_PARAMETER_STRUCT()
};

class FGaussianSplatBillboardsRasterVS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatBillboardsRasterVS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatBillboardsRasterVS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(FVector2f, ViewRectMin)
        SHADER_PARAMETER(FVector2f, ViewSize)
        SHADER_PARAMETER(FVector4f, ViewWorldOrigin)
        SHADER_PARAMETER(float, PointSize)
        SHADER_PARAMETER(float, OpacityScale)
        SHADER_PARAMETER(uint32, Stride)
        SHADER_PARAMETER(FMatrix44f, ViewMatrix)
        SHADER_PARAMETER(FMatrix44f, LocalToWorldMatrix)
        SHADER_PARAMETER(FMatrix44f, ProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
        SHADER_PARAMETER(FVector4f, WorldToLocalRow0)
        SHADER_PARAMETER(FVector4f, WorldToLocalRow1)
        SHADER_PARAMETER(FVector4f, WorldToLocalRow2)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplatOrderBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatPositionBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatCovariance0Buffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatCovariance1Buffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatColorBuffer)
        SHADER_PARAMETER(uint32, HasSH)
        SHADER_PARAMETER(uint32, PerPixelDepth)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatSHBuffer)
    END_SHADER_PARAMETER_STRUCT()
};

class FGaussianSplatBillboardsRasterPS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatBillboardsRasterPS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatBillboardsRasterPS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(float, AlphaCutoff)
        SHADER_PARAMETER_STRUCT_INCLUDE(FGaussianSplatDepthTestParameters, DepthTest)
    END_SHADER_PARAMETER_STRUCT()
};

class FGaussianSplatCompositePS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatCompositePS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatCompositePS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(FVector2f, SceneColorTextureSize)
        SHADER_PARAMETER(uint32, ConvertSplatToLinear)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SplatTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SplatSampler)
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()
};
