#include "Modules/ModuleManager.h"

#include "AssetToolsModule.h"
#include "AssetTypeActions_Base.h"
#include "DetailCustomization/GaussianSplatAssetDetails.h"
#include "DetailCustomization/GaussianSplatComponentDetails.h"
#include "GaussianSplatAsset.h"
#include "IAssetTools.h"
#include "PropertyEditorModule.h"

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
    }

    virtual void ShutdownModule() override
    {
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
    TArray<TSharedRef<IAssetTypeActions>> RegisteredAssetTypeActions;
};

IMPLEMENT_MODULE(FGaussianSplattingEditorModule, GaussianSplattingEditor)
