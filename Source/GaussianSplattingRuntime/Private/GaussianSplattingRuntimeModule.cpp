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
        // 告诉 UE 着色器编译系统，插件自带的 usf/ush 应该映射到哪个虚拟目录。
        MapShaderDirectory();

        // SceneViewExtension 依赖 GEngine，若引擎尚未初始化完成，就等到 PostEngineInit 再创建。
        if (GEngine)
        {
            CreateViewExtension();
        }
        else
        {
            PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FGaussianSplattingRuntimeModule::HandlePostEngineInit);
        }
    }

    virtual void ShutdownModule() override
    {
        // 如果之前注册过延迟初始化回调，退出时要移除。
        if (PostEngineInitHandle.IsValid())
        {
            FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
            PostEngineInitHandle.Reset();
        }

        ViewExtension.Reset();
    }

private:
    void HandlePostEngineInit()
    {
        if (PostEngineInitHandle.IsValid())
        {
            FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
            PostEngineInitHandle.Reset();
        }

        CreateViewExtension();
    }

    void MapShaderDirectory()
    {
        // 插件 Shader 文件位于磁盘目录 Shaders/，但 IMPLEMENT_GLOBAL_SHADER 里用的是虚拟路径 /GaussianSplatting/...。
        // 这里就是把两者关联起来。
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealGaussianSplatting"));
        if (!Plugin.IsValid())
        {
            return;
        }

        const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
        AddShaderSourceDirectoryMapping(TEXT("/GaussianSplatting"), ShaderDir);
    }

    void CreateViewExtension()
    {
        // ViewExtension 是插件接入 UE 渲染管线的核心对象。
        // 之后每个 ViewFamily 渲染时，UE 都会回调它，让插件有机会往后处理链里塞 RDG（渲染依赖图） Pass。
        if (ViewExtension.IsValid())
        {
            return;
        }

        ViewExtension = FSceneViewExtensions::NewExtension<FGaussianSplatViewExtension>();
    }

    FDelegateHandle PostEngineInitHandle;
    TSharedPtr<FGaussianSplatViewExtension, ESPMode::ThreadSafe> ViewExtension;
};

IMPLEMENT_MODULE(FGaussianSplattingRuntimeModule, GaussianSplattingRuntime)
