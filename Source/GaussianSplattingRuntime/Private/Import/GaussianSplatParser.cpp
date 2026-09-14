#include "Import/GaussianSplatParser.h"

#include "Containers/StringConv.h"
#include "GaussianSplatAsset.h"
#include "Math/Matrix.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatParser, Log, All);

namespace
{
    constexpr float SHC0 = 0.28209479177387814f;

    enum class EPlyScalarType : uint8
    {
        Invalid,
        Int8,
        UInt8,
        Int16,
        UInt16,
        Int32,
        UInt32,
        Float32,
        Float64,
    };

    struct FPlyProperty
    {
        FString Name;
        EPlyScalarType Type = EPlyScalarType::Invalid;
    };

    struct FPlyHeader
    {
        bool bAscii = false;
        bool bBinaryLittleEndian = false;
        int32 VertexCount = 0;
        int32 HeaderByteSize = 0;
        TArray<FPlyProperty> VertexProperties;
    };

    struct FImportedGaussian
    {
        FVector3f Position = FVector3f::ZeroVector;

        // Rotation and log-scale rather than a baked covariance. The covariance
        // is six floats that cannot be quantized well -- its entries are squared
        // lengths spanning many orders of magnitude -- whereas a unit quaternion
        // packs into 32 bits at ~0.1 degrees and a LOG scale into three fp16
        // with no dynamic-range problem at all. The PLY stores exactly these two
        // natively, so keeping them is also strictly less processing.
        FQuat4f Rotation = FQuat4f::Identity;
        FVector3f LogScale = FVector3f(-4.0f, -4.0f, -4.0f);
    };

    EPlyScalarType ParsePlyType(const FString& TypeText)
    {
        const FString T = TypeText.ToLower();
        if (T == TEXT("char") || T == TEXT("int8")) return EPlyScalarType::Int8;
        if (T == TEXT("uchar") || T == TEXT("uint8")) return EPlyScalarType::UInt8;
        if (T == TEXT("short") || T == TEXT("int16")) return EPlyScalarType::Int16;
        if (T == TEXT("ushort") || T == TEXT("uint16")) return EPlyScalarType::UInt16;
        if (T == TEXT("int") || T == TEXT("int32")) return EPlyScalarType::Int32;
        if (T == TEXT("uint") || T == TEXT("uint32")) return EPlyScalarType::UInt32;
        if (T == TEXT("float") || T == TEXT("float32")) return EPlyScalarType::Float32;
        if (T == TEXT("double") || T == TEXT("float64")) return EPlyScalarType::Float64;
        return EPlyScalarType::Invalid;
    }

    int32 GetTypeSize(EPlyScalarType Type)
    {
        switch (Type)
        {
        case EPlyScalarType::Int8:
        case EPlyScalarType::UInt8:
            return 1;
        case EPlyScalarType::Int16:
        case EPlyScalarType::UInt16:
            return 2;
        case EPlyScalarType::Int32:
        case EPlyScalarType::UInt32:
        case EPlyScalarType::Float32:
            return 4;
        case EPlyScalarType::Float64:
            return 8;
        default:
            return 0;
        }
    }

    bool ParseHeader(const TArray64<uint8>& RawData, FPlyHeader& OutHeader)
    {
        static const ANSICHAR EndA[] = "end_header\n";
        static const ANSICHAR EndB[] = "end_header\r\n";

        int64 HeaderEnd = INDEX_NONE;
        for (int64 I = 0; I < RawData.Num(); ++I)
        {
            const int64 Remaining = RawData.Num() - I;
            if (Remaining >= UE_ARRAY_COUNT(EndA) - 1 && FMemory::Memcmp(RawData.GetData() + I, EndA, UE_ARRAY_COUNT(EndA) - 1) == 0)
            {
                HeaderEnd = I + (UE_ARRAY_COUNT(EndA) - 1);
                break;
            }

            if (Remaining >= UE_ARRAY_COUNT(EndB) - 1 && FMemory::Memcmp(RawData.GetData() + I, EndB, UE_ARRAY_COUNT(EndB) - 1) == 0)
            {
                HeaderEnd = I + (UE_ARRAY_COUNT(EndB) - 1);
                break;
            }
        }

        if (HeaderEnd <= 0)
        {
            return false;
        }

        OutHeader.HeaderByteSize = static_cast<int32>(HeaderEnd);
        FUTF8ToTCHAR HeaderConvert(reinterpret_cast<const ANSICHAR*>(RawData.GetData()), HeaderEnd);
        const FString HeaderText(HeaderConvert.Length(), HeaderConvert.Get());

        TArray<FString> Lines;
        HeaderText.ParseIntoArrayLines(Lines, true);
        if (Lines.Num() == 0 || !Lines[0].StartsWith(TEXT("ply")))
        {
            return false;
        }

        bool bInVertexElement = false;
        for (const FString& RawLine : Lines)
        {
            const FString Line = RawLine.TrimStartAndEnd();
            if (Line.IsEmpty())
            {
                continue;
            }

            TArray<FString> Tokens;
            Line.ParseIntoArrayWS(Tokens, nullptr, true);
            if (Tokens.Num() == 0)
            {
                continue;
            }

            if (Tokens[0] == TEXT("format") && Tokens.Num() >= 2)
            {
                OutHeader.bAscii = Tokens[1] == TEXT("ascii");
                OutHeader.bBinaryLittleEndian = Tokens[1] == TEXT("binary_little_endian");
                continue;
            }

            if (Tokens[0] == TEXT("element") && Tokens.Num() >= 3)
            {
                bInVertexElement = Tokens[1] == TEXT("vertex");
                if (bInVertexElement)
                {
                    OutHeader.VertexCount = FCString::Atoi(*Tokens[2]);
                }
                continue;
            }

            if (Tokens[0] == TEXT("property") && bInVertexElement && Tokens.Num() >= 3 && Tokens[1] != TEXT("list"))
            {
                FPlyProperty Property;
                Property.Type = ParsePlyType(Tokens[1]);
                Property.Name = Tokens[2];
                OutHeader.VertexProperties.Add(Property);
            }
        }

        return (OutHeader.bAscii || OutHeader.bBinaryLittleEndian) && OutHeader.VertexCount > 0 && OutHeader.VertexProperties.Num() > 0;
    }

    int32 FindPropertyIndex(const TArray<FPlyProperty>& Properties, const TCHAR* Name)
    {
        for (int32 I = 0; I < Properties.Num(); ++I)
        {
            if (Properties[I].Name.Equals(Name, ESearchCase::IgnoreCase))
            {
                return I;
            }
        }
        return INDEX_NONE;
    }

    float ReadScalarAsFloat(const uint8* Data, EPlyScalarType Type)
    {
        switch (Type)
        {
        case EPlyScalarType::Int8: return static_cast<float>(*reinterpret_cast<const int8*>(Data));
        case EPlyScalarType::UInt8: return static_cast<float>(*reinterpret_cast<const uint8*>(Data));
        case EPlyScalarType::Int16: return static_cast<float>(*reinterpret_cast<const int16*>(Data));
        case EPlyScalarType::UInt16: return static_cast<float>(*reinterpret_cast<const uint16*>(Data));
        case EPlyScalarType::Int32: return static_cast<float>(*reinterpret_cast<const int32*>(Data));
        case EPlyScalarType::UInt32: return static_cast<float>(*reinterpret_cast<const uint32*>(Data));
        case EPlyScalarType::Float32: return *reinterpret_cast<const float*>(Data);
        case EPlyScalarType::Float64: return static_cast<float>(*reinterpret_cast<const double*>(Data));
        default: return 0.0f;
        }
    }

    float ReadValueOr(const TArray<float>& Values, int32 Index, float DefaultValue)
    {
        return Values.IsValidIndex(Index) ? Values[Index] : DefaultValue;
    }

    // A capture without f_rest_* properties has no view-dependent colour, only the
    // f_dc base. Appending 45 zero floats per splat for those wastes 180 B/splat of
    // RAM and asset size -- and at 85.8M splats it is 3.86 billion floats, which
    // overflows TArray's int32 index and hard-crashes the editor mid-import.
    bool CaptureHasSH(const TArray<int32>& RestIndices)
    {
        for (const int32 Index : RestIndices)
        {
            if (Index != INDEX_NONE)
            {
                return true;
            }
        }
        return false;
    }

    // TArray is int32-indexed, so SH storage caps out well before the vertex count does.
    bool SHWouldOverflow(int32 VertexCount)
    {
        return static_cast<int64>(VertexCount) * 45LL > static_cast<int64>(MAX_int32);
    }

    void AppendReorderedSH(const TArray<float>& Values, const TArray<int32>& RestIndices, UGaussianSplatAsset& Asset)
    {
        float RawSH[45] = {};
        for (int32 I = 0; I < RestIndices.Num() && I < 45; ++I)
        {
            RawSH[I] = ReadValueOr(Values, RestIndices[I], 0.0f);
        }

        float ReorderedSH[45] = {};
        for (int32 CoeffIndex = 0; CoeffIndex < 15; ++CoeffIndex)
        {
            ReorderedSH[CoeffIndex * 3 + 0] = RawSH[CoeffIndex];
            ReorderedSH[CoeffIndex * 3 + 1] = RawSH[CoeffIndex + 15];
            ReorderedSH[CoeffIndex * 3 + 2] = RawSH[CoeffIndex + 30];
        }

        Asset.SHCoefficients.Append(ReorderedSH, UE_ARRAY_COUNT(ReorderedSH));
    }

    // The PLY stores log scale; keep it in that form. The linear values span
    // 0.0000-134 m on this capture, which fp16 handles badly at the small end,
    // while the log spans about -16 to +5, which it handles trivially.
    FVector3f BuildLogScale(const TArray<float>& Values, int32 SX, int32 SY, int32 SZ)
    {
        if (Values.IsValidIndex(SX) && Values.IsValidIndex(SY) && Values.IsValidIndex(SZ))
        {
            return FVector3f(Values[SX], Values[SY], Values[SZ]);
        }

        return FVector3f(-3.912f, -3.912f, -3.912f);   // ln(0.02)
    }

    FQuat4f BuildRotation(const TArray<float>& Values, int32 RW, int32 RX, int32 RY, int32 RZ)
    {
        if (Values.IsValidIndex(RW) && Values.IsValidIndex(RX) && Values.IsValidIndex(RY) && Values.IsValidIndex(RZ))
        {
            FQuat4f Q(Values[RX], Values[RY], Values[RZ], Values[RW]);
            Q.Normalize();
            return Q;
        }

        return FQuat4f::Identity;
    }

    FMatrix44f BuildRotationMatrix(const FQuat4f& Rotation)
    {
        const float X = Rotation.X;
        const float Y = Rotation.Y;
        const float Z = Rotation.Z;
        const float W = Rotation.W;

        const float XX = X * X;
        const float YY = Y * Y;
        const float ZZ = Z * Z;
        const float XY = X * Y;
        const float XZ = X * Z;
        const float YZ = Y * Z;
        const float WX = W * X;
        const float WY = W * Y;
        const float WZ = W * Z;

        FMatrix44f M = FMatrix44f::Identity;
        M.M[0][0] = 1.0f - 2.0f * (YY + ZZ);
        M.M[0][1] = 2.0f * (XY - WZ);
        M.M[0][2] = 2.0f * (XZ + WY);
        M.M[1][0] = 2.0f * (XY + WZ);
        M.M[1][1] = 1.0f - 2.0f * (XX + ZZ);
        M.M[1][2] = 2.0f * (YZ - WX);
        M.M[2][0] = 2.0f * (XZ - WY);
        M.M[2][1] = 2.0f * (YZ + WX);
        M.M[2][2] = 1.0f - 2.0f * (XX + YY);
        return M;
    }

    FMatrix44f Multiply3x3(const FMatrix44f& A, const FMatrix44f& B)
    {
        FMatrix44f Result = FMatrix44f::Identity;
        for (int32 Row = 0; Row < 3; ++Row)
        {
            for (int32 Col = 0; Col < 3; ++Col)
            {
                float Sum = 0.0f;
                for (int32 K = 0; K < 3; ++K)
                {
                    Sum += A.M[Row][K] * B.M[K][Col];
                }
                Result.M[Row][Col] = Sum;
            }
        }
        return Result;
    }

    FMatrix44f Transpose3x3(const FMatrix44f& M)
    {
        FMatrix44f Result = FMatrix44f::Identity;
        for (int32 Row = 0; Row < 3; ++Row)
        {
            for (int32 Col = 0; Col < 3; ++Col)
            {
                Result.M[Row][Col] = M.M[Col][Row];
            }
        }
        return Result;
    }

    FVector3f ApplyColmapToUEPosition(const FVector3f& Position)
    {
        return FVector3f(Position.Z, Position.X, -Position.Y);
    }

    // The COLMAP -> UE axis swap applied to a rotation rather than to a built
    // covariance.
    //
    // Sigma = R S^2 R^T, so T Sigma T^T = (T R) S^2 (T R)^T -- but T is a
    // REFLECTION (determinant -1, because the handedness flips), which makes T R
    // improper and therefore not expressible as a quaternion. Post-multiplying by
    // D = diag(-1, 1, 1) restores a proper rotation and changes nothing, since D
    // and S^2 are both diagonal so D S^2 D = S^2.
    //
    // Verified against 20,000 splats of the Vuores capture: the rebuilt
    // covariance matches the old baked path to 4e-16 relative, and 3e-15 after a
    // full quaternion round trip. Scale is untouched by the swap.
    FQuat4f ApplyColmapToUERotation(const FQuat4f& Rotation)
    {
        FMatrix44f Swap = FMatrix44f::Identity;
        Swap.M[0][0] = 0.0f; Swap.M[0][1] = 0.0f; Swap.M[0][2] = 1.0f;
        Swap.M[1][0] = 1.0f; Swap.M[1][1] = 0.0f; Swap.M[1][2] = 0.0f;
        Swap.M[2][0] = 0.0f; Swap.M[2][1] = -1.0f; Swap.M[2][2] = 0.0f;

        FMatrix44f Flip = FMatrix44f::Identity;
        Flip.M[0][0] = -1.0f;

        FMatrix44f Proper = Multiply3x3(Multiply3x3(Swap, BuildRotationMatrix(Rotation)), Flip);

        // TRANSPOSED on purpose. BuildRotationMatrix above produces the standard
        // column-vector form (v' = M v), but UE's TQuat(const TMatrix&) expects
        // the engine's row-vector form (v' = v M) -- its trace branch reads
        // X = (M[1][2] - M[2][1]) * s, the opposite sign to the column
        // convention. Feeding it the column form yields the INVERSE rotation, so
        // the covariance rebuilds as R^T S^2 R instead of R S^2 R^T. That is
        // identical for a spherical splat and badly wrong for an elongated one,
        // which showed up as the shredded cars while foliage looked fine.
        //
        // Simulated against UE's exact quaternion and RotateVector code over
        // 5,000 splats: as written 2.2 relative error, transposed 2.3e-15.
        FMatrix44f RowVectorForm = Transpose3x3(Proper);
        RowVectorForm.M[3][3] = 1.0f;

        FQuat4f Result(RowVectorForm);
        Result.Normalize();
        return Result;
    }

    FImportedGaussian BuildImportedGaussian(
        const TArray<float>& Values,
        int32 XIndex,
        int32 YIndex,
        int32 ZIndex,
        int32 Scale0Index,
        int32 Scale1Index,
        int32 Scale2Index,
        int32 Rot0Index,
        int32 Rot1Index,
        int32 Rot2Index,
        int32 Rot3Index)
    {
        FImportedGaussian Result;
        Result.Position = ApplyColmapToUEPosition(FVector3f(Values[XIndex], Values[YIndex], Values[ZIndex]));
        Result.Rotation = ApplyColmapToUERotation(BuildRotation(Values, Rot0Index, Rot1Index, Rot2Index, Rot3Index));
        Result.LogScale = BuildLogScale(Values, Scale0Index, Scale1Index, Scale2Index);
        return Result;
    }

    FLinearColor BuildColor(const TArray<float>& Values, int32 Dc0, int32 Dc1, int32 Dc2, int32 Opacity)
    {
        const float R = 0.5f + SHC0 * ReadValueOr(Values, Dc0, 0.0f);
        const float G = 0.5f + SHC0 * ReadValueOr(Values, Dc1, 0.0f);
        const float B = 0.5f + SHC0 * ReadValueOr(Values, Dc2, 0.0f);
        const float A = 1.0f / (1.0f + FMath::Exp(-ReadValueOr(Values, Opacity, 0.0f)));
        return FLinearColor(R, G, B, A);
    }

    bool ResolveCommonPropertyIndices(
        const FPlyHeader& Header,
        int32& OutXIndex,
        int32& OutYIndex,
        int32& OutZIndex,
        int32& OutDc0Index,
        int32& OutDc1Index,
        int32& OutDc2Index,
        int32& OutOpacityIndex,
        int32& OutScale0Index,
        int32& OutScale1Index,
        int32& OutScale2Index,
        int32& OutRot0Index,
        int32& OutRot1Index,
        int32& OutRot2Index,
        int32& OutRot3Index,
        TArray<int32>& OutRestIndices)
    {
        OutXIndex = FindPropertyIndex(Header.VertexProperties, TEXT("x"));
        OutYIndex = FindPropertyIndex(Header.VertexProperties, TEXT("y"));
        OutZIndex = FindPropertyIndex(Header.VertexProperties, TEXT("z"));
        OutDc0Index = FindPropertyIndex(Header.VertexProperties, TEXT("f_dc_0"));
        OutDc1Index = FindPropertyIndex(Header.VertexProperties, TEXT("f_dc_1"));
        OutDc2Index = FindPropertyIndex(Header.VertexProperties, TEXT("f_dc_2"));
        OutOpacityIndex = FindPropertyIndex(Header.VertexProperties, TEXT("opacity"));
        OutScale0Index = FindPropertyIndex(Header.VertexProperties, TEXT("scale_0"));
        OutScale1Index = FindPropertyIndex(Header.VertexProperties, TEXT("scale_1"));
        OutScale2Index = FindPropertyIndex(Header.VertexProperties, TEXT("scale_2"));
        OutRot0Index = FindPropertyIndex(Header.VertexProperties, TEXT("rot_0"));
        OutRot1Index = FindPropertyIndex(Header.VertexProperties, TEXT("rot_1"));
        OutRot2Index = FindPropertyIndex(Header.VertexProperties, TEXT("rot_2"));
        OutRot3Index = FindPropertyIndex(Header.VertexProperties, TEXT("rot_3"));

        OutRestIndices.Reset();
        OutRestIndices.Reserve(45);
        for (int32 SHIndex = 0; SHIndex < 45; ++SHIndex)
        {
            OutRestIndices.Add(FindPropertyIndex(Header.VertexProperties, *FString::Printf(TEXT("f_rest_%d"), SHIndex)));
        }

        return OutXIndex != INDEX_NONE && OutYIndex != INDEX_NONE && OutZIndex != INDEX_NONE;
    }

    void ResetAssetData(UGaussianSplatAsset& Asset, int32 VertexCount)
    {
        Asset.Positions.Reset(VertexCount);
        Asset.Covariances.Reset();
        Asset.Rotations.Reset(VertexCount);
        Asset.LogScales.Reset(VertexCount);
        Asset.ColorsOpacity.Reset(VertexCount);
        Asset.SHCoefficients.Reset();
    }

    bool FillAssetFromAscii(const TArray64<uint8>& RawData, const FPlyHeader& Header, UGaussianSplatAsset& Asset)
    {
        // The ascii path converts the whole body to an FString, so it is bounded by
        // int32 regardless of the 64-bit read. Fail loudly rather than truncating:
        // a >2 GB ascii PLY would silently import as garbage.
        const int64 BodyByteSize64 = RawData.Num() - static_cast<int64>(Header.HeaderByteSize);
        if (BodyByteSize64 > static_cast<int64>(MAX_int32))
        {
            return false;
        }
        const int32 BodyByteSize = static_cast<int32>(BodyByteSize64);
        FUTF8ToTCHAR BodyConvert(reinterpret_cast<const ANSICHAR*>(RawData.GetData() + Header.HeaderByteSize), BodyByteSize);
        const FString BodyText(BodyConvert.Length(), BodyConvert.Get());

        TArray<FString> Lines;
        BodyText.ParseIntoArrayLines(Lines, true);
        if (Lines.Num() < Header.VertexCount)
        {
            return false;
        }

        int32 XIndex, YIndex, ZIndex, Dc0Index, Dc1Index, Dc2Index, OpacityIndex, Scale0Index, Scale1Index, Scale2Index, Rot0Index, Rot1Index, Rot2Index, Rot3Index;
        TArray<int32> RestIndices;
        if (!ResolveCommonPropertyIndices(
            Header, XIndex, YIndex, ZIndex, Dc0Index, Dc1Index, Dc2Index, OpacityIndex,
            Scale0Index, Scale1Index, Scale2Index, Rot0Index, Rot1Index, Rot2Index, Rot3Index, RestIndices))
        {
            return false;
        }

        const bool bHasSH = CaptureHasSH(RestIndices);
        if (bHasSH && SHWouldOverflow(Header.VertexCount))
        {
            return false;
        }

        ResetAssetData(Asset, Header.VertexCount);
        for (int32 I = 0; I < Header.VertexCount; ++I)
        {
            TArray<FString> Tokens;
            Lines[I].TrimStartAndEnd().ParseIntoArrayWS(Tokens, nullptr, true);
            if (Tokens.Num() < Header.VertexProperties.Num())
            {
                return false;
            }

            TArray<float> Values;
            Values.Reserve(Header.VertexProperties.Num());
            for (const FString& Token : Tokens)
            {
                Values.Add(FCString::Atof(*Token));
            }

            const FImportedGaussian Gaussian = BuildImportedGaussian(
                Values, XIndex, YIndex, ZIndex, Scale0Index, Scale1Index, Scale2Index, Rot0Index, Rot1Index, Rot2Index, Rot3Index);
            Asset.Positions.Add(Gaussian.Position);
            Asset.Rotations.Add(Gaussian.Rotation);
            Asset.LogScales.Add(Gaussian.LogScale);

            const FLinearColor Color = BuildColor(Values, Dc0Index, Dc1Index, Dc2Index, OpacityIndex);
            Asset.ColorsOpacity.Add(FVector4f(Color.R, Color.G, Color.B, Color.A));
            if (bHasSH)
            {
                AppendReorderedSH(Values, RestIndices, Asset);
            }
        }

        Asset.RefreshDerivedData();
        return true;
    }

    bool FillAssetFromBinaryLE(const TArray64<uint8>& RawData, const FPlyHeader& Header, UGaussianSplatAsset& Asset)
    {
        int32 XIndex, YIndex, ZIndex, Dc0Index, Dc1Index, Dc2Index, OpacityIndex, Scale0Index, Scale1Index, Scale2Index, Rot0Index, Rot1Index, Rot2Index, Rot3Index;
        TArray<int32> RestIndices;
        if (!ResolveCommonPropertyIndices(
            Header, XIndex, YIndex, ZIndex, Dc0Index, Dc1Index, Dc2Index, OpacityIndex,
            Scale0Index, Scale1Index, Scale2Index, Rot0Index, Rot1Index, Rot2Index, Rot3Index, RestIndices))
        {
            return false;
        }

        int32 VertexStride = 0;
        for (const FPlyProperty& Property : Header.VertexProperties)
        {
            const int32 Size = GetTypeSize(Property.Type);
            if (Size <= 0)
            {
                return false;
            }
            VertexStride += Size;
        }

        const int64 BodyBytes = static_cast<int64>(Header.VertexCount) * static_cast<int64>(VertexStride);
        if (Header.HeaderByteSize + BodyBytes > RawData.Num())
        {
            return false;
        }

        const bool bHasSH = CaptureHasSH(RestIndices);
        if (bHasSH && SHWouldOverflow(Header.VertexCount))
        {
            return false;
        }
        if (!bHasSH)
        {
            UE_LOG(LogGaussianSplatParser, Display,
                TEXT("PLY has no f_rest_* properties: importing %d splats without spherical harmonics."),
                Header.VertexCount);
        }

        ResetAssetData(Asset, Header.VertexCount);
        const uint8* Cursor = RawData.GetData() + Header.HeaderByteSize;
        for (int32 I = 0; I < Header.VertexCount; ++I)
        {
            TArray<float> Values;
            Values.Reserve(Header.VertexProperties.Num());

            for (const FPlyProperty& Property : Header.VertexProperties)
            {
                Values.Add(ReadScalarAsFloat(Cursor, Property.Type));
                Cursor += GetTypeSize(Property.Type);
            }

            const FImportedGaussian Gaussian = BuildImportedGaussian(
                Values, XIndex, YIndex, ZIndex, Scale0Index, Scale1Index, Scale2Index, Rot0Index, Rot1Index, Rot2Index, Rot3Index);
            Asset.Positions.Add(Gaussian.Position);
            Asset.Rotations.Add(Gaussian.Rotation);
            Asset.LogScales.Add(Gaussian.LogScale);

            const FLinearColor Color = BuildColor(Values, Dc0Index, Dc1Index, Dc2Index, OpacityIndex);
            Asset.ColorsOpacity.Add(FVector4f(Color.R, Color.G, Color.B, Color.A));
            if (bHasSH)
            {
                AppendReorderedSH(Values, RestIndices, Asset);
            }
        }

        Asset.RefreshDerivedData();
        return true;
    }
}

namespace GaussianSplatParser
{
    bool ParseFromFile(const FString& FilePath, UGaussianSplatAsset& OutAsset, FString& OutError)
    {
        // TArray is 32-bit indexed, so LoadFileToArray refuses anything over 2 GB
        // ("too large for 32-bit reader, use TArray64"). Real captures exceed that:
        // an 85.8M-splat PLY is 4.5 GiB. The binary reader below already walks a raw
        // pointer with int64 offsets, so only the container had to change.
        TArray64<uint8> RawData;
        if (!FFileHelper::LoadFileToArray(RawData, *FilePath))
        {
            OutError = FString::Printf(TEXT("Failed to read source file: %s"), *FPaths::ConvertRelativePathToFull(FilePath));
            return false;
        }

        return ParseFromBytes(RawData, OutAsset, OutError);
    }

    bool ParseFromBytes(const TArray64<uint8>& RawData, UGaussianSplatAsset& OutAsset, FString& OutError)
    {
        FPlyHeader Header;
        if (!ParseHeader(RawData, Header))
        {
            OutError = TEXT("Invalid or unsupported PLY header.");
            return false;
        }

        const bool bImported = Header.bAscii
            ? FillAssetFromAscii(RawData, Header, OutAsset)
            : FillAssetFromBinaryLE(RawData, Header, OutAsset);
        if (!bImported)
        {
            OutError = TEXT("Failed to parse PLY vertex data.");
            return false;
        }

        OutError.Reset();
        return true;
    }
}
