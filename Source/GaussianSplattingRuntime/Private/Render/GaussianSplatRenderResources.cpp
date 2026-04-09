#include "Render/GaussianSplatRenderResources.h"

#include "GaussianSplatBoundsUtils.h"
#include "RHICommandList.h"

void FGaussianSplatRenderResources::BuildFromAssetData(
    const TArray<FVector3f>& InPositions,
    const TArray<FGaussianCovariance3f>& InCovariances,
    const TArray<FVector4f>& InColorsOpacity,
    const TArray<float>& InSHCoefficients)
{
    PointCount = InPositions.Num();

    PositionData.Empty(PointCount);
    Covariance0Data.Empty(PointCount);
    Covariance1Data.Empty(PointCount);
    ColorData.Empty(PointCount);
    SHData.Empty(InSHCoefficients.Num() / 3);

    for (int32 Index = 0; Index < InPositions.Num(); ++Index)
    {
        const FVector3f Position = InPositions[Index];
        const FGaussianCovariance3f Covariance = InCovariances.IsValidIndex(Index)
            ? InCovariances[Index]
            : GaussianSplatBoundsUtils::MakeIsotropic(0.02f);
        const FVector4f Color = InColorsOpacity[Index];

        PositionData.Add(FVector4f(Position.X, Position.Y, Position.Z, 1.0f));
        Covariance0Data.Add(FVector4f(Covariance.XX, Covariance.XY, Covariance.XZ, Covariance.YY));
        Covariance1Data.Add(FVector4f(Covariance.YZ, Covariance.ZZ, 0.0f, 0.0f));
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
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetCovariance0"), Covariance0Data, Covariance0Buffer, Covariance0SRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetCovariance1"), Covariance1Data, Covariance1Buffer, Covariance1SRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetColors"), ColorData, ColorBuffer, ColorSRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetSH"), SHData, SHBuffer, SHSRV);
}

void FGaussianSplatRenderResources::ReleaseRHI()
{
    PositionSRV.SafeRelease();
    Covariance0SRV.SafeRelease();
    Covariance1SRV.SafeRelease();
    ColorSRV.SafeRelease();
    SHSRV.SafeRelease();

    PositionBuffer.SafeRelease();
    Covariance0Buffer.SafeRelease();
    Covariance1Buffer.SafeRelease();
    ColorBuffer.SafeRelease();
    SHBuffer.SafeRelease();
}
