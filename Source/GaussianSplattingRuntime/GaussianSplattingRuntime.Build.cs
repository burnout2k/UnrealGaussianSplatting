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

		// The Fix 5 probe reads VK_EXT_memory_budget and writes its own timestamps through the Vulkan RHI's interface.
		if (Target.Platform == UnrealTargetPlatform.Linux || Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("VulkanRHI");
			AddEngineThirdPartyPrivateStaticDependencies(Target, "Vulkan");
			PrivateDefinitions.Add("GAUSSIANSPLAT_WITH_VULKAN=1");
		}
		else
		{
			PrivateDefinitions.Add("GAUSSIANSPLAT_WITH_VULKAN=0");
		}

		// MIT requires the GPUSorting notice in every copy, and packages ship the compiled sort
		// shaders without their sources.
		RuntimeDependencies.Add("$(PluginDir)/ThirdPartyNotices.txt");
	}
}