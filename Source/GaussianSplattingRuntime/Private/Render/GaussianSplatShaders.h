#pragma once

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

class FGaussianSplatCullSortCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatCullSortCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatCullSortCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, NumElements)
        SHADER_PARAMETER(uint32, PaddedNumElements)
        SHADER_PARAMETER(uint32, Stride)
        SHADER_PARAMETER(uint32, SortK)
        SHADER_PARAMETER(uint32, SortJ)
        SHADER_PARAMETER(uint32, PassType)
        SHADER_PARAMETER(FVector3f, ViewWorldOrigin)
        SHADER_PARAMETER(FVector3f, ViewForward)
        SHADER_PARAMETER(FMatrix44f, LocalToWorldMatrix)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatPositionBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SplatOrderBufferUAV)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SplatKeyBufferUAV)
    END_SHADER_PARAMETER_STRUCT()
};

class FGaussianSplatRasterVS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatRasterVS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatRasterVS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(FVector2f, ViewRectMin)
        SHADER_PARAMETER(FVector2f, ViewSize)
        SHADER_PARAMETER(FVector3f, ViewWorldOrigin)
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
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SplatOrderBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatPositionBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatRotationBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatScaleBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatColorBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatSHBuffer)
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()
};

class FGaussianSplatRasterPS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatRasterPS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatRasterPS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
    END_SHADER_PARAMETER_STRUCT()
};

class FGaussianSplatCompositePS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatCompositePS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatCompositePS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(FVector2f, SceneColorTextureSize)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SplatTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SplatSampler)
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()
};
