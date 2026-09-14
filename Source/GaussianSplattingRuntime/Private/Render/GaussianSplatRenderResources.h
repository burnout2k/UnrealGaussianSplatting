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
        const TArray<FQuat4f>& InRotations,
        const TArray<FVector3f>& InLogScales,
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

    // 20 B per splat across two naturally-aligned buffers, so there is no
    // ByteAddressBuffer alignment question:
    //   A (uint4): posX|posY, posZ|logScaleX, logScaleY|logScaleZ, rotation
    //   B (uint) : colour and opacity, four bytes
    FShaderResourceViewRHIRef GetPackedASRV() const { return PackedASRV; }
    FShaderResourceViewRHIRef GetPackedBSRV() const { return PackedBSRV; }

    // Two float4 per cell: origin.xyz then extent.xyz. Positions are 16-bit
    // fractions of their own cell, which is what makes 1 mm precision possible
    // -- over the whole capture the same 16 bits would be 40 mm, and over its
    // outlier-stretched bounding box, 217 mm.
    FShaderResourceViewRHIRef GetCellBoundsSRV() const { return CellBoundsSRV; }
    FShaderResourceViewRHIRef GetSHSRV() const { return SHSRV; }

    // (min, range) that RGB was quantized over, fitted to this capture. Opacity
    // is a sigmoid so it always occupies [0,1] and is not part of this.
    FVector2f GetColorEncoding() const { return ColorEncoding; }

    // What the upload actually costs, summed from the arrays themselves rather
    // than from a bytes-per-splat constant -- a constant silently keeps
    // reporting the old figure after the layout changes, which it already did
    // once. Valid between BuildFromAssetData and the RHI discarding the arrays.
    uint64 GetGpuBytes() const
    {
        return static_cast<uint64>(PackedAData.GetResourceDataSize())
            + static_cast<uint64>(PackedBData.GetResourceDataSize())
            + static_cast<uint64>(CellBoundsData.GetResourceDataSize())
            + static_cast<uint64>(bHasSH ? SHData.GetResourceDataSize() : 0);
    }

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

    // 20 B per splat. The 40 B layout before this stored the covariance as six
    // floats, which cannot be quantized well -- its entries are squared lengths
    // spanning many orders of magnitude. Rotation and scale can: a unit
    // quaternion into 32 bits at ~0.1 degrees, a LOG scale into three fp16.
    TResourceArray<FUintVector4, VERTEXBUFFER_ALIGNMENT> PackedAData;
    TResourceArray<uint32, VERTEXBUFFER_ALIGNMENT> PackedBData;
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> CellBoundsData;
    TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> SHData;

    FBufferRHIRef PackedABuffer;
    FBufferRHIRef PackedBBuffer;
    FBufferRHIRef CellBoundsBuffer;
    FBufferRHIRef SHBuffer;

    FShaderResourceViewRHIRef PackedASRV;
    FShaderResourceViewRHIRef PackedBSRV;
    FShaderResourceViewRHIRef CellBoundsSRV;
    FShaderResourceViewRHIRef SHSRV;
};
