// Copyright Solessfir 2026. All Rights Reserved.

#include "LoreSourceControlCommand.h"
#include "LoreSourceControlOperations.h"
#include "LoreSourceControlProvider.h"
#include "LoreSourceControlState.h"
#include "LoreSourceControlUtils.h"
#include "Features/IModularFeatures.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SourceControlOperations.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	struct FSubmitWorkerFixture
	{
		FSubmitWorkerFixture()
			: Operation(MakeShared<FCheckIn>())
			, Worker(MakeShared<FLoreCheckInWorker>())
			, Command(Operation, Worker)
		{
			Operation->SetDescription(FText::FromString(TEXT("Test commit")));
			FString TestFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("LoreSourceControlWorkerTests/Content/Test.uasset")));
			FPaths::NormalizeFilename(TestFile);
			Command.Files.Add(TestFile);
			Command.bShouldLockFiles = false;
			Command.Identity = TEXT("Test user");
			Command.RunLoreCommandOverride = [this](const FString& InCommand, const TArray<FString>& InParameters, const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrors)
			{
				Calls.Add(InCommand);
				if (InCommand == TEXT("stage"))
				{
					StageParameters = InParameters;
					if (!bStageSucceeds)
					{
						OutErrors.Add(TEXT("Simulated stage failure."));
						return false;
					}
				}
				if (InCommand == TEXT("commit"))
				{
					CommitParameters = InParameters;
				}
				if (InCommand == TEXT("branch push") && !bPushSucceeds)
				{
					OutErrors.Add(TEXT("Simulated push failure."));
					return false;
				}
				return true;
			};
			Command.ReadStagedPathsOverride = [this](TArray<FString>& OutFiles, TArray<FString>& OutDirectories, TArray<FString>& OutErrors)
			{
				Calls.Add(TEXT("read staged paths"));
				OutFiles = StagedFiles.IsEmpty() ? Command.Files : StagedFiles;
				return true;
			};
			Command.RefreshStatusOverride = [this](const TArray<FString>& InFiles, bool bQueryLocks, TArray<FString>& OutErrors, TArray<FLoreSourceControlState>& OutStates)
			{
				Calls.Add(TEXT("refresh status"));
				return true;
			};
			Command.QueryLockStatusOverride = [this](TLorePathMap<FLoreLockOwner>& OutLockedBy, TArray<FString>& OutErrors, FString* OutOwnIdentity)
			{
				Calls.Add(TEXT("query locks"));
				OutLockedBy = LockedBy;
				if (OutOwnIdentity && !OwnIdentity.IsEmpty())
				{
					*OutOwnIdentity = OwnIdentity;
				}
				if (!bLockQuerySucceeds)
				{
					OutErrors.Add(TEXT("Simulated lock query failure."));
				}
				return bLockQuerySucceeds;
			};
		}

		TSharedRef<FCheckIn> Operation;
		TSharedRef<FLoreCheckInWorker> Worker;
		FLoreSourceControlCommand Command;
		TArray<FString> Calls;
		TArray<FString> CommitParameters;
		TArray<FString> StageParameters;
		TArray<FString> StagedFiles;
		TLorePathMap<FLoreLockOwner> LockedBy;
		FString OwnIdentity;
		bool bPushSucceeds = true;
		bool bStageSucceeds = true;
		bool bLockQuerySucceeds = true;
	};

	bool ContainsMessage(const TArray<FString>& Messages, const FString& ExpectedText)
	{
		return Messages.ContainsByPredicate([&ExpectedText](const FString& Message) { return Message.Contains(ExpectedText); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreLocalSubmitWorkerTest, "LoreSourceControl.Workers.CheckIn.LocalCommit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreLocalSubmitWorkerTest::RunTest(const FString& Parameters)
{
	FSubmitWorkerFixture Fixture;
	Fixture.Command.bHasRemote = false;

	TestTrue(TEXT("Local submit succeeds"), Fixture.Worker->Execute(Fixture.Command));
	TestEqual(TEXT("Local submit sequence"), FString::Join(Fixture.Calls, TEXT(" -> ")), FString(TEXT("stage -> read staged paths -> commit -> refresh status")));
#if PLATFORM_LINUX
	TestEqual(TEXT("Commit message is passed to Lore"), FString::Join(Fixture.CommitParameters, TEXT(" ")), FString(TEXT("-- 'Test commit'")));
#else
	TestEqual(TEXT("Commit message is passed to Lore"), FString::Join(Fixture.CommitParameters, TEXT(" ")), FString(TEXT("-- \"Test commit\"")));
#endif
	TestTrue(TEXT("Local result is reported"), ContainsMessage(Fixture.Command.InfoMessages, TEXT("kept locally")));
	TestEqual(TEXT("Local success message"), Fixture.Operation->GetSuccessMessage().ToString(), FString(TEXT("Committed revision \"Test commit\" locally.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreLiteralCommitDescriptionWorkerTest, "LoreSourceControl.Workers.CheckIn.LiteralDescription", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreLiteralCommitDescriptionWorkerTest::RunTest(const FString& Parameters)
{
	FSubmitWorkerFixture Fixture;
	Fixture.Operation->SetDescription(FText::FromString(TEXT("-message 'quoted' \"double quoted\"\nsecond line")));

	TestTrue(TEXT("Literal description submit succeeds"), Fixture.Worker->Execute(Fixture.Command));
#if PLATFORM_LINUX
	TestEqual(TEXT("Description is protected from option parsing and quoted as one argument"), FString::Join(Fixture.CommitParameters, TEXT(" ")), FString(TEXT("-- '-message '\"'\"'quoted'\"'\"' \"double quoted\"\nsecond line'")));
#else
	TestEqual(TEXT("Description is protected from option parsing and quoted as one argument"), FString::Join(Fixture.CommitParameters, TEXT(" ")), FString(TEXT("-- \"-message 'quoted' \\\"double quoted\\\"\nsecond line\"")));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreRemoteSubmitWorkerTest, "LoreSourceControl.Workers.CheckIn.RemotePush", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreRemoteSubmitWorkerTest::RunTest(const FString& Parameters)
{
	FSubmitWorkerFixture Fixture;
	Fixture.Command.bHasRemote = true;

	TestTrue(TEXT("Remote submit succeeds"), Fixture.Worker->Execute(Fixture.Command));
	TestEqual(TEXT("Remote submit sequence"), FString::Join(Fixture.Calls, TEXT(" -> ")), FString(TEXT("stage -> read staged paths -> commit -> branch push -> refresh status")));
	TestEqual(TEXT("Remote success message"), Fixture.Operation->GetSuccessMessage().ToString(), FString(TEXT("Submitted revision \"Test commit\".")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLorePushFailureWorkerTest, "LoreSourceControl.Workers.CheckIn.PushFailure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLorePushFailureWorkerTest::RunTest(const FString& Parameters)
{
	FSubmitWorkerFixture Fixture;
	Fixture.Command.bHasRemote = true;
	Fixture.bPushSucceeds = false;

	TestFalse(TEXT("Push failure fails the submit"), Fixture.Worker->Execute(Fixture.Command));
	TestEqual(TEXT("Status still refreshes after push failure"), FString::Join(Fixture.Calls, TEXT(" -> ")), FString(TEXT("stage -> read staged paths -> commit -> branch push -> refresh status")));
	TestTrue(TEXT("Local commit recovery is explained"), ContainsMessage(Fixture.Command.ErrorMessages, TEXT("Commit succeeded locally, but push to remote failed")));
	TestTrue(TEXT("Underlying push error is retained"), ContainsMessage(Fixture.Command.ErrorMessages, TEXT("Simulated push failure")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreUnexpectedStageWorkerTest, "LoreSourceControl.Workers.CheckIn.UnexpectedStage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreUnexpectedStageWorkerTest::RunTest(const FString& Parameters)
{
	FSubmitWorkerFixture Fixture;
	Fixture.StagedFiles = Fixture.Command.Files;
	Fixture.StagedFiles.Add(FPaths::Combine(FPaths::GetPath(Fixture.Command.Files[0]), TEXT("Unrelated.uasset")));

	TestFalse(TEXT("Unexpected staged work blocks submit"), Fixture.Worker->Execute(Fixture.Command));
	TestEqual(TEXT("Worker stops before commit"), FString::Join(Fixture.Calls, TEXT(" -> ")), FString(TEXT("stage -> read staged paths")));
	TestTrue(TEXT("Unexpected path is reported"), ContainsMessage(Fixture.Command.ErrorMessages, TEXT("Unrelated.uasset")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreAnonymousLockWorkerTest, "LoreSourceControl.Workers.CheckIn.AnonymousOwnLock", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreAnonymousLockWorkerTest::RunTest(const FString& Parameters)
{
	FSubmitWorkerFixture Fixture;
	Fixture.Command.bShouldLockFiles = true;
	Fixture.OwnIdentity = TEXT("<unknown>");
	Fixture.LockedBy.Add(Fixture.Command.Files[0], FLoreLockOwner{ TEXT("<unknown>"), TEXT("<unknown>") });

	TestTrue(TEXT("Anonymous own lock permits submit"), Fixture.Worker->Execute(Fixture.Command));
	TestEqual(TEXT("Submit queries locks before staging"), FString::Join(Fixture.Calls, TEXT(" -> ")), FString(TEXT("query locks -> stage -> read staged paths -> commit -> refresh status")));
	TestEqual(TEXT("Commit identity is preserved"), Fixture.Command.Identity, FString(TEXT("Test user")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreAuthenticatedLockWorkerTest, "LoreSourceControl.Workers.CheckIn.AuthenticatedOwnLock", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreAuthenticatedLockWorkerTest::RunTest(const FString& Parameters)
{
	FSubmitWorkerFixture Fixture;
	Fixture.Command.bShouldLockFiles = true;
	Fixture.OwnIdentity = TEXT("authenticated-user-id");
	Fixture.LockedBy.Add(Fixture.Command.Files[0], FLoreLockOwner{ Fixture.OwnIdentity, TEXT("Test user") });

	TestTrue(TEXT("Authenticated own lock permits submit with a different commit identity"), Fixture.Worker->Execute(Fixture.Command));
	TestEqual(TEXT("Commit identity is preserved"), Fixture.Command.Identity, FString(TEXT("Test user")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreCommitIdentityForeignLockWorkerTest, "LoreSourceControl.Workers.CheckIn.CommitIdentityIsNotLockIdentity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreCommitIdentityForeignLockWorkerTest::RunTest(const FString& Parameters)
{
	FSubmitWorkerFixture Fixture;
	Fixture.Command.bShouldLockFiles = true;
	Fixture.OwnIdentity = TEXT("authenticated-user-id");
	Fixture.LockedBy.Add(Fixture.Command.Files[0], FLoreLockOwner{ Fixture.Command.Identity, TEXT("Another user") });

	TestFalse(TEXT("Matching commit identity does not permit another principal's lock"), Fixture.Worker->Execute(Fixture.Command));
	TestEqual(TEXT("Worker stops after lock query"), FString::Join(Fixture.Calls, TEXT(" -> ")), FString(TEXT("query locks")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreForeignLockWorkerTest, "LoreSourceControl.Workers.CheckIn.ForeignLock", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreForeignLockWorkerTest::RunTest(const FString& Parameters)
{
	FSubmitWorkerFixture Fixture;
	Fixture.Command.bShouldLockFiles = true;
	Fixture.LockedBy.Add(Fixture.Command.Files[0], FLoreLockOwner{ TEXT("other-user"), TEXT("Another user") });

	TestFalse(TEXT("Another user's lock blocks submit"), Fixture.Worker->Execute(Fixture.Command));
	TestEqual(TEXT("Worker stops after lock query"), FString::Join(Fixture.Calls, TEXT(" -> ")), FString(TEXT("query locks")));
	TestTrue(TEXT("Lock owner is reported"), ContainsMessage(Fixture.Command.ErrorMessages, TEXT("locked by Another user")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreLockQueryFailureWorkerTest, "LoreSourceControl.Workers.CheckIn.LockQueryFailure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreLockQueryFailureWorkerTest::RunTest(const FString& Parameters)
{
	FSubmitWorkerFixture Fixture;
	Fixture.Command.bShouldLockFiles = true;
	Fixture.bLockQuerySucceeds = false;

	TestFalse(TEXT("Unverified lock ownership blocks submit"), Fixture.Worker->Execute(Fixture.Command));
	TestEqual(TEXT("Worker stops after failed lock query"), FString::Join(Fixture.Calls, TEXT(" -> ")), FString(TEXT("query locks")));
	TestTrue(TEXT("Lock query error is retained"), ContainsMessage(Fixture.Command.ErrorMessages, TEXT("Simulated lock query failure")));
	TestTrue(TEXT("Safety rejection is explained"), ContainsMessage(Fixture.Command.ErrorMessages, TEXT("lock ownership could not be verified")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreStageFailureWorkerTest, "LoreSourceControl.Workers.CheckIn.StageFailure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreStageFailureWorkerTest::RunTest(const FString& Parameters)
{
	FSubmitWorkerFixture Fixture;
	Fixture.bStageSucceeds = false;

	TestFalse(TEXT("Stage failure blocks submit"), Fixture.Worker->Execute(Fixture.Command));
	TestEqual(TEXT("Stage is attempted once"), FString::Join(Fixture.Calls, TEXT(" -> ")), FString(TEXT("stage")));
	TestEqual(TEXT("Stage uses filesystem scanning"), FString::Join(Fixture.StageParameters, TEXT(" ")), FString(TEXT("--scan")));
	TestTrue(TEXT("Lore stage error is retained"), ContainsMessage(Fixture.Command.ErrorMessages, TEXT("Simulated stage failure")));
	TestTrue(TEXT("Submit rejection is explained"), ContainsMessage(Fixture.Command.ErrorMessages, TEXT("selected files could not be staged")));
	return true;
}

#if PLATFORM_LINUX

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreCaseDistinctStageWorkerTest, "LoreSourceControl.Workers.CheckIn.CaseDistinctStage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreCaseDistinctStageWorkerTest::RunTest(const FString& Parameters)
{
	FSubmitWorkerFixture Fixture;
	const FString Root = FPaths::GetPath(Fixture.Command.Files[0]);
	Fixture.Command.Files = { FPaths::Combine(Root, TEXT("Case.txt")) };
	Fixture.StagedFiles = { Fixture.Command.Files[0], FPaths::Combine(Root, TEXT("case.txt")) };

	TestFalse(TEXT("A case-distinct unrelated staged file blocks submit"), Fixture.Worker->Execute(Fixture.Command));
	TestEqual(TEXT("Submit stops before commit"), FString::Join(Fixture.Calls, TEXT(" -> ")), FString(TEXT("stage -> read staged paths")));
	TestTrue(TEXT("The unrelated case-distinct path is reported"), ContainsMessage(Fixture.Command.ErrorMessages, TEXT("case.txt")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreCaseDistinctLockWorkerTest, "LoreSourceControl.Workers.CheckIn.CaseDistinctLock", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreCaseDistinctLockWorkerTest::RunTest(const FString& Parameters)
{
	FSubmitWorkerFixture Fixture;
	Fixture.Command.bShouldLockFiles = true;
	Fixture.OwnIdentity = TEXT("user-1");
	const FString Root = FPaths::GetPath(Fixture.Command.Files[0]);
	Fixture.Command.Files = { FPaths::Combine(Root, TEXT("Case.txt")) };
	Fixture.LockedBy.Add(FPaths::Combine(Root, TEXT("case.txt")), FLoreLockOwner{ TEXT("user-2"), TEXT("Another user") });

	TestTrue(TEXT("A lock on case.txt does not block submitting Case.txt"), Fixture.Worker->Execute(Fixture.Command));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreCaseSensitiveCacheScopeTest, "LoreSourceControl.Cache.CaseSensitiveScopes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreCaseSensitiveCacheScopeTest::RunTest(const FString& Parameters)
{
	ISourceControlProvider* RegisteredProvider = nullptr;
	for (ISourceControlProvider* Candidate : IModularFeatures::Get().GetModularFeatureImplementations<ISourceControlProvider>(TEXT("SourceControl")))
	{
		if (Candidate->GetName() == FName(TEXT("Lore")))
		{
			RegisteredProvider = Candidate;
			break;
		}
	}
	if (!RegisteredProvider)
	{
		AddError(TEXT("The Lore provider is not registered."));
		return false;
	}

	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/LoreSourceControl"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const FString UpperFile = FPaths::Combine(Root, TEXT("Case.txt"));
	const FString LowerFile = FPaths::Combine(Root, TEXT("case.txt"));
	const FString UpperDirectoryFile = FPaths::Combine(Root, TEXT("Dir/file.txt"));
	const FString LowerDirectory = FPaths::Combine(Root, TEXT("dir"));
	const FString LowerDirectoryFile = FPaths::Combine(LowerDirectory, TEXT("file.txt"));
	if (!IFileManager::Get().MakeDirectory(*LowerDirectory, true))
	{
		AddError(TEXT("Could not create the temporary cache-test directory."));
		return false;
	}

	FLoreCheckInWorker Refresh;
	Refresh.Provider = static_cast<FLoreSourceControlProvider*>(RegisteredProvider);
	Refresh.bApplyStateResults = true;
	Refresh.StateScanPaths = { Root };
	for (const FString& File : TArray<FString>{ UpperFile, LowerFile, UpperDirectoryFile, LowerDirectoryFile })
	{
		FLoreSourceControlState State(File);
		State.bIsModified = true;
		Refresh.States.Add(State);
	}
	Refresh.UpdateStates();

	auto IsCachedModified = [RegisteredProvider](const FString& File)
	{
		return !RegisteredProvider->GetCachedStateByPredicate([&File](const FSourceControlStateRef& State)
		{
			return State->GetFilename().Equals(File, ESearchCase::CaseSensitive) && State->IsModified();
		}).IsEmpty();
	};
	TestTrue(TEXT("Modified Case.txt is cached before refreshing"), IsCachedModified(UpperFile));
	TestTrue(TEXT("Modified case.txt is cached before refreshing"), IsCachedModified(LowerFile));
	TestTrue(TEXT("Modified Dir/file.txt is cached before refreshing"), IsCachedModified(UpperDirectoryFile));
	TestTrue(TEXT("Modified dir/file.txt is cached before refreshing"), IsCachedModified(LowerDirectoryFile));

	Refresh.States = { FLoreSourceControlState(LowerFile) };
	Refresh.StateScanPaths = { LowerFile };
	Refresh.UpdateStates();
	TestTrue(TEXT("Refreshing case.txt preserves modified Case.txt"), IsCachedModified(UpperFile));
	TestFalse(TEXT("Refreshing case.txt clears its previous modified state"), IsCachedModified(LowerFile));

	Refresh.States = { FLoreSourceControlState(LowerDirectoryFile) };
	Refresh.StateScanPaths = { LowerDirectory };
	Refresh.UpdateStates();
	TestTrue(TEXT("Refreshing dir preserves modified Dir/file.txt"), IsCachedModified(UpperDirectoryFile));
	TestFalse(TEXT("Refreshing dir clears its previous modified child state"), IsCachedModified(LowerDirectoryFile));

	Refresh.States.Reset();
	Refresh.StateScanPaths = { Root };
	Refresh.UpdateStates();
	TestTrue(TEXT("Temporary cache-test directory is removed"), IFileManager::Get().DeleteDirectory(*Root, false, true));
	return true;
}

#endif

#endif
