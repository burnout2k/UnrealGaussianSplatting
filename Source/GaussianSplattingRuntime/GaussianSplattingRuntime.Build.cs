using UnrealBuildTool;

public class GaussianSplattingRuntime : ModuleRules
{
	public GaussianSplattingRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",
			"RenderCore",
			"RHI",
			"RHICore",
			"Renderer"
		});
	}
}