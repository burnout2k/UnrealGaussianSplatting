#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Render/GaussianSplatViewExtension.h"
#include "ShaderCore.h"

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplattingRuntimeModule, Log, All);

class FGaussianSplattingRuntimeModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        UE_LOG(LogGaussianSplattingRuntimeModule, Log, TEXT("StartupModule: GaussianSplattingRuntime starting."));

        MapShaderDirectory();

        if (GEngine)
        {
            CreateViewExtension();
        }
        else
        {
            UE_LOG(LogGaussianSplattingRuntimeModule, Log, TEXT("StartupModule: GEngine not ready, deferring ViewExtension creation until PostEngineInit."));
            PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FGaussianSplattingRuntimeModule::HandlePostEngineInit);
        }
    }

    virtual void ShutdownModule() override
    {
        if (PostEngineInitHandle.IsValid())
        {
            FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
            PostEngineInitHandle.Reset();
        }

        UE_LOG(
            LogGaussianSplattingRuntimeModule,
            Log,
            TEXT("ShutdownModule: Resetting ViewExtension. WasValid=%d"),
            ViewExtension.IsValid() ? 1 : 0);
        ViewExtension.Reset();
    }

private:
    void HandlePostEngineInit()
    {
        UE_LOG(LogGaussianSplattingRuntimeModule, Log, TEXT("HandlePostEngineInit: GEngine is ready, creating ViewExtension."));
        if (PostEngineInitHandle.IsValid())
        {
            FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
            PostEngineInitHandle.Reset();
        }

        CreateViewExtension();
    }

    void MapShaderDirectory()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealGaussianSplatting"));
        if (!Plugin.IsValid())
        {
            UE_LOG(LogGaussianSplattingRuntimeModule, Warning, TEXT("MapShaderDirectory: Plugin 'UnrealGaussianSplatting' not found."));
            return;
        }

        const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
        AddShaderSourceDirectoryMapping(TEXT("/GaussianSplatting"), ShaderDir);
        UE_LOG(LogGaussianSplattingRuntimeModule, Log, TEXT("MapShaderDirectory: Shader dir mapped to %s"), *ShaderDir);
    }

    void CreateViewExtension()
    {
        if (ViewExtension.IsValid())
        {
            UE_LOG(LogGaussianSplattingRuntimeModule, Log, TEXT("CreateViewExtension: ViewExtension already valid, skipping."));
            return;
        }

        ViewExtension = FSceneViewExtensions::NewExtension<FGaussianSplatViewExtension>();
        UE_LOG(
            LogGaussianSplattingRuntimeModule,
            Log,
            TEXT("CreateViewExtension: ViewExtension created. IsValid=%d GEngine=%d"),
            ViewExtension.IsValid() ? 1 : 0,
            GEngine ? 1 : 0);
    }

    FDelegateHandle PostEngineInitHandle;
    TSharedPtr<FGaussianSplatViewExtension, ESPMode::ThreadSafe> ViewExtension;
};

IMPLEMENT_MODULE(FGaussianSplattingRuntimeModule, GaussianSplattingRuntime)
