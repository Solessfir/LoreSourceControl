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
#endif

#define LOCTEXT_NAMESPACE "LoreSourceControl"

// Quote a free-form string (e.g. a commit message) as a single command-line argument. Escaping only
// the quotes is not enough: backslashes that precede a quote (or the end of the argument) must be
// doubled, or a message ending in '\' swallows the closing quote and corrupts the whole command line.
static FString QuoteCommandLineArgument(const FString& InArg)
{
	FString Escaped;
	Escaped.Reserve(InArg.Len() + 2);

	int32 PendingBackslashes = 0;
	for (const TCHAR Char : InArg)
	{
		if (Char == TEXT('\\'))
		{
			++PendingBackslashes;
			continue;
		}

		if (Char == TEXT('"'))
		{
			Escaped.Append(FString::ChrN(PendingBackslashes * 2 + 1, TEXT('\\')));
			Escaped.AppendChar(TEXT('"'));
		}
		else
		{
			Escaped.Append(FString::ChrN(PendingBackslashes, TEXT('\\')));
			Escaped.AppendChar(Char);
		}
		PendingBackslashes = 0;
	}

	Escaped.Append(FString::ChrN(PendingBackslashes * 2, TEXT('\\')));
	return FString::Printf(TEXT("\"%s\""), *Escaped);
}

//-----------------------------------------------------------------------------
// Connect
//-----------------------------------------------------------------------------
bool FLoreConnectWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	// Runs off the game thread (see FLoreSourceControlProvider::Init / IssueCommand). Must go through
	// InCommand.Provider (captured on the game thread) rather than looking it up via FModuleManager/
	// ISourceControlModule here - doing that from a pool thread has raced the game thread and deadlocked.
	FLoreSourceControlProvider& Provider = *InCommand.Provider;
	Provider.UpdateCurrentBranchName();

	// Scan the whole Content tree once up front and warm the state cache with it, so the Content
	// Browser shows real checkout/lock/modified icons immediately - without this, per-asset states
	// stay on the provider's generic "unknown file" default until something else happens to query
	// that exact file, which is why a manual Revision Control > Refresh was needed after every launch.
	TArray<FString> ContentDir;
	ContentDir.Add(FPaths::ProjectContentDir());
	InCommand.bCommandSuccessful = FLoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, ContentDir, Provider, InCommand.ErrorMessages, States);
	InCommand.bCommandSuccessful &= Provider.IsAvailable();
	return InCommand.bCommandSuccessful;
}

bool FLoreConnectWorker::UpdateStates() const
{
	return FLoreSourceControlUtils::UpdateCachedStates(States);
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

	// First, make sure files are staged.
	// Strategy: use `lore stage --scan <files>` then `lore commit "msg"`
	TArray<FString> StageParams;
	StageParams.Add(TEXT("--scan"));

	TArray<FString> StageErrors;
	TArray<FString> StageResults;
	bool bStaged = FLoreSourceControlUtils::RunLoreCommand(TEXT("stage"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, StageParams, InCommand.Files, StageResults, StageErrors);
	if (!bStaged)
	{
		// Try without --scan as fallback
		FLoreSourceControlUtils::RunLoreCommand(TEXT("stage"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), InCommand.Files, StageResults, StageErrors);
	}

	// Now commit
	TArray<FString> CommitParams;
	CommitParams.Add(QuoteCommandLineArgument(CommitMessage));

	TArray<FString> CommitResults;
	TArray<FString> CommitErrors;
	InCommand.bCommandSuccessful = FLoreSourceControlUtils::RunLoreCommand(TEXT("commit"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, CommitParams, TArray<FString>(), CommitResults, CommitErrors);

	InCommand.InfoMessages.Append(CommitResults);
	InCommand.ErrorMessages.Append(CommitErrors);

	// Push right after commit - a commit that only lives locally isn't "submitted" from the rest of
	// the team's point of view, matching the P4/Git "Submit = it's on the server now" expectation.
	// Doesn't fail the whole command if push fails: the commit itself already succeeded locally,
	// so just surface it as an error the user can act on (retry the push manually).
	if (InCommand.bCommandSuccessful)
	{
		TArray<FString> PushResults;
		TArray<FString> PushErrors;
		const bool bPushed = FLoreSourceControlUtils::RunLoreCommand(TEXT("branch push"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), TArray<FString>(), PushResults, PushErrors);

		InCommand.InfoMessages.Append(PushResults);
		if (!bPushed)
		{
			InCommand.ErrorMessages.Append(PushErrors);
			InCommand.ErrorMessages.Add(TEXT("Commit succeeded locally, but push to remote failed. Run 'lore branch push' manually to publish it."));
		}
	}

	// The change is now committed, so there is nothing left to protect by holding the lock -
	// release it, same as Revert. Best-effort: a file that was never locked has nothing to release.
	if (InCommand.bCommandSuccessful && FLoreSourceControlUtils::ShouldLockFiles())
	{
		TArray<FString> UnlockResults, UnlockErrors;
		FLoreSourceControlUtils::RunLoreCommand(TEXT("lock release"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), InCommand.Files, UnlockResults, UnlockErrors);
	}

	// Refresh states for the files
	FLoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Files, *InCommand.Provider, InCommand.ErrorMessages, States);

	InCommand.Provider->UpdateCurrentBranchName();

	// After successful commit, suggest asset reload to user / do it for content files
	if (InCommand.bCommandSuccessful)
	{
		// We don't force reload here automatically for all assets (can be disruptive).
		// But we can log that user may want to "Reload All" or use the Sync action which triggers more.
		InCommand.InfoMessages.Add(TEXT("Commit successful. You may need to reload modified assets in the Content Browser."));

		// The engine's Submit dialog reads this for its success toast (Operation->GetSuccessMessage()) -
		// Git/Perforce/Plastic all set it the same way; without it the notification title is blank.
		Operation->SetSuccessMessage(FText::Format(
			LOCTEXT("CheckInSuccess", "Submitted revision \"{0}\"."),
			FText::FromString(CommitMessage)));
	}

	return InCommand.bCommandSuccessful;
}

bool FLoreCheckInWorker::UpdateStates() const
{
	return FLoreSourceControlUtils::UpdateCachedStates(States);
}

//-----------------------------------------------------------------------------
// Sync
//-----------------------------------------------------------------------------
bool FLoreSyncWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	// Optional: support syncing specific revision if provided somehow via extended API in future.
	// For now plain sync.

	Provider = InCommand.Provider;

	TArray<FString> Results;
	TArray<FString> Errors;
	InCommand.bCommandSuccessful = FLoreSourceControlUtils::RunSync(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, Results, Errors, ChangedContentPaths, bRequiresRestart);

	InCommand.InfoMessages.Append(Results);
	InCommand.ErrorMessages.Append(Errors);

	// After sync, update status of provided files (or whole tree)
	TArray<FString> FilesToUpdate = InCommand.Files;
	if (FilesToUpdate.Num() == 0)
	{
		// Update a broad set - project content at least
		FilesToUpdate.Add(FPaths::ProjectContentDir());
	}

	FLoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, FilesToUpdate, *InCommand.Provider, InCommand.ErrorMessages, States);

	// Refresh branch name in case sync switched branches
	InCommand.Provider->UpdateCurrentBranchName();

	if (bRequiresRestart)
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
	if (bRequiresRestart)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Sync complete. Source/Config files changed - restart the editor now to pick up the new code.")));
	}
	else
#endif
	if (!ChangedContentPaths.IsEmpty() && Provider)
	{
		Provider->ReloadContentPackages(ChangedContentPaths);
	}

	return FLoreSourceControlUtils::UpdateCachedStates(States);
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
		InCommand.ErrorMessages,
		States);

	// History is a separate, per-file "lore file history" call - only worth the extra round trips
	// when something actually asked for it (the History window sets this), not on every routine
	// status refresh.
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
	return FLoreSourceControlUtils::UpdateCachedStates(States);
}

//-----------------------------------------------------------------------------
// CheckOut = lore lock acquire
//-----------------------------------------------------------------------------
bool FLoreCheckOutWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const bool bShouldLock = FLoreSourceControlUtils::ShouldLockFiles();

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
		// Locking disabled (ULoreSourceControlSettings::bLockFiles) - Check Out just needs to make
		// the files available to edit locally, which they already always are (Lore never enforces
		// local read-only state either way), so there is nothing left to do here.
		InCommand.bCommandSuccessful = true;
	}

	if (InCommand.bCommandSuccessful)
	{
		// Lore lock acquire is advisory; ensure the files are writable on disk so UE editor
		// accepts the checkout and does not warn "writable on disk but not checked out".
		for (const FString& F : InCommand.Files)
		{
			FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*F, false);
		}
	}

	// Refresh status/locks
	FLoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Files, *InCommand.Provider, InCommand.ErrorMessages, States);

	// Optimistically ensure checkout state for the files we successfully locked (in case post-acquire lock status
	// reports nothing due to capture/owner/branch timing; the acquire RC already confirmed success).
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
	return FLoreSourceControlUtils::UpdateCachedStates(States);
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

	// Reverting local edits also gives up any lock held on the file - there is nothing left to
	// check in, so there is no reason to keep it locked. Best-effort: a file that was never locked
	// simply has nothing to release, so this does not affect InCommand.bCommandSuccessful.
	if (FLoreSourceControlUtils::ShouldLockFiles())
	{
		TArray<FString> UnlockResults, UnlockErrors;
		FLoreSourceControlUtils::RunLoreCommand(TEXT("lock release"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), InCommand.Files, UnlockResults, UnlockErrors);
	}

	FLoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Files, *InCommand.Provider, InCommand.ErrorMessages, States);
	InCommand.Provider->UpdateCurrentBranchName();
	return InCommand.bCommandSuccessful;
}

bool FLoreRevertWorker::UpdateStates() const
{
	return FLoreSourceControlUtils::UpdateCachedStates(States);
}

//-----------------------------------------------------------------------------
// MarkForAdd
//-----------------------------------------------------------------------------
bool FLoreMarkForAddWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	// Deliberately does not call `lore stage` here - staging is left entirely to CheckIn's own
	// unconditional `stage --scan` right before commit, same as modified files. Keeps "staged in
	// lore" meaning only one thing: about to be committed, not "the editor touched it at some point".
	InCommand.bCommandSuccessful = FLoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Files, *InCommand.Provider, InCommand.ErrorMessages, States);
	InCommand.Provider->UpdateCurrentBranchName();
	return InCommand.bCommandSuccessful;
}

bool FLoreMarkForAddWorker::UpdateStates() const
{
	return FLoreSourceControlUtils::UpdateCachedStates(States);
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
		if (InCommand.Provider->TryGetStateFromCache(File, CachedState) && CachedState.CanAdd())
		{
			FilesToUnstage.Add(File);
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

	FLoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Files, *InCommand.Provider, InCommand.ErrorMessages, States);
	InCommand.Provider->UpdateCurrentBranchName();
	return InCommand.bCommandSuccessful;
}

bool FLoreDeleteWorker::UpdateStates() const
{
	return FLoreSourceControlUtils::UpdateCachedStates(States);
}

//-----------------------------------------------------------------------------
// Unlock = lore lock release
//-----------------------------------------------------------------------------
bool FLoreUnlockWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	if (FLoreSourceControlUtils::ShouldLockFiles())
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

	FLoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Files, *InCommand.Provider, InCommand.ErrorMessages, States);
	InCommand.Provider->UpdateCurrentBranchName();
	return InCommand.bCommandSuccessful;
}

bool FLoreUnlockWorker::UpdateStates() const
{
	return FLoreSourceControlUtils::UpdateCachedStates(States);
}

#undef LOCTEXT_NAMESPACE
