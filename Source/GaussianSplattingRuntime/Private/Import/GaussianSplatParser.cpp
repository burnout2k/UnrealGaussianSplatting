#include "Import/GaussianSplatParser.h"

#include "Containers/StringConv.h"
#include "GaussianSplatAsset.h"
#include "Math/Matrix.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatParser, Log, All);

#include "Import/GaussianSplatPlyReader.h"

// The reader moved to its own header so the paged importer reads rows the same
// way; everything below is unchanged and still sees the same names.
using namespace GaussianSplatPly;

namespace
{
    // These fill the legacy UGaussianSplatAsset, so they stay here rather than in the
    // shared reader: the paged importer fills a different class entirely.
    void AppendReorderedSH(const TArray<float>& Values, const TArray<int32>& RestIndices, UGaussianSplatAsset& Asset)
    {
        float ReorderedSH[45] = {};
        BuildReorderedSH(Values, RestIndices, ReorderedSH);
        Asset.SHCoefficients.Append(ReorderedSH, UE_ARRAY_COUNT(ReorderedSH));
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
