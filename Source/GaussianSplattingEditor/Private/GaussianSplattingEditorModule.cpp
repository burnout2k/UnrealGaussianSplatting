#include "Modules/ModuleManager.h"

#include "AssetToolsModule.h"
#include "AssetTypeActions_Base.h"
#include "DetailCustomization/GaussianSplatAssetDetails.h"
#include "DetailCustomization/GaussianSplatComponentDetails.h"
#include "GaussianSplatAsset.h"
#include "IAssetTools.h"
#include "PropertyEditorModule.h"

// 这个类型动作只负责让内容浏览器认识这个自定义 Asset 类型，并给它一个显示名/颜色。
class FGaussianSplatAssetTypeActions final : public FAssetTypeActions_Base
{
public:
    virtual FText GetName() const override
    {
        return NSLOCTEXT("GaussianSplatting", "AssetTypeActions_GaussianSplatAsset", "Gaussian Splat Asset");
    }

    virtual FColor GetTypeColor() const override
    {
        return FColor(57, 190, 170);
    }

    virtual UClass* GetSupportedClass() const override
    {
        return UGaussianSplatAsset::StaticClass();
    }

    virtual uint32 GetCategories() override
    {
        return EAssetTypeCategories::Misc;
    }
};

class FGaussianSplattingEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        // 注册自定义 Asset 类型动作，让内容浏览器能把 GaussianSplatAsset 当成独立资源展示。
        IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
        const TSharedRef<IAssetTypeActions> Action = MakeShared<FGaussianSplatAssetTypeActions>();
        AssetTools.RegisterAssetTypeActions(Action);
        RegisteredAssetTypeActions.Add(Action);

        // 注册 Details 面板自定义布局，让 Asset 和 Component 的属性面板更易读。
        FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
        PropertyModule.RegisterCustomClassLayout(TEXT("GaussianSplatAsset"), FOnGetDetailCustomizationInstance::CreateStatic(&FGaussianSplatAssetDetails::MakeInstance));
        PropertyModule.RegisterCustomClassLayout(TEXT("GaussianSplatComponent"), FOnGetDetailCustomizationInstance::CreateStatic(&FGaussianSplatComponentDetails::MakeInstance));
        PropertyModule.NotifyCustomizationModuleChanged();
    }

    virtual void ShutdownModule() override
    {
        // 编辑器模块关闭时做对称反注册，避免模块热重载后残留旧注册项。
        if (!FModuleManager::Get().IsModuleLoaded(TEXT("AssetTools")))
        {
            return;
        }

        IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
        for (const TSharedRef<IAssetTypeActions>& Action : RegisteredAssetTypeActions)
        {
            AssetTools.UnregisterAssetTypeActions(Action);
        }
        RegisteredAssetTypeActions.Reset();

        if (FModuleManager::Get().IsModuleLoaded(TEXT("PropertyEditor")))
        {
            FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
            PropertyModule.UnregisterCustomClassLayout(TEXT("GaussianSplatAsset"));
            PropertyModule.UnregisterCustomClassLayout(TEXT("GaussianSplatComponent"));
            PropertyModule.NotifyCustomizationModuleChanged();
        }
    }

private:
    // 保存已注册的 AssetTypeActions，便于 Shutdown 时逐一注销。
    TArray<TSharedRef<IAssetTypeActions>> RegisteredAssetTypeActions;
};

IMPLEMENT_MODULE(FGaussianSplattingEditorModule, GaussianSplattingEditor)
