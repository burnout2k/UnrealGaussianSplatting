#pragma once

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

class FGaussianSplatCullSortCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatCullSortCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatCullSortCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
    END_SHADER_PARAMETER_STRUCT()
};

class FGaussianSplatRasterPS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatRasterPS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatRasterPS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(FVector2f, ViewSize)
        SHADER_PARAMETER(uint32, PointCount)
        SHADER_PARAMETER(float, ViewProjYScale)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SplatPositionRadiusBuffer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SplatColorBuffer)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()
};
