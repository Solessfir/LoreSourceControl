// Copyright Solessfir 2026. All Rights Reserved.

#include "LoreSourceControlOperations.h"
#include "LoreSourceControlCommand.h"
#include "LoreSourceControlUtils.h"
#include "LoreSourceControlProvider.h"
#include "SourceControlOperations.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#if SOURCE_CONTROL_WITH_SLATE
#include "Misc/MessageDialog.h"
#include "UnrealEdMisc.h"
#endif

#define LOCTEXT_NAMESPACE "LoreSourceControl"

#if SOURCE_CONTROL_WITH_SLATE
static void OfferEditorRestart(const FText& Reason)
{
	const FText Message = FText::Format(LOCTEXT("RestartEditorPrompt", "{0}\n\nRestart the editor now? You will be prompted to save any remaining changes before it closes."), Reason);
	if (FMessageDialog::Open(EAppMsgType::YesNo, Message, LOCTEXT("RestartEditorTitle", "Editor Restart Required")) == EAppReturnType::Yes)
	{
		FUnrealEdMisc::Get().RestartEditor(false);
	}
}
#endif

static FString NormalizeComparisonPath(const FString& InPath)
{
	FString Result = FPaths::ConvertRelativePathToFull(InPath);
	FPaths::NormalizeFilename(Result);
	Result.ReplaceInline(TEXT("\\"), TEXT("/"));
#if PLATFORM_WINDOWS
	Result.ToLowerInline();
#endif
	return Result;
}

//-----------------------------------------------------------------------------
// Connect
//-----------------------------------------------------------------------------
bool FLoreConnectWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	// Runs off the game thread (see FLoreSourceControlProvider::Init / IssueCommand).
	// Use InCommand.Provider, which was captured on the game thread.
	// Looking it up through FModuleManager or ISourceControlModule from a pool thread has raced the game thread and deadlocked.
	FLoreSourceControlProvider& LoreProvider = *InCommand.Provider;
	LoreProvider.UpdateCurrentBranchName();

	// Scan the entire Content tree up front to warm the state cache and immediately show real checkout, lock, and modified icons in the Content Browser.
	// Otherwise, per-asset states remain on the provider's generic "unknown file" default until another action queries the exact file.
	// This is why Revision Control > Refresh previously had to be run after every launch.
	TArray<FString> ContentDir;
	ContentDir.Add(FPaths::ProjectContentDir());
	StateScanPaths = ContentDir;
	bApplyStateResults = FLoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, ContentDir, LoreProvider, InCommand.bShouldLockFiles, InCommand.ErrorMessages, States);
	InCommand.bCommandSuccessful = bApplyStateResults;
	InCommand.bCommandSuccessful &= LoreProvider.IsAvailable();
	return InCommand.bCommandSuccessful;
}

bool FLoreConnectWorker::UpdateStates() const
{
	return FLoreSourceControlUtils::UpdateCachedStates(Provider, States, StateScanPaths, bApplyStateResults);
}

//-----------------------------------------------------------------------------
// CheckIn (commit)
//-----------------------------------------------------------------------------
bool FLoreCheckInWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TSharedRef<FCheckIn> Operation = StaticCastSharedRef<FCheckIn>(InCommand.Operation);
	const FText& Description = Operation->GetDescription();

	FString CommitMessage = Description.ToString();
	if (CommitMessage.IsEmpty())
	{
		CommitMessage = TEXT("Unreal Editor auto-commit");
	}

	if (InCommand.Files.IsEmpty())
	{
		InCommand.ErrorMessages.Add(TEXT("Submit was cancelled because no repository files were selected."));
		return false;
	}

	// Lore locks are advisory, so validate against a fresh server query immediately before staging.
	// A query failure is a hard stop: stale cache data must never be treated as permission to submit.
	if (InCommand.bShouldLockFiles)
	{
		TMap<FString, FLoreLockOwner> LockedBy;
		if (!InCommand.QueryLockStatus(LockedBy, InCommand.ErrorMessages))
		{
			InCommand.ErrorMessages.Add(TEXT("Submit aborted because current lock ownership could not be verified."));
			return false;
		}

		for (const FString& File : InCommand.Files)
		{
			const FString NormalizedFile = NormalizeComparisonPath(File);
			for (const TPair<FString, FLoreLockOwner>& Lock : LockedBy)
			{
				if (NormalizeComparisonPath(Lock.Key) != NormalizedFile)
				{
					continue;
				}

				const bool bOwnLock = Lock.Value.Identity.Equals(TEXT("me"), ESearchCase::IgnoreCase)
					|| Lock.Value.Identity.Equals(TEXT("self"), ESearchCase::IgnoreCase)
					|| (!InCommand.Identity.IsEmpty() && Lock.Value.Identity.Equals(InCommand.Identity, ESearchCase::IgnoreCase));
				if (!bOwnLock)
				{
					const FString LockOwner = Lock.Value.GetDisplayName();
					InCommand.ErrorMessages.Add(FString::Printf(
						TEXT("Submit aborted: %s is locked by %s."),
						*FPaths::GetCleanFilename(File),
						LockOwner.IsEmpty() ? TEXT("another user") : *LockOwner));
					return false;
				}
			}
		}
	}

	// First, make sure files are staged.
	// Strategy: use `lore stage --scan <files>` then `lore commit "msg"`
	TArray<FString> StageParams;
	StageParams.Add(TEXT("--scan"));

	TArray<FString> StageErrors;
	TArray<FString> StageResults;
	const bool bStaged = InCommand.RunLoreCommand(TEXT("stage"), StageParams, InCommand.Files, StageResults, StageErrors);
	InCommand.InfoMessages.Append(StageResults);
	if (!bStaged)
	{
		InCommand.ErrorMessages.Append(StageErrors);
		InCommand.ErrorMessages.Add(TEXT("Submit aborted because the selected files could not be staged."));
		return false;
	}

	// `lore commit` commits the entire stage.
	// Verify the stage contains no unrelated path before invoking it, preserving the user's existing stage instead of silently committing extra work.
	TArray<FString> StagedFiles;
	TArray<FString> StagedDirectories;
	if (!InCommand.ReadStagedPaths(StagedFiles, StagedDirectories, InCommand.ErrorMessages))
	{
		InCommand.ErrorMessages.Add(TEXT("Submit aborted because the staged path set could not be verified."));
		return false;
	}

	TSet<FString> SelectedPaths;
	for (const FString& File : InCommand.Files)
	{
		SelectedPaths.Add(NormalizeComparisonPath(File));
	}

	TArray<FString> UnexpectedStagedPaths;
	for (const FString& File : StagedFiles)
	{
		if (!SelectedPaths.Contains(NormalizeComparisonPath(File)))
		{
			UnexpectedStagedPaths.Add(File);
		}
	}

	for (const FString& Directory : StagedDirectories)
	{
		const FString NormalizedDirectory = NormalizeComparisonPath(Directory);
		const FString DirectoryPrefix = NormalizedDirectory.EndsWith(TEXT("/")) ? NormalizedDirectory : NormalizedDirectory + TEXT("/");
		bool bContainsSelectedPath = SelectedPaths.Contains(NormalizedDirectory);
		for (const FString& SelectedPath : SelectedPaths)
		{
			bContainsSelectedPath |= SelectedPath.StartsWith(DirectoryPrefix);
		}

		if (!bContainsSelectedPath)
		{
			UnexpectedStagedPaths.Add(Directory);
		}
	}

	if (!UnexpectedStagedPaths.IsEmpty())
	{
		InCommand.ErrorMessages.Add(TEXT("Submit aborted: Lore's stage also contains paths outside the current selection:"));
		for (const FString& Path : UnexpectedStagedPaths)
		{
			InCommand.ErrorMessages.Add(FString::Printf(TEXT("  %s"), *Path));
		}
		return false;
	}

	// Now commit
	TArray<FString> CommitParams;
	CommitParams.Add(FLoreSourceControlUtils::QuoteCommandLineArgument(CommitMessage));

	TArray<FString> CommitResults;
	TArray<FString> CommitErrors;
	InCommand.bCommandSuccessful = InCommand.RunLoreCommand(TEXT("commit"), CommitParams, TArray<FString>(), CommitResults, CommitErrors);

	InCommand.InfoMessages.Append(CommitResults);
	InCommand.ErrorMessages.Append(CommitErrors);

	// Push after committing when the repository has a remote. Offline repositories use the same Submit flow but keep the commit local.
	bool bPushed = false;
	if (InCommand.bCommandSuccessful && InCommand.bHasRemote)
	{
		TArray<FString> PushResults;
		TArray<FString> PushErrors;
		bPushed = InCommand.RunLoreCommand(TEXT("branch push"), TArray<FString>(), TArray<FString>(), PushResults, PushErrors);

		InCommand.InfoMessages.Append(PushResults);
		if (!bPushed)
		{
			InCommand.ErrorMessages.Append(PushErrors);
			InCommand.ErrorMessages.Add(TEXT("Commit succeeded locally, but push to remote failed. Run 'lore branch push' manually to publish it."));
			if (InCommand.Provider)
			{
				InCommand.Provider->SetHasChangesToPush(true);
			}
			InCommand.bCommandSuccessful = false;
		}
	}
	else if (InCommand.bCommandSuccessful)
	{
		InCommand.InfoMessages.Add(TEXT("No remote is configured. The commit was kept locally."));
	}

	// The change is now committed, so there is nothing left to protect by holding the lock - release it, same as Revert.
	// Best-effort: a file that was never locked has nothing to release.
	if (InCommand.bCommandSuccessful && InCommand.bHasRemote && bPushed && InCommand.bShouldLockFiles)
	{
		TArray<FString> UnlockResults, UnlockErrors;
		InCommand.RunLoreCommand(TEXT("lock release"), TArray<FString>(), InCommand.Files, UnlockResults, UnlockErrors);
	}

	// Refresh states for the files
	StateScanPaths = InCommand.Files;
	bApplyStateResults = InCommand.RefreshStatus(InCommand.Files, InCommand.bHasRemote && InCommand.bShouldLockFiles, InCommand.ErrorMessages, States);

	if (InCommand.Provider)
	{
		InCommand.Provider->UpdateCurrentBranchName();
	}

	// After successful commit, suggest asset reload to user / do it for content files
	if (InCommand.bCommandSuccessful)
	{
		// We don't force reload here automatically for all assets (can be disruptive).
		// But we can log that user may want to "Reload All" or use the Sync action which triggers more.
		InCommand.InfoMessages.Add(TEXT("Commit successful. You may need to reload modified assets in the Content Browser."));

		// The engine's Submit dialog reads this for its success toast through Operation->GetSuccessMessage().
		// Git, Perforce, and Plastic set it the same way; without it, the notification title is blank.
		Operation->SetSuccessMessage(FText::Format(
			InCommand.bHasRemote ? LOCTEXT("CheckInSuccess", "Submitted revision \"{0}\".") : LOCTEXT("LocalCheckInSuccess", "Committed revision \"{0}\" locally."),
			FText::FromString(CommitMessage)));
	}

	return InCommand.bCommandSuccessful;
}

bool FLoreCheckInWorker::UpdateStates() const
{
	return FLoreSourceControlUtils::UpdateCachedStates(Provider, States, StateScanPaths, bApplyStateResults);
}
//-----------------------------------------------------------------------------
// Branch history
//-----------------------------------------------------------------------------
bool FLoreRefreshBranchHistoryWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TSharedRef<FLoreRefreshBranchHistoryOperation> Operation = StaticCastSharedRef<FLoreRefreshBranchHistoryOperation>(InCommand.Operation);

	TArray<FLoreBranchHistoryEntry> History;
	InCommand.bCommandSuccessful = FLoreSourceControlUtils::RunGetBranchHistory(
		InCommand.PathToLoreBinary,
		InCommand.PathToRepositoryRoot,
		Operation->GetBranchName(),
		History,
		InCommand.ErrorMessages);
	Operation->SetHistory(MoveTemp(History));
	return InCommand.bCommandSuccessful;
}

//-----------------------------------------------------------------------------
// Sync
//-----------------------------------------------------------------------------
bool FLoreSyncWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	// Optional: support syncing specific revision if provided somehow via extended API in future.
	// For now plain sync.

	TArray<FString> Results;
	TArray<FString> Errors;
	bSyncSucceeded = FLoreSourceControlUtils::RunSync(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, Results, Errors, ChangedContentPaths, bRequiresRestart);
	InCommand.bCommandSuccessful = bSyncSucceeded;

	InCommand.InfoMessages.Append(Results);
	InCommand.ErrorMessages.Append(Errors);

	// After sync, update status of provided files (or whole tree)
	TArray<FString> FilesToUpdate = InCommand.Files;
	if (FilesToUpdate.Num() == 0)
	{
		// Update a broad set - project content at least
		FilesToUpdate.Add(FPaths::ProjectContentDir());
	}

	StateScanPaths = FilesToUpdate;
	bApplyStateResults = FLoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, FilesToUpdate, *InCommand.Provider, InCommand.bShouldLockFiles, InCommand.ErrorMessages, States);

	// Refresh branch name in case sync switched branches
	InCommand.Provider->UpdateCurrentBranchName();

	if (!InCommand.bCommandSuccessful)
	{
		InCommand.ErrorMessages.Add(TEXT("Sync failed; the working copy was not reported as up to date."));
	}
	else if (bRequiresRestart)
	{
		InCommand.InfoMessages.Add(TEXT("Sync complete. Source/Config files changed - restart the editor to pick up the new code."));
	}
	else if (!ChangedContentPaths.IsEmpty())
	{
		InCommand.InfoMessages.Add(FString::Printf(TEXT("Sync complete. Reloading %d updated asset(s)."), ChangedContentPaths.Num()));
	}
	else
	{
		InCommand.InfoMessages.Add(TEXT("Sync complete."));
	}

	return InCommand.bCommandSuccessful;
}

bool FLoreSyncWorker::UpdateStates() const
{
#if SOURCE_CONTROL_WITH_SLATE
	if (bSyncSucceeded && bRequiresRestart)
	{
		OfferEditorRestart(LOCTEXT("SyncRestartReason", "Sync completed, but Source or Config files changed."));
	}
#endif

	if (bSyncSucceeded && !bRequiresRestart && !ChangedContentPaths.IsEmpty() && Provider)
	{
		Provider->ReloadContentPackages(ChangedContentPaths);
	}

	return FLoreSourceControlUtils::UpdateCachedStates(Provider, States, StateScanPaths, bApplyStateResults);
}

//-----------------------------------------------------------------------------
// UpdateStatus
//-----------------------------------------------------------------------------
bool FLoreUpdateStatusWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	InCommand.bCommandSuccessful = FLoreSourceControlUtils::RunUpdateStatus(
		InCommand.PathToLoreBinary,
		InCommand.PathToRepositoryRoot,
		InCommand.Files,
		*InCommand.Provider,
		InCommand.bShouldLockFiles,
		InCommand.ErrorMessages,
		States);
	StateScanPaths = InCommand.Files.IsEmpty() ? TArray<FString>{ InCommand.PathToRepositoryRoot } : InCommand.Files;
	bApplyStateResults = InCommand.bCommandSuccessful;

	// History requires a separate "lore file history" call for every file.
	// Make those extra round trips only when requested, such as when the History window sets this option, rather than on every routine status refresh.
	const TSharedRef<FUpdateStatus> Operation = StaticCastSharedRef<FUpdateStatus>(InCommand.Operation);
	if (Operation->ShouldUpdateHistory())
	{
		for (const FString& File : InCommand.Files)
		{
			FString NormFile = FPaths::ConvertRelativePathToFull(File);
			FPaths::NormalizeFilename(NormFile);
			NormFile.ReplaceInline(TEXT("\\"), TEXT("/"));

			FLoreSourceControlState* State = States.FindByPredicate([&NormFile](const FLoreSourceControlState& S) { return S.LocalFilename == NormFile; });
			if (State)
			{
				FLoreSourceControlUtils::RunGetHistory(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, File, InCommand.ErrorMessages, State->History);
			}
		}
	}

	return InCommand.bCommandSuccessful;
}

bool FLoreUpdateStatusWorker::UpdateStates() const
{
	return FLoreSourceControlUtils::UpdateCachedStates(Provider, States, StateScanPaths, bApplyStateResults);
}

//-----------------------------------------------------------------------------
// CheckOut = lore lock acquire
//-----------------------------------------------------------------------------
bool FLoreCheckOutWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const bool bShouldLock = InCommand.bShouldLockFiles;

	if (bShouldLock)
	{
		TArray<FString> Errors;
		TArray<FString> Results;
		InCommand.bCommandSuccessful = FLoreSourceControlUtils::RunLoreCommand(TEXT("lock acquire"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, {}, InCommand.Files, Results, Errors);
		InCommand.InfoMessages.Append(Results);
		InCommand.ErrorMessages.Append(Errors);
	}
	else
	{
		// With ULoreSourceControlSettings::bLockFiles disabled, Check Out only needs to make the files editable locally.
		// The files are already editable because Lore never enforces local read-only state, so there is nothing to do here.
		InCommand.bCommandSuccessful = true;
	}

	if (InCommand.bCommandSuccessful)
	{
		// Lore lock acquire is advisory; ensure the files are writable on disk so UE editor accepts the checkout and does not warn "writable on disk but not checked out".
		for (const FString& F : InCommand.Files)
		{
			FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*F, false);
		}
	}

	// Refresh status/locks
	StateScanPaths = InCommand.Files;
	bApplyStateResults = FLoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Files, *InCommand.Provider, InCommand.bShouldLockFiles, InCommand.ErrorMessages, States);

	// Optimistically ensure checkout state for the files we successfully locked.
	// A post-acquire status query can report nothing due to capture, owner, or branch timing, but the acquire return code already confirmed success.
	if (bShouldLock && InCommand.bCommandSuccessful)
	{
		for (FLoreSourceControlState& S : States)
		{
			S.bIsCheckedOut = true;
			S.bIsCheckedOutOther = false;
		}
	}

	InCommand.Provider->UpdateCurrentBranchName();
	return InCommand.bCommandSuccessful;
}

bool FLoreCheckOutWorker::UpdateStates() const
{
	return FLoreSourceControlUtils::UpdateCachedStates(Provider, States, StateScanPaths, bApplyStateResults);
}

//-----------------------------------------------------------------------------
// Revert
//-----------------------------------------------------------------------------
bool FLoreRevertWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	TArray<FString> Results;
	TArray<FString> Errors;

	InCommand.bCommandSuccessful = FLoreSourceControlUtils::RunLoreCommand(TEXT("file reset"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, {}, InCommand.Files, Results, Errors);

	InCommand.InfoMessages.Append(Results);
	InCommand.ErrorMessages.Append(Errors);

	// Reverting local edits also gives up any lock held on the file - there is nothing left to check in, so there is no reason to keep it locked.
	// Best-effort: a file that was never locked simply has nothing to release, so this does not affect InCommand.bCommandSuccessful.
	if (InCommand.bCommandSuccessful && InCommand.bShouldLockFiles)
	{
		TArray<FString> UnlockResults, UnlockErrors;
		FLoreSourceControlUtils::RunLoreCommand(TEXT("lock release"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), InCommand.Files, UnlockResults, UnlockErrors);
		InCommand.InfoMessages.Append(UnlockResults);
		InCommand.ErrorMessages.Append(UnlockErrors);
	}

	StateScanPaths = InCommand.Files;
	bApplyStateResults = FLoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Files, *InCommand.Provider, InCommand.bShouldLockFiles, InCommand.ErrorMessages, States);
	InCommand.Provider->UpdateCurrentBranchName();
	return InCommand.bCommandSuccessful;
}

bool FLoreRevertWorker::UpdateStates() const
{
	return FLoreSourceControlUtils::UpdateCachedStates(Provider, States, StateScanPaths, bApplyStateResults);
}

//-----------------------------------------------------------------------------
// MarkForAdd
//-----------------------------------------------------------------------------
bool FLoreMarkForAddWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	// Deliberately does not call `lore stage` here - staging is left entirely to CheckIn's own unconditional `stage --scan` right before commit, same as modified files.
	// Keeps "staged in lore" meaning only one thing: about to be committed, not "the editor touched it at some point".
	StateScanPaths = InCommand.Files;
	bApplyStateResults = FLoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Files, *InCommand.Provider, InCommand.bShouldLockFiles, InCommand.ErrorMessages, States);
	InCommand.bCommandSuccessful = bApplyStateResults;
	InCommand.Provider->UpdateCurrentBranchName();
	return InCommand.bCommandSuccessful;
}

bool FLoreMarkForAddWorker::UpdateStates() const
{
	return FLoreSourceControlUtils::UpdateCachedStates(Provider, States, StateScanPaths, bApplyStateResults);
}

//-----------------------------------------------------------------------------
// Delete
//-----------------------------------------------------------------------------
bool FLoreDeleteWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	// A never-committed Add just gets unstaged; a genuinely tracked file gets its deletion staged.
	TArray<FString> FilesToUnstage;
	TArray<FString> FilesToStage;

	for (const FString& File : InCommand.Files)
	{
		// Runs on a pool thread - read via the provider's lock, not a raw reference to the map.
		FLoreSourceControlState CachedState(File);
		if (InCommand.Provider->TryGetStateFromCache(File, CachedState) && CachedState.IsAdded())
		{
			// A never-staged add needs no Lore-side delete action.
			// A staged add must be removed from the stage so a later repository-wide commit cannot resurrect it.
			if (CachedState.bIsStaged)
			{
				FilesToUnstage.Add(File);
			}
		}
		else
		{
			FilesToStage.Add(File);
		}
	}

	TArray<FString> Results;
	TArray<FString> Errors;
	InCommand.bCommandSuccessful = true;

	if (FilesToUnstage.Num() > 0)
	{
		InCommand.bCommandSuccessful &= FLoreSourceControlUtils::RunLoreCommand(TEXT("file unstage"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), FilesToUnstage, Results, Errors);
	}

	if (FilesToStage.Num() > 0)
	{
		InCommand.bCommandSuccessful &= FLoreSourceControlUtils::RunLoreCommand(TEXT("stage"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), FilesToStage, Results, Errors);
	}
	InCommand.InfoMessages.Append(Results);
	InCommand.ErrorMessages.Append(Errors);

	StateScanPaths = InCommand.Files;
	bApplyStateResults = FLoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Files, *InCommand.Provider, InCommand.bShouldLockFiles, InCommand.ErrorMessages, States);
	InCommand.Provider->UpdateCurrentBranchName();
	return InCommand.bCommandSuccessful;
}

bool FLoreDeleteWorker::UpdateStates() const
{
	return FLoreSourceControlUtils::UpdateCachedStates(Provider, States, StateScanPaths, bApplyStateResults);
}

//-----------------------------------------------------------------------------
// Unlock = lore lock release
//-----------------------------------------------------------------------------
bool FLoreUnlockWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	if (InCommand.bShouldLockFiles)
	{
		TArray<FString> Results;
		TArray<FString> Errors;
		InCommand.bCommandSuccessful = FLoreSourceControlUtils::RunLoreCommand(TEXT("lock release"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), InCommand.Files, Results, Errors);

		InCommand.InfoMessages.Append(Results);
		InCommand.ErrorMessages.Append(Errors);
	}
	else
	{
		// Locking disabled - nothing was ever locked, so there is nothing to release.
		InCommand.bCommandSuccessful = true;
	}

	StateScanPaths = InCommand.Files;
	bApplyStateResults = FLoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Files, *InCommand.Provider, InCommand.bShouldLockFiles, InCommand.ErrorMessages, States);
	InCommand.Provider->UpdateCurrentBranchName();
	return InCommand.bCommandSuccessful;
}

bool FLoreUnlockWorker::UpdateStates() const
{
	return FLoreSourceControlUtils::UpdateCachedStates(Provider, States, StateScanPaths, bApplyStateResults);
}

//-----------------------------------------------------------------------------
// Internal asynchronous operations
//-----------------------------------------------------------------------------
bool FLorePrintStatusWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	TArray<FString> Results;
	InCommand.bCommandSuccessful = FLoreSourceControlUtils::RunLoreCommand(
		TEXT("status"),
		InCommand.PathToLoreBinary,
		InCommand.PathToRepositoryRoot,
		TArray<FString>{ TEXT("--scan") },
		TArray<FString>(),
		Results,
		InCommand.ErrorMessages,
		false);
	InCommand.InfoMessages.Append(Results);
	return InCommand.bCommandSuccessful;
}

bool FLoreRefreshBranchesWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	bRefreshSucceeded = FLoreSourceControlUtils::RunGetBranches(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, Branches, &InCommand.ErrorMessages);
	InCommand.bCommandSuccessful = bRefreshSucceeded;
	return InCommand.bCommandSuccessful;
}

bool FLoreRefreshBranchesWorker::UpdateStates() const
{
	if (!Provider)
	{
		return false;
	}

	Provider->SetBranchRefreshResult(Branches, bRefreshSucceeded);
	return true;
}

bool FLoreSwitchBranchWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TSharedRef<FLoreSwitchBranchOperation> Operation = StaticCastSharedRef<FLoreSwitchBranchOperation>(InCommand.Operation);

	TArray<FString> ChangedPaths;
	bSwitchSucceeded = FLoreSourceControlUtils::RunSwitchBranch(
		InCommand.PathToLoreBinary,
		InCommand.PathToRepositoryRoot,
		Operation->GetBranchName(),
		InCommand.ErrorMessages,
		&ChangedPaths);

	if (!bSwitchSucceeded)
	{
		const bool bStagedStateBlocked = InCommand.ErrorMessages.ContainsByPredicate([](const FString& Error)
		{
			return Error.Contains(TEXT("staged state"), ESearchCase::IgnoreCase);
		});
		Operation->SetOutcome(bStagedStateBlocked
			? FLoreSwitchBranchOperation::EOutcome::StagedStateBlocked
			: FLoreSwitchBranchOperation::EOutcome::Failed);
		return false;
	}

	bRequiresRestart = FLoreSourceControlUtils::ClassifyChangedPaths(ChangedPaths, ChangedContentPaths);
	StateScanPaths = TArray<FString>{ InCommand.PathToRepositoryRoot };
	bApplyStateResults = FLoreSourceControlUtils::RunUpdateStatus(
		InCommand.PathToLoreBinary,
		InCommand.PathToRepositoryRoot,
		StateScanPaths,
		*InCommand.Provider,
		InCommand.bShouldLockFiles,
		InCommand.ErrorMessages,
		States);

	// The switch itself succeeded even if the follow-up status/lock refresh did not.
	InCommand.Provider->SetBranchName(Operation->GetBranchName());
	Operation->SetOutcome(FLoreSwitchBranchOperation::EOutcome::Success);
	InCommand.bCommandSuccessful = true;
	return true;
}

bool FLoreSwitchBranchWorker::UpdateStates() const
{
	if (!Provider || !bSwitchSucceeded)
	{
		return false;
	}

	// The working copy changed underneath every cached state.
	// Always discard the old cache, even if the follow-up status query failed, so callers never consume pre-switch state.
	Provider->ClearStateCache();
	FLoreSourceControlUtils::UpdateCachedStates(Provider, States, StateScanPaths, bApplyStateResults);

#if SOURCE_CONTROL_WITH_SLATE
	if (bRequiresRestart)
	{
		OfferEditorRestart(LOCTEXT("SwitchBranchRestartReason", "The branch switched, but Source or Config files changed."));
	}
#endif

	if (!bRequiresRestart && !ChangedContentPaths.IsEmpty())
	{
		Provider->ReloadContentPackages(ChangedContentPaths);
	}

	return true;
}

#undef LOCTEXT_NAMESPACE
