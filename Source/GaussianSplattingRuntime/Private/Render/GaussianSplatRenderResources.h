#pragma once

#include "CoreMinimal.h"
#include "Containers/DynamicRHIResourceArray.h"
#include "RHIResources.h"
#include "RenderResource.h"

class FGaussianSplatRenderResources final : public FRenderResource
{
public:
    void BuildFromAssetData(
        const TArray<FVector3f>& InPositions,
        const TArray<FQuat4f>& InRotations,
        const TArray<FVector3f>& InScales,
        const TArray<FVector4f>& InColorsOpacity,
        const TArray<float>& InSHCoefficients);

    virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
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
    template<typename ElementType>
    void InitStructuredBuffer(
        FRHICommandListBase& RHICmdList,
        const TCHAR* DebugName,
        TResourceArray<ElementType, VERTEXBUFFER_ALIGNMENT>& ResourceArray,
        FBufferRHIRef& OutBuffer,
        FShaderResourceViewRHIRef& OutSRV);

    uint32 PointCount = 0;

    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> PositionData;
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> RotationData;
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> ScaleData;
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> ColorData;
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> SHData;

    FBufferRHIRef PositionBuffer;
    FBufferRHIRef RotationBuffer;
    FBufferRHIRef ScaleBuffer;
    FBufferRHIRef ColorBuffer;
    FBufferRHIRef SHBuffer;

    FShaderResourceViewRHIRef PositionSRV;
    FShaderResourceViewRHIRef RotationSRV;
    FShaderResourceViewRHIRef ScaleSRV;
    FShaderResourceViewRHIRef ColorSRV;
    FShaderResourceViewRHIRef SHSRV;
};
