#pragma once

#include "CoreMinimal.h"
#include "Containers/DynamicRHIResourceArray.h"
#include "RHIResources.h"
#include "RenderResource.h"

// FRenderResource 是 UE 封装 GPU 资源生命周期的常见基类。
// 这个类负责把 UGaussianSplatAsset 里的 CPU 数组上传成 StructuredBuffer + SRV（着色器资源视图）。
class FGaussianSplatRenderResources final : public FRenderResource
{
public:
    // 这里只做 CPU 侧数据准备，不触发实际 RHI（硬件渲染接口） 创建。
    // 真正创建 Buffer 的时机在 InitRHI，也就是 BeginInitResource 之后的渲染线程阶段。
    void BuildFromAssetData(
        const TArray<FVector3f>& InPositions,
        const TArray<FQuat4f>& InRotations,
        const TArray<FVector3f>& InScales,
        const TArray<FVector4f>& InColorsOpacity,
        const TArray<float>& InSHCoefficients);

    // 渲染线程回调：创建底层 StructuredBuffer / SRV。
    virtual void InitRHI(FRHICommandListBase& RHICmdList) override;

    // 渲染线程回调：释放底层 RHI 资源。
    virtual void ReleaseRHI() override;

    uint32 GetPointCount() const
    {
        return PointCount;
    }

    FShaderResourceViewRHIRef GetPositionSRV() const { return PositionSRV; }
    FShaderResourceViewRHIRef GetRotationSRV() const { return RotationSRV; }
    FShaderResourceViewRHIRef GetScaleSRV() const { return ScaleSRV; }
    FShaderResourceViewRHIRef GetColorSRV() const { return ColorSRV; }
    FShaderResourceViewRHIRef GetSHSRV() const { return SHSRV; }

private:
    // 模板化封装，避免 Position / Rotation / Scale / Color / SH 五套几乎一样的建 Buffer 代码重复。
    template<typename ElementType>
    void InitStructuredBuffer(
        FRHICommandListBase& RHICmdList,
        const TCHAR* DebugName,
        TResourceArray<ElementType, VERTEXBUFFER_ALIGNMENT>& ResourceArray,
        FBufferRHIRef& OutBuffer,
        FShaderResourceViewRHIRef& OutSRV);

    // 原始高斯数量，不等于 SHData 数组长度。
    uint32 PointCount = 0;

    // CPU staging 数据。InitRHI 时会把它们作为初始内容上传到 GPU。
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> PositionData;
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> RotationData;
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> ScaleData;
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> ColorData;
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> SHData;

    // 真正给 GPU 用的 Buffer。
    FBufferRHIRef PositionBuffer;
    FBufferRHIRef RotationBuffer;
    FBufferRHIRef ScaleBuffer;
    FBufferRHIRef ColorBuffer;
    FBufferRHIRef SHBuffer;

    // Shader Resource View，shader 侧通过 SRV 把 Buffer 当 StructuredBuffer<float4> 读取。
    FShaderResourceViewRHIRef PositionSRV;
    FShaderResourceViewRHIRef RotationSRV;
    FShaderResourceViewRHIRef ScaleSRV;
    FShaderResourceViewRHIRef ColorSRV;
    FShaderResourceViewRHIRef SHSRV;
};
