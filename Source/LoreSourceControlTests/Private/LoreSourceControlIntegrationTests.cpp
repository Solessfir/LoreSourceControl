// Copyright Solessfir 2026. All Rights Reserved.

#include "LoreSourceControlUtils.h"
#include "LoreSourceControlCommand.h"
#include "LoreSourceControlOperations.h"
#include "LoreSourceControlProvider.h"
#include "Features/IModularFeatures.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SourceControlOperations.h"

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
	const FString FirstRevisionFile = FPaths::Combine(Repository.GetRoot(), TEXT("First/Same.txt"));
	const FString SecondRevisionFile = FPaths::Combine(Repository.GetRoot(), TEXT("Second/Same.txt"));
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(FirstRevisionFile), true)
		|| !IFileManager::Get().MakeDirectory(*FPaths::GetPath(SecondRevisionFile), true)
		|| !FFileHelper::SaveStringToFile(TEXT("First revision contents\n"), *FirstRevisionFile)
		|| !FFileHelper::SaveStringToFile(TEXT("Second revision contents\n"), *SecondRevisionFile)
		|| !FFileHelper::SaveStringToFile(TEXT("Temporary Lore integration test\n"), *TestFile))
	{
		AddError(FString::Printf(TEXT("Could not write temporary repository file: %s"), *TestFile));
		return false;
	}

	Results.Reset();
	Errors.Reset();
	const TArray<FString> StageParameters{ TEXT("--scan"), TEXT("--no-gc") };
	if (!FLoreSourceControlUtils::RunLoreCommand(TEXT("stage"), LoreBinary, Repository.GetRoot(), StageParameters, TArray<FString>{ TestFile, FirstRevisionFile, SecondRevisionFile }, Results, Errors))
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
	FLoreSourceControlHistory FirstHistory;
	FLoreSourceControlHistory SecondHistory;
	TestTrue(TEXT("Read first same-name file history"), FLoreSourceControlUtils::RunGetHistory(LoreBinary, Repository.GetRoot(), FirstRevisionFile, Errors, FirstHistory));
	TestTrue(TEXT("Read second same-name file history"), FLoreSourceControlUtils::RunGetHistory(LoreBinary, Repository.GetRoot(), SecondRevisionFile, Errors, SecondHistory));
	if (FirstHistory.IsEmpty() || SecondHistory.IsEmpty())
	{
		AddError(TEXT("The same-name revision fixture has no file history."));
		return false;
	}
	FString FirstOutput;
	FString SecondOutput;
	TestTrue(TEXT("Retrieve first same-name revision"), FirstHistory[0]->Get(FirstOutput));
	TestTrue(TEXT("Retrieve second same-name revision"), SecondHistory[0]->Get(SecondOutput));
	TestNotEqual(TEXT("Same-name files in one revision use different output paths"), FirstOutput, SecondOutput);
	FString FirstContents;
	FString SecondContents;
	TestTrue(TEXT("Read first revision output"), FFileHelper::LoadFileToString(FirstContents, *FirstOutput));
	TestTrue(TEXT("Read second revision output"), FFileHelper::LoadFileToString(SecondContents, *SecondOutput));
	TestEqual(TEXT("First revision contains the first file"), FirstContents, FString(TEXT("First revision contents\n")));
	TestEqual(TEXT("Second revision contains the second file"), SecondContents, FString(TEXT("Second revision contents\n")));
	TestTrue(TEXT("Retrieve a revision into an existing output path"), SecondHistory[0]->Get(FirstOutput));
	TestTrue(TEXT("Read the replaced revision output"), FFileHelper::LoadFileToString(FirstContents, *FirstOutput));
	TestEqual(TEXT("The requested revision replaces existing output contents"), FirstContents, FString(TEXT("Second revision contents\n")));
	if (!FirstOutput.IsEmpty())
	{
		IFileManager::Get().Delete(*FirstOutput);
	}
	if (!SecondOutput.IsEmpty() && SecondOutput != FirstOutput)
	{
		IFileManager::Get().Delete(*SecondOutput);
	}
	const FString RenamedFile = FPaths::Combine(Repository.GetRoot(), TEXT("First/Renamed.txt"));
	TestTrue(TEXT("Rename the committed file on disk"), IFileManager::Get().Move(*RenamedFile, *FirstRevisionFile));
	Errors.Reset();
	TestTrue(TEXT("Stage the renamed file"), FLoreSourceControlUtils::RunLoreCommand(TEXT("stage"), LoreBinary, Repository.GetRoot(), StageParameters, { RenamedFile }, Results, Errors));
	TestTrue(TEXT("Record the native rename"), FLoreSourceControlUtils::RunLoreCommand(TEXT("stage move"), LoreBinary, Repository.GetRoot(), { TEXT("--no-gc") }, { FirstRevisionFile, RenamedFile }, Results, Errors));
	TestTrue(TEXT("Commit the rename"), FLoreSourceControlUtils::RunLoreCommand(TEXT("commit"), LoreBinary, Repository.GetRoot(), { TEXT("--no-gc"), TEXT("--"), TEXT("Rename") }, {}, Results, Errors));
	FLoreSourceControlHistory RenamedHistory;
	TestTrue(TEXT("Read history across the native rename"), FLoreSourceControlUtils::RunGetHistory(LoreBinary, Repository.GetRoot(), RenamedFile, Errors, RenamedHistory));
	if (RenamedHistory.Num() >= 2)
	{
		TestEqual(TEXT("Older history uses the path at that revision"), RenamedHistory.Last()->GetFilename(), FirstRevisionFile);
		FString HistoricalOutput;
		TestTrue(TEXT("Retrieve the revision before the rename"), RenamedHistory.Last()->Get(HistoricalOutput));
		FString HistoricalContents;
		TestTrue(TEXT("Read the pre-rename revision"), FFileHelper::LoadFileToString(HistoricalContents, *HistoricalOutput));
		TestEqual(TEXT("Pre-rename revision retains its original contents"), HistoricalContents, FString(TEXT("First revision contents\n")));
		if (!HistoricalOutput.IsEmpty())
		{
			IFileManager::Get().Delete(*HistoricalOutput);
		}
	}
	else
	{
		AddError(TEXT("The renamed file has no pre-rename history."));
	}
	Errors.Reset();
	const TArray<FString> MissingRevisionParameters{
		TEXT("--path=Integration.txt"),
		TEXT("--revision=missing-review-revision"),
		TEXT("--output=") + FLoreSourceControlUtils::QuoteCommandLineArgument(FPaths::Combine(Repository.GetRoot(), TEXT("MissingRevision.txt")))
	};
	TestFalse(TEXT("Missing revision reports a native command failure"), FLoreSourceControlUtils::RunLoreCommand(TEXT("file write"), LoreBinary, Repository.GetRoot(), MissingRevisionParameters, {}, Results, Errors));
	TestFalse(TEXT("Native failure provides a diagnostic"), Errors.IsEmpty());
	const TArray<FString> OriginalErrors = Errors;
	TestTrue(TEXT("A successful status refresh follows the failure"), FLoreSourceControlUtils::RunLoreCommand(TEXT("status"), LoreBinary, Repository.GetRoot(), {}, {}, Results, Errors));
	for (const FString& Error : OriginalErrors)
	{
		TestTrue(TEXT("Successful refresh preserves the earlier failure diagnostic"), Errors.Contains(Error));
	}
	TestTrue(TEXT("Temporary repository is removed"), Repository.Cleanup());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreArgumentRoundTripIntegrationTest, "LoreSourceControl.Integration.ArgumentRoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreArgumentRoundTripIntegrationTest::RunTest(const FString& Parameters)
{
	const FString LoreBinary = FLoreSourceControlUtils::FindLoreBinaryPath();
	if (!FLoreSourceControlUtils::CheckLoreAvailability(LoreBinary))
	{
		AddError(FString::Printf(TEXT("The argument test requires a working Lore binary. Tried: %s"), *LoreBinary));
		return false;
	}
	FTemporaryLoreRepository Repository;
	if (!Repository.Create())
	{
		AddError(TEXT("Could not create the temporary argument-test directory."));
		return false;
	}
	TArray<FString> Results;
	TArray<FString> Errors;
	const FString Identity = TEXT("Lore \"automation\" \\ user");
	const TArray<FString> CreateParameters{
		TEXT("--offline"), TEXT("--identity"), FLoreSourceControlUtils::QuoteCommandLineArgument(Identity),
		TEXT("--repository"), FLoreSourceControlUtils::QuoteCommandLineArgument(Repository.GetRoot()),
		TEXT("--no-gc"), TEXT("argument-test")
	};
	if (!FLoreSourceControlUtils::RunLoreCommand(TEXT("repository create"), LoreBinary, Repository.GetRoot(), CreateParameters, {}, Results, Errors))
	{
		AddError(FormatCommandErrors(Errors));
		return false;
	}
	FString RemoteUrl;
	FString ConfigIdentity;
	TestTrue(TEXT("Read the native repository configuration"), FLoreSourceControlUtils::ReadRepositoryConfig(Repository.GetRoot(), RemoteUrl, ConfigIdentity));
	TestEqual(TEXT("Native literal TOML identity round-trips exactly"), ConfigIdentity, Identity);
	for (const FString& Name : TArray<FString>{ TEXT("-notes.txt"), TEXT("move"), TEXT("merge"), TEXT("space name.txt"), TEXT("literal ' $ ` ; name.txt") })
	{
		const FString File = FPaths::Combine(Repository.GetRoot(), Name);
		TestTrue(FString::Printf(TEXT("Write %s"), *Name), FFileHelper::SaveStringToFile(TEXT("Native argument test\n"), *File));
		Results.Reset();
		Errors.Reset();
		const bool bStaged = FLoreSourceControlUtils::RunLoreCommand(TEXT("stage"), LoreBinary, Repository.GetRoot(), { TEXT("--scan"), TEXT("--no-gc") }, { File }, Results, Errors);
		TestTrue(FString::Printf(TEXT("Stage literal path %s: %s"), *Name, *FormatCommandErrors(Errors)), bStaged);
	}
	const FString MessageFile = FPaths::Combine(Repository.GetRoot(), TEXT("Message.txt"));
	int32 Revision = 0;
	for (const FString& Message : TArray<FString>{ TEXT("Quotes: \"two words\" and 'apostrophes'"), TEXT("Backslashes: C:\\Lore\\Trailing\\"), TEXT("First line\nSecond line"), TEXT("-leading message"), TEXT("Literal $(printf changed); `printf changed` $HOME") })
	{
		TestTrue(TEXT("Write message fixture"), FFileHelper::SaveStringToFile(FString::FromInt(++Revision), *MessageFile));
		Results.Reset();
		Errors.Reset();
		if (!FLoreSourceControlUtils::RunLoreCommand(TEXT("stage"), LoreBinary, Repository.GetRoot(), { TEXT("--scan"), TEXT("--no-gc") }, { MessageFile }, Results, Errors))
		{
			AddError(FormatCommandErrors(Errors));
			continue;
		}
		Results.Reset();
		Errors.Reset();
		if (!FLoreSourceControlUtils::RunLoreCommand(TEXT("commit"), LoreBinary, Repository.GetRoot(), { TEXT("--no-gc"), TEXT("--"), FLoreSourceControlUtils::QuoteCommandLineArgument(Message) }, {}, Results, Errors))
		{
			AddError(FString::Printf(TEXT("Commit message %s: %s"), *Message, *FormatCommandErrors(Errors)));
			continue;
		}
		FLoreSourceControlHistory History;
		Errors.Reset();
		TestTrue(TEXT("Read committed message history"), FLoreSourceControlUtils::RunGetHistory(LoreBinary, Repository.GetRoot(), MessageFile, Errors, History));
		if (History.Num() > 0)
		{
			TestEqual(TEXT("Commit message round-trips exactly"), History[0]->GetDescription(), Message);
		}
		else
		{
			AddError(TEXT("Committed message history is empty."));
		}
	}
	const FString BasicIdentity = TEXT("Lore \"automation\" 'quoted' \\ user");
	const FString BasicConfig = TEXT("remote_url = \"\"\nidentity = \"Lore \\\"automation\\\" 'quoted' \\\\ user\"\n[store]\nidentity = \"nested\"\n");
	TestTrue(TEXT("Write escaped basic TOML configuration"), FFileHelper::SaveStringToFile(BasicConfig, *FPaths::Combine(Repository.GetRoot(), TEXT(".lore/config.toml"))));
	TestTrue(TEXT("Read escaped basic TOML configuration"), FLoreSourceControlUtils::ReadRepositoryConfig(Repository.GetRoot(), RemoteUrl, ConfigIdentity));
	TestEqual(TEXT("Basic TOML identity decodes quotes and backslashes"), ConfigIdentity, BasicIdentity);
	TestTrue(TEXT("Offline remote remains empty"), RemoteUrl.IsEmpty());
	for (const FString& NativeIdentity : TArray<FString>{
		TEXT("John \"Jack\" O'Brien"),
		TEXT("Lore \"automation\" 'quoted' \\ user"),
		TEXT("\nLore\r \"automation\" 'quoted' literal\\n\nsecond line\n\n last line "),
		TEXT("Lore \"automation\" 'quoted' \\ user literal\\n\nsecond line\n\n last line "),
		TEXT("John \"Jack\" O'Brien\"\""),
		TEXT("Lore \"automation\" 'quoted' \\ user''") })
	{
		FTemporaryLoreRepository NativeRepository;
		if (!NativeRepository.Create())
		{
			AddError(TEXT("Could not create the native configuration-test directory."));
			return false;
		}
		Errors.Reset();
		const TArray<FString> NativeCreateParameters{
			TEXT("--offline"), TEXT("--identity"), FLoreSourceControlUtils::QuoteCommandLineArgument(NativeIdentity),
			TEXT("--repository"), FLoreSourceControlUtils::QuoteCommandLineArgument(NativeRepository.GetRoot()),
			TEXT("--no-gc"), TEXT("native-config-test")
		};
		if (!FLoreSourceControlUtils::RunLoreCommand(TEXT("repository create"), LoreBinary, NativeRepository.GetRoot(), NativeCreateParameters, {}, Results, Errors))
		{
			AddError(FormatCommandErrors(Errors));
			return false;
		}
		TestTrue(TEXT("Read native triple-string configuration"), FLoreSourceControlUtils::ReadRepositoryConfig(NativeRepository.GetRoot(), RemoteUrl, ConfigIdentity));
		TestEqual(TEXT("Native triple-string identity round-trips exactly"), ConfigIdentity, NativeIdentity);
		TestTrue(TEXT("Native offline remote remains empty"), RemoteUrl.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreDeleteRevertIntegrationTest, "LoreSourceControl.Integration.DeleteRevert", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreDeleteRevertIntegrationTest::RunTest(const FString& Parameters)
{
	const FString LoreBinary = FLoreSourceControlUtils::FindLoreBinaryPath();
	FTemporaryLoreRepository Repository;
	if (!Repository.Create())
	{
		AddError(TEXT("Could not create the temporary delete/revert-test directory."));
		return false;
	}
	FLoreSourceControlProvider* Provider = nullptr;
	for (ISourceControlProvider* Candidate : IModularFeatures::Get().GetModularFeatureImplementations<ISourceControlProvider>(TEXT("SourceControl")))
	{
		if (Candidate->GetName() == FName(TEXT("Lore")))
		{
			Provider = static_cast<FLoreSourceControlProvider*>(Candidate);
			break;
		}
	}
	if (!Provider)
	{
		AddError(TEXT("The Lore provider is not registered."));
		return false;
	}
	auto Run = [&](const FString& Command, const TArray<FString>& Params, const TArray<FString>& Files)
	{
		TArray<FString> Results;
		TArray<FString> Errors;
		const bool bOk = FLoreSourceControlUtils::RunLoreCommand(Command, LoreBinary, Repository.GetRoot(), Params, Files, Results, Errors);
		if (!bOk)
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Command, *FormatCommandErrors(Errors)));
		}
		return bOk;
	};
	auto RunWorker = [&](const FSourceControlOperationRef& Operation, const FLoreSourceControlWorkerRef& Worker, const FString& File)
	{
		FLoreSourceControlCommand Command(Operation, Worker);
		Command.Provider = Provider;
		Command.PathToLoreBinary = LoreBinary;
		Command.PathToRepositoryRoot = Repository.GetRoot();
		Command.bShouldLockFiles = false;
		Command.Files = { File };
		const bool bOk = Worker->Execute(Command);
		if (!bOk)
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Operation->GetName().ToString(), *FormatCommandErrors(Command.ErrorMessages)));
		}
		return bOk;
	};
	const FString Selected = FPaths::Combine(Repository.GetRoot(), TEXT("Selected.txt"));
	const FString Extra = FPaths::Combine(Repository.GetRoot(), TEXT("Extra.txt"));
	const FString Added = FPaths::Combine(Repository.GetRoot(), TEXT("Added.txt"));
	if (!Run(TEXT("repository create"), { TEXT("--offline"), TEXT("--identity"), FLoreSourceControlUtils::QuoteCommandLineArgument(TEXT("Lore automation")), TEXT("--no-gc"), TEXT("delete-revert-test") }, {})
		|| !FFileHelper::SaveStringToFile(TEXT("Original selected\n"), *Selected)
		|| !FFileHelper::SaveStringToFile(TEXT("Original extra\n"), *Extra)
		|| !Run(TEXT("stage"), { TEXT("--scan"), TEXT("--no-gc") }, { Selected, Extra })
		|| !Run(TEXT("commit"), { TEXT("--no-gc"), TEXT("--"), FLoreSourceControlUtils::QuoteCommandLineArgument(TEXT("Delete/revert fixture")) }, {})
		|| !FFileHelper::SaveStringToFile(TEXT("Changed selected\n"), *Selected)
		|| !FFileHelper::SaveStringToFile(TEXT("Changed extra\n"), *Extra)
		|| !Run(TEXT("stage"), { TEXT("--scan"), TEXT("--no-gc") }, { Selected, Extra }))
	{
		return false;
	}
	TArray<FString> StagedFiles;
	TArray<FString> StagedDirectories;
	TArray<FString> Errors;
	auto CheckStage = [&](bool bSelectedStaged)
	{
		StagedFiles.Reset();
		StagedDirectories.Reset();
		Errors.Reset();
		TestTrue(TEXT("Read stage after operation"), FLoreSourceControlUtils::RunGetStagedPaths(LoreBinary, Repository.GetRoot(), StagedFiles, StagedDirectories, Errors));
		TestEqual(TEXT("Only the intended selected stage is changed"), StagedFiles.Contains(Selected), bSelectedStaged);
		TestTrue(TEXT("Unrelated staged file is preserved"), StagedFiles.Contains(Extra));
	};
	if (!RunWorker(MakeShared<FRevert>(), MakeShared<FLoreRevertWorker>(), Selected))
	{
		return false;
	}
	FString Contents;
	TestTrue(TEXT("Read reverted selected file"), FFileHelper::LoadFileToString(Contents, *Selected));
	TestEqual(TEXT("Staged modification is restored"), Contents, FString(TEXT("Original selected\n")));
	CheckStage(false);
	if (!RunWorker(MakeShared<FDelete>(), MakeShared<FLoreDeleteWorker>(), Selected))
	{
		return false;
	}
	TestFalse(TEXT("Delete removes an existing tracked file from disk"), FPaths::FileExists(Selected));
	CheckStage(true);
	if (!RunWorker(MakeShared<FRevert>(), MakeShared<FLoreRevertWorker>(), Selected))
	{
		return false;
	}
	TestTrue(TEXT("Revert restores a staged deletion"), FPaths::FileExists(Selected));
	CheckStage(false);
	if (!FFileHelper::SaveStringToFile(TEXT("New staged file\n"), *Added)
		|| !Run(TEXT("stage"), { TEXT("--scan"), TEXT("--no-gc") }, { Added })
		|| !RunWorker(MakeShared<FDelete>(), MakeShared<FLoreDeleteWorker>(), Added))
	{
		return false;
	}
	TestFalse(TEXT("Delete removes a staged addition from disk"), FPaths::FileExists(Added));
	CheckStage(false);
	TestFalse(TEXT("Deleted addition cannot be committed later"), StagedFiles.Contains(Added));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreRemoteSubmitIntegrationTest, "LoreSourceControl.Integration.RemoteSubmit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreRemoteSubmitIntegrationTest::RunTest(const FString& Parameters)
{
	const FString Remote = FPlatformMisc::GetEnvironmentVariable(TEXT("LORE_TEST_REMOTE"));
	if (Remote.IsEmpty())
	{
		AddInfo(TEXT("Set LORE_TEST_REMOTE to a new disposable Lore repository URL to run the remote submit test."));
		return true;
	}
	const FString LoreBinary = FLoreSourceControlUtils::FindLoreBinaryPath();
	FTemporaryLoreRepository Repository;
	if (!Repository.Create())
	{
		AddError(TEXT("Could not create the temporary remote-test directory."));
		return false;
	}
	auto Run = [&](const FString& Command, const TArray<FString>& Params, const TArray<FString>& Files)
	{
		TArray<FString> Results;
		TArray<FString> Errors;
		const bool bOk = FLoreSourceControlUtils::RunLoreCommand(Command, LoreBinary, Repository.GetRoot(), Params, Files, Results, Errors);
		if (!bOk)
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Command, *FormatCommandErrors(Errors)));
		}
		return bOk;
	};
	if (!Run(TEXT("repository create"), { TEXT("--identity"), FLoreSourceControlUtils::QuoteCommandLineArgument(TEXT("Lore automation")), TEXT("--no-gc"), FLoreSourceControlUtils::QuoteCommandLineArgument(Remote) }, {}))
	{
		return false;
	}
	FLoreSourceControlProvider* Provider = nullptr;
	for (ISourceControlProvider* Candidate : IModularFeatures::Get().GetModularFeatureImplementations<ISourceControlProvider>(TEXT("SourceControl")))
	{
		if (Candidate->GetName() == FName(TEXT("Lore")))
		{
			Provider = static_cast<FLoreSourceControlProvider*>(Candidate);
			break;
		}
	}
	if (!Provider)
	{
		AddError(TEXT("The Lore provider is not registered."));
		return false;
	}
	const FString Selected = FPaths::Combine(Repository.GetRoot(), TEXT("Selected.txt"));
	const FString Extra = FPaths::Combine(Repository.GetRoot(), TEXT("Extra.txt"));
	if (!FFileHelper::SaveStringToFile(TEXT("Original selected\n"), *Selected)
		|| !FFileHelper::SaveStringToFile(TEXT("Original extra\n"), *Extra)
		|| !Run(TEXT("stage"), { TEXT("--scan"), TEXT("--no-gc") }, { Selected, Extra })
		|| !Run(TEXT("commit"), { TEXT("--no-gc"), TEXT("--"), FLoreSourceControlUtils::QuoteCommandLineArgument(TEXT("Remote fixture")) }, {})
		|| !Run(TEXT("branch push"), { TEXT("--no-gc") }, {})
		|| !Run(TEXT("lock acquire"), {}, { Selected }))
	{
		return false;
	}
	TLorePathMap<FLoreLockOwner> Locks;
	TArray<FString> Errors;
	FString OwnIdentity;
	TestTrue(TEXT("Query the native checkout lock"), FLoreSourceControlUtils::GetLoreLockStatus(LoreBinary, Repository.GetRoot(), *Provider, Locks, &Errors, &OwnIdentity));
	const FLoreLockOwner* OwnLock = Locks.Find(Selected);
	TestTrue(TEXT("Checkout belongs to the authenticated or explicit anonymous principal"), OwnLock && !OwnIdentity.IsEmpty() && OwnLock->Identity == OwnIdentity);
	if (!OwnLock || OwnIdentity.IsEmpty() || OwnLock->Identity != OwnIdentity)
	{
		return false;
	}
	if (!FFileHelper::SaveStringToFile(TEXT("Changed extra\n"), *Extra)
		|| !FFileHelper::SaveStringToFile(TEXT("Changed selected\n"), *Selected)
		|| !Run(TEXT("stage"), { TEXT("--scan"), TEXT("--no-gc") }, { Extra }))
	{
		return false;
	}
	auto Submit = [&]()
	{
		const TSharedRef<FCheckIn, ESPMode::ThreadSafe> Operation = MakeShared<FCheckIn>();
		Operation->SetDescription(FText::FromString(TEXT("Native submit \"quotes\"\nsecond line")));
		const FLoreSourceControlWorkerRef Worker = MakeShared<FLoreCheckInWorker>();
		FLoreSourceControlCommand Command(Operation, Worker);
		Command.Provider = Provider;
		Command.PathToLoreBinary = LoreBinary;
		Command.PathToRepositoryRoot = Repository.GetRoot();
		Command.Identity = TEXT("Lore automation");
		Command.bHasRemote = true;
		Command.bShouldLockFiles = true;
		Command.Files = { Selected };
		const bool bOk = Worker->Execute(Command);
		Errors = Command.ErrorMessages;
		return bOk;
	};
	TestFalse(TEXT("Partial submit rejects unrelated staged work"), Submit());
	TestTrue(TEXT("Partial submit explains the unrelated stage"), Errors.ContainsByPredicate([](const FString& Error) { return Error.Contains(TEXT("stage also contains paths outside")); }));
	FLoreSourceControlHistory History;
	Errors.Reset();
	TestTrue(TEXT("Inspect history after rejected partial submit"), FLoreSourceControlUtils::RunGetHistory(LoreBinary, Repository.GetRoot(), Selected, Errors, History));
	TestTrue(TEXT("Rejected partial submit creates no revision"), History.Num() == 1 && History[0]->GetDescription() == TEXT("Remote fixture"));
	if (!Run(TEXT("file unstage"), {}, { Extra }))
	{
		return false;
	}
	TestTrue(FString::Printf(TEXT("Submit the selected file with its own lock: %s"), *FormatCommandErrors(Errors)), Submit());
	Errors.Reset();
	Locks.Reset();
	TestTrue(TEXT("Query locks after native submit"), FLoreSourceControlUtils::GetLoreLockStatus(LoreBinary, Repository.GetRoot(), *Provider, Locks, &Errors));
	TestFalse(TEXT("Successful submit releases the checkout lock"), Locks.Contains(Selected));
	return true;
}

#endif
