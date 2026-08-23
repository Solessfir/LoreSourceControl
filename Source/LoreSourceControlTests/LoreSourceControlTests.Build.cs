// Copyright Solessfir 2026. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class LoreSourceControlTests : ModuleRules
{
	public LoreSourceControlTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"LoreSourceControl",
				"SourceControl",
			}
		);

		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "LoreSourceControl", "Private"));
	}
}
