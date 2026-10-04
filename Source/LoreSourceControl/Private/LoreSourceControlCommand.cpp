// Copyright Solessfir 2026. All Rights Reserved.

#include "LoreSourceControlCommand.h"
#include "ILoreSourceControlWorker.h"
#include "LoreSourceControlProvider.h"
#include "LoreSourceControlUtils.h"
#include "HAL/PlatformAtomics.h"

bool FLoreSourceControlCommand::DoWork()
{
	bCommandSuccessful = Worker->Execute(*this);
	FPlatformAtomics::InterlockedExchange(&bExecuteProcessed, 1);

	return bCommandSuccessful;
}

void FLoreSourceControlCommand::DoThreadedWork()
{
	DoWork();
}

void FLoreSourceControlCommand::Abandon()
{
	FPlatformAtomics::InterlockedExchange(&bExecuteProcessed, 1);
}

ECommandResult::Type FLoreSourceControlCommand::ReturnResults()
{
	for (const FString& Message : InfoMessages)
	{
		Operation->AddInfoMessge(FText::FromString(Message));
	}

	for (const FString& Message : ErrorMessages)
	{
		Operation->AddErrorMessge(FText::FromString(Message));
	}

	const ECommandResult::Type Result = bCommandSuccessful ? ECommandResult::Succeeded : ECommandResult::Failed;
	OperationCompleteDelegate.ExecuteIfBound(Operation, Result);
	return Result;
}

bool FLoreSourceControlCommand::RunLoreCommand(const FString& InCommand, const TArray<FString>& InParameters, const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages) const
{
	if (RunLoreCommandOverride)
	{
		return RunLoreCommandOverride(InCommand, InParameters, InFiles, OutResults, OutErrorMessages);
	}

	return FLoreSourceControlUtils::RunLoreCommand(InCommand, PathToLoreBinary, PathToRepositoryRoot, InParameters, InFiles, OutResults, OutErrorMessages);
}

bool FLoreSourceControlCommand::ReadStagedPaths(TArray<FString>& OutStagedFiles, TArray<FString>& OutStagedDirectories, TArray<FString>& OutErrorMessages) const
{
	if (ReadStagedPathsOverride)
	{
		return ReadStagedPathsOverride(OutStagedFiles, OutStagedDirectories, OutErrorMessages);
	}

	return FLoreSourceControlUtils::RunGetStagedPaths(PathToLoreBinary, PathToRepositoryRoot, OutStagedFiles, OutStagedDirectories, OutErrorMessages);
}

bool FLoreSourceControlCommand::RefreshStatus(const TArray<FString>& InFiles, bool bQueryLocks, TArray<FString>& OutErrorMessages, TArray<FLoreSourceControlState>& OutStates) const
{
	if (RefreshStatusOverride)
	{
		return RefreshStatusOverride(InFiles, bQueryLocks, OutErrorMessages, OutStates);
	}

	if (!Provider)
	{
		OutErrorMessages.Add(TEXT("Lore status refresh requires a source control provider."));
		return false;
	}

	return FLoreSourceControlUtils::RunUpdateStatus(PathToLoreBinary, PathToRepositoryRoot, InFiles, *Provider, bQueryLocks, OutErrorMessages, OutStates);
}

bool FLoreSourceControlCommand::QueryLockStatus(TLorePathMap<FLoreLockOwner>& OutLockedBy, TArray<FString>& OutErrorMessages, FString* OutOwnIdentity) const
{
	if (QueryLockStatusOverride)
	{
		if (OutOwnIdentity)
		{
			*OutOwnIdentity = Identity;
		}
		return QueryLockStatusOverride(OutLockedBy, OutErrorMessages, OutOwnIdentity);
	}

	if (!Provider)
	{
		OutErrorMessages.Add(TEXT("Lore lock query requires a source control provider."));
		return false;
	}

	return FLoreSourceControlUtils::GetLoreLockStatus(PathToLoreBinary, PathToRepositoryRoot, *Provider, OutLockedBy, &OutErrorMessages, OutOwnIdentity);
}
