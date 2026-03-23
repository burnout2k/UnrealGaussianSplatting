#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Render/GaussianSplatViewExtension.h"
#include "ShaderCore.h"

class FGaussianSplattingRuntimeModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GaussianSplatting"));
        if (Plugin.IsValid())
        {
            const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
            AddShaderSourceDirectoryMapping(TEXT("/GaussianSplatting"), ShaderDir);
        }

        ViewExtension = FSceneViewExtensions::NewExtension<FGaussianSplatViewExtension>();
    }

    virtual void ShutdownModule() override
    {
        ViewExtension.Reset();
    }

private:
    TSharedPtr<FGaussianSplatViewExtension, ESPMode::ThreadSafe> ViewExtension;
};

IMPLEMENT_MODULE(FGaussianSplattingRuntimeModule, GaussianSplattingRuntime)
