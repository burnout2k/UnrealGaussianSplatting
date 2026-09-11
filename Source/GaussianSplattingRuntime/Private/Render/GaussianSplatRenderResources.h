#pragma once

#include "CoreMinimal.h"
#include "Containers/DynamicRHIResourceArray.h"
#include "GaussianSplatAsset.h"
#include "RHIResources.h"
#include "RenderResource.h"

class FGaussianSplatRenderResources final : public FRenderResource
{
public:
    void BuildFromAssetData(
        const TArray<FVector3f>& InPositions,
        const TArray<FGaussianCovariance3f>& InCovariances,
        const TArray<FVector4f>& InColorsOpacity,
        const TArray<float>& InSHCoefficients,
        const TArray<FGaussianSplatCell>& InCells,
        int32 MaxPointCount);

    virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
    virtual void ReleaseRHI() override;

    uint32 GetPointCount() const
    {
        return PointCount;
    }

    FShaderResourceViewRHIRef GetPositionSRV() const { return PositionSRV; }
    FShaderResourceViewRHIRef GetCovariance0SRV() const { return Covariance0SRV; }
    FShaderResourceViewRHIRef GetCovariance1SRV() const { return Covariance1SRV; }
    FShaderResourceViewRHIRef GetColorSRV() const { return ColorSRV; }
    FShaderResourceViewRHIRef GetSHSRV() const { return SHSRV; }

    // False for captures exported at SH degree 0; the SH buffer is then a
    // single dummy element and must not be sampled.
    bool HasSH() const { return bHasSH; }

    // Cells in UPLOAD index space, not asset index space. Only a prefix of each
    // asset cell is resident, so the two numbering schemes differ.
    const TArray<FGaussianSplatCell>& GetCells() const { return Cells; }

private:
    template<typename ElementType>
    void InitStructuredBuffer(
        FRHICommandListBase& RHICmdList,
        const TCHAR* DebugName,
        TResourceArray<ElementType, VERTEXBUFFER_ALIGNMENT>& ResourceArray,
        FBufferRHIRef& OutBuffer,
        FShaderResourceViewRHIRef& OutSRV);

    uint32 PointCount = 0;
    bool bHasSH = false;

    TArray<FGaussianSplatCell> Cells;

    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> PositionData;
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> Covariance0Data;
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> Covariance1Data;
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> ColorData;
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> SHData;

    FBufferRHIRef PositionBuffer;
    FBufferRHIRef Covariance0Buffer;
    FBufferRHIRef Covariance1Buffer;
    FBufferRHIRef ColorBuffer;
    FBufferRHIRef SHBuffer;

    FShaderResourceViewRHIRef PositionSRV;
    FShaderResourceViewRHIRef Covariance0SRV;
    FShaderResourceViewRHIRef Covariance1SRV;
    FShaderResourceViewRHIRef ColorSRV;
    FShaderResourceViewRHIRef SHSRV;
};
