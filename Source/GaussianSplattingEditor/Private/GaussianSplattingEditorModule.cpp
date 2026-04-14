#include "Modules/ModuleManager.h"

#include "AssetToolsModule.h"
#include "AssetTypeActions_Base.h"
#include "DetailCustomization/GaussianSplatAssetDetails.h"
#include "DetailCustomization/GaussianSplatComponentDetails.h"
#include "GaussianSplatAsset.h"
#include "Framework/Docking/TabManager.h"
#include "IAssetTools.h"
#include "LevelEditor.h"
#include "PropertyEditorModule.h"
#include "ToolMenus.h"
#include "UI/SGaussianSplatEditorPanel.h"
#include "Widgets/Docking/SDockTab.h"

namespace
{
    static const FName GaussianSplatEditorTabName(TEXT("GaussianSplatEditor"));
}

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
        IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
        const TSharedRef<IAssetTypeActions> Action = MakeShared<FGaussianSplatAssetTypeActions>();
        AssetTools.RegisterAssetTypeActions(Action);
        RegisteredAssetTypeActions.Add(Action);

        FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
        PropertyModule.RegisterCustomClassLayout(TEXT("GaussianSplatAsset"), FOnGetDetailCustomizationInstance::CreateStatic(&FGaussianSplatAssetDetails::MakeInstance));
        PropertyModule.RegisterCustomClassLayout(TEXT("GaussianSplatComponent"), FOnGetDetailCustomizationInstance::CreateStatic(&FGaussianSplatComponentDetails::MakeInstance));
        PropertyModule.NotifyCustomizationModuleChanged();

        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
            GaussianSplatEditorTabName,
            FOnSpawnTab::CreateRaw(this, &FGaussianSplattingEditorModule::SpawnEditorTab))
            .SetDisplayName(NSLOCTEXT("GaussianSplatting", "GaussianSplatEditorTabTitle", "Gaussian Splat Editor"))
            .SetMenuType(ETabSpawnerMenuType::Hidden);

        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FGaussianSplattingEditorModule::RegisterMenus));
    }

    virtual void ShutdownModule() override
    {
        if (UToolMenus* ToolMenus = UToolMenus::TryGet())
        {
            UToolMenus::UnRegisterStartupCallback(this);
            ToolMenus->UnregisterOwner(this);
        }

        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GaussianSplatEditorTabName);

        if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetTools")))
        {
            IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
            for (const TSharedRef<IAssetTypeActions>& Action : RegisteredAssetTypeActions)
            {
                AssetTools.UnregisterAssetTypeActions(Action);
            }
            RegisteredAssetTypeActions.Reset();
        }

        if (FModuleManager::Get().IsModuleLoaded(TEXT("PropertyEditor")))
        {
            FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
            PropertyModule.UnregisterCustomClassLayout(TEXT("GaussianSplatAsset"));
            PropertyModule.UnregisterCustomClassLayout(TEXT("GaussianSplatComponent"));
            PropertyModule.NotifyCustomizationModuleChanged();
        }
    }

private:
    TSharedRef<SDockTab> SpawnEditorTab(const FSpawnTabArgs& Args)
    {
        return SNew(SDockTab)
            .TabRole(ETabRole::NomadTab)
            [
                SNew(SGaussianSplatEditorPanel)
            ];
    }

    void RegisterMenus()
    {
        FToolMenuOwnerScoped OwnerScoped(this);

        UToolMenu* WindowMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window"));
        FToolMenuSection& Section = WindowMenu->FindOrAddSection(TEXT("WindowLayout"));
        Section.AddMenuEntry(
            TEXT("OpenGaussianSplatEditor"),
            NSLOCTEXT("GaussianSplatting", "OpenGaussianSplatEditor", "Gaussian Splat Editor"),
            NSLOCTEXT("GaussianSplatting", "OpenGaussianSplatEditorTooltip", "Open the Gaussian Splat editor panel."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateRaw(this, &FGaussianSplattingEditorModule::OpenEditorTab)));
    }

    void OpenEditorTab()
    {
        FGlobalTabmanager::Get()->TryInvokeTab(GaussianSplatEditorTabName);
    }

    TArray<TSharedRef<IAssetTypeActions>> RegisteredAssetTypeActions;
};

IMPLEMENT_MODULE(FGaussianSplattingEditorModule, GaussianSplattingEditor)
