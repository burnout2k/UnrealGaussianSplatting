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

    // xyz = position, w = colour and opacity as four bytes. The old layout kept
    // a separate float4 colour buffer and left position.w holding a constant
    // 1.0; folding one into the other costs nothing and saves 16 B per splat.
    FShaderResourceViewRHIRef GetPositionColorSRV() const { return PositionColorSRV; }
    FShaderResourceViewRHIRef GetCovariance0SRV() const { return Covariance0SRV; }
    FShaderResourceViewRHIRef GetCovariance1SRV() const { return Covariance1SRV; }
    FShaderResourceViewRHIRef GetSHSRV() const { return SHSRV; }

    // (min, range) that RGB was quantized over, fitted to this capture. Opacity
    // is a sigmoid so it always occupies [0,1] and is not part of this.
    FVector2f GetColorEncoding() const { return ColorEncoding; }

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
    FVector2f ColorEncoding = FVector2f(0.0f, 1.0f);

    // 40 B per splat: 16 (position + packed colour) + 16 (XX XY XZ YY) + 8
    // (YZ ZZ). The previous four-float4 layout cost 64 B and spent 12 of them on
    // padding -- position.w was always 1.0, covariance.zw always zero, and
    // colour was four float32 holding eight-bit data.
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> PositionColorData;
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> Covariance0Data;
    TResourceArray<FVector2f, VERTEXBUFFER_ALIGNMENT> Covariance1Data;
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> SHData;

    FBufferRHIRef PositionColorBuffer;
    FBufferRHIRef Covariance0Buffer;
    FBufferRHIRef Covariance1Buffer;
    FBufferRHIRef SHBuffer;

    FShaderResourceViewRHIRef PositionColorSRV;
    FShaderResourceViewRHIRef Covariance0SRV;
    FShaderResourceViewRHIRef Covariance1SRV;
    FShaderResourceViewRHIRef SHSRV;
};
