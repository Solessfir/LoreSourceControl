// Copyright Solessfir 2026. All Rights Reserved.

#include "LoreSourceControlCommand.h"
#include "LoreSourceControlProvider.h"
#include "HAL/FileManager.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/QueuedThreadPool.h"
#include "SourceControlOperations.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	class FProviderTestWorker : public ILoreSourceControlWorker
	{
	public:
		explicit FProviderTestWorker(TFunction<bool(FLoreSourceControlCommand&)> InExecute, TFunction<bool()> InUpdateStates = nullptr)
			: ExecuteBody(MoveTemp(InExecute))
			, UpdateStatesBody(MoveTemp(InUpdateStates))
		{
		}

		virtual FName GetName() const override { return FName(TEXT("UpdateStatus")); }
		virtual bool Execute(FLoreSourceControlCommand& InCommand) override { return ExecuteBody(InCommand); }
		virtual bool UpdateStates() const override { return UpdateStatesBody ? UpdateStatesBody() : false; }

	private:
		TFunction<bool(FLoreSourceControlCommand&)> ExecuteBody;
		TFunction<bool()> UpdateStatesBody;
	};

	struct FProviderFixture
	{
		explicit FProviderFixture(TFunction<bool(FLoreSourceControlCommand&)> InExecute)
		{
			Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/LoreSourceControl"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			if (!IFileManager::Get().MakeDirectory(*FPaths::Combine(Root, TEXT(".lore")), true))
			{
				return;
			}

			Provider.CheckRepositoryStatus(Root);
			Provider.RegisterWorker(TEXT("UpdateStatus"), FLoreGetSourceControlWorker::CreateLambda([ExecuteBody = MoveTemp(InExecute)]() -> FLoreSourceControlWorkerRef
			{
				return MakeShared<FProviderTestWorker>(ExecuteBody);
			}));
		}

		~FProviderFixture()
		{
			Provider.Close(false);
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		FLoreSourceControlProvider Provider;
		FString Root;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreProviderPathScopeTest, "LoreSourceControl.Provider.PathScopes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreProviderPathScopeTest::RunTest(const FString& Parameters)
{
	TArray<FString> ExecutedFiles;
	FProviderFixture Fixture([&ExecutedFiles](FLoreSourceControlCommand& Command)
	{
		ExecutedFiles = Command.Files;
		return true;
	});
	if (!TestTrue(TEXT("Temporary repository was discovered"), Fixture.Provider.IsLoreRepositoryFound()))
	{
		return false;
	}

	auto Refresh = [&Fixture](const TArray<FString>& Files)
	{
		return Fixture.Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), Files);
	};
	TestEqual(TEXT("Repository root without a trailing slash is accepted"), Refresh({ Fixture.Provider.GetRepositoryRoot() }), ECommandResult::Succeeded);
	TestEqual(TEXT("Repository root with a trailing slash is accepted"), Refresh({ Fixture.Root + TEXT("/") }), ECommandResult::Succeeded);
	TestEqual(TEXT("Sibling directory is rejected"), Refresh({ Fixture.Root + TEXT("Other/file.txt") }), ECommandResult::Cancelled);
	TestEqual(TEXT("Parent traversal outside the repository is rejected"), Refresh({ Fixture.Root + TEXT("/../outside.txt") }), ECommandResult::Cancelled);

	const FString Inside = FPaths::Combine(Fixture.Root, TEXT("inside.txt"));
	TestEqual(TEXT("Mixed paths refresh only repository files"), Refresh({ Inside, Fixture.Root + TEXT("/../outside.txt") }), ECommandResult::Succeeded);
	TestTrue(TEXT("Only the repository file reaches the worker"), ExecutedFiles.Num() == 1 && ExecutedFiles[0] == Inside);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreProviderCloseTest, "LoreSourceControl.Provider.CloseCompletesQueuedOperations", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreProviderCloseTest::RunTest(const FString& Parameters)
{
	if (!GThreadPool)
	{
		AddError(TEXT("The provider lifecycle test requires the engine thread pool."));
		return false;
	}
	FThreadSafeCounter QueuedExecutions;
	int32 FirstCallbacks = 0;
	int32 AsyncCallbacks = 0;
	int32 SyncCallbacks = 0;
	int32 RejectedCallbacks = 0;
	FProviderFixture Fixture([&QueuedExecutions](FLoreSourceControlCommand& Command)
	{
		QueuedExecutions.Increment();
		return true;
	});
	if (!TestTrue(TEXT("Temporary repository was discovered"), Fixture.Provider.IsLoreRepositoryFound()))
	{
		return false;
	}
	const FSourceControlOperationRef FirstOperation = ISourceControlOperation::Create<FUpdateStatus>();
	const FLoreSourceControlWorkerRef FirstWorker = MakeShared<FProviderTestWorker>([](FLoreSourceControlCommand& Command) { return true; });
	FLoreSourceControlCommand* FirstCommand = new FLoreSourceControlCommand(FirstOperation, FirstWorker);
	FirstCommand->OperationCompleteDelegate = FSourceControlOperationComplete::CreateLambda([this, &Fixture, &FirstCallbacks](const FSourceControlOperationRef&, ECommandResult::Type Result)
	{
		++FirstCallbacks;
		TestEqual(TEXT("Completed work retains its result"), Result, ECommandResult::Succeeded);
		Fixture.Provider.Close();
	});
	Fixture.Provider.IssueCommand(*FirstCommand);
	const double Deadline = FPlatformTime::Seconds() + 5.0;
	while (!FirstCommand->bExecuteProcessed && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(0.001f);
	}
	if (!TestTrue(TEXT("First worker completed before queueing more work"), FirstCommand->bExecuteProcessed != 0))
	{
		return false;
	}

	Fixture.Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), TArray<FString>(), EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateLambda([this, &Fixture, &AsyncCallbacks, &RejectedCallbacks](const FSourceControlOperationRef&, ECommandResult::Type Result)
		{
			++AsyncCallbacks;
			TestEqual(TEXT("Undispatched asynchronous work is cancelled"), Result, ECommandResult::Cancelled);
			Fixture.Provider.Close();
			Fixture.Provider.Init(false);
			TestFalse(TEXT("Completion callbacks cannot reopen a closing provider"), Fixture.Provider.IsEnabled());
			const ECommandResult::Type RejectedResult = Fixture.Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), TArray<FString>(), EConcurrency::Asynchronous,
				FSourceControlOperationComplete::CreateLambda([this, &RejectedCallbacks](const FSourceControlOperationRef&, ECommandResult::Type Rejected)
				{
					++RejectedCallbacks;
					TestEqual(TEXT("Work issued during Close is cancelled"), Rejected, ECommandResult::Cancelled);
				}));
			TestEqual(TEXT("Close rejects reentrant execution"), RejectedResult, ECommandResult::Cancelled);
		}));
	const ECommandResult::Type SyncResult = Fixture.Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), TArray<FString>(), EConcurrency::Synchronous,
		FSourceControlOperationComplete::CreateLambda([this, &SyncCallbacks](const FSourceControlOperationRef&, ECommandResult::Type Result)
		{
			++SyncCallbacks;
			TestEqual(TEXT("Undispatched synchronous work is cancelled"), Result, ECommandResult::Cancelled);
		}));

	TestEqual(TEXT("Waiting synchronous caller returns cancellation"), SyncResult, ECommandResult::Cancelled);
	TestEqual(TEXT("Cancelled workers never execute"), QueuedExecutions.GetValue(), 0);
	TestEqual(TEXT("Completed callback fires once"), FirstCallbacks, 1);
	TestEqual(TEXT("Asynchronous callback fires once"), AsyncCallbacks, 1);
	TestEqual(TEXT("Synchronous callback fires once"), SyncCallbacks, 1);
	TestEqual(TEXT("Rejected callback fires once"), RejectedCallbacks, 1);
	TestFalse(TEXT("Provider stays closed after callbacks finish"), Fixture.Provider.IsEnabled());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreProviderCloseResultsTest, "LoreSourceControl.Provider.CloseAppliesCompletedResults", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreProviderCloseResultsTest::RunTest(const FString& Parameters)
{
	if (!GThreadPool)
	{
		AddError(TEXT("The provider lifecycle test requires the engine thread pool."));
		return false;
	}
	for (const bool bApplyStates : { false, true })
	{
		int32 CompletedUpdates = 0;
		int32 CancelledUpdates = 0;
		int32 CompletedCallbacks = 0;
		int32 CancelledCallbacks = 0;
		FLoreSourceControlProvider Provider;
		const FLoreSourceControlWorkerRef CompletedWorker = MakeShared<FProviderTestWorker>([](FLoreSourceControlCommand&) { return true; }, [&Provider, &CompletedUpdates]()
		{
			++CompletedUpdates;
			Provider.Close(false);
			return true;
		});
		FLoreSourceControlCommand* CompletedCommand = new FLoreSourceControlCommand(ISourceControlOperation::Create<FUpdateStatus>(), CompletedWorker);
		CompletedCommand->OperationCompleteDelegate = FSourceControlOperationComplete::CreateLambda([this, &CompletedCallbacks](const FSourceControlOperationRef&, ECommandResult::Type Result)
		{
			++CompletedCallbacks;
			TestEqual(TEXT("Completed work reports success when closing"), Result, ECommandResult::Succeeded);
		});
		Provider.IssueCommand(*CompletedCommand);
		const double Deadline = FPlatformTime::Seconds() + 5.0;
		while (!CompletedCommand->bExecuteProcessed && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.001f);
		}
		if (!TestTrue(TEXT("Worker completed before Close"), CompletedCommand->bExecuteProcessed != 0))
		{
			Provider.Close(false);
			return false;
		}

		const FLoreSourceControlWorkerRef CancelledWorker = MakeShared<FProviderTestWorker>([](FLoreSourceControlCommand&) { return true; }, [&CancelledUpdates]()
		{
			++CancelledUpdates;
			return true;
		});
		FLoreSourceControlCommand* CancelledCommand = new FLoreSourceControlCommand(ISourceControlOperation::Create<FUpdateStatus>(), CancelledWorker);
		CancelledCommand->OperationCompleteDelegate = FSourceControlOperationComplete::CreateLambda([this, &CancelledCallbacks](const FSourceControlOperationRef&, ECommandResult::Type Result)
		{
			++CancelledCallbacks;
			TestEqual(TEXT("Unstarted work reports cancellation when closing"), Result, ECommandResult::Cancelled);
		});
		Provider.IssueCommand(*CancelledCommand);
		Provider.Close(bApplyStates);

		TestEqual(TEXT("Provider switch applies results and shutdown suppresses them"), CompletedUpdates, bApplyStates ? 1 : 0);
		TestEqual(TEXT("Cancelled work never applies results"), CancelledUpdates, 0);
		TestEqual(TEXT("Completed callback still fires once"), CompletedCallbacks, 1);
		TestEqual(TEXT("Cancelled callback still fires once"), CancelledCallbacks, 1);
	}
	return true;
}

#endif
