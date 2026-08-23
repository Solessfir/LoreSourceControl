// Copyright Solessfir 2026. All Rights Reserved.

#include "LoreSourceControlState.h"
#include "RevisionControlStyle/RevisionControlStyle.h"

#define LOCTEXT_NAMESPACE "LoreSourceControl"

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
		return LOCTEXT("StateConflicted", "Conflicted");
	}

	if (bIsCheckedOutOther)
	{
		return LOCTEXT("StateCheckedOutByOther", "Checked Out by Other User");
	}

	if (IsCheckedOut())
	{
		return LOCTEXT("StateCheckedOut", "Checked Out");
	}

	if (IsAdded())
	{
		return LOCTEXT("StateAdded", "Added");
	}

	if (IsDeleted())
	{
		return LOCTEXT("StateDeleted", "Deleted");
	}

	if (IsModified())
	{
		return LOCTEXT("StateModified", "Modified");
	}

	if (IsIgnored())
	{
		return LOCTEXT("StateIgnored", "Ignored");
	}

	if (!IsCurrent())
	{
		return LOCTEXT("StateNotAtHead", "Not at head");
	}

	if (IsSourceControlled())
	{
		return LOCTEXT("StateUnderLore", "Under Lore");
	}

	return LOCTEXT("StateNotUnderLore", "Not Under Lore");
}

FText FLoreSourceControlState::GetDisplayTooltip() const
{
	FText Tooltip;

	if (IsConflicted())
	{
		Tooltip = LOCTEXT("StateConflictedTooltip", "Has conflicts that need to be resolved");
	}
	else if (bIsCheckedOutOther)
	{
		Tooltip = FText::Format(LOCTEXT("StateCheckedOutByOtherTooltip", "Checked out by {0}"), FText::FromString(CheckedOutOther));
	}
	else if (IsCheckedOut())
	{
		Tooltip = LOCTEXT("StateCheckedOutTooltip", "Checked out by you");
	}
	else if (IsAdded())
	{
		Tooltip = LOCTEXT("StateAddedTooltip", "Added, pending commit");
	}
	else if (IsDeleted())
	{
		Tooltip = LOCTEXT("StateDeletedTooltip", "Deleted, pending commit");
	}
	else if (IsModified())
	{
		Tooltip = LOCTEXT("StateModifiedTooltip", "Modified locally");
	}
	else if (IsIgnored())
	{
		Tooltip = LOCTEXT("StateIgnoredTooltip", "Ignored by Lore");
	}
	else if (IsSourceControlled())
	{
		Tooltip = LOCTEXT("StateUnderLoreTooltip", "Tracked by Lore");
	}
	else
	{
		Tooltip = LOCTEXT("StateNotUnderLoreTooltip", "Not tracked by Lore");
	}

	if (!BranchName.IsEmpty())
	{
		Tooltip = FText::Format(LOCTEXT("StateTooltipWithBranch", "{0} ({1})"), Tooltip, FText::FromString(BranchName));
	}

	return Tooltip;
}

#undef LOCTEXT_NAMESPACE
