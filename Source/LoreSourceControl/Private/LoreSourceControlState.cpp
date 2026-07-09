// Copyright Solessfir 2026. All Rights Reserved.

#include "LoreSourceControlState.h"
#include "RevisionControlStyle/RevisionControlStyle.h"

TSharedPtr<ISourceControlRevision> FLoreSourceControlState::GetHistoryItem(int32 HistoryIndex) const
{
	if (History.IsValidIndex(HistoryIndex))
	{
		return History[HistoryIndex];
	}
	return nullptr;
}

TSharedPtr<ISourceControlRevision> FLoreSourceControlState::FindHistoryRevision(int32 RevisionNumber) const
{
	for (const auto& Revision : History)
	{
		if (Revision->GetRevisionNumber() == RevisionNumber)
		{
			return Revision;
		}
	}
	return nullptr;
}

TSharedPtr<ISourceControlRevision> FLoreSourceControlState::FindHistoryRevision(const FString& InRevision) const
{
	for (const auto& Revision : History)
	{
		if (Revision->GetRevision() == InRevision)
		{
			return Revision;
		}
	}
	return nullptr;
}

#if SOURCE_CONTROL_WITH_SLATE
FSlateIcon FLoreSourceControlState::GetIcon() const
{
	if (IsConflicted())
	{
		return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.Conflicted");
	}

	if (!IsCurrent())
	{
		return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.NotAtHeadRevision");
	}

	if (bIsCheckedOutOther)
	{
		return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.CheckedOutByOtherUser", NAME_None, "RevisionControl.CheckedOutByOtherUserBadge");
	}

	if (IsCheckedOut())
	{
		return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.CheckedOut");
	}

	if (IsAdded())
	{
		return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.OpenForAdd");
	}

	if (IsDeleted())
	{
		return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.MarkedForDelete");
	}

	if (IsModified())
	{
		return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.ModifiedLocally");
	}

	if (!IsSourceControlled())
	{
		return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.NotInDepot");
	}

	// Clean and up to date: no overlay icon, matching Git/Perforce/Plastic convention.
	return FSlateIcon();
}
#endif

bool FLoreSourceControlState::IsCheckedOutOther(FString* Who) const
{
	if (Who)
	{
		*Who = CheckedOutOther;
	}
	return bIsCheckedOutOther;
}

FText FLoreSourceControlState::GetDisplayName() const
{
	if (IsConflicted())
	{
		return FText::FromString(TEXT("Conflicted"));
	}

	if (IsCheckedOut())
	{
		return FText::FromString(TEXT("Checked Out"));
	}

	if (IsAdded())
	{
		return FText::FromString(TEXT("Added"));
	}

	if (IsModified())
	{
		return FText::FromString(TEXT("Modified"));
	}

	if (!IsCurrent())
	{
		return FText::FromString(TEXT("Not at head"));
	}

	if (IsSourceControlled())
	{
		return FText::FromString(TEXT("Under Lore"));
	}

	return FText::FromString(TEXT("Not Under Lore"));
}

FText FLoreSourceControlState::GetDisplayTooltip() const
{
	FString Tooltip;

	if (IsConflicted())
	{
		Tooltip = TEXT("Has conflicts that need to be resolved");
	}
	else if (bIsCheckedOutOther)
	{
		Tooltip = FString::Printf(TEXT("Checked out by %s"), *CheckedOutOther);
	}
	else if (IsCheckedOut())
	{
		Tooltip = TEXT("Checked out by you");
	}
	else if (IsAdded())
	{
		Tooltip = TEXT("Added, pending commit");
	}
	else if (IsModified())
	{
		Tooltip = TEXT("Modified locally");
	}
	else if (IsSourceControlled())
	{
		Tooltip = TEXT("Tracked by Lore");
	}
	else
	{
		Tooltip = TEXT("Not tracked by Lore");
	}

	if (!BranchName.IsEmpty())
	{
		Tooltip += FString::Printf(TEXT(" (%s)"), *BranchName);
	}

	return FText::FromString(Tooltip);
}
