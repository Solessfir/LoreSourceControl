// Copyright Solessfir 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FLoreSourceControlCommand;

class ILoreSourceControlWorker : public TSharedFromThis<ILoreSourceControlWorker>
{
public:
	virtual ~ILoreSourceControlWorker() {}

	/**
	 * Name of the operation this worker handles (e.g. "Sync", "CheckIn")
	 */
	virtual FName GetName() const = 0;

	/**
	 * Execute the operation (called on worker thread for async)
	 */
	virtual bool Execute(FLoreSourceControlCommand& InCommand) = 0;

	/**
	 * Update the source control states based on the results of the operation.
	 */
	virtual bool UpdateStates() const = 0;
};

typedef TSharedRef<ILoreSourceControlWorker> FLoreSourceControlWorkerRef;
typedef TSharedPtr<ILoreSourceControlWorker> FLoreSourceControlWorkerPtr;

DECLARE_DELEGATE_RetVal(FLoreSourceControlWorkerRef, FLoreGetSourceControlWorker)
