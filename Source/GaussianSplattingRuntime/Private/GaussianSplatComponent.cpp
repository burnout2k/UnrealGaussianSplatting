#include "GaussianSplatComponent.h"

#include "Engine/Texture2D.h"
#include "GaussianSplatAsset.h"
#include "Render/GaussianSplatSceneProxy.h"

FPrimitiveSceneProxy* UGaussianSplatComponent::CreateSceneProxy()
{
    if (!Asset)
    {
        return nullptr;
    }

    if (PreviewRenderMode == EGaussianPreviewRenderMode::Billboards)
    {
        return nullptr;
    }

    return new FGaussianSplatSceneProxy(this);
}

FBoxSphereBounds UGaussianSplatComponent::CalcBounds(const FTransform& LocalToWorld) const
{
    if (!Asset)
    {
        return FBoxSphereBounds(EForceInit::ForceInitToZero);
    }

    return Asset->Bounds.TransformBy(LocalToWorld);
}

void UGaussianSplatComponent::OnRegister()
{
    Super::OnRegister();
    EnsureGaussianFalloffTexture();
    MarkRenderStateDirty();
}

void UGaussianSplatComponent::OnUnregister()
{
    Super::OnUnregister();
}

void UGaussianSplatComponent::SendRenderDynamicData_Concurrent()
{
    Super::SendRenderDynamicData_Concurrent();
}

void UGaussianSplatComponent::SetGaussianAsset(UGaussianSplatAsset* InAsset)
{
    Asset = InAsset;
    MarkRenderStateDirty();
}

void UGaussianSplatComponent::BuildDefaultGaussianFalloffTexture()
{
    constexpr int32 TextureSize = 64;
    TArray<uint8> PixelData;
    PixelData.SetNumUninitialized(TextureSize * TextureSize * 4);

    const float Half = (TextureSize - 1) * 0.5f;
    for (int32 Y = 0; Y < TextureSize; ++Y)
    {
        for (int32 X = 0; X < TextureSize; ++X)
        {
            const float DX = (X - Half) / Half;
            const float DY = (Y - Half) / Half;
            const float R2 = DX * DX + DY * DY;

            const float Gaussian = FMath::Exp(-R2 * 3.5f);
            const float Alpha = FMath::Clamp(Gaussian, 0.0f, 1.0f);

            const int32 PixelIndex = (Y * TextureSize + X) * 4;
            PixelData[PixelIndex + 0] = 255;
            PixelData[PixelIndex + 1] = 255;
            PixelData[PixelIndex + 2] = 255;
            PixelData[PixelIndex + 3] = static_cast<uint8>(Alpha * 255.0f);
        }
    }

    GaussianFalloffTexture = UTexture2D::CreateTransient(TextureSize, TextureSize, PF_B8G8R8A8, TEXT("GaussianSplatFalloff"), PixelData);
    if (GaussianFalloffTexture)
    {
        GaussianFalloffTexture->Filter = TF_Bilinear;
        GaussianFalloffTexture->AddressX = TA_Clamp;
        GaussianFalloffTexture->AddressY = TA_Clamp;
        GaussianFalloffTexture->SRGB = false;
        GaussianFalloffTexture->UpdateResource();
    }
}

void UGaussianSplatComponent::EnsureGaussianFalloffTexture()
{
    if (!GaussianFalloffTexture)
    {
        BuildDefaultGaussianFalloffTexture();
    }
}

#if WITH_EDITOR
void UGaussianSplatComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    EnsureGaussianFalloffTexture();
    MarkRenderStateDirty();
}
#endif
