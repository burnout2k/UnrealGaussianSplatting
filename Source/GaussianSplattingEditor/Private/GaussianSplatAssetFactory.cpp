#include "GaussianSplatAssetFactory.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/StringConv.h"
#include "GaussianSplatAsset.h"
#include "Math/Matrix.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatFactory, Log, All);

namespace
{
    // Unity Gaussian Splatting 常用到的 SH 直流项常数。
    constexpr float SHC0 = 0.28209479177387814f;

    // PLY header 里可能出现的标量类型。
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

    // 记录一个 vertex property 的名字和类型。
    struct FPlyProperty
    {
        FString Name;
        EPlyScalarType Type = EPlyScalarType::Invalid;
    };

    // 这里只解析导入高斯所需的最小 PLY 头信息。
    struct FPlyHeader
    {
        bool bAscii = false;
        bool bBinaryLittleEndian = false;
        int32 VertexCount = 0;
        int32 HeaderByteSize = 0;
        TArray<FPlyProperty> VertexProperties;
    };

    // 把 PLY 类型名转成内部枚举。
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

    // 返回单个标量在二进制 PLY 中占多少字节。
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

    // 扫描 PLY header，确定编码方式、顶点数和 vertex property 列表。
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

    // 通过字段名查 property 在每行/每个顶点记录中的序号。
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

    // 二进制 PLY 中统一按 float 读回，便于后续复用同一套构建逻辑。
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

    // 安全读取一个字段，不存在时回落到默认值。
    float ReadValueOr(const TArray<float>& Values, int32 Index, float DefaultValue)
    {
        return Values.IsValidIndex(Index) ? Values[Index] : DefaultValue;
    }

    // 把源文件里的 SH rest 项重排成运行时 shader 期待的 [coeff][rgb] 紧密布局。
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

    // 导入阶段先整理成一个统一的中间表示，再写入 Asset。
    struct FImportedGaussian
    {
        FVector3f Position = FVector3f::ZeroVector;
        FQuat4f Rotation = FQuat4f::Identity;
        FVector3f Scale = FVector3f(0.02f, 0.02f, 0.02f);
    };

    // 在 3DGS 的参数存储中，缩放向量存储的是对数形式（Log-space），因此这里需要做一次 exp。
    FVector3f BuildScale(const TArray<float>& Values, int32 SX, int32 SY, int32 SZ)
    {
        if (Values.IsValidIndex(SX) && Values.IsValidIndex(SY) && Values.IsValidIndex(SZ))
        {
            return FVector3f(
                FMath::Exp(Values[SX]),
                FMath::Exp(Values[SY]),
                FMath::Exp(Values[SZ]));
        }

        return FVector3f(0.02f, 0.02f, 0.02f);
    }

    // 从 rot_0..3 还原四元数，字段顺序按 [w, x, y, z] 理解。
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

    // 自己展开四元数转矩阵，避免在这个低层工具区引入更多高层类型转换。
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

    // 只对左上 3x3 做乘法，因为这里处理的都是协方差/旋转子矩阵。
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

    // 只转置左上 3x3。
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

    // 由旋转和尺度恢复 3D 协方差矩阵。
    FMatrix44f BuildCovariance(const FQuat4f& Rotation, const FVector3f& Scale)
    {
        const FMatrix44f RotationMatrix = BuildRotationMatrix(Rotation);

        FMatrix44f ScaleMatrix = FMatrix44f::Identity;
        ScaleMatrix.M[0][0] = Scale.X;
        ScaleMatrix.M[1][1] = Scale.Y;
        ScaleMatrix.M[2][2] = Scale.Z;

        FMatrix44f L = Multiply3x3(RotationMatrix, ScaleMatrix);
        
        return Multiply3x3(L, Transpose3x3(L));
    }

    // 把 COLMAP 坐标系转换成 UE 坐标系。
    FVector3f ApplyColmapToUEPosition(const FVector3f& Position)
    {
        return FVector3f(Position.Z, Position.X, -Position.Y);
    }

    // 协方差也必须在同一线性变换下同步变换，否则位置和椭球方向会不一致。
    FMatrix44f ApplyColmapToUECovariance(const FMatrix44f& Covariance)
    {
        FMatrix44f Transform = FMatrix44f::Identity;
        Transform.M[0][0] = 0.0f;
        Transform.M[0][1] = 0.0f;
        Transform.M[0][2] = 1.0f;
        Transform.M[1][0] = 1.0f;
        Transform.M[1][1] = 0.0f;
        Transform.M[1][2] = 0.0f;
        Transform.M[2][0] = 0.0f;
        Transform.M[2][1] = -1.0f;
        Transform.M[2][2] = 0.0f;

        return Multiply3x3(Multiply3x3(Transform, Covariance), Transpose3x3(Transform));
    }

    // 对对称 3x3 协方差做 Jacobi 特征分解，取出主轴方向和三个特征值。
    void JacobiDiagonalizeSymmetric3x3(const FMatrix44f& Input, FMatrix44f& OutEigenvectors, FVector3f& OutEigenvalues)
    {
        FMatrix44f A = Input;
        OutEigenvectors = FMatrix44f::Identity;

        for (int32 Iteration = 0; Iteration < 12; ++Iteration)
        {
            int32 P = 0;
            int32 Q = 1;
            float MaxOffDiag = FMath::Abs(A.M[0][1]);

            auto ConsiderPair = [&](int32 Row, int32 Col)
            {
                const float Value = FMath::Abs(A.M[Row][Col]);
                if (Value > MaxOffDiag)
                {
                    MaxOffDiag = Value;
                    P = Row;
                    Q = Col;
                }
            };

            ConsiderPair(0, 2);
            ConsiderPair(1, 2);

            if (MaxOffDiag < 1e-6f)
            {
                break;
            }

            const float App = A.M[P][P];
            const float Aqq = A.M[Q][Q];
            const float Apq = A.M[P][Q];
            const float Tau = (Aqq - App) / (2.0f * Apq);
            const float T = (Tau >= 0.0f)
                ? 1.0f / (Tau + FMath::Sqrt(1.0f + Tau * Tau))
                : -1.0f / (-Tau + FMath::Sqrt(1.0f + Tau * Tau));
            const float C = 1.0f / FMath::Sqrt(1.0f + T * T);
            const float S = T * C;

            for (int32 K = 0; K < 3; ++K)
            {
                if (K == P || K == Q)
                {
                    continue;
                }

                const float Akp = A.M[K][P];
                const float Akq = A.M[K][Q];
                A.M[K][P] = C * Akp - S * Akq;
                A.M[P][K] = A.M[K][P];
                A.M[K][Q] = S * Akp + C * Akq;
                A.M[Q][K] = A.M[K][Q];
            }

            A.M[P][P] = C * C * App - 2.0f * S * C * Apq + S * S * Aqq;
            A.M[Q][Q] = S * S * App + 2.0f * S * C * Apq + C * C * Aqq;
            A.M[P][Q] = 0.0f;
            A.M[Q][P] = 0.0f;

            for (int32 K = 0; K < 3; ++K)
            {
                const float Vip = OutEigenvectors.M[K][P];
                const float Viq = OutEigenvectors.M[K][Q];
                OutEigenvectors.M[K][P] = C * Vip - S * Viq;
                OutEigenvectors.M[K][Q] = S * Vip + C * Viq;
            }
        }

        OutEigenvalues = FVector3f(A.M[0][0], A.M[1][1], A.M[2][2]);
    }

    // 把特征值按从大到小排序，并保持特征向量构成右手系。
    void SortEigenbasisDescending(FMatrix44f& InOutEigenvectors, FVector3f& InOutEigenvalues)
    {
        auto GetComponent = [](const FVector3f& Vector, int32 Index) -> float
        {
            switch (Index)
            {
            case 0: return Vector.X;
            case 1: return Vector.Y;
            default: return Vector.Z;
            }
        };

        int32 Order[3] = { 0, 1, 2 };
        for (int32 I = 0; I < 3; ++I)
        {
            for (int32 J = I + 1; J < 3; ++J)
            {
                if (GetComponent(InOutEigenvalues, Order[J]) > GetComponent(InOutEigenvalues, Order[I]))
                {
                    Swap(Order[I], Order[J]);
                }
            }
        }

        FVector3f SortedValues;
        SortedValues.X = GetComponent(InOutEigenvalues, Order[0]);
        SortedValues.Y = GetComponent(InOutEigenvalues, Order[1]);
        SortedValues.Z = GetComponent(InOutEigenvalues, Order[2]);

        FMatrix44f SortedVectors = FMatrix44f::Identity;
        for (int32 Col = 0; Col < 3; ++Col)
        {
            for (int32 Row = 0; Row < 3; ++Row)
            {
                SortedVectors.M[Row][Col] = InOutEigenvectors.M[Row][Order[Col]];
            }
        }

        const FVector3f C0(SortedVectors.M[0][0], SortedVectors.M[1][0], SortedVectors.M[2][0]);
        const FVector3f C1(SortedVectors.M[0][1], SortedVectors.M[1][1], SortedVectors.M[2][1]);
        const FVector3f C2(SortedVectors.M[0][2], SortedVectors.M[1][2], SortedVectors.M[2][2]);
        if (FVector3f::DotProduct(FVector3f::CrossProduct(C0, C1), C2) < 0.0f)
        {
            for (int32 Row = 0; Row < 3; ++Row)
            {
                SortedVectors.M[Row][2] *= -1.0f;
            }
        }

        InOutEigenvectors = SortedVectors;
        InOutEigenvalues = SortedValues;
    }

    // 把主轴矩阵重新编码成四元数，便于运行时 shader 复用。
    FQuat4f QuaternionFromRotationMatrix(const FMatrix44f& Matrix)
    {
        const float Trace = Matrix.M[0][0] + Matrix.M[1][1] + Matrix.M[2][2];
        float X;
        float Y;
        float Z;
        float W;

        if (Trace > 0.0f)
        {
            const float S = FMath::Sqrt(Trace + 1.0f) * 2.0f;
            W = 0.25f * S;
            X = (Matrix.M[2][1] - Matrix.M[1][2]) / S;
            Y = (Matrix.M[0][2] - Matrix.M[2][0]) / S;
            Z = (Matrix.M[1][0] - Matrix.M[0][1]) / S;
        }
        else if (Matrix.M[0][0] > Matrix.M[1][1] && Matrix.M[0][0] > Matrix.M[2][2])
        {
            const float S = FMath::Sqrt(1.0f + Matrix.M[0][0] - Matrix.M[1][1] - Matrix.M[2][2]) * 2.0f;
            W = (Matrix.M[2][1] - Matrix.M[1][2]) / S;
            X = 0.25f * S;
            Y = (Matrix.M[0][1] + Matrix.M[1][0]) / S;
            Z = (Matrix.M[0][2] + Matrix.M[2][0]) / S;
        }
        else if (Matrix.M[1][1] > Matrix.M[2][2])
        {
            const float S = FMath::Sqrt(1.0f + Matrix.M[1][1] - Matrix.M[0][0] - Matrix.M[2][2]) * 2.0f;
            W = (Matrix.M[0][2] - Matrix.M[2][0]) / S;
            X = (Matrix.M[0][1] + Matrix.M[1][0]) / S;
            Y = 0.25f * S;
            Z = (Matrix.M[1][2] + Matrix.M[2][1]) / S;
        }
        else
        {
            const float S = FMath::Sqrt(1.0f + Matrix.M[2][2] - Matrix.M[0][0] - Matrix.M[1][1]) * 2.0f;
            W = (Matrix.M[1][0] - Matrix.M[0][1]) / S;
            X = (Matrix.M[0][2] + Matrix.M[2][0]) / S;
            Y = (Matrix.M[1][2] + Matrix.M[2][1]) / S;
            Z = 0.25f * S;
        }

        FQuat4f Result(X, Y, Z, W);
        Result.Normalize();
        return Result;
    }

    // 统一构建导入后的高斯：
    // - 读取位置/尺度/旋转；
    // - 若源数据包含 anisotropic Gaussian 参数，则顺便做坐标系转换和协方差重分解。
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
        Result.Rotation = BuildRotation(Values, Rot0Index, Rot1Index, Rot2Index, Rot3Index);
        Result.Scale = BuildScale(Values, Scale0Index, Scale1Index, Scale2Index);
        const FMatrix44f CovarianceUE = ApplyColmapToUECovariance(BuildCovariance(Result.Rotation, Result.Scale));
        // 以上的 Position、Covariance 都已经由 Colmap 坐标系转换到了 UE 坐标系。
        // 如果在 Shader 中直接使用协方差矩阵（例如进行 EWA Splatting），不需要单独处理旋转和缩放。
        // SH 系数决定了高斯点的颜色随视角的变化。当旋转了坐标系，SH 的基函数方向也变了。
        // 对于 Level 0 (DC 项)：它是常数，代表基础颜色，不需要旋转。
        // 对于 Level 1 及以上：它们具有方向性。需要旋转。可以在渲染时将“相机观察向量”转换回 COLMAP 空间再去采样 SH，避免复杂的 SH 旋转计算。
        
        // 为什么要把协方差矩阵又转回缩放向量和四元数？？？
        // 如果在代码中直接把 OutEigenvalues 的 x, y, z 塞回 scale_0, 1, 2，
        // 而没有调整对应的特征向量（旋转矩阵的列），高斯球的方向会直接偏转 90 度或 180 度，导致渲染出来的物体表面全是细碎的毛刺。
        FMatrix44f Eigenvectors;
        FVector3f Eigenvalues;
        JacobiDiagonalizeSymmetric3x3(CovarianceUE, Eigenvectors, Eigenvalues);
        SortEigenbasisDescending(Eigenvectors, Eigenvalues);

        Result.Scale = FVector3f(
            FMath::Sqrt(FMath::Max(Eigenvalues.X, 1e-10f)),
            FMath::Sqrt(FMath::Max(Eigenvalues.Y, 1e-10f)),
            FMath::Sqrt(FMath::Max(Eigenvalues.Z, 1e-10f)));
        Result.Rotation = QuaternionFromRotationMatrix(Eigenvectors);
        
        return Result;
    }
    
    // RGB 来自 SH 直流项，A 来自 sigmoid(opacity)。
    FLinearColor BuildColor(const TArray<float>& Values, int32 Dc0, int32 Dc1, int32 Dc2, int32 Opacity)
    {
        const float R = 0.5f + SHC0 * ReadValueOr(Values, Dc0, 0.0f);
        const float G = 0.5f + SHC0 * ReadValueOr(Values, Dc1, 0.0f);
        const float B = 0.5f + SHC0 * ReadValueOr(Values, Dc2, 0.0f);
        const float A = 1.0f / (1.0f + FMath::Exp(-ReadValueOr(Values, Opacity, 0.0f)));
        return FLinearColor(R, G, B, A);
    }

    // 解析 ASCII PLY。
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
        TArray<int32> RestIndices;
        RestIndices.Reserve(45);
        for (int32 SHIndex = 0; SHIndex < 45; ++SHIndex)
        {
            RestIndices.Add(FindPropertyIndex(Header.VertexProperties, *FString::Printf(TEXT("f_rest_%d"), SHIndex)));
        }

        // 没有基础位置字段就无法导入。
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
            // 先把一行顶点记录打平成 float 数组，再走统一构建逻辑。
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
                Values,
                XIndex,
                YIndex,
                ZIndex,
                Scale0Index,
                Scale1Index,
                Scale2Index,
                Rot0Index,
                Rot1Index,
                Rot2Index,
                Rot3Index);
            Asset.Positions.Add(Gaussian.Position);
            Asset.Rotations.Add(Gaussian.Rotation);
            Asset.Scales.Add(Gaussian.Scale);

            const FLinearColor Color = BuildColor(Values, Dc0Index, Dc1Index, Dc2Index, OpacityIndex);
            Asset.ColorsOpacity.Add(FVector4f(Color.R, Color.G, Color.B, Color.A));
            AppendReorderedSH(Values, RestIndices, Asset);
        }

        // 写完 CPU 数据后立即刷新 Bounds 和 GPU 资源。
        Asset.RefreshDerivedData();
        return true;
    }

    // 解析 binary_little_endian PLY，和 ASCII 路径的差别主要在于“如何取每个字段的值”。
    bool FillAssetFromBinaryLE(const TArray<uint8>& RawData, const FPlyHeader& Header, UGaussianSplatAsset& Asset)
    {
        const int32 XIndex = FindPropertyIndex(Header.VertexProperties, TEXT("x"));
        const int32 YIndex = FindPropertyIndex(Header.VertexProperties, TEXT("y"));
        const int32 ZIndex = FindPropertyIndex(Header.VertexProperties, TEXT("z"));
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

        // 先算出一条顶点记录的总字节数，便于校验 body 是否完整。
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
            // 按 header 中声明的字段顺序逐个解码，再走和 ASCII 路径相同的构建逻辑。
            TArray<float> Values;
            Values.Reserve(Header.VertexProperties.Num());

            for (const FPlyProperty& Property : Header.VertexProperties)
            {
                Values.Add(ReadScalarAsFloat(Cursor, Property.Type));
                Cursor += GetTypeSize(Property.Type);
            }

            const FImportedGaussian Gaussian = BuildImportedGaussian(
                Values,
                XIndex,
                YIndex,
                ZIndex,
                Scale0Index,
                Scale1Index,
                Scale2Index,
                Rot0Index,
                Rot1Index,
                Rot2Index,
                Rot3Index);
            Asset.Positions.Add(Gaussian.Position);
            Asset.Rotations.Add(Gaussian.Rotation);
            Asset.Scales.Add(Gaussian.Scale);

            const FLinearColor Color = BuildColor(Values, Dc0Index, Dc1Index, Dc2Index, OpacityIndex);
            Asset.ColorsOpacity.Add(FVector4f(Color.R, Color.G, Color.B, Color.A));
            AppendReorderedSH(Values, RestIndices, Asset);
        }

        Asset.RefreshDerivedData();
        return true;
    }
}

UGaussianSplatAssetFactory::UGaussianSplatAssetFactory()
{
    // 这是一个“只能导入文件”的工厂，不支持在内容浏览器里凭空创建新 Asset。
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

    // 导入器整体流程：
    // 1. 读原始文件；
    // 2. 解析 header；
    // 3. 创建目标 UGaussianSplatAsset；
    // 4. 按 ASCII / Binary 分支填充 Asset；
    // 5. 注册到 AssetRegistry。
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
        (void)Asset->GetPackage()->MarkPackageDirty();
    }

    UE_LOG(LogGaussianSplatFactory, Display, TEXT("Imported %d points from %s"), Asset->GetPointCount(), *FPaths::GetCleanFilename(Filename));

    return Asset;
}
