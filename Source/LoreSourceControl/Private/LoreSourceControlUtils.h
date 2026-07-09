// Copyright Solessfir 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LoreSourceControlRevision.h"

class FLoreSourceControlState;
class FLoreSourceControlProvider;

/** One entry from "lore branch list" */
struct FLoreBranchInfo
{
	FString Name;
	bool bIsCurrent = false;
};

/** Repository-level facts extracted from one "lore status" run, alongside the per-file states. */
struct FLoreStatusSummary
{
	/** Current branch, from the "repositoryStatusRevision" event (empty if not reported) */
	FString BranchName;

	/** The remote is ahead of (or has diverged from) the local branch - a sync is needed */
	bool bIsRemoteAhead = false;
};

namespace FLoreSourceControlUtils
{
	/**
	 * Returns the effective path (or command name) to use for the lore executable.
	 *
	 * Priority:
	 *   1. User-specified path from ULoreSourceControlSettings (Project Settings)
	 *   2. "lore" / "lore.exe" if available via system PATH (tried by executing)
	 *   3. Default install location: C:/Program Files/lore/lore.exe (and variants)
	 *   4. Bare "lore.exe" as last resort
	 */
	FString FindLoreBinaryPath();

	/**
	 * Read the top-level "remote_url" and "identity" scalars out of ".lore/config.toml".
	 * Returns false only if the file could not be read; either output may still be empty
	 * (e.g. identity is unset until the first commit, or the repo was created fully offline).
	 */
	bool ReadRepositoryConfig(const FString& InRepositoryRoot, FString& OutRemoteUrl, FString& OutIdentity);

	/**
	 * Run "lore --version" to verify the binary works.
	 */
	bool CheckLoreAvailability(const FString& InLoreBinaryPath);

	/**
	 * Find the lore repository root by walking up looking for a ".lore" directory.
	 */
	bool FindRootDirectory(const FString& InPath, FString& OutRepositoryRoot);

	/**
	 * Run a lore command and capture output.
	 * The command is executed with working directory = RepositoryRoot if provided.
	 * bUseJson defaults to true because every internal caller parses structured events out of
	 * OutResults - pass false only to get lore's own human-readable text (e.g. for printing status
	 * straight to the log), never for a result this module intends to parse itself.
	 */
	bool RunLoreCommand(const FString& InCommand, const FString& InLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages, bool bUseJson = true);

	/**
	 * Run "lore status" (with --scan recommended for accuracy) and parse results into states.
	 *
	 * InProvider must be the caller's provider, captured on the game thread (FLoreSourceControlCommand::Provider) -
	 * this can run on a background thread, where looking the provider up via FModuleManager/ISourceControlModule
	 * has raced the game thread and deadlocked (observed hanging indefinitely on new-asset creation).
	 */
	bool RunUpdateStatus(const FString& InLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InFiles, FLoreSourceControlProvider& InProvider, TArray<FString>& OutErrorMessages, TArray<FLoreSourceControlState>& OutStates);

	/**
	 * Parser for lore status --json output. Populates per-file states and, if OutSummary is given,
	 * the repository-level facts (branch name, dirty/behind-remote flags) from the same single pass.
	 */
	void ParseStatusResults(const FString& InResults, const TArray<FString>& InFiles, const FString& InRepositoryRoot, TArray<FLoreSourceControlState>& OutStates, FLoreStatusSummary* OutSummary = nullptr);

	/**
	 * Run `lore file history <path>` and parse it into revision history entries, combining each
	 * "fileHistory" event with the "metadata" events (message/created-by/timestamp) that follow it -
	 * Lore reports commit message/author/date as separate metadata events, not on the history entry itself.
	 */
	bool RunGetHistory(const FString& InLoreBinary, const FString& InRepositoryRoot, const FString& InFile, TArray<FString>& OutErrorMessages, FLoreSourceControlHistory& OutHistory);

	/**
	 * Run `lore branch list` (local branches only) and parse the resulting "branchListEntry" events.
	 */
	bool RunGetBranches(const FString& InLoreBinary, const FString& InRepositoryRoot, TArray<FLoreBranchInfo>& OutBranches);

	/**
	 * Run `lore branch switch <name>` to switch the working copy to a different branch.
	 * Caller is responsible for warning the user beforehand - this changes files on disk.
	 */
	bool RunSwitchBranch(const FString& InLoreBinary, const FString& InRepositoryRoot, const FString& InBranchName, TArray<FString>& OutErrorMessages);

	/**
	 * Run `lore branch diff <target>` (current branch vs. target) and collect the repository-relative
	 * paths of every changed file. Used before a branch switch to decide whether Source/Config files
	 * are affected (requires an editor restart) or the change is Content-only (safe to hot-reload).
	 */
	bool RunGetBranchDiff(const FString& InLoreBinary, const FString& InRepositoryRoot, const FString& InTargetBranch, TArray<FString>& OutChangedPaths, TArray<FString>& OutErrorMessages);

	/**
	 * Splits a list of repository-relative changed paths into "touches Source/Config/.uplugin/.uproject"
	 * (returns true - needs an editor restart) vs plain Content paths (returned via OutContentPaths,
	 * safe to hot-reload). Shared by branch switch and sync, which both need this same classification.
	 */
	bool ClassifyChangedPaths(const TArray<FString>& InPaths, TArray<FString>& OutContentPaths);

	/**
	 * Run `lore sync` and classify the "revisionSyncFile" events it reports the same way a branch
	 * switch is classified (see ClassifyChangedPaths) - so a Content-only sync can auto-reload
	 * instead of just telling the user to reload manually.
	 */
	bool RunSync(const FString& InLoreBinary, const FString& InRepositoryRoot, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages, TArray<FString>& OutChangedContentPaths, bool& OutRequiresRestart);

	/**
	 * Query every lock on the repository's current branch ("lock query --branch <name>").
	 * Unlike "lock status", this needs no file list - it lists every locked path in one call,
	 * which is what both a broad refresh and a single-file one actually need.
	 */
	bool GetLoreLockStatus(const FString& InLoreBinary, const FString& InRepositoryRoot, const FLoreSourceControlProvider& InProvider, TMap<FString, FString>& OutLockedBy);

	/**
	 * Settings using UDeveloperSettings (Project Settings > Editor > Lore Source Control).
	 * These replace the old manual ini file.
	 */
	void LoadSettings(FString& OutLoreBinaryPath);
	void SaveSettings(const FString& InLoreBinaryPath);

	/**
	 * Get the user-configured binary path from ULoreSourceControlSettings (may be empty).
	 */
	FString GetUserConfiguredLoreBinaryPath();

	/**
	 * Persist a binary path into the developer settings.
	 */
	void SetUserConfiguredLoreBinaryPath(const FString& InPath);

	/**
	 * Whether Check Out/Revert/Submit should actually acquire/release Lore locks (ULoreSourceControlSettings::bLockFiles).
	 */
	bool ShouldLockFiles();

	/**
	 * Update the provider's cached states from worker results.
	 */
	bool UpdateCachedStates(const TArray<FLoreSourceControlState>& InStates);

	/**
	 * Retrieve the name of the currently active branch.
	 * Preferred: `lore branch info` ("branchInfo" event)
	 * Fallbacks: `lore status --revision-only` ("repositoryStatusRevision" event),
	 * then `lore branch list` (the entry with isCurrent set).
	 */
	FString GetCurrentBranchName(const FString& InLoreBinary, const FString& InRepositoryRoot);

} // namespace FLoreSourceControlUtils
