// Copyright Solessfir 2026. All Rights Reserved.

using UnrealBuildTool;

public class LoreSourceControl : ModuleRules
{
	public LoreSourceControl(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"DesktopWidgets",
				"DeveloperSettings",
				"Engine",
				"Json",
				"MainFrame",
				"SourceControl",
				"SourceControlWindows",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"UnrealEd",
			}
		);
	}
}
