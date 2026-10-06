#pragma once

// The GPU record and the bake order, in ONE place.
//
// Two code paths now produce splat records: the legacy asset packs them at load
// from its source arrays (GaussianSplatRenderResources.cpp), and the paged
// importer packs them once at import into the asset's payload
// (Import/GaussianSplatPagedImporter.cpp). If the two ever disagree by a single
// rounding step, a paged asset and the legacy one it was built from draw
// different pixels, and the Step 1 "within noise" gate becomes meaningless --
// a differently rounded or differently ordered prefix is not noise.
//
// So both call these functions, and neither has a copy. Everything here is a
// verbatim move out of the anonymous namespaces of GaussianSplatAsset.cpp and
// GaussianSplatRenderResources.cpp; nothing changed value.

#include "CoreMinimal.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatBoundsUtils.h"
#include "Math/Float16.h"

namespace GaussianSplatFormat
{
    // ---------------------------------------------------------------- ordering

    // H3DGS's merge weight: opacity * sqrt(det Sigma). sqrt(det Sigma) is the
    // product of the three axis scales, so this ranks a splat by roughly how
    // much visible volume it accounts for -- exactly the order in which you want
    // to drop splats. Computable from the stored covariance alone: no camera
    // poses, no source imagery, no retraining.
    //
    // With the scales in hand the weight needs no determinant at all -- and no
    // double precision to survive cubing values as small as 1e-4.
    FORCEINLINE float SplatImportance(const FVector3f& LogScale, float Opacity)
    {
        return Opacity * FMath::Exp(LogScale.X + LogScale.Y + LogScale.Z);
    }

    // The covariance form, for assets imported before rotation and scale were
    // kept. The determinant is accumulated in double because the scales span
    // 0.0000-134 m on real captures, and its cube underflows float badly at the
    // small end.
    FORCEINLINE float SplatImportance(const FGaussianCovariance3f& C, float Opacity)
    {
        const double Det =
              static_cast<double>(C.XX) * (static_cast<double>(C.YY) * C.ZZ - static_cast<double>(C.YZ) * C.YZ)
            - static_cast<double>(C.XY) * (static_cast<double>(C.XY) * C.ZZ - static_cast<double>(C.XZ) * C.YZ)
            + static_cast<double>(C.XZ) * (static_cast<double>(C.XY) * C.YZ - static_cast<double>(C.XZ) * C.YY);

        return static_cast<float>(Opacity * FMath::Sqrt(FMath::Max(Det, 0.0)));
    }

    // The grid slot a splat falls in. Float arithmetic on the already-swapped
    // axes, exactly as UGaussianSplatAsset::BuildCells does it -- a double here
    // would put splats near a cell wall on the other side of it, which is a
    // different bake.
    FORCEINLINE FIntVector CellCoord(const FVector3f& Position, float InvCellPitch)
    {
        return FIntVector(
            FMath::FloorToInt(Position.X * InvCellPitch),
            FMath::FloorToInt(Position.Y * InvCellPitch),
            FMath::FloorToInt(Position.Z * InvCellPitch));
    }

    // Cells are emitted in coordinate order, not discovery order, so that a
    // rebuild of the same capture is reproducible and neighbouring cells land
    // near each other in memory.
    FORCEINLINE bool CellCoordLess(const FIntVector& A, const FIntVector& B)
    {
        if (A.X != B.X) { return A.X < B.X; }
        if (A.Y != B.Y) { return A.Y < B.Y; }
        return A.Z < B.Z;
    }

    // Inside a cell: most important first, ties broken by the original index so
    // the order is stable across rebuilds and across the two packers.
    FORCEINLINE bool ImportanceLess(float ImportanceA, int32 IndexA, float ImportanceB, int32 IndexB)
    {
        return ImportanceA != ImportanceB ? ImportanceA > ImportanceB : IndexA < IndexB;
    }

    // ----------------------------------------------------------------- packing

    // Smallest-three: the largest component of a unit quaternion is recoverable
    // from the other three, so 2 bits name it and 10 bits each carry the rest
    // over [-1/sqrt(2), 1/sqrt(2)] -- the widest any non-largest component can
    // be. 32 bits total, ~0.1 degrees of error.
    FORCEINLINE uint32 PackUnitQuaternion(const FQuat4f& Q)
    {
        float C[4] = { Q.X, Q.Y, Q.Z, Q.W };

        int32 Largest = 0;
        for (int32 I = 1; I < 4; ++I)
        {
            if (FMath::Abs(C[I]) > FMath::Abs(C[Largest]))
            {
                Largest = I;
            }
        }

        // q and -q are the same rotation, so force the named component positive
        // and the decoder can take the positive square root unconditionally.
        if (C[Largest] < 0.0f)
        {
            for (int32 I = 0; I < 4; ++I)
            {
                C[I] = -C[I];
            }
        }

        constexpr float Range = 0.70710678f;   // 1/sqrt(2)
        uint32 Packed = static_cast<uint32>(Largest);
        int32 Shift = 2;
        for (int32 I = 0; I < 4; ++I)
        {
            if (I == Largest)
            {
                continue;
            }

            const float Normalised = FMath::Clamp(C[I] / Range * 0.5f + 0.5f, 0.0f, 1.0f);
            Packed |= static_cast<uint32>(FMath::RoundToInt(Normalised * 1023.0f)) << Shift;
            Shift += 10;
        }
        return Packed;
    }

    FORCEINLINE uint32 PackHalf2(float A, float B)
    {
        return static_cast<uint32>(FFloat16(A).Encoded)
            | (static_cast<uint32>(FFloat16(B).Encoded) << 16);
    }

    FORCEINLINE uint32 QuantizeUnit16(float Normalised)
    {
        return static_cast<uint32>(FMath::RoundToInt(FMath::Clamp(Normalised, 0.0f, 1.0f) * 65535.0f));
    }

    // A cell one splat wide has zero extent on some axis; the reciprocal would
    // be infinite and the quantized value NaN.
    FORCEINLINE FVector3f CellExtent(const FVector3f& BoundsMin, const FVector3f& BoundsMax)
    {
        return FVector3f(
            FMath::Max(BoundsMax.X - BoundsMin.X, UE_KINDA_SMALL_NUMBER),
            FMath::Max(BoundsMax.Y - BoundsMin.Y, UE_KINDA_SMALL_NUMBER),
            FMath::Max(BoundsMax.Z - BoundsMin.Z, UE_KINDA_SMALL_NUMBER));
    }

    // PackedA, 16 B: posX|posY, posZ|logScaleX, logScaleY|logScaleZ, rotation.
    // Positions are 16-bit fractions of their own cell's shrink-wrapped box,
    // which is what makes 1 mm precision possible -- over a whole capture the
    // same 16 bits would be 40 mm.
    FORCEINLINE FUintVector4 PackSplatA(
        const FVector3f& Position,
        const FVector3f& CellOrigin,
        const FVector3f& CellExtentValue,
        const FVector3f& LogScale,
        const FQuat4f& Rotation)
    {
        const FVector3f Local = (Position - CellOrigin) / CellExtentValue;
        return FUintVector4(
            QuantizeUnit16(Local.X) | (QuantizeUnit16(Local.Y) << 16),
            QuantizeUnit16(Local.Z) | (static_cast<uint32>(FFloat16(LogScale.X).Encoded) << 16),
            PackHalf2(LogScale.Y, LogScale.Z),
            PackUnitQuaternion(Rotation));
    }

    // PackedB, 4 B: colour and opacity. RGB is quantized over the range fitted
    // to the whole capture (ColorEncoding = min, range), opacity over [0,1]
    // because it is a sigmoid.
    FORCEINLINE uint32 PackSplatB(const FVector4f& Color, const FVector2f& ColorEncoding)
    {
        const FVector3f Normalised =
            (FVector3f(Color.X, Color.Y, Color.Z) - FVector3f(ColorEncoding.X)) / ColorEncoding.Y;
        return static_cast<uint32>(FMath::RoundToInt(FMath::Clamp(Normalised.X, 0.0f, 1.0f) * 255.0f))
            | static_cast<uint32>(FMath::RoundToInt(FMath::Clamp(Normalised.Y, 0.0f, 1.0f) * 255.0f)) << 8
            | static_cast<uint32>(FMath::RoundToInt(FMath::Clamp(Normalised.Z, 0.0f, 1.0f) * 255.0f)) << 16
            | static_cast<uint32>(FMath::RoundToInt(FMath::Clamp(Color.W, 0.0f, 1.0f) * 255.0f)) << 24;
    }

    // The RGB quantization range, fitted over EVERY splat rather than the
    // resident subset: otherwise the range shifts whenever the budget changes
    // (measured [-0.037, 1.840] at 1M resident against [-0.037, 2.259] at 60M),
    // which makes the encoding depend on an unrelated setting and makes two
    // point counts incomparable. A degenerate range would divide by zero.
    FORCEINLINE FVector2f MakeColorEncoding(float ColorMin, float ColorMax)
    {
        if (ColorMin > ColorMax)
        {
            ColorMin = 0.0f;
            ColorMax = 1.0f;
        }
        return FVector2f(ColorMin, FMath::Max(ColorMax - ColorMin, KINDA_SMALL_NUMBER));
    }
}
