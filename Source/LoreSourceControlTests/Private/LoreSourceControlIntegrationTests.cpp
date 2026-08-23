// Copyright Solessfir 2026. All Rights Reserved.

#include "LoreSourceControlUtils.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	class FTemporaryLoreRepository
	{
	public:
		FTemporaryLoreRepository()
		{
			Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/LoreSourceControl"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			FPaths::NormalizeDirectoryName(Root);
		}

		~FTemporaryLoreRepository()
		{
			Cleanup();
		}

		bool Create() const
		{
			return IFileManager::Get().MakeDirectory(*Root, true);
		}

		bool Cleanup() const
		{
			return !IFileManager::Get().DirectoryExists(*Root) || IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		const FString& GetRoot() const
		{
			return Root;
		}

	private:
		FString Root;
	};

	FString FormatCommandErrors(const TArray<FString>& Errors)
	{
		return Errors.IsEmpty() ? TEXT("Lore returned no error details.") : FString::Join(Errors, TEXT("\n"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreTemporaryRepositoryIntegrationTest, "LoreSourceControl.Integration.TemporaryRepository", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreTemporaryRepositoryIntegrationTest::RunTest(const FString& Parameters)
{
	const FString LoreBinary = FLoreSourceControlUtils::FindLoreBinaryPath();
	FString LoreVersion;
	if (!FLoreSourceControlUtils::CheckLoreAvailability(LoreBinary, &LoreVersion))
	{
		AddError(FString::Printf(TEXT("The temporary repository test requires a working Lore binary. Tried: %s"), *LoreBinary));
		return false;
	}

	FTemporaryLoreRepository Repository;
	if (!Repository.Create())
	{
		AddError(FString::Printf(TEXT("Could not create temporary test directory: %s"), *Repository.GetRoot()));
		return false;
	}

	TArray<FString> Results;
	TArray<FString> Errors;
	const TArray<FString> CreateParameters{
		TEXT("--offline"),
		TEXT("--identity"),
		FLoreSourceControlUtils::QuoteCommandLineArgument(TEXT("Lore automation")),
		TEXT("--repository"),
		FLoreSourceControlUtils::QuoteCommandLineArgument(Repository.GetRoot()),
		TEXT("--no-gc"),
		FLoreSourceControlUtils::QuoteCommandLineArgument(TEXT("integration-test"))
	};
	if (!FLoreSourceControlUtils::RunLoreCommand(TEXT("repository create"), LoreBinary, Repository.GetRoot(), CreateParameters, TArray<FString>(), Results, Errors))
	{
		AddError(FString::Printf(TEXT("Could not initialize the temporary Lore repository:\n%s"), *FormatCommandErrors(Errors)));
		return false;
	}

	const FString ConfigPath = FPaths::Combine(Repository.GetRoot(), TEXT(".lore/config.toml"));
	TestTrue(TEXT("Repository configuration is created"), FPaths::FileExists(ConfigPath));

	FString ConfigContents;
	TestTrue(TEXT("Repository configuration is readable"), FFileHelper::LoadFileToString(ConfigContents, *ConfigPath));
	TestTrue(TEXT("Configured identity is retained"), ConfigContents.Contains(TEXT("Lore automation")));

	const FString TestFile = FPaths::Combine(Repository.GetRoot(), TEXT("Integration.txt"));
	if (!FFileHelper::SaveStringToFile(TEXT("Temporary Lore integration test\n"), *TestFile))
	{
		AddError(FString::Printf(TEXT("Could not write temporary repository file: %s"), *TestFile));
		return false;
	}

	Results.Reset();
	Errors.Reset();
	const TArray<FString> StageParameters{ TEXT("--scan"), TEXT("--no-gc") };
	if (!FLoreSourceControlUtils::RunLoreCommand(TEXT("stage"), LoreBinary, Repository.GetRoot(), StageParameters, TArray<FString>{ TestFile }, Results, Errors))
	{
		AddError(FString::Printf(TEXT("Could not stage the temporary repository file:\n%s"), *FormatCommandErrors(Errors)));
		return false;
	}

	TArray<FString> StagedFiles;
	TArray<FString> StagedDirectories;
	Errors.Reset();
	if (!FLoreSourceControlUtils::RunGetStagedPaths(LoreBinary, Repository.GetRoot(), StagedFiles, StagedDirectories, Errors))
	{
		AddError(FString::Printf(TEXT("Could not inspect the temporary repository stage:\n%s"), *FormatCommandErrors(Errors)));
		return false;
	}

	FString NormalizedTestFile = FPaths::ConvertRelativePathToFull(TestFile);
	FPaths::NormalizeFilename(NormalizedTestFile);
	NormalizedTestFile.ReplaceInline(TEXT("\\"), TEXT("/"));
	TestTrue(TEXT("Staged file is reported by Lore"), StagedFiles.Contains(NormalizedTestFile));

	Results.Reset();
	Errors.Reset();
	const TArray<FString> CommitParameters{ TEXT("--no-gc"), FLoreSourceControlUtils::QuoteCommandLineArgument(TEXT("Temporary repository integration test")) };
	if (!FLoreSourceControlUtils::RunLoreCommand(TEXT("commit"), LoreBinary, Repository.GetRoot(), CommitParameters, TArray<FString>(), Results, Errors))
	{
		AddError(FString::Printf(TEXT("Could not commit the temporary repository file:\n%s"), *FormatCommandErrors(Errors)));
		return false;
	}

	StagedFiles.Reset();
	StagedDirectories.Reset();
	Errors.Reset();
	if (!FLoreSourceControlUtils::RunGetStagedPaths(LoreBinary, Repository.GetRoot(), StagedFiles, StagedDirectories, Errors))
	{
		AddError(FString::Printf(TEXT("Could not inspect the temporary repository after commit:\n%s"), *FormatCommandErrors(Errors)));
		return false;
	}

	TestTrue(TEXT("Commit clears staged files"), StagedFiles.IsEmpty());
	TestTrue(TEXT("Commit clears staged directories"), StagedDirectories.IsEmpty());
	TestTrue(TEXT("Temporary repository is removed"), Repository.Cleanup());
	return true;
}

#endif
