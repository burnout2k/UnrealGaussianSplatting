#include "Render/GaussianSplatRenderResources.h"

#include "RHICommandList.h"

void FGaussianSplatRenderResources::BuildFromAssetData(
    const TArray<FVector3f>& InPositions,
    const TArray<FQuat4f>& InRotations,
    const TArray<FVector3f>& InScales,
    const TArray<FVector4f>& InColorsOpacity,
    const TArray<float>& InSHCoefficients)
{
    // 这里把 Asset 的“面向业务”的数组布局转换成 shader 更容易直接读取的 float4 阵列布局。
    PointCount = InPositions.Num();

    PositionData.Empty(PointCount);
    RotationData.Empty(PointCount);
    ScaleData.Empty(PointCount);
    ColorData.Empty(PointCount);

    // SH 系数按 float4 打包，W 只是占位，方便 StructuredBuffer<float4> 对齐读取。
    SHData.Empty(InSHCoefficients.Num() / 3);

    for (int32 Index = 0; Index < InPositions.Num(); ++Index)
    {
        const FVector3f Position = InPositions[Index];
        const FQuat4f Rotation = InRotations[Index];
        const FVector3f Scale = InScales[Index];
        const FVector4f Color = InColorsOpacity[Index];

        PositionData.Add(FVector4f(Position.X, Position.Y, Position.Z, 1.0f));
        RotationData.Add(FVector4f(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W));
        ScaleData.Add(FVector4f(Scale.X, Scale.Y, Scale.Z, 0.0f));
        ColorData.Add(Color);
    }

    // 每 3 个 float 组成一个 RGB SH 系数，打包进一个 float4。
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
        // 没数据就不创建空 Buffer，调用方自行处理 SRV 为 null 的情况。
        return;
    }

    FRHIResourceCreateInfo CreateInfo(DebugName, &ResourceArray);

    // StructuredBuffer 的 stride 就是单个元素大小，底层数据来自 ResourceArray。
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
    // 这一步发生在渲染线程，之后 shader 就能通过对应 SRV 访问 Asset 数据。
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetPositions"), PositionData, PositionBuffer, PositionSRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetRotations"), RotationData, RotationBuffer, RotationSRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetScales"), ScaleData, ScaleBuffer, ScaleSRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetColors"), ColorData, ColorBuffer, ColorSRV);
    InitStructuredBuffer(RHICmdList, TEXT("GaussianSplat.AssetSH"), SHData, SHBuffer, SHSRV);
}

void FGaussianSplatRenderResources::ReleaseRHI()
{
    // UE 的 RHI 资源普遍用引用计数句柄，SafeRelease 能在不同平台后端下统一释放。
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
