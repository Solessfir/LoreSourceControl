// Copyright Solessfir 2026. All Rights Reserved.

#include "LoreSourceControlCommand.h"
#include "LoreSourceControlOperations.h"
#include "LoreSourceControlState.h"
#include "LoreSourceControlUtils.h"
#include "Misc/AutomationTest.h"
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
					const bool bScansFiles = InParameters.Contains(TEXT("--scan"));
					StageAttempts.Add(bScansFiles ? TEXT("scan") : TEXT("fallback"));
					if (bScansFiles && !bScanStageSucceeds)
					{
						OutErrors.Add(TEXT("Simulated stage scan failure."));
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
			Command.QueryLockStatusOverride = [this](TMap<FString, FLoreLockOwner>& OutLockedBy, TArray<FString>& OutErrors)
			{
				Calls.Add(TEXT("query locks"));
				OutLockedBy = LockedBy;
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
		TArray<FString> StageAttempts;
		TArray<FString> StagedFiles;
		TMap<FString, FLoreLockOwner> LockedBy;
		bool bPushSucceeds = true;
		bool bScanStageSucceeds = true;
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
	TestEqual(TEXT("Commit message is passed to Lore"), FString::Join(Fixture.CommitParameters, TEXT(" ")), FString(TEXT("\"Test commit\"")));
	TestTrue(TEXT("Local result is reported"), ContainsMessage(Fixture.Command.InfoMessages, TEXT("kept locally")));
	TestEqual(TEXT("Local success message"), Fixture.Operation->GetSuccessMessage().ToString(), FString(TEXT("Committed revision \"Test commit\" locally.")));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreStageScanFallbackWorkerTest, "LoreSourceControl.Workers.CheckIn.StageScanFallback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreStageScanFallbackWorkerTest::RunTest(const FString& Parameters)
{
	FSubmitWorkerFixture Fixture;
	Fixture.bScanStageSucceeds = false;

	TestTrue(TEXT("Submit succeeds through stage fallback"), Fixture.Worker->Execute(Fixture.Command));
	TestEqual(TEXT("Stage is retried without scan"), FString::Join(Fixture.StageAttempts, TEXT(" -> ")), FString(TEXT("scan -> fallback")));
	TestEqual(TEXT("Submit continues after fallback"), FString::Join(Fixture.Calls, TEXT(" -> ")), FString(TEXT("stage -> stage -> read staged paths -> commit -> refresh status")));
	TestFalse(TEXT("Recovered scan error is not reported"), ContainsMessage(Fixture.Command.ErrorMessages, TEXT("Simulated stage scan failure")));
	return true;
}

#endif
