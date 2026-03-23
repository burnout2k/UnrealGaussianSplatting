using UnrealBuildTool;

public class GaussianSplattingEditor : ModuleRules
{
    public GaussianSplattingEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "GaussianSplattingRuntime"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "AssetRegistry",
            "AssetTools",
            "EditorFramework",
            "InteractiveToolsFramework",
            "LevelEditor",
            "PropertyEditor",
            "Slate",
            "SlateCore",
            "ToolMenus",
            "UnrealEd"
        });
    }
}
