#include "Render/GaussianSplatRenderResources.h"

#include "RHICommandList.h"

void FGaussianSplatRenderResources::BuildFromAssetData(
    const TArray<FVector3f>& InPositions,
    const TArray<FQuat4f>& InRotations,
    const TArray<FVector3f>& InScales,
    const TArray<FVector4f>& InColorsOpacity,
    const TArray<float>& InSHCoefficients)
{
    PointCount = InPositions.Num();

    PositionData.Empty(PointCount);
    RotationData.Empty(PointCount);
    ScaleData.Empty(PointCount);
    ColorData.Empty(PointCount);
    SHData.Empty(InSHCoefficients.Num() / 3);

    for (int32 Index = 0; Index < InPositions.Num(); ++Index)
    {
        const FVector3f Position = InPositions[Index];
        const FQuat4f Rotation = InRotations.IsValidIndex(Index) ? InRotations[Index] : FQuat4f::Identity;
        const FVector3f Scale = InScales.IsValidIndex(Index) ? InScales[Index] : FVector3f(0.02f, 0.02f, 0.02f);
        const FVector4f Color = InColorsOpacity.IsValidIndex(Index) ? InColorsOpacity[Index] : FVector4f(1, 1, 1, 1);

        PositionData.Add(FVector4f(Position.X, Position.Y, Position.Z, 1.0f));
        RotationData.Add(FVector4f(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W));
        ScaleData.Add(FVector4f(Scale.X, Scale.Y, Scale.Z, 0.0f));
        ColorData.Add(Color);
    }

    for (int32 Index = 0; Index + 2 < InSHCoefficients.Num(); Index += 3)
    {
        SHData.Add(FVector4f(
            InSHCoefficients[Index + 0],
            InSHCoefficients[Index + 1],
            InSHCoefficients[Index + 2],
            0.0f));
    }
}

template<typename ElementType>
void FGaussianSplatRenderResources::InitStructuredBuffer(
    FRHICommandListBase& RHICmdList,
    const TCHAR* DebugName,
    TResourceArray<ElementType, VERTEXBUFFER_ALIGNMENT>& ResourceArray,
    FBufferRHIRef& OutBuffer,
    FShaderResourceViewRHIRef& OutSRV)
{
    if (ResourceArray.IsEmpty())
    {
        return;
    }

    FRHIResourceCreateInfo CreateInfo(DebugName, &ResourceArray);
    OutBuffer = RHICmdList.CreateStructuredBuffer(
        sizeof(ElementType),
        ResourceArray.GetResourceDataSize(),
        BUF_Static | BUF_ShaderResource,
        ERHIAccess::SRVMask,
        CreateInfo);
    OutSRV = RHICmdList.CreateShaderResourceView(OutBuffer);
}

void FGaussianSplatRenderResources::InitRHI(FRHICommandListBase& RHICmdList)
{
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetPositions"), PositionData, PositionBuffer, PositionSRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetRotations"), RotationData, RotationBuffer, RotationSRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetScales"), ScaleData, ScaleBuffer, ScaleSRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetColors"), ColorData, ColorBuffer, ColorSRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetSH"), SHData, SHBuffer, SHSRV);
}

void FGaussianSplatRenderResources::ReleaseRHI()
{
    PositionSRV.SafeRelease();
    RotationSRV.SafeRelease();
    ScaleSRV.SafeRelease();
    ColorSRV.SafeRelease();
    SHSRV.SafeRelease();

    PositionBuffer.SafeRelease();
    RotationBuffer.SafeRelease();
    ScaleBuffer.SafeRelease();
    ColorBuffer.SafeRelease();
    SHBuffer.SafeRelease();
}
