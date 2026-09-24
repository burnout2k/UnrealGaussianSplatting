#pragma once

#include "DataDrivenShaderPlatformInfo.h"
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
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, SplatCellRanges)
        SHADER_PARAMETER(uint32, SplatCellCount)
        SHADER_PARAMETER_SRV(StructuredBuffer<uint4>, SplatPackedA)
        SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SplatPackedB)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatCellBounds)
        SHADER_PARAMETER(FVector2f, SplatColorEncoding)
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
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, SplatCellRanges)
        SHADER_PARAMETER(uint32, SplatCellCount)
        SHADER_PARAMETER_SRV(StructuredBuffer<uint4>, SplatPackedA)
        SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SplatPackedB)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatCellBounds)
        SHADER_PARAMETER(FVector2f, SplatColorEncoding)
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
        SHADER_PARAMETER(float, OpacityScale)
        SHADER_PARAMETER(float, MinSplatOpacity)
        SHADER_PARAMETER(float, MaxSplatDistance)
        SHADER_PARAMETER(float, MinScreenVariance)
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
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, SplatCellRanges)
        SHADER_PARAMETER(uint32, SplatCellCount)
        SHADER_PARAMETER_SRV(StructuredBuffer<uint4>, SplatPackedA)
        SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SplatPackedB)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatCellBounds)
        SHADER_PARAMETER(FVector2f, SplatColorEncoding)
        SHADER_PARAMETER(FMatrix44f, LocalToWorldMatrix)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplatOrderBuffer)
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
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, SplatCellRanges)
        SHADER_PARAMETER(uint32, SplatCellCount)
        SHADER_PARAMETER_SRV(StructuredBuffer<uint4>, SplatPackedA)
        SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SplatPackedB)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SplatCellBounds)
        SHADER_PARAMETER(FVector2f, SplatColorEncoding)
        SHADER_PARAMETER(FMatrix44f, ViewMatrix)
        SHADER_PARAMETER(FMatrix44f, LocalToWorldMatrix)
        SHADER_PARAMETER(FMatrix44f, ProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
        SHADER_PARAMETER(FVector4f, WorldToLocalRow0)
        SHADER_PARAMETER(FVector4f, WorldToLocalRow1)
        SHADER_PARAMETER(FVector4f, WorldToLocalRow2)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplatOrderBuffer)
        SHADER_PARAMETER(uint32, HasSH)
        SHADER_PARAMETER(float, MinScreenVariance)
        SHADER_PARAMETER(uint32, PerPixelDepth)
        SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SplatSHIndexBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float>, SplatSHPaletteBuffer)
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

// ---------------------------------------------------------------------------------------------
// r.GaussianSplat.SortMode 2: DeviceRadixSort (GaussianSplatDeviceRadixSort.usf, which includes the
// vendored ThirdParty/GPUSorting shaders).

// Keys per partition (one Upsweep/Downsweep thread group). Must equal PART_SIZE in the vendored
// SortCommon.ush; the wrapper checks it with a _Static_assert.
inline constexpr uint32 GSRadixPartSize = 3840;

// One struct for all four kernels; each binds only what it reads. RadixSetup gets the UAV view of
// the indirect args and Upsweep/Downsweep the SRV view: kept apart by design, since RDG would not
// reject both in one pass but silently merge them to UAV access.
BEGIN_SHADER_PARAMETER_STRUCT(FGaussianSplatRadixSortParameters, )
    SHADER_PARAMETER(uint32, e_radixShift)
    SHADER_PARAMETER(uint32, e_threadBlocks)
    SHADER_PARAMETER(uint32, e_maxKeys)
    SHADER_PARAMETER(uint32, e_useGpuCount)
    SHADER_PARAMETER(uint32, e_gpuCap)
    SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, b_sortCount)
    SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, b_sort)
    SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, b_sortPayload)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, b_sortCountUAV)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, b_alt)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, b_altPayload)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, b_globalHist)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, b_passHist)
END_SHADER_PARAMETER_STRUCT()

// Vulkan only in this phase: that is the only platform the port has been validated on, so a D3D12
// cook never compiles untested code. At runtime the plugin also checks the device (NVIDIA, wave 32)
// and that every permutation exists before using any of these.
class FGaussianSplatRadixSortCS : public FGlobalShader
{
public:
    FGaussianSplatRadixSortCS() = default;
    FGaussianSplatRadixSortCS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
        : FGlobalShader(Initializer)
    {
    }

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsVulkanPlatform(Parameters.Platform)
            && IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5)
            && RHISupportsWaveOperations(Parameters.Platform);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        // A no-op on the Vulkan shader format (SM 6.6 is its baseline); kept for D3D12.
        OutEnvironment.CompilerFlags.Add(CFLAG_WaveOperations);
        OutEnvironment.SetDefine(TEXT("GS_RADIX_PART_SIZE"), GSRadixPartSize);
    }
};

class FGaussianSplatRadixSetupCS final : public FGaussianSplatRadixSortCS
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatRadixSetupCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatRadixSetupCS, FGaussianSplatRadixSortCS);
    using FParameters = FGaussianSplatRadixSortParameters;
};

class FGaussianSplatRadixUpsweepCS final : public FGaussianSplatRadixSortCS
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatRadixUpsweepCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatRadixUpsweepCS, FGaussianSplatRadixSortCS);
    using FParameters = FGaussianSplatRadixSortParameters;
};

class FGaussianSplatRadixScanCS final : public FGaussianSplatRadixSortCS
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatRadixScanCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatRadixScanCS, FGaussianSplatRadixSortCS);
    using FParameters = FGaussianSplatRadixSortParameters;
};

class FGaussianSplatRadixDownsweepCS final : public FGaussianSplatRadixSortCS
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatRadixDownsweepCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatRadixDownsweepCS, FGaussianSplatRadixSortCS);
    using FParameters = FGaussianSplatRadixSortParameters;

    // 1 = aras-p's barrier layout (a barrier per bit in the multisplit), 0 = upstream b0nes164's
    // (one barrier before ranking). r.GaussianSplat.RadixSafeBarriers picks one.
    class FSafeBarriersDim : SHADER_PERMUTATION_BOOL("GS_RADIX_SAFE_BARRIERS");
    using FPermutationDomain = TShaderPermutationDomain<FSafeBarriersDim>;
};

// Debug-only sort validation (GaussianSplatSortValidate.usf). Gated separately from the sort:
// production mode 2 never depends on these compiling.
BEGIN_SHADER_PARAMETER_STRUCT(FGaussianSplatSortValidateParameters, )
    SHADER_PARAMETER(uint32, MaxKeys)
    SHADER_PARAMETER(uint32, GpuCap)
    SHADER_PARAMETER(uint32, OrderBound)
    SHADER_PARAMETER(uint32, TailCheck)
    SHADER_PARAMETER(uint32, SelfCheckCount)
    SHADER_PARAMETER(uint32, GridStride)
    SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SortCount)
    SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SrcKeys)
    SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SrcOrder)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TmpKeys)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TmpOrder)
    SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CheckKeys)
    SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CheckOrder)
    SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RefKeys)
    SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RefOrder)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Stats)
END_SHADER_PARAMETER_STRUCT()

class FGaussianSplatSortValidateCS : public FGlobalShader
{
public:
    FGaussianSplatSortValidateCS() = default;
    FGaussianSplatSortValidateCS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
        : FGlobalShader(Initializer)
    {
    }

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsVulkanPlatform(Parameters.Platform)
            && IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

class FGaussianSplatSortPrepareCS final : public FGaussianSplatSortValidateCS
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatSortPrepareCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatSortPrepareCS, FGaussianSplatSortValidateCS);
    using FParameters = FGaussianSplatSortValidateParameters;
};

class FGaussianSplatSortCompareCS final : public FGaussianSplatSortValidateCS
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatSortCompareCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatSortCompareCS, FGaussianSplatSortValidateCS);
    using FParameters = FGaussianSplatSortValidateParameters;
};

class FGaussianSplatSortSelfCheckCS final : public FGaussianSplatSortValidateCS
{
public:
    DECLARE_GLOBAL_SHADER(FGaussianSplatSortSelfCheckCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatSortSelfCheckCS, FGaussianSplatSortValidateCS);
    using FParameters = FGaussianSplatSortValidateParameters;
};
