// Copyright Solessfir 2026. All Rights Reserved.

#include "LoreSourceControlProvider.h"
#include "LoreSourceControlCommand.h"
#include "LoreSourceControlUtils.h"
#include "SourceControlOperations.h"
#include "SourceControlHelpers.h"
#include "ScopedSourceControlProgress.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/QueuedThreadPool.h"
#include "Async/Async.h"
#include "Misc/MessageDialog.h"
#include "Logging/MessageLog.h"
#include "PackageTools.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/GarbageCollection.h"
#if SOURCE_CONTROL_WITH_SLATE
#include "SLoreSourceControlSettings.h"
#endif

#define LOCTEXT_NAMESPACE "LoreSourceControl"

void FLoreSourceControlProvider::Init(bool bForceConnection)
{
	// Init() can be called more than once (e.g. re-registering the modular feature); avoid
	// re-running the external "lore --version" probe every time.
	if (!bLoreAvailable)
	{
		CheckLoreAvailability();
	}

	// Filesystem-only repository discovery (looks for a ".lore" directory) - never spawns a
	// process, so this is always safe to run synchronously here.
	CheckRepositoryStatus();

	// Branch name and dirty/behind-remote flags require invoking the lore binary, which can be
	// slow or, if it ever stalls (e.g. waiting on its own server connection), hang outright.
	// Fetch them asynchronously via the thread pool so editor startup is never gated on lore.exe
	// (see FLoreConnectWorker). This is what previously caused the editor to hang on restart.
	if (bLoreAvailable && bLoreRepositoryFound)
	{
		Execute(ISourceControlOperation::Create<FConnect>(), TArray<FString>(), EConcurrency::Asynchronous);
	}
}

void FLoreSourceControlProvider::Close()
{
	// Commands still in flight reference this provider, and Tick() will never see them again once
	// the queue is emptied - retract the ones the pool hasn't started yet and wait out the rest,
	// so none of them outlive us or leak.
	for (FLoreSourceControlCommand* Command : CommandQueue)
	{
		if (!GThreadPool || !GThreadPool->RetractQueuedWork(Command))
		{
			while (!Command->bExecuteProcessed)
			{
				FPlatformProcess::Sleep(0.01f);
			}
		}
		delete Command;
	}
	CommandQueue.Empty();

	StateCache.Empty();

	bHasChangesToSync = false;
	BranchName.Empty();
}

FText FLoreSourceControlProvider::GetStatusText() const
{
	FScopeLock ScopeLock(&CriticalSection);

	FFormatNamedArguments Args;
	Args.Add(TEXT("Root"), FText::FromString(PathToRepositoryRoot));
	Args.Add(TEXT("RemoteUrl"), FText::FromString(RemoteUrl.IsEmpty() ? TEXT("(none)") : RemoteUrl));
	Args.Add(TEXT("Branch"), FText::FromString(BranchName.IsEmpty() ? TEXT("unknown") : BranchName));
	Args.Add(TEXT("Identity"), FText::FromString(Identity.IsEmpty() ? TEXT("(not configured)") : Identity));

	FText Base = FText::Format(LOCTEXT("LoreStatus", "Local repository: {Root}\nRemote origin: {RemoteUrl}\nBranch: {Branch}\nUser: {Identity}"), Args);

	if (!bLoreAvailable)
	{
		Base = FText::Format(LOCTEXT("LoreStatusNotAvailable",
			"{0}\n\n"
			"To configure: Project Settings > Editor > Lore Source Control (set 'Lore Path' or leave empty for auto-detect)."),
			Base);
	}

	return Base;
}

TMap<ISourceControlProvider::EStatus, FString> FLoreSourceControlProvider::GetStatus() const
{
	TMap<EStatus, FString> Result;
	Result.Add(EStatus::Enabled, IsEnabled() ? TEXT("Yes") : TEXT("No"));
	Result.Add(EStatus::Connected, IsAvailable() ? TEXT("Yes") : TEXT("No"));
	Result.Add(EStatus::ScmVersion, TEXT("lore (Epic)"));
	Result.Add(EStatus::PluginVersion, TEXT("0.1"));
	Result.Add(EStatus::WorkspacePath, PathToRepositoryRoot);
	Result.Add(EStatus::Branch, BranchName);
	return Result;
}

bool FLoreSourceControlProvider::IsEnabled() const
{
	return true; // Provider is enabled when registered
}

bool FLoreSourceControlProvider::IsAvailable() const
{
	FScopeLock Lock(&CriticalSection);
	return bLoreAvailable && bLoreRepositoryFound;
}

bool FLoreSourceControlProvider::IsLoreBinaryAvailable() const
{
	FScopeLock Lock(&CriticalSection);
	return bLoreAvailable;
}

const FName& FLoreSourceControlProvider::GetName() const
{
	static const FName ProviderName("Lore");
	return ProviderName;
}

ECommandResult::Type FLoreSourceControlProvider::GetState(const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
	TArray<FString> AbsoluteFiles = SourceControlHelpers::AbsoluteFilenames(InFiles);

	if (InStateCacheUsage == EStateCacheUsage::ForceUpdate)
	{
		// Synchronous Execute() dispatches to the thread pool and blocks this (game) thread until the
		// worker finishes - and that worker needs CriticalSection too (e.g. to read/set the branch
		// name). Must not hold the lock across this call or the two threads deadlock on it.
		TSharedRef<FUpdateStatus> UpdateStatusOperation = ISourceControlOperation::Create<FUpdateStatus>();
		Execute(UpdateStatusOperation, AbsoluteFiles);
	}

	FScopeLock ScopeLock(&CriticalSection);

	for (const FString& File : AbsoluteFiles)
	{
		if (const FLoreSourceControlState* State = StateCache.Find(File))
		{
			OutState.Add(MakeShareable(new FLoreSourceControlState(*State)));
		}
		else
		{
			// Unknown file - default to source controlled if under our repo root so that
			// UE asset discovery does not report thousands of "uncontrolled" assets for a lore workspace.
			FLoreSourceControlState NewState(File);
			if (bLoreRepositoryFound && !PathToRepositoryRoot.IsEmpty())
			{
				FString NormFile = FPaths::ConvertRelativePathToFull(File);
				FPaths::NormalizeFilename(NormFile);
				FString NormRoot = PathToRepositoryRoot;
				FPaths::NormalizeDirectoryName(NormRoot);
				if (!NormRoot.EndsWith(TEXT("/"))) { NormRoot += TEXT("/"); }
				NormFile = NormFile.Replace(TEXT("\\"), TEXT("/"));
				if (NormFile.StartsWith(NormRoot))
				{
					NewState.bIsSourceControlled = true;
					NewState.bIsCurrent = true;
					NewState.bIsUnknown = false;
				}
				else
				{
					NewState.bIsUnknown = true;
				}
			}
			else
			{
				NewState.bIsUnknown = true;
			}
			NewState.SetBranchName(GetBranchName());
			OutState.Add(MakeShareable(new FLoreSourceControlState(NewState)));
		}
	}

	return ECommandResult::Succeeded;
}

ECommandResult::Type FLoreSourceControlProvider::GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
	// Lore does not use changelists in the traditional sense (yet)
	return ECommandResult::Failed;
}

TArray<FSourceControlStateRef> FLoreSourceControlProvider::GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const
{
	FScopeLock ScopeLock(&CriticalSection);

	TArray<FSourceControlStateRef> Result;
	for (const auto& Pair : StateCache)
	{
		const FSourceControlStateRef State = MakeShareable(new FLoreSourceControlState(Pair.Value));
		if (Predicate(State))
		{
			Result.Add(State);
		}
	}
	return Result;
}

FDelegateHandle FLoreSourceControlProvider::RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged)
{
	return OnSourceControlStateChanged.Add(SourceControlStateChanged);
}

void FLoreSourceControlProvider::UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle)
{
	OnSourceControlStateChanged.Remove(Handle);
}

ECommandResult::Type FLoreSourceControlProvider::Execute(const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency, const FSourceControlOperationComplete& InOperationCompleteDelegate)
{
	if (!IsEnabled())
	{
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	TArray<FString> AbsoluteFiles = SourceControlHelpers::AbsoluteFilenames(InFiles);

	// A file outside our repository (e.g. Engine/Content) can't be queried by lore - it errors
	// "invalid path" and fails the whole batch, which took down "Submit Content" and similar ops
	// that gather every loaded package regardless of origin. Silently drop those instead.
	if (!PathToRepositoryRoot.IsEmpty())
	{
		FString NormRoot = PathToRepositoryRoot;
		FPaths::NormalizeDirectoryName(NormRoot);
		if (!NormRoot.EndsWith(TEXT("/")))
		{
			NormRoot += TEXT("/");
		}

		AbsoluteFiles.RemoveAll([&NormRoot](const FString& File)
		{
			FString NormFile = File;
			FPaths::NormalizeFilename(NormFile);
			NormFile.ReplaceInline(TEXT("\\"), TEXT("/"));
			return !NormFile.StartsWith(NormRoot);
		});

		// Every requested file was outside the repository. Running lore with an empty path list
		// would make path-scoped commands (stage --scan, lock acquire, ...) act on the whole
		// repository instead of on nothing - don't run anything.
		if (AbsoluteFiles.IsEmpty() && InFiles.Num() > 0)
		{
			InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Cancelled);
			return ECommandResult::Cancelled;
		}
	}

	// Lore never rejects a commit/push from someone who skipped locking (see docs/faq.md) - the
	// server will not stop this for us, so a file locked by someone else is left out of the submit
	// entirely rather than asking "commit anyway?": you can keep it writable and testing locally all
	// day, it just never gets staged/committed by us until that lock clears.
	if (InOperation->GetName() == "CheckIn" && FLoreSourceControlUtils::ShouldLockFiles())
	{
		AbsoluteFiles = FilterOutOtherLockedFiles(AbsoluteFiles);
		if (AbsoluteFiles.Num() == 0 && InFiles.Num() > 0)
		{
			InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Cancelled);
			return ECommandResult::Cancelled;
		}
	}

	TSharedPtr<ILoreSourceControlWorker> Worker = CreateWorker(InOperation->GetName());
	if (!Worker.IsValid())
	{
		FFormatNamedArguments Arguments;
		Arguments.Add(TEXT("OperationName"), FText::FromName(InOperation->GetName()));
		Arguments.Add(TEXT("ProviderName"), FText::FromName(GetName()));
		FText Message = FText::Format(LOCTEXT("UnsupportedOperation", "Operation '{OperationName}' not supported by revision control provider '{ProviderName}'"), Arguments);
		FMessageLog("SourceControl").Error(Message);
		InOperation->AddErrorMessge(Message);

		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	FLoreSourceControlCommand* Command = new FLoreSourceControlCommand(InOperation, Worker.ToSharedRef());
	Command->Provider = this;
	Command->Files = AbsoluteFiles;
	Command->PathToLoreBinary = GetLoreBinaryPath();
	Command->PathToRepositoryRoot = PathToRepositoryRoot;
	Command->OperationCompleteDelegate = InOperationCompleteDelegate;

	if (InConcurrency == EConcurrency::Synchronous)
	{
		Command->bAutoDelete = false;
		return ExecuteSynchronousCommand(*Command, InOperation->GetInProgressString());
	}

	Command->bAutoDelete = true;
	return IssueCommand(*Command);
}

bool FLoreSourceControlProvider::CanExecuteOperation(const FSourceControlOperationRef& InOperation) const
{
	return WorkersMap.Find(InOperation->GetName()) != nullptr;
}

bool FLoreSourceControlProvider::CanCancelOperation(const FSourceControlOperationRef& InOperation) const
{
	return false;
}

void FLoreSourceControlProvider::CancelOperation(const FSourceControlOperationRef& InOperation)
{
}

bool FLoreSourceControlProvider::UsesLocalReadOnlyState() const
{
	// Lore locks are advisory only (see FLoreSourceControlState::CanEdit) - acquiring one never
	// touches local file permissions, so unlike Perforce the read-only bit isn't a meaningful signal
	// here. Returning true would make the editor rely on a flag we never actually set.
	return false;
}

bool FLoreSourceControlProvider::UsesChangelists() const
{
	return false;
}

bool FLoreSourceControlProvider::UsesUncontrolledChangelists() const
{
	return false;
}

bool FLoreSourceControlProvider::UsesCheckout() const
{
	// Also the master switch gating the per-asset "Check Out" button (e.g. FEditorFileUtils::
	// IsCheckOutSelectedDisabled), so it must stay true - Check Out is actively used here. Tradeoff:
	// the Perforce-style "Check Out Modified Files" bulk dialog also becomes available.
	return true;
}

bool FLoreSourceControlProvider::UsesFileRevisions() const
{
	// Matches Git's own plugin (also false) despite having full per-file history support - this
	// flag isn't actually read anywhere in engine code, it exists purely for provider self-description.
	return false;
}

bool FLoreSourceControlProvider::UsesSnapshots() const
{
	return false;
}

bool FLoreSourceControlProvider::UsesSoftRevertOnDelete() const
{
	return false;
}

bool FLoreSourceControlProvider::AllowsDiffAgainstDepot() const
{
	// FLoreSourceControlRevision::Get() (used by the File History window) already fetches historical
	// content via "lore file write --revision", so the editor's built-in "Diff Against Depot" works
	// for free - it just needs this flag on to show up.
	return true;
}

TOptional<bool> FLoreSourceControlProvider::HasChangesToSync() const
{
	FScopeLock Lock(&CriticalSection);
	return TOptional<bool>(bHasChangesToSync);
}

TOptional<bool> FLoreSourceControlProvider::HasChangesToCheckIn() const
{
	// Scans the cache instead of a per-scan flag - a narrow scan reporting "nothing dirty" shouldn't
	// clobber a dirty file elsewhere that just wasn't part of it.
	FScopeLock Lock(&CriticalSection);
	for (const auto& Pair : StateCache)
	{
		if (Pair.Value.CanCheckIn())
		{
			return TOptional<bool>(true);
		}
	}
	return TOptional<bool>(false);
}

ECommandResult::Type FLoreSourceControlProvider::Login(const FString& InPassword, EConcurrency::Type InConcurrency, const FSourceControlOperationComplete& InOperationCompleteDelegate)
{
	if (!IsLoreBinaryAvailable())
	{
		const FText Error = LOCTEXT("LoginFailedNoLorePath", "Cannot accept settings: Lore executable path is not resolved or not valid. Please set a valid path in the settings above.");
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Error(Error);
		SourceControlLog.Notify(Error, EMessageSeverity::Error, /*bForce=*/true);

		if (InOperationCompleteDelegate.IsBound())
		{
			// Call with a dummy operation to report failure
			const TSharedRef<FConnect> DummyOp = ISourceControlOperation::Create<FConnect>();
			DummyOp->AddErrorMessge(Error);
			InOperationCompleteDelegate.Execute(DummyOp, ECommandResult::Failed);
		}
		return ECommandResult::Failed;
	}

	// If we have a valid path, consider login successful for settings acceptance (actual repo connect happens separately)
	if (InOperationCompleteDelegate.IsBound())
	{
		const TSharedRef<FConnect> Op = ISourceControlOperation::Create<FConnect>();
		InOperationCompleteDelegate.Execute(Op, ECommandResult::Succeeded);
	}
	return ECommandResult::Succeeded;
}

void FLoreSourceControlProvider::Tick()
{
	bool bStatesUpdated = false;

	for (int32 CommandIndex = 0; CommandIndex < CommandQueue.Num(); ++CommandIndex)
	{
		FLoreSourceControlCommand& Command = *CommandQueue[CommandIndex];
		if (Command.bExecuteProcessed)
		{
			CommandQueue.RemoveAt(CommandIndex);

			bStatesUpdated |= Command.Worker->UpdateStates();

			OutputCommandMessages(Command);

			Command.ReturnResults();

			if (Command.bAutoDelete)
			{
				delete &Command;
			}

			// Only process one completed command per tick: the completion delegate above may
			// itself touch CommandQueue (e.g. issuing a follow-up operation).
			break;
		}
	}

	if (bStatesUpdated)
	{
		BroadcastStateChanged();
	}
}

TArray<TSharedRef<ISourceControlLabel>> FLoreSourceControlProvider::GetLabels(const FString& InMatchingSpec) const
{
	return TArray<TSharedRef<ISourceControlLabel>>();
}

TArray<FSourceControlChangelistRef> FLoreSourceControlProvider::GetChangelists(EStateCacheUsage::Type InStateCacheUsage)
{
	return TArray<FSourceControlChangelistRef>();
}

#if SOURCE_CONTROL_WITH_SLATE
TSharedRef<SWidget> FLoreSourceControlProvider::MakeSettingsWidget() const
{
	return SNew(SLoreSourceControlSettings);
}
#endif

FString FLoreSourceControlProvider::GetLoreBinaryPath() const
{
	FScopeLock Lock(&CriticalSection);
	return LoreBinaryPath;
}

bool FLoreSourceControlProvider::SetLoreBinaryPath(const FString& InPath)
{
	FScopeLock Lock(&CriticalSection);
	LoreBinaryPath = InPath;

	// Persist the choice into the proper Developer Settings
	FLoreSourceControlUtils::SetUserConfiguredLoreBinaryPath(InPath);
	return true;
}

void FLoreSourceControlProvider::CheckLoreAvailability()
{
	// Resolve and probe the binary before taking the lock - both spawn external "lore --version"
	// processes, and holding CriticalSection across those stalls every thread that touches a getter.
	const FString UserPath = FLoreSourceControlUtils::GetUserConfiguredLoreBinaryPath();

	FString NewBinaryPath;
	bool bAvailable;

	if (!UserPath.IsEmpty())
	{
		NewBinaryPath = UserPath;
		bAvailable = FLoreSourceControlUtils::CheckLoreAvailability(NewBinaryPath);
	}
	else
	{
		NewBinaryPath = FLoreSourceControlUtils::FindLoreBinaryPath();
		bAvailable = !NewBinaryPath.IsEmpty() && FLoreSourceControlUtils::CheckLoreAvailability(NewBinaryPath);

		// Auto-apply a successfully discovered path so the setting is populated
		// and the user doesn't have to manage "leave empty for auto-detection".
		if (bAvailable)
		{
			FLoreSourceControlUtils::SetUserConfiguredLoreBinaryPath(NewBinaryPath);
		}
	}

	{
		FScopeLock Lock(&CriticalSection);
		LoreBinaryPath = NewBinaryPath;
		bLoreAvailable = bAvailable;
	}

	if (!bAvailable)
	{
#if PLATFORM_WINDOWS
		static const TCHAR* DefaultLocationText = TEXT("C:\\Program Files\\lore");
#elif PLATFORM_MAC
		static const TCHAR* DefaultLocationText = TEXT("/usr/local/bin or /opt/homebrew/bin");
#else
		static const TCHAR* DefaultLocationText = TEXT("/usr/local/bin or /usr/bin");
#endif

		FMessageLog("SourceControl").Warning(
			FText::Format(
				LOCTEXT("LoreBinaryNotFound",
					"Lore binary not found or not responding.\n"
					"Current path/command: {0}\n\n"
					"Make sure the 'lore' command is available in your PATH, or install it to the platform default location ({1}).\n"
					"Download it from https://github.com/EpicGames/lore/releases.\n"
					"You can also explicitly set the path to the lore executable in Project Settings > Editor > Lore Source Control."),
				FText::FromString(NewBinaryPath.IsEmpty() ? TEXT("<none>") : NewBinaryPath),
				FText::FromString(DefaultLocationText)
			)
		);
	}
}

void FLoreSourceControlProvider::CheckRepositoryStatus()
{
	FScopeLock Lock(&CriticalSection);

	// Filesystem-only (looks for a ".lore" directory) - deliberately never invokes the lore
	// binary here. Branch name and dirty/behind-remote flags are fetched asynchronously by
	// FLoreConnectWorker so this can never block the calling thread on an external process.
	FString RepoRoot;
	if (FLoreSourceControlUtils::FindRootDirectory(FPaths::ProjectDir(), RepoRoot))
	{
		PathToRepositoryRoot = FPaths::ConvertRelativePathToFull(RepoRoot);
		FPaths::NormalizeDirectoryName(PathToRepositoryRoot);
		bLoreRepositoryFound = true;

		FLoreSourceControlUtils::ReadRepositoryConfig(PathToRepositoryRoot, RemoteUrl, Identity);
	}
	else
	{
		bLoreRepositoryFound = false;
		PathToRepositoryRoot.Empty();
		BranchName.Empty();
		RemoteUrl.Empty();
		Identity.Empty();
		bHasChangesToSync = false;
	}
}

void FLoreSourceControlProvider::RegisterWorker(const FName& InName, const FLoreGetSourceControlWorker& InDelegate)
{
	WorkersMap.Add(InName, InDelegate);
}

TSharedPtr<ILoreSourceControlWorker> FLoreSourceControlProvider::CreateWorker(const FName& InOperationName) const
{
	if (const FLoreGetSourceControlWorker* WorkerDelegate = WorkersMap.Find(InOperationName))
	{
		return WorkerDelegate->Execute();
	}
	return TSharedPtr<ILoreSourceControlWorker>();
}

ECommandResult::Type FLoreSourceControlProvider::ExecuteSynchronousCommand(FLoreSourceControlCommand& InCommand, const FText& Task)
{
	ECommandResult::Type Result = ECommandResult::Failed;

	// Show a progress dialog (if Slate is up) while we wait. Ticking it - and Tick() below -
	// keeps Slate pumping, so the editor stays responsive even if the underlying lore.exe call
	// takes a while; it can never freeze the UI since the call itself runs on a pool thread.
	{
		FScopedSourceControlProgress Progress(Task);
		IssueCommand(InCommand);

		while (!InCommand.bExecuteProcessed)
		{
			Tick();
			Progress.Tick();
			FPlatformProcess::Sleep(0.01f);
		}

		// One more tick to make sure this command is picked up and cleaned out of the queue.
		Tick();

		if (InCommand.bCommandSuccessful)
		{
			Result = ECommandResult::Succeeded;
		}
	}

	check(!InCommand.bAutoDelete);
	CommandQueue.Remove(&InCommand);
	delete &InCommand;

	return Result;
}

ECommandResult::Type FLoreSourceControlProvider::IssueCommand(FLoreSourceControlCommand& InCommand)
{
	if (GThreadPool)
	{
		GThreadPool->AddQueuedWork(&InCommand);
		CommandQueue.Add(&InCommand);
		return ECommandResult::Succeeded;
	}

	const FText Message(LOCTEXT("NoSCCThreads", "There are no threads available to process the Lore revision control command."));
	FMessageLog("SourceControl").Error(Message);
	InCommand.Operation->AddErrorMessge(Message);
	InCommand.bCommandSuccessful = false;

	// No pool thread will ever run this command, so mark it processed ourselves - otherwise
	// ExecuteSynchronousCommand's wait loop would spin on bExecuteProcessed forever - and since
	// it will never reach Tick() either, take over Tick()'s cleanup duty for auto-deleted (async) commands.
	FPlatformAtomics::InterlockedExchange(&InCommand.bExecuteProcessed, 1);
	const ECommandResult::Type Result = InCommand.ReturnResults();

	if (InCommand.bAutoDelete)
	{
		delete &InCommand;
	}

	return Result;
}

void FLoreSourceControlProvider::OutputCommandMessages(const FLoreSourceControlCommand& InCommand)
{
	FMessageLog SourceControlLog("SourceControl");

	for (const FString& ErrorMessage : InCommand.ErrorMessages)
	{
		SourceControlLog.Error(FText::FromString(ErrorMessage));
	}

	for (const FString& InfoMessage : InCommand.InfoMessages)
	{
		SourceControlLog.Info(FText::FromString(InfoMessage));
	}
}

TArray<FString> FLoreSourceControlProvider::FilterOutOtherLockedFiles(const TArray<FString>& InFiles)
{
	TArray<FString> Filtered;
	TArray<FString> ExcludedFiles;
	Filtered.Reserve(InFiles.Num());

	FString ExcludedList;
	{
		FScopeLock Lock(&CriticalSection);
		for (const FString& File : InFiles)
		{
			const FLoreSourceControlState* State = StateCache.Find(File);
			FString Who;
			if (State && State->IsCheckedOutOther(&Who))
			{
				ExcludedFiles.Add(File);
				ExcludedList += FString::Printf(TEXT("\n%s (locked by %s)"), *FPaths::GetCleanFilename(File), Who.IsEmpty() ? TEXT("someone else") : *Who);
			}
			else
			{
				Filtered.Add(File);
			}
		}
	}

#if SOURCE_CONTROL_WITH_SLATE
	if (!ExcludedFiles.IsEmpty())
	{
		const EAppReturnType::Type Choice = FMessageDialog::Open(
			EAppMsgType::YesNo,
			FText::Format(
				LOCTEXT("ExcludedOtherLockedFiles",
					"The following file(s) are locked by someone else, you're not allowed to commit locked files.\n{0}\n\n"
					"If you don't need your local changes to them, revert them now? "
					"(No leaves them as they are - you can revert manually later via Source Control > Revert.)"),
				FText::FromString(ExcludedList)
			)
		);

		if (Choice == EAppReturnType::Yes)
		{
			Execute(ISourceControlOperation::Create<FRevert>(), ExcludedFiles, EConcurrency::Asynchronous);
		}
	}
#endif

	return Filtered;
}

bool FLoreSourceControlProvider::TryGetStateFromCache(const FString& Filename, FLoreSourceControlState& OutState) const
{
	FScopeLock Lock(&CriticalSection);
	if (const FLoreSourceControlState* State = StateCache.Find(Filename))
	{
		OutState = *State;
		return true;
	}
	return false;
}

void FLoreSourceControlProvider::AddStatesToCache(const TArray<FLoreSourceControlState>& InStates)
{
	FScopeLock Lock(&CriticalSection);
	for (const FLoreSourceControlState& State : InStates)
	{
		FLoreSourceControlState Copy = State;
		if (!BranchName.IsEmpty())
		{
			Copy.SetBranchName(BranchName);
		}
		StateCache.Add(Copy.LocalFilename, MoveTemp(Copy));
	}
}

bool FLoreSourceControlProvider::RemoveStateFromCache(const FString& Filename)
{
	FScopeLock Lock(&CriticalSection);
	return StateCache.Remove(Filename) > 0;
}

void FLoreSourceControlProvider::BroadcastStateChanged() const
{
	OnSourceControlStateChanged.Broadcast();
}

void FLoreSourceControlProvider::LoadSettings()
{
	// If still empty after load (no user override, no migration), leave it empty so that
	// CheckLoreAvailability / FindLoreBinaryPath will perform full auto-detection.
	FLoreSourceControlUtils::LoadSettings(LoreBinaryPath);
}

void FLoreSourceControlProvider::SaveSettings() const
{
	FLoreSourceControlUtils::SaveSettings(LoreBinaryPath);
}

void FLoreSourceControlProvider::UpdateCurrentBranchName()
{
	// GetCurrentBranchName shells out to lore.exe (up to three commands) and this runs on pool
	// threads - holding CriticalSection across it would block the game thread's getters (branch
	// name is polled by UI) for the whole exec duration. Copy inputs out, exec, then write back.
	FString LocalBinary;
	FString LocalRoot;
	bool bRepositoryFound;
	{
		FScopeLock Lock(&CriticalSection);
		LocalBinary = LoreBinaryPath;
		LocalRoot = PathToRepositoryRoot;
		bRepositoryFound = bLoreRepositoryFound;
	}

	const FString NewBranch = FLoreSourceControlUtils::GetCurrentBranchName(LocalBinary, LocalRoot);

	FScopeLock Lock(&CriticalSection);
	if (!NewBranch.IsEmpty())
	{
		BranchName = NewBranch;
	}
	else if (bRepositoryFound)
	{
		BranchName = TEXT("unknown");
	}
}

void FLoreSourceControlProvider::SetBranchName(const FString& InBranchName)
{
	FScopeLock Lock(&CriticalSection);
	if (!InBranchName.IsEmpty())
	{
		BranchName = InBranchName;
	}
}

FString FLoreSourceControlProvider::GetBranchName() const
{
	FScopeLock Lock(&CriticalSection);
	return BranchName;
}

FString FLoreSourceControlProvider::GetRemoteUrl() const
{
	FScopeLock Lock(&CriticalSection);
	return RemoteUrl;
}

FString FLoreSourceControlProvider::GetIdentity() const
{
	FScopeLock Lock(&CriticalSection);
	return Identity;
}

FString FLoreSourceControlProvider::GetRepositoryRoot() const
{
	FScopeLock Lock(&CriticalSection);
	return PathToRepositoryRoot;
}

TArray<FLoreBranchInfo> FLoreSourceControlProvider::GetCachedBranches() const
{
	FScopeLock Lock(&CriticalSection);
	return CachedBranches;
}

int32 FLoreSourceControlProvider::GetCachedBranchCount() const
{
	FScopeLock Lock(&CriticalSection);
	return CachedBranchCount;
}

void FLoreSourceControlProvider::RefreshBranchesAsync()
{
	FString LocalBinary;
	FString LocalRoot;
	{
		FScopeLock Lock(&CriticalSection);
		LocalBinary = LoreBinaryPath;
		LocalRoot = PathToRepositoryRoot;
	}

	TWeakPtr<uint8> WeakAlive = AliveMarker;
	Async(EAsyncExecution::ThreadPool, [this, WeakAlive, LocalBinary, LocalRoot]()
	{
		TArray<FLoreBranchInfo> Branches;
		FLoreSourceControlUtils::RunGetBranches(LocalBinary, LocalRoot, Branches);

		AsyncTask(ENamedThreads::GameThread, [this, WeakAlive, Branches]()
		{
			// Provider destroyed (module shutdown) while the fetch was in flight - don't touch it.
			if (!WeakAlive.IsValid())
			{
				return;
			}

			FScopeLock Lock(&CriticalSection);
			CachedBranches = Branches;
			CachedBranchCount = Branches.Num();
		});
	});
}

bool FLoreSourceControlProvider::DoesBranchSwitchTouchCode(const FString& InTargetBranch, TArray<FString>& OutChangedContentPaths) const
{
	FString LocalBinary;
	FString LocalRoot;
	{
		FScopeLock Lock(&CriticalSection);
		LocalBinary = LoreBinaryPath;
		LocalRoot = PathToRepositoryRoot;
	}

	TArray<FString> ChangedPaths;
	TArray<FString> Errors;
	if (!FLoreSourceControlUtils::RunGetBranchDiff(LocalBinary, LocalRoot, InTargetBranch, ChangedPaths, Errors))
	{
		// Could not determine what actually changed - assume the risky case.
		return true;
	}

	return FLoreSourceControlUtils::ClassifyChangedPaths(ChangedPaths, OutChangedContentPaths);
}

FLoreSourceControlProvider::ELoreSwitchResult FLoreSourceControlProvider::SwitchBranch(const FString& InBranchName)
{
	FString LocalBinary;
	FString LocalRoot;
	{
		FScopeLock Lock(&CriticalSection);
		LocalBinary = LoreBinaryPath;
		LocalRoot = PathToRepositoryRoot;
	}

	TArray<FString> Errors;
	bool bOk = FLoreSourceControlUtils::RunSwitchBranch(LocalBinary, LocalRoot, InBranchName, Errors);

	if (!bOk)
	{
		bool bStagedStateBlocked = false;
		for (const FString& Error : Errors)
		{
			FMessageLog("SourceControl").Error(FText::FromString(Error));
			if (Error.Contains(TEXT("staged state")))
			{
				bStagedStateBlocked = true;
			}
		}
		return bStagedStateBlocked ? ELoreSwitchResult::StagedStateBlocked : ELoreSwitchResult::Failed;
	}

	// The switch just changed files on disk out from under the working copy - the cache is stale
	// and any loaded assets need to be reloaded, which the caller handles based on DoesBranchSwitchTouchCode.
	{
		FScopeLock Lock(&CriticalSection);
		StateCache.Empty();
	}

	UpdateCurrentBranchName();
	BroadcastStateChanged();

	return ELoreSwitchResult::Success;
}

void FLoreSourceControlProvider::ReloadContentPackages(const TArray<FString>& InChangedRelativePaths) const
{
	const FString RepoRoot = GetRepositoryRoot();
	if (RepoRoot.IsEmpty())
	{
		return;
	}

	TArray<UPackage*> PackagesToReload;
	for (const FString& RelativePath : InChangedRelativePaths)
	{
		FString AbsolutePath = FPaths::Combine(RepoRoot, RelativePath);
		FPaths::NormalizeFilename(AbsolutePath);

		FString PackageName;
		if (!FPackageName::TryConvertFilenameToLongPackageName(AbsolutePath, PackageName))
		{
			continue;
		}

		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			PackagesToReload.Add(Package);
		}
	}

	if (PackagesToReload.IsEmpty())
	{
		return;
	}

	FText ErrorMessage;
	UPackageTools::ReloadPackages(PackagesToReload, ErrorMessage, EReloadPackagesInteractionMode::Interactive);

	if (!ErrorMessage.IsEmpty())
	{
		FMessageLog("SourceControl").Warning(ErrorMessage);
	}

	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
}

void FLoreSourceControlProvider::SetHasChangesToSync(const bool bInHasChanges)
{
	FScopeLock Lock(&CriticalSection);
	bHasChangesToSync = bInHasChanges;
}

#undef LOCTEXT_NAMESPACE
