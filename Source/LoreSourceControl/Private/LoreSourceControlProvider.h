// Copyright Solessfir 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlProvider.h"
#include "ILoreSourceControlWorker.h"
#include "LoreSourceControlState.h"
#include "LoreSourceControlUtils.h"

class FLoreSourceControlCommand;

class FLoreSourceControlProvider : public ISourceControlProvider
{
public:
	/* ISourceControlProvider implementation */
	virtual void Init(bool bForceConnection = true) override;
	virtual void Close() override;
	virtual FText GetStatusText() const override;
	virtual TMap<EStatus, FString> GetStatus() const override;
	virtual bool IsEnabled() const override;
	virtual bool IsAvailable() const override;
	virtual const FName& GetName() const override;
	virtual bool QueryStateBranchConfig(const FString& ConfigSrc, const FString& ConfigDest) override { return false; }
	virtual void RegisterStateBranches(const TArray<FString>& BranchNames, const FString& ContentRoot) override {}
	virtual int32 GetStateBranchIndex(const FString& InBranchName) const override { return INDEX_NONE; }
	virtual bool GetStateBranchAtIndex(int32 BranchIndex, FString& OutBranchName) const override { return false; }
	virtual ECommandResult::Type GetState(const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override;
	virtual ECommandResult::Type GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override;
	virtual TArray<FSourceControlStateRef> GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const override;
	virtual FDelegateHandle RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged) override;
	virtual void UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle) override;
	virtual ECommandResult::Type Execute(const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency = EConcurrency::Synchronous, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete()) override;
	virtual bool CanExecuteOperation(const FSourceControlOperationRef& InOperation) const override;
	virtual bool CanCancelOperation(const FSourceControlOperationRef& InOperation) const override;
	virtual void CancelOperation(const FSourceControlOperationRef& InOperation) override;
	virtual bool UsesLocalReadOnlyState() const override;
	virtual bool UsesChangelists() const override;
	virtual bool UsesUncontrolledChangelists() const override;
	virtual bool UsesCheckout() const override;
	virtual bool UsesFileRevisions() const override;
	virtual bool UsesSnapshots() const override;
	virtual bool UsesSoftRevertOnDelete() const override;
	virtual bool AllowsDiffAgainstDepot() const override;
	virtual TOptional<bool> HasChangesToSync() const override;
	virtual TOptional<bool> HasChangesToCheckIn() const override;

	/** Override to prevent accepting settings if lore path is not resolved */
	virtual ECommandResult::Type Login(const FString& InPassword = FString(), EConcurrency::Type InConcurrency = EConcurrency::Synchronous, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete()) override;
	virtual void Tick() override;
	virtual TArray<TSharedRef<ISourceControlLabel>> GetLabels(const FString& InMatchingSpec) const override;
	virtual TArray<FSourceControlChangelistRef> GetChangelists(EStateCacheUsage::Type InStateCacheUsage) override;
#if SOURCE_CONTROL_WITH_SLATE
	virtual TSharedRef<SWidget> MakeSettingsWidget() const override;
#endif

	using ISourceControlProvider::Execute;

	/** Get the current lore binary path (from settings or auto detected) */
	FString GetLoreBinaryPath() const;

	/** Set lore binary path */
	bool SetLoreBinaryPath(const FString& InPath);

	/** Check if lore is available */
	void CheckLoreAvailability();

	/** Check if current workspace is a lore repository */
	void CheckRepositoryStatus();

	/** Register a worker for a specific operation name */
	void RegisterWorker(const FName& InName, const FLoreGetSourceControlWorker& InDelegate);

	/** Run a command on the thread pool and block (pumping Slate) until it completes */
	ECommandResult::Type ExecuteSynchronousCommand(FLoreSourceControlCommand& InCommand, const FText& Task);

	/** Queue a command onto the thread pool; Tick() picks up the result when it finishes */
	ECommandResult::Type IssueCommand(FLoreSourceControlCommand& InCommand);

	/** Copy the cached state for a file (thread safe). Returns false if the file has no cached state. */
	bool TryGetStateFromCache(const FString& Filename, FLoreSourceControlState& OutState) const;

	/** Add/update cached states, stamping the current branch name onto each (thread safe) */
	void AddStatesToCache(const TArray<FLoreSourceControlState>& InStates);

	/** Remove a state from cache */
	bool RemoveStateFromCache(const FString& Filename);

	/** Invoke state changed delegate */
	void BroadcastStateChanged() const;

	/** Load/save settings */
	void LoadSettings();
	void SaveSettings() const;

	/** Update the known current branch name (called after connect or branch-changing operations) */
	void UpdateCurrentBranchName();

	/** Directly set the branch name (used when we already parsed it from status output) */
	void SetBranchName(const FString& InBranchName);

	/** Get the current branch name (thread safe) */
	FString GetBranchName() const;

	/** Get the repository root path (thread safe) */
	FString GetRepositoryRoot() const;

	/** Returns the last-fetched branch list instantly, without shelling out to lore.exe (see RefreshBranchesAsync) */
	TArray<FLoreBranchInfo> GetCachedBranches() const;

	/** Number of branches as of the last fetch (cheap, cached - avoids shelling out to lore.exe on every UI tick) */
	int32 GetCachedBranchCount() const;

	/** Re-fetches the branch list on a background thread and updates the cache when done (fire-and-forget) */
	void RefreshBranchesAsync();

	enum class ELoreSwitchResult : uint8
	{
		Success,
		/** Lore refused because there is a pending staged (committed-to-stage, not yet pushed) change */
		StagedStateBlocked,
		Failed
	};

	/**
	 * Diffs the current branch against InTargetBranch to decide whether the switch would touch
	 * Source/Config/.uplugin files (needs an editor restart) or only Content (safe to hot-reload).
	 * Returns true (the safe default) if the diff itself could not be determined.
	 * OutChangedContentPaths is filled with the repository-relative paths of changed Content files.
	 */
	bool DoesBranchSwitchTouchCode(const FString& InTargetBranch, TArray<FString>& OutChangedContentPaths) const;

	/**
	 * Switch the working copy to a different branch, synchronously, then refresh status.
	 * Caller is responsible for warning the user beforehand - this changes files on disk.
	 */
	ELoreSwitchResult SwitchBranch(const FString& InBranchName);

	/**
	 * Finds any currently loaded packages under the given repository-relative Content paths and
	 * reloads them from disk - used after a branch switch or sync that only touched Content, where
	 * a full editor restart isn't necessary. Must be called from the game thread.
	 */
	void ReloadContentPackages(const TArray<FString>& InChangedRelativePaths) const;

	/** Get the repository's configured remote server URL, e.g. "lore://host:port/repo" (thread safe) */
	FString GetRemoteUrl() const;

	/** Get the commit identity configured for this repository, if any (thread safe) */
	FString GetIdentity() const;

	/** Returns true if the lore binary was found and can be executed */
	bool IsLoreBinaryAvailable() const;

	/** Set the "needs to sync from remote" flag (called from status parsing) */
	void SetHasChangesToSync(const bool bInHasChanges);

private:
	/** Create a worker for a given operation */
	TSharedPtr<ILoreSourceControlWorker> CreateWorker(const FName& InOperationName) const;

	/** Send a finished command's info/error messages to the SourceControl message log */
	static void OutputCommandMessages(const FLoreSourceControlCommand& InCommand);

	/**
	 * Returns InFiles with anything locked by someone else removed, so a submit can never overwrite
	 * another user's lock - Lore's server does not stop that for us (see ShouldLockFiles). If anything
	 * was excluded, asks the user whether to revert those files now (or leave them as local, later-
	 * revertible edits) and, if they agree, fires an async Revert on them.
	 */
	TArray<FString> FilterOutOtherLockedFiles(const TArray<FString>& InFiles);

	/** Critical section for thread safety */
	mutable FCriticalSection CriticalSection;

	/** Map of registered workers by operation name */
	TMap<FName, FLoreGetSourceControlWorker> WorkersMap;

	/** Cached states */
	TMap<FString, FLoreSourceControlState> StateCache;

	/** Delegate for state changes */
	FSourceControlStateChanged OnSourceControlStateChanged;

	/** Commands queued on the thread pool, awaiting pickup by Tick() */
	TArray<FLoreSourceControlCommand*> CommandQueue;

	/** Is lore binary available */
	bool bLoreAvailable = false;

	/** Is a lore repository found */
	bool bLoreRepositoryFound = false;

	/** Path to lore.exe */
	FString LoreBinaryPath;

	/** Root of current lore repo */
	FString PathToRepositoryRoot;

	/** Current branch / info for status text */
	FString BranchName;

	/** Remote server URL, read from .lore/config.toml (may be empty for an offline-only repo) */
	FString RemoteUrl;

	/** Commit identity, read from .lore/config.toml (may be empty until first configured) */
	FString Identity;

	/** Cached "has incoming changes" (remote is ahead or diverged) */
	bool bHasChangesToSync = false;

	/** Branch list from the last successful RefreshBranchesAsync(), guarded by CriticalSection */
	TArray<FLoreBranchInfo> CachedBranches;

	/** Count from the last branch fetch, used by GetCachedBranchCount() (-1 = never fetched yet) */
	int32 CachedBranchCount = -1;

	/** Alive token for async lambdas that hop back to the game thread (see RefreshBranchesAsync) */
	TSharedPtr<uint8, ESPMode::ThreadSafe> AliveMarker = MakeShared<uint8, ESPMode::ThreadSafe>(0);
};
