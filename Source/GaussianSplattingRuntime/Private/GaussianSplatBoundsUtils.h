#pragma once

#include "CoreMinimal.h"
#include "GaussianSplatAsset.h"

namespace GaussianSplatBoundsUtils
{
    inline FGaussianCovariance3f MakeIsotropic(float Radius)
    {
        const float Variance = Radius * Radius;
        return FGaussianCovariance3f(Variance, 0.0f, 0.0f, Variance, 0.0f, Variance);
    }

    namespace Private
    {
        inline FMatrix44f ToMatrix(const FGaussianCovariance3f& Covariance)
        {
            FMatrix44f Matrix = FMatrix44f::Identity;
            Matrix.M[0][0] = Covariance.XX;
            Matrix.M[0][1] = Covariance.XY;
            Matrix.M[0][2] = Covariance.XZ;
            Matrix.M[1][0] = Covariance.XY;
            Matrix.M[1][1] = Covariance.YY;
            Matrix.M[1][2] = Covariance.YZ;
            Matrix.M[2][0] = Covariance.XZ;
            Matrix.M[2][1] = Covariance.YZ;
            Matrix.M[2][2] = Covariance.ZZ;
            return Matrix;
        }

        inline void JacobiDiagonalizeSymmetric3x3(const FMatrix44f& Input, FMatrix44f& OutEigenvectors, FVector3f& OutEigenvalues)
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

        inline void SortEigenbasisDescending(FMatrix44f& InOutEigenvectors, FVector3f& InOutEigenvalues)
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
    }

    inline FVector ComputeExtent(const FGaussianCovariance3f& Covariance)
    {
        FMatrix44f Eigenvectors;
        FVector3f Eigenvalues;
        Private::JacobiDiagonalizeSymmetric3x3(Private::ToMatrix(Covariance), Eigenvectors, Eigenvalues);
        Private::SortEigenbasisDescending(Eigenvectors, Eigenvalues);

        const FVector3f Axis0 = FVector3f(Eigenvectors.M[0][0], Eigenvectors.M[1][0], Eigenvectors.M[2][0]) *
            FMath::Sqrt(FMath::Max(Eigenvalues.X, 1e-10f));
        const FVector3f Axis1 = FVector3f(Eigenvectors.M[0][1], Eigenvectors.M[1][1], Eigenvectors.M[2][1]) *
            FMath::Sqrt(FMath::Max(Eigenvalues.Y, 1e-10f));
        const FVector3f Axis2 = FVector3f(Eigenvectors.M[0][2], Eigenvectors.M[1][2], Eigenvectors.M[2][2]) *
            FMath::Sqrt(FMath::Max(Eigenvalues.Z, 1e-10f));

        return FVector(
            FMath::Abs(Axis0.X) + FMath::Abs(Axis1.X) + FMath::Abs(Axis2.X),
            FMath::Abs(Axis0.Y) + FMath::Abs(Axis1.Y) + FMath::Abs(Axis2.Y),
            FMath::Abs(Axis0.Z) + FMath::Abs(Axis1.Z) + FMath::Abs(Axis2.Z));
    }
}
