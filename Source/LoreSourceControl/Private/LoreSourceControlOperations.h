// Copyright Solessfir 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ILoreSourceControlWorker.h"
#include "SourceControlOperationBase.h"

class FLoreSourceControlState;
class FLoreSourceControlProvider;
struct FLoreBranchInfo;

/** Internal operation used to serialize a human-readable console status with all other Lore work. */
class FLorePrintStatusOperation : public FSourceControlOperationBase
{
public:
	virtual FName GetName() const override { return "LorePrintStatus"; }
	virtual FText GetInProgressString() const override { return NSLOCTEXT("LoreSourceControl", "PrintingStatus", "Reading Lore status..."); }
};

/** Internal operation used to refresh the branch menu without unmanaged fire-and-forget tasks. */
class FLoreRefreshBranchesOperation : public FSourceControlOperationBase
{
public:
	virtual FName GetName() const override { return "LoreRefreshBranches"; }
	virtual FText GetInProgressString() const override { return NSLOCTEXT("LoreSourceControl", "RefreshingBranches", "Refreshing Lore branches..."); }
};

/** Internal operation for a non-blocking branch switch. */
class FLoreSwitchBranchOperation : public FSourceControlOperationBase
{
public:
	enum class EOutcome : uint8
	{
		Success,
		StagedStateBlocked,
		Failed
	};

	explicit FLoreSwitchBranchOperation(FString InBranchName)
		: BranchName(MoveTemp(InBranchName))
	{
	}

	virtual FName GetName() const override { return "LoreSwitchBranch"; }
	virtual FText GetInProgressString() const override { return NSLOCTEXT("LoreSourceControl", "SwitchingBranch", "Switching Lore branch..."); }

	const FString& GetBranchName() const { return BranchName; }
	EOutcome GetOutcome() const { return Outcome; }
	void SetOutcome(EOutcome InOutcome) { Outcome = InOutcome; }

private:
	FString BranchName;
	EOutcome Outcome = EOutcome::Failed;
};

/** Connect / initialize */
class FLoreConnectWorker : public ILoreSourceControlWorker
{
public:
	virtual FName GetName() const override { return "Connect"; }
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	TArray<FLoreSourceControlState> States;
};

/** CheckIn / Commit */
class LORESOURCECONTROL_API FLoreCheckInWorker : public ILoreSourceControlWorker
{
public:
	virtual FName GetName() const override { return "CheckIn"; }
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	TArray<FLoreSourceControlState> States;
};

/** Sync / Pull */
class FLoreSyncWorker : public ILoreSourceControlWorker
{
public:
	virtual FName GetName() const override { return "Sync"; }
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	TArray<FLoreSourceControlState> States;

	/** Content paths sync brought in, classified via FLoreSourceControlUtils::ClassifyChangedPaths */
	TArray<FString> ChangedContentPaths;

	/** True if sync touched Source/Config/.uplugin/.uproject - needs an editor restart, no auto-reload */
	bool bRequiresRestart = false;
	bool bSyncSucceeded = false;

};

/** UpdateStatus */
class FLoreUpdateStatusWorker : public ILoreSourceControlWorker
{
public:
	virtual FName GetName() const override { return "UpdateStatus"; }
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	TArray<FLoreSourceControlState> States;
};

/** CheckOut (acquire lock) */
class FLoreCheckOutWorker : public ILoreSourceControlWorker
{
public:
	virtual FName GetName() const override { return "CheckOut"; }
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	TArray<FLoreSourceControlState> States;
};

/** Revert */
class FLoreRevertWorker : public ILoreSourceControlWorker
{
public:
	virtual FName GetName() const override { return "Revert"; }
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	TArray<FLoreSourceControlState> States;
};

/** MarkForAdd (stage) */
class FLoreMarkForAddWorker : public ILoreSourceControlWorker
{
public:
	virtual FName GetName() const override { return "MarkForAdd"; }
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	TArray<FLoreSourceControlState> States;
};

/** Delete */
class FLoreDeleteWorker : public ILoreSourceControlWorker
{
public:
	virtual FName GetName() const override { return "Delete"; }
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	TArray<FLoreSourceControlState> States;
};

/** Unlock (release lore lock) */
class FLoreUnlockWorker : public ILoreSourceControlWorker
{
public:
	virtual FName GetName() const override { return "Unlock"; }
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	TArray<FLoreSourceControlState> States;
};

class FLorePrintStatusWorker : public ILoreSourceControlWorker
{
public:
	virtual FName GetName() const override { return "LorePrintStatus"; }
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override { return false; }
};

class FLoreRefreshBranchesWorker : public ILoreSourceControlWorker
{
public:
	virtual FName GetName() const override { return "LoreRefreshBranches"; }
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

private:
	TArray<FLoreBranchInfo> Branches;
	bool bRefreshSucceeded = false;
};

class FLoreSwitchBranchWorker : public ILoreSourceControlWorker
{
public:
	virtual FName GetName() const override { return "LoreSwitchBranch"; }
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

private:
	TArray<FLoreSourceControlState> States;
	TArray<FString> ChangedContentPaths;
	bool bSwitchSucceeded = false;
	bool bRequiresRestart = false;
};
