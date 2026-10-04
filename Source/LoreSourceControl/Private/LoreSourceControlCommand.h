// Copyright Solessfir 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlOperation.h"
#include "ISourceControlProvider.h"
#include "ILoreSourceControlWorker.h"
#include "LoreSourceControlUtils.h"
#include "Misc/IQueuedWork.h"

class FLoreSourceControlProvider;
class FLoreSourceControlState;
struct FLoreLockOwner;

/**
 * A single Lore operation in flight. Implements IQueuedWork so it can be run on the engine's
 * thread pool (see FLoreSourceControlProvider::IssueCommand) - the actual lore invocation
 * never runs on the game thread, so a slow or unresponsive binary cannot freeze the editor.
 */
class LORESOURCECONTROL_API FLoreSourceControlCommand : public IQueuedWork
{
public:
	using FRunLoreCommand = TFunction<bool(const FString&, const TArray<FString>&, const TArray<FString>&, TArray<FString>&, TArray<FString>&)>;
	using FReadStagedPaths = TFunction<bool(TArray<FString>&, TArray<FString>&, TArray<FString>&)>;
	using FRefreshStatus = TFunction<bool(const TArray<FString>&, bool, TArray<FString>&, TArray<FLoreSourceControlState>&)>;
	using FQueryLockStatus = TFunction<bool(TLorePathMap<FLoreLockOwner>&, TArray<FString>&, FString*)>;

	FLoreSourceControlCommand(const FSourceControlOperationRef& InOperation, const FLoreSourceControlWorkerRef& InWorker)
		: Operation(InOperation)
		, Worker(InWorker)
		, bAutoDelete(true)
		, bCommandSuccessful(false)
		, bExecuteProcessed(0)
	{
	}

	/** Runs the worker and records completion. Called from DoThreadedWork() on a pool thread. */
	bool DoWork();

	//~ IQueuedWork interface
	virtual void DoThreadedWork() override;
	virtual void Abandon() override;

	/** Save any accumulated messages and fire the completion delegate. Called on the game thread. */
	ECommandResult::Type ReturnResults();

	/** Execute through the real Lore utility unless a worker test supplied a deterministic replacement. */
	bool RunLoreCommand(const FString& InCommand, const TArray<FString>& InParameters, const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages) const;
	bool ReadStagedPaths(TArray<FString>& OutStagedFiles, TArray<FString>& OutStagedDirectories, TArray<FString>& OutErrorMessages) const;
	bool RefreshStatus(const TArray<FString>& InFiles, bool bQueryLocks, TArray<FString>& OutErrorMessages, TArray<FLoreSourceControlState>& OutStates) const;
	bool QueryLockStatus(TLorePathMap<FLoreLockOwner>& OutLockedBy, TArray<FString>& OutErrorMessages, FString* OutOwnIdentity = nullptr) const;

	/**
	 * Provider that issued this command, captured on the game thread at Execute() time. Workers
	 * must go through this instead of looking the provider up via FModuleManager/ISourceControlModule
	 * themselves: DoThreadedWork() runs on a pool thread, and FModuleManager access from there has
	 * raced with the game thread and deadlocked (observed hanging indefinitely on new-asset creation,
	 * where the game thread is itself busy with module-touching Content Browser/Asset Registry work).
	 */
	FLoreSourceControlProvider* Provider = nullptr;

	/** Operation we want to perform */
	FSourceControlOperationRef Operation;

	/** The worker to perform this operation */
	FLoreSourceControlWorkerRef Worker;

	/** Delegate to call when the operation completes */
	FSourceControlOperationComplete OperationCompleteDelegate;

	/** Files to perform the operation on */
	TArray<FString> Files;

	/** Lore binary path */
	FString PathToLoreBinary;

	/** Root of the lore repository (working copy) */
	FString PathToRepositoryRoot;

	/** Info and error messages */
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;

	/** Whether we should auto-delete this command (for async) */
	bool bAutoDelete;

	/** Whether the command succeeded */
	bool bCommandSuccessful;

	/** Settings snapshot captured on the game thread; UObject settings must not be read by workers. */
	bool bShouldLockFiles = true;

	/** Repository values captured with the rest of the command inputs. */
	bool bHasRemote = false;
	FString Identity;

	/** Narrow command boundaries used by worker-level tests. Production commands leave these unbound. */
	FRunLoreCommand RunLoreCommandOverride;
	FReadStagedPaths ReadStagedPathsOverride;
	FRefreshStatus RefreshStatusOverride;
	FQueryLockStatus QueryLockStatusOverride;

	/** True once the provider has submitted this FIFO entry to the engine thread pool. */
	bool bDispatched = false;

	/** Set (atomically) once the pool thread has finished DoWork(); polled by the game thread */
	volatile int32 bExecuteProcessed;
};
