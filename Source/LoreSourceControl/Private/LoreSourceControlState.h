// Copyright Solessfir 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlState.h"
#include "LoreSourceControlRevision.h"

class FLoreSourceControlState : public ISourceControlState
{
public:
	explicit FLoreSourceControlState(const FString& InLocalFilename)
		: LocalFilename(InLocalFilename)
	{
	}

	void SetBranchName(const FString& InName) { BranchName = InName; }

	// ISourceControlState interface
	virtual int32 GetHistorySize() const override { return History.Num(); }
	virtual TSharedPtr<ISourceControlRevision> GetHistoryItem(int32 HistoryIndex) const override;
	virtual TSharedPtr<ISourceControlRevision> FindHistoryRevision(int32 RevisionNumber) const override;
	virtual TSharedPtr<ISourceControlRevision> FindHistoryRevision(const FString& InRevision) const override;
	// Lore's status doesn't map onto a single "current" revision - the full history list (above) already covers this.
	virtual TSharedPtr<ISourceControlRevision> GetCurrentRevision() const override { return nullptr; }
#if SOURCE_CONTROL_WITH_SLATE
	virtual FSlateIcon GetIcon() const override;
#endif
	virtual FText GetDisplayName() const override;
	virtual FText GetDisplayTooltip() const override;
	virtual const FString& GetFilename() const override { return LocalFilename; }
	virtual const FDateTime& GetTimeStamp() const override { return TimeStamp; }
	virtual bool CanCheckIn() const override { return bCanCheckIn; }
	// Must actually be tracked by Lore already, and not already locked.
	virtual bool CanCheckout() const override { return bIsSourceControlled && !bIsCheckedOut && !bIsCheckedOutOther; }
	virtual bool IsCheckedOut() const override { return bIsCheckedOut; }
	virtual bool IsCheckedOutOther(FString* Who) const override;
	virtual bool IsCheckedOutInOtherBranch(const FString& CurrentBranch = FString()) const override { return false; }
	virtual bool IsModifiedInOtherBranch(const FString& CurrentBranch = FString()) const override { return false; }
	virtual bool IsCheckedOutOrModifiedInOtherBranch(const FString& CurrentBranch = FString()) const override { return false; }
	virtual TArray<FString> GetCheckedOutBranches() const override { return TArray<FString>(); }
	virtual FString GetOtherUserBranchCheckedOuts() const override { return FString(); }
	virtual bool GetOtherBranchHeadModification(FString& HeadBranchOut, FString& ActionOut, int32& HeadChangeListOut) const override { return false; }
	virtual bool IsCurrent() const override { return bIsCurrent; }
	virtual bool IsSourceControlled() const override { return bIsSourceControlled; }
	virtual bool IsAdded() const override { return bIsAdded; }
	virtual bool IsDeleted() const override { return bIsDeleted; }
	// "pathIgnore" maps to "new, addable file" (see ParseStatusResults) - never surfaced as ignored.
	virtual bool IsIgnored() const override { return false; }
	// Lore locks are advisory only - files stay editable regardless of checkout state.
	virtual bool CanEdit() const override { return true; }
	virtual bool CanDelete() const override { return true; }
	virtual bool IsUnknown() const override { return bIsUnknown; }
	virtual bool IsModified() const override { return bIsModified; }
	virtual bool CanAdd() const override { return bCanAdd; }
	virtual bool CanRevert() const override { return IsModified() || IsCheckedOut() || IsAdded(); }

	// Extra data we track
	FString LocalFilename;
	FDateTime TimeStamp = FDateTime::Now();
	FString BranchName;
	FLoreSourceControlHistory History;

	// State flags
	bool bIsUnknown = false;
	bool bIsSourceControlled = false;
	bool bIsAdded = false;
	bool bIsDeleted = false;
	bool bIsModified = false;
	bool bIsCheckedOut = false;
	bool bIsCheckedOutOther = false;
	FString CheckedOutOther;
	bool bCanAdd = false;
	bool bCanCheckIn = false;
	bool bIsCurrent = true;
};
