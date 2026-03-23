#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"

class FGaussianSplatRenderResources final : public FRenderResource
{
public:
    virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
    virtual void ReleaseRHI() override;
};
