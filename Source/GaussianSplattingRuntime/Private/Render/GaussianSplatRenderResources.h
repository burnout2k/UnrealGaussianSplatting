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

    // SH as a palette: each DISTINCT set of 45 coefficients is stored once (45
    // floats, no padding) and every splat holds a uint index into it. Bit-exact
    // -- sets are matched by their bytes, never approximated. Captures converted
    // from SOG already share their SH through per-chunk palettes: the 20.9M-splat
    // Uno holds only 2,293,753 distinct sets, 474 MiB here vs 4,788 MiB as one
    // padded float4 per coefficient. A capture with unique SH per splat pays
    // 4 B/splat for the index and still saves the 60 B of padding.
    FShaderResourceViewRHIRef GetSHIndexSRV() const { return SHIndexSRV; }
    FShaderResourceViewRHIRef GetSHPaletteSRV() const { return SHPaletteSRV; }

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
            + static_cast<uint64>(bHasSH ? SHIndexData.GetResourceDataSize() : 0)
            + static_cast<uint64>(bHasSH ? SHPaletteData.GetResourceDataSize() : 0);
    }

    // False for captures exported at SH degree 0 (or whose palette would not fit
    // one buffer); the SH buffers are then single dummy elements and must not be
    // sampled.
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
    TResourceArray<uint32, VERTEXBUFFER_ALIGNMENT> SHIndexData;
    TResourceArray<float, VERTEXBUFFER_ALIGNMENT> SHPaletteData;

    FBufferRHIRef PackedABuffer;
    FBufferRHIRef PackedBBuffer;
    FBufferRHIRef CellBoundsBuffer;
    FBufferRHIRef SHIndexBuffer;
    FBufferRHIRef SHPaletteBuffer;

    FShaderResourceViewRHIRef PackedASRV;
    FShaderResourceViewRHIRef PackedBSRV;
    FShaderResourceViewRHIRef CellBoundsSRV;
    FShaderResourceViewRHIRef SHIndexSRV;
    FShaderResourceViewRHIRef SHPaletteSRV;
};
