#include "GaussianSplatAssetFactory.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/StringConv.h"
#include "GaussianSplatAsset.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatFactory, Log, All);

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

    bool ParseHeader(const TArray<uint8>& RawData, FPlyHeader& OutHeader)
    {
        static const ANSICHAR EndA[] = "end_header\n";
        static const ANSICHAR EndB[] = "end_header\r\n";

        int32 HeaderEnd = INDEX_NONE;
        for (int32 I = 0; I < RawData.Num(); ++I)
        {
            const int32 Remaining = RawData.Num() - I;
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

        OutHeader.HeaderByteSize = HeaderEnd;
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
            FString Line = RawLine;
            Line = Line.TrimStartAndEnd();
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

            if (Tokens[0] == TEXT("property") && bInVertexElement)
            {
                if (Tokens.Num() >= 3 && Tokens[1] != TEXT("list"))
                {
                    FPlyProperty Property;
                    Property.Type = ParsePlyType(Tokens[1]);
                    Property.Name = Tokens[2];
                    OutHeader.VertexProperties.Add(Property);
                }
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

    FLinearColor BuildColor(
        const TArray<float>& Values,
        int32 RIndex,
        int32 GIndex,
        int32 BIndex,
        int32 AIndex)
    {
        auto SampleColor = [&](int32 Index, float DefaultValue) -> float
        {
            if (!Values.IsValidIndex(Index))
            {
                return DefaultValue;
            }
            const float Raw = Values[Index];
            return Raw > 1.0f ? FMath::Clamp(Raw / 255.0f, 0.0f, 1.0f) : FMath::Clamp(Raw, 0.0f, 1.0f);
        };

        return FLinearColor(
            SampleColor(RIndex, 1.0f),
            SampleColor(GIndex, 1.0f),
            SampleColor(BIndex, 1.0f),
            SampleColor(AIndex, 1.0f));
    }

    float ReadValueOr(const TArray<float>& Values, int32 Index, float DefaultValue)
    {
        return Values.IsValidIndex(Index) ? Values[Index] : DefaultValue;
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

    FVector3f BuildScale(const TArray<float>& Values, int32 SX, int32 SY, int32 SZ)
    {
        if (Values.IsValidIndex(SX) && Values.IsValidIndex(SY) && Values.IsValidIndex(SZ))
        {
            // Unity Gaussian data stores log-space scale.
            return FVector3f(
                FMath::Exp(Values[SX]),
                FMath::Exp(Values[SY]),
                FMath::Exp(Values[SZ]));
        }

        return FVector3f(0.02f, 0.02f, 0.02f);
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

    FLinearColor BuildUnityGaussianColor(const TArray<float>& Values, int32 Dc0, int32 Dc1, int32 Dc2, int32 Opacity)
    {
        const float R = 0.5f + SHC0 * ReadValueOr(Values, Dc0, 0.0f);
        const float G = 0.5f + SHC0 * ReadValueOr(Values, Dc1, 0.0f);
        const float B = 0.5f + SHC0 * ReadValueOr(Values, Dc2, 0.0f);
        const float A = 1.0f / (1.0f + FMath::Exp(-ReadValueOr(Values, Opacity, 0.0f)));
        return FLinearColor(R, G, B, A);
    }

    bool FillAssetFromAscii(const TArray<uint8>& RawData, const FPlyHeader& Header, UGaussianSplatAsset& Asset)
    {
        const int32 BodyByteSize = RawData.Num() - Header.HeaderByteSize;
        FUTF8ToTCHAR BodyConvert(reinterpret_cast<const ANSICHAR*>(RawData.GetData() + Header.HeaderByteSize), BodyByteSize);
        FString BodyText(BodyConvert.Length(), BodyConvert.Get());
        TArray<FString> Lines;
        BodyText.ParseIntoArrayLines(Lines, true);
        if (Lines.Num() < Header.VertexCount)
        {
            return false;
        }

        const int32 XIndex = FindPropertyIndex(Header.VertexProperties, TEXT("x"));
        const int32 YIndex = FindPropertyIndex(Header.VertexProperties, TEXT("y"));
        const int32 ZIndex = FindPropertyIndex(Header.VertexProperties, TEXT("z"));
        const int32 RIndex = FindPropertyIndex(Header.VertexProperties, TEXT("red"));
        const int32 GIndex = FindPropertyIndex(Header.VertexProperties, TEXT("green"));
        const int32 BIndex = FindPropertyIndex(Header.VertexProperties, TEXT("blue"));
        const int32 AIndex = FindPropertyIndex(Header.VertexProperties, TEXT("alpha"));
        const int32 Dc0Index = FindPropertyIndex(Header.VertexProperties, TEXT("f_dc_0"));
        const int32 Dc1Index = FindPropertyIndex(Header.VertexProperties, TEXT("f_dc_1"));
        const int32 Dc2Index = FindPropertyIndex(Header.VertexProperties, TEXT("f_dc_2"));
        const int32 OpacityIndex = FindPropertyIndex(Header.VertexProperties, TEXT("opacity"));
        const int32 Scale0Index = FindPropertyIndex(Header.VertexProperties, TEXT("scale_0"));
        const int32 Scale1Index = FindPropertyIndex(Header.VertexProperties, TEXT("scale_1"));
        const int32 Scale2Index = FindPropertyIndex(Header.VertexProperties, TEXT("scale_2"));
        const int32 Rot0Index = FindPropertyIndex(Header.VertexProperties, TEXT("rot_0"));
        const int32 Rot1Index = FindPropertyIndex(Header.VertexProperties, TEXT("rot_1"));
        const int32 Rot2Index = FindPropertyIndex(Header.VertexProperties, TEXT("rot_2"));
        const int32 Rot3Index = FindPropertyIndex(Header.VertexProperties, TEXT("rot_3"));
        const bool bHasUnityGaussianFields =
            Dc0Index != INDEX_NONE && Dc1Index != INDEX_NONE && Dc2Index != INDEX_NONE && OpacityIndex != INDEX_NONE;
        TArray<int32> RestIndices;
        RestIndices.Reserve(45);
        for (int32 SHIndex = 0; SHIndex < 45; ++SHIndex)
        {
            RestIndices.Add(FindPropertyIndex(Header.VertexProperties, *FString::Printf(TEXT("f_rest_%d"), SHIndex)));
        }

        if (XIndex == INDEX_NONE || YIndex == INDEX_NONE || ZIndex == INDEX_NONE)
        {
            return false;
        }

        Asset.Positions.Reset(Header.VertexCount);
        Asset.Rotations.Reset(Header.VertexCount);
        Asset.Scales.Reset(Header.VertexCount);
        Asset.ColorsOpacity.Reset(Header.VertexCount);
        Asset.SHCoefficients.Reset();

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

            Asset.Positions.Add(FVector3f(Values[XIndex], Values[YIndex], Values[ZIndex]));
            Asset.Rotations.Add(BuildRotation(Values, Rot0Index, Rot1Index, Rot2Index, Rot3Index));
            Asset.Scales.Add(BuildScale(Values, Scale0Index, Scale1Index, Scale2Index));

            const FLinearColor Color = bHasUnityGaussianFields
                ? BuildUnityGaussianColor(Values, Dc0Index, Dc1Index, Dc2Index, OpacityIndex)
                : BuildColor(Values, RIndex, GIndex, BIndex, AIndex);
            Asset.ColorsOpacity.Add(FVector4f(Color.R, Color.G, Color.B, Color.A));
            AppendReorderedSH(Values, RestIndices, Asset);
        }

        Asset.RebuildBounds();
        return true;
    }

    bool FillAssetFromBinaryLE(const TArray<uint8>& RawData, const FPlyHeader& Header, UGaussianSplatAsset& Asset)
    {
        const int32 XIndex = FindPropertyIndex(Header.VertexProperties, TEXT("x"));
        const int32 YIndex = FindPropertyIndex(Header.VertexProperties, TEXT("y"));
        const int32 ZIndex = FindPropertyIndex(Header.VertexProperties, TEXT("z"));
        const int32 RIndex = FindPropertyIndex(Header.VertexProperties, TEXT("red"));
        const int32 GIndex = FindPropertyIndex(Header.VertexProperties, TEXT("green"));
        const int32 BIndex = FindPropertyIndex(Header.VertexProperties, TEXT("blue"));
        const int32 AIndex = FindPropertyIndex(Header.VertexProperties, TEXT("alpha"));
        const int32 Dc0Index = FindPropertyIndex(Header.VertexProperties, TEXT("f_dc_0"));
        const int32 Dc1Index = FindPropertyIndex(Header.VertexProperties, TEXT("f_dc_1"));
        const int32 Dc2Index = FindPropertyIndex(Header.VertexProperties, TEXT("f_dc_2"));
        const int32 OpacityIndex = FindPropertyIndex(Header.VertexProperties, TEXT("opacity"));
        const int32 Scale0Index = FindPropertyIndex(Header.VertexProperties, TEXT("scale_0"));
        const int32 Scale1Index = FindPropertyIndex(Header.VertexProperties, TEXT("scale_1"));
        const int32 Scale2Index = FindPropertyIndex(Header.VertexProperties, TEXT("scale_2"));
        const int32 Rot0Index = FindPropertyIndex(Header.VertexProperties, TEXT("rot_0"));
        const int32 Rot1Index = FindPropertyIndex(Header.VertexProperties, TEXT("rot_1"));
        const int32 Rot2Index = FindPropertyIndex(Header.VertexProperties, TEXT("rot_2"));
        const int32 Rot3Index = FindPropertyIndex(Header.VertexProperties, TEXT("rot_3"));
        const bool bHasUnityGaussianFields =
            Dc0Index != INDEX_NONE && Dc1Index != INDEX_NONE && Dc2Index != INDEX_NONE && OpacityIndex != INDEX_NONE;
        TArray<int32> RestIndices;
        RestIndices.Reserve(45);
        for (int32 SHIndex = 0; SHIndex < 45; ++SHIndex)
        {
            RestIndices.Add(FindPropertyIndex(Header.VertexProperties, *FString::Printf(TEXT("f_rest_%d"), SHIndex)));
        }

        if (XIndex == INDEX_NONE || YIndex == INDEX_NONE || ZIndex == INDEX_NONE)
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

        Asset.Positions.Reset(Header.VertexCount);
        Asset.Rotations.Reset(Header.VertexCount);
        Asset.Scales.Reset(Header.VertexCount);
        Asset.ColorsOpacity.Reset(Header.VertexCount);
        Asset.SHCoefficients.Reset();

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

            Asset.Positions.Add(FVector3f(Values[XIndex], Values[YIndex], Values[ZIndex]));
            Asset.Rotations.Add(BuildRotation(Values, Rot0Index, Rot1Index, Rot2Index, Rot3Index));
            Asset.Scales.Add(BuildScale(Values, Scale0Index, Scale1Index, Scale2Index));

            const FLinearColor Color = bHasUnityGaussianFields
                ? BuildUnityGaussianColor(Values, Dc0Index, Dc1Index, Dc2Index, OpacityIndex)
                : BuildColor(Values, RIndex, GIndex, BIndex, AIndex);
            Asset.ColorsOpacity.Add(FVector4f(Color.R, Color.G, Color.B, Color.A));
            AppendReorderedSH(Values, RestIndices, Asset);
        }

        Asset.RebuildBounds();
        return true;
    }
}

UGaussianSplatAssetFactory::UGaussianSplatAssetFactory()
{
    bEditorImport = true;
    bCreateNew = false;
    SupportedClass = UGaussianSplatAsset::StaticClass();
    Formats.Add(TEXT("ply;Gaussian Splat PLY"));
}

UObject* UGaussianSplatAssetFactory::FactoryCreateFile(
    UClass* InClass,
    UObject* InParent,
    FName InName,
    EObjectFlags Flags,
    const FString& Filename,
    const TCHAR* Parms,
    FFeedbackContext* Warn,
    bool& bOutOperationCanceled)
{
    bOutOperationCanceled = false;

    TArray<uint8> RawData;
    if (!FFileHelper::LoadFileToArray(RawData, *Filename))
    {
        bOutOperationCanceled = true;
        return nullptr;
    }

    FPlyHeader Header;
    if (!ParseHeader(RawData, Header))
    {
        UE_LOG(LogGaussianSplatFactory, Error, TEXT("Invalid PLY header: %s"), *Filename);
        bOutOperationCanceled = true;
        return nullptr;
    }

    UGaussianSplatAsset* Asset = NewObject<UGaussianSplatAsset>(InParent, InClass, InName, Flags);

    const bool bImported = Header.bAscii
        ? FillAssetFromAscii(RawData, Header, *Asset)
        : FillAssetFromBinaryLE(RawData, Header, *Asset);

    if (!bImported)
    {
        UE_LOG(LogGaussianSplatFactory, Error, TEXT("Failed to parse PLY data: %s"), *Filename);
        bOutOperationCanceled = true;
        return nullptr;
    }

    FAssetRegistryModule::AssetCreated(Asset);
    if (Asset->GetPackage())
    {
        Asset->GetPackage()->MarkPackageDirty();
    }

    UE_LOG(LogGaussianSplatFactory, Display, TEXT("Imported %d points from %s"), Asset->GetPointCount(), *FPaths::GetCleanFilename(Filename));

    return Asset;
}
