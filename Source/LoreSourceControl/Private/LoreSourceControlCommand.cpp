// Copyright Solessfir 2026. All Rights Reserved.

#include "LoreSourceControlCommand.h"
#include "ILoreSourceControlWorker.h"
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
