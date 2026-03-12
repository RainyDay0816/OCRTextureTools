using UnrealBuildTool;

public class OcrTextureTools : ModuleRules
{
	public OcrTextureTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"Core",
				"DeveloperSettings"
			});

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"AssetRegistry",
				"AssetTools",
				"CoreUObject",
				"Engine",
				"MaterialEditor",
				"Slate",
				"SlateCore",
				"UnrealEd"
			});
	}
}
