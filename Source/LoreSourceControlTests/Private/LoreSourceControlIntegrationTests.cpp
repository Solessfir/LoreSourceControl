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
	const TArray<FString> CreateParameters{
		TEXT("--offline"), TEXT("--identity"), FLoreSourceControlUtils::QuoteCommandLineArgument(TEXT("Lore automation")),
		TEXT("--repository"), FLoreSourceControlUtils::QuoteCommandLineArgument(Repository.GetRoot()),
		TEXT("--no-gc"), TEXT("argument-test")
	};
	if (!FLoreSourceControlUtils::RunLoreCommand(TEXT("repository create"), LoreBinary, Repository.GetRoot(), CreateParameters, {}, Results, Errors))
	{
		AddError(FormatCommandErrors(Errors));
		return false;
	}
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
