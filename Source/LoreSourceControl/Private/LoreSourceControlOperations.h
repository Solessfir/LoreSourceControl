// Copyright Solessfir 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ILoreSourceControlWorker.h"

class FLoreSourceControlState;
class FLoreSourceControlProvider;

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
class FLoreCheckInWorker : public ILoreSourceControlWorker
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

	/** Captured on the game thread during Execute() so UpdateStates() can trigger a Content reload */
	FLoreSourceControlProvider* Provider = nullptr;
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
